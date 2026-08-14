#!/usr/bin/env python3
"""Assemble isolated FG endpoint/intermediate replays without inventing missing buffers."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any


ENDPOINT_ROLE = "FrameGenerationEndpoints"
REVERSE_ENDPOINT_ROLE = "FrameGenerationReverseEndpoints"
INTERMEDIATE_ROLE = "FrameGenerationIntermediate"
INTENTIONAL_CVAR_DIFFERENCES = {
    "r.MotionVectorSimulation": {
        "endpoints": "1",
        "reverseEndpoints": "1",
        "intermediate": "0",
        "reason": (
            "Endpoint replay preserves the last captured component transforms so t1 motion spans "
            "the complete t0-to-t1 interval; the isolated intermediate replay must not do so."
        ),
    },
}


def sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def canonical_sha1(value: Any) -> str:
    encoded = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha1(encoded).hexdigest().upper()


def require_capture(dataset: Path, role: str) -> tuple[dict[str, Any], dict[str, Any]]:
    manifest_path = dataset / "manifest.json"
    report_path = dataset / "validation_report.json"
    if not manifest_path.is_file() or not report_path.is_file():
        raise ValueError(f"capture and validation report are required: {dataset}")
    manifest = load_json(manifest_path)
    report = load_json(report_path)
    if manifest.get("state") != "Completed":
        raise ValueError(f"capture is not complete: {dataset}")
    if manifest.get("replayPass") != role:
        raise ValueError(f"expected replayPass={role}, found {manifest.get('replayPass')}: {dataset}")
    required_failures = [
        check for check in report.get("checks", [])
        if check.get("required", True) and not check.get("passed", False)
    ]
    if report.get("formatAndIntegrityGate") != "pass" or required_failures:
        raise ValueError(f"validation gate did not pass: {dataset}")
    return manifest, report


def require_wpo_fixture_gate(
    manifest: dict[str, Any], report: dict[str, Any], role: str
) -> bool:
    if not manifest.get("job", {}).get("bEnableSemanticValidationFixture", False):
        return False
    if int(report.get("validatorVersion", 0)) < 5:
        raise ValueError(f"{role} requires ValidateDataset.py validatorVersion >= 5")
    checks = {
        str(check.get("name")): bool(check.get("passed", False))
        for check in report.get("checks", [])
    }
    wpo_checks = {
        name: passed
        for name, passed in checks.items()
        if ".semantic_fixture.wpo_" in name
    }
    required_categories = (
        "_object_visible",
        "_velocity_coverage",
        "_motion_direction_magnitude",
    )
    if role != INTERMEDIATE_ROLE:
        required_categories += ("_reset_motion_zero",)
    if (
        not wpo_checks
        or not all(wpo_checks.values())
        or not all(any(name.endswith(suffix) for name in wpo_checks) for suffix in required_categories)
        or checks.get("provenance.vertex_deformation_velocity_enabled") is not True
    ):
        raise ValueError(
            f"{role} lacks a complete passing PreviousFrameSwitch WPO fixture gate"
        )
    return True


def compatible(
    endpoint: dict[str, Any],
    reverse_endpoint: dict[str, Any],
    intermediate: dict[str, Any],
) -> dict[str, Any]:
    endpoint_job = endpoint["job"]
    reverse_job = reverse_endpoint["job"]
    intermediate_job = intermediate["job"]
    fields = (
        "expectedMap",
        "captureFrameRateNumerator",
        "captureFrameRateDenominator",
        "hRResolution",
        "lRResolution",
        "randomSeed",
        "bDisableMotionBlur",
        "bLockExposure",
        "bForceSynchronousRendering",
        "bEnableSemanticValidationFixture",
        "bBlockOnStreamingBeforeCapture",
        "streamingWaitSeconds",
        "bLockTemporalJitterToLogicalFrame",
        "temporalJitterSequenceLength",
        "temporalJitterPhaseOffset",
    )
    mismatches = [
        name
        for name in fields
        if endpoint_job.get(name) != intermediate_job.get(name)
        or endpoint_job.get(name) != reverse_job.get(name)
    ]
    if mismatches:
        raise ValueError(f"forward/reverse/intermediate job mismatch: {mismatches}")
    endpoint_provenance = endpoint.get("provenance", {})
    reverse_provenance = reverse_endpoint.get("provenance", {})
    intermediate_provenance = intermediate.get("provenance", {})
    provenance_fields = (
        "engineVersion",
        "engineChangelist",
        "buildVersion",
        "projectName",
        "rhi",
        "gpuAdapter",
        "gpuVendorId",
        "gpuDeviceId",
        "contentMapSha1",
        "shaderSourceSha1",
        "streamingBarrierEnabled",
        "streamingBarrierWaitSeconds",
        "streamingBarrierComplete",
        "streamingRequestsAfterBarrier",
        "streamingTextureCountAfterBarrier",
        "pendingStreamingTextureCountAfterBarrier",
        "streamingStateAfterBarrierSha1",
        "streamingStateHashScope",
    )
    mismatches = [
        name for name in provenance_fields
        if endpoint_provenance.get(name) != intermediate_provenance.get(name)
        or endpoint_provenance.get(name) != reverse_provenance.get(name)
    ]
    if mismatches:
        raise ValueError(f"forward/reverse/intermediate provenance mismatch: {mismatches}")

    endpoint_cvars = endpoint_provenance.get("cvars")
    reverse_cvars = reverse_provenance.get("cvars")
    intermediate_cvars = intermediate_provenance.get("cvars")
    if (
        not isinstance(endpoint_cvars, dict)
        or not isinstance(reverse_cvars, dict)
        or not isinstance(intermediate_cvars, dict)
    ):
        raise ValueError("all three source replays must contain a provenance.cvars object")

    all_cvar_names = sorted(set(endpoint_cvars) | set(reverse_cvars) | set(intermediate_cvars))
    unexpected_cvar_differences = [
        name
        for name in all_cvar_names
        if name not in INTENTIONAL_CVAR_DIFFERENCES
        and (
            endpoint_cvars.get(name) != intermediate_cvars.get(name)
            or endpoint_cvars.get(name) != reverse_cvars.get(name)
        )
    ]
    if unexpected_cvar_differences:
        raise ValueError(
            "endpoint/intermediate CVar mismatch outside the declared replay isolation exceptions: "
            f"{unexpected_cvar_differences}"
        )

    for name, expected in INTENTIONAL_CVAR_DIFFERENCES.items():
        endpoint_value = str(endpoint_cvars.get(name))
        reverse_value = str(reverse_cvars.get(name))
        intermediate_value = str(intermediate_cvars.get(name))
        if (
            endpoint_value != expected["endpoints"]
            or reverse_value != expected["reverseEndpoints"]
            or intermediate_value != expected["intermediate"]
        ):
            raise ValueError(
                f"intentional CVar difference {name} has unexpected values: "
                f"endpoints={endpoint_value}, reverseEndpoints={reverse_value}, "
                f"intermediate={intermediate_value}"
            )

    normalized_cvars = {
        name: endpoint_cvars[name]
        for name in sorted(endpoint_cvars)
        if name not in INTENTIONAL_CVAR_DIFFERENCES
    }
    return {
        "normalizedCVarProfileSha1": canonical_sha1(normalized_cvars),
        "intentionalCVarDifferences": INTENTIONAL_CVAR_DIFFERENCES,
    }


def source_file(dataset: Path, frame: dict[str, Any], modality: str) -> Path:
    relative = frame.get("files", {}).get(modality)
    if not relative:
        raise ValueError(f"frame {frame.get('logicalFrameId')} lacks modality {modality}")
    path = dataset / relative
    if not path.is_file():
        raise ValueError(f"missing source file: {path}")
    expected_hash = str(frame.get("sha1", {}).get(modality, "")).upper()
    actual_hash = sha1(path)
    if not expected_hash or actual_hash != expected_hash:
        raise ValueError(f"source hash mismatch: {path}")
    return path


def copy_modality(
    staging: Path,
    pair_name: str,
    output_modality: str,
    dataset: Path,
    frame: dict[str, Any],
    source_modality: str,
    files: dict[str, str],
    hashes: dict[str, str],
) -> None:
    source = source_file(dataset, frame, source_modality)
    extension = source.suffix.lower()
    relative = Path(output_modality) / f"{pair_name}{extension}"
    destination = staging / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    files[output_modality] = relative.as_posix()
    hashes[output_modality] = sha1(destination)


def unjittered_camera(frame: dict[str, Any]) -> dict[str, Any]:
    temporal = frame["temporalDiagnostics"]
    return {
        "viewToClip": temporal["viewToClipCurrentUnjittered"],
        "translatedWorldToView": temporal["translatedWorldToViewCurrent"],
        "viewToTranslatedWorld": temporal["viewToTranslatedWorldCurrent"],
        "translatedWorldToClip": temporal["translatedWorldToClipCurrentUnjittered"],
        "worldViewOriginHigh": temporal["worldViewOriginHighCurrent"],
        "worldViewOriginLow": temporal["worldViewOriginLowCurrent"],
        "matrixLayout": temporal["matrixLayout"],
        "matrixVectorConvention": temporal["matrixVectorConvention"],
        "coordinateSystem": temporal["coordinateSystem"],
        "clipZRange": temporal["clipZRange"],
        "nearPlane": temporal["nearPlane"],
        "reversedZ": temporal["reversedZ"],
        "infiniteFar": temporal["infiniteFar"],
    }


def current_grid_signature(frame: dict[str, Any]) -> dict[str, Any]:
    temporal = frame["temporalDiagnostics"]
    fields = (
        "renderSize",
        "displaySize",
        "viewRect",
        "sceneBufferSize",
        "resolutionFraction",
        "jitterCurrentNDC",
        "jitterCurrentRenderPixel",
        "jitterCurrentDisplayPixel",
        "jitterIndex",
        "jitterIndexSource",
        "jitterLogicalFrameLocked",
        "jitterSequenceLength",
        "jitterPhaseOffset",
        "viewToClipCurrentJittered",
        "viewToClipCurrentUnjittered",
        "translatedWorldToViewCurrent",
        "viewToTranslatedWorldCurrent",
        "translatedWorldToClipCurrentJittered",
        "translatedWorldToClipCurrentUnjittered",
        "worldViewOriginHighCurrent",
        "worldViewOriginLowCurrent",
        "preViewTranslationHighCurrent",
        "preViewTranslationLowCurrent",
        "preExposure",
        "exposure",
        "nearPlane",
        "reversedZ",
        "infiniteFar",
    )
    return {
        "logicalFrameId": int(frame["logicalFrameId"]),
        "simulationTimeS": frame["simulationTimeS"],
        "camera": frame["camera"],
        "temporal": {name: temporal.get(name) for name in fields},
    }


def require_matching_endpoint_grid(
    forward: dict[str, Any], reverse: dict[str, Any]
) -> bool:
    frame_id = int(forward["logicalFrameId"])
    if int(reverse.get("logicalFrameId", -1)) != frame_id:
        raise ValueError(f"reverse replay lacks matching logical frame {frame_id}")
    if current_grid_signature(forward) != current_grid_signature(reverse):
        raise ValueError(
            f"forward/reverse current raster grid, jitter, camera, or exposure differs at frame {frame_id}"
        )
    for modality in (
        "depth_device_raw",
        "depth_view_linear_meters",
        "depth_valid",
        "object_id",
        "motion_valid",
    ):
        forward_hash = str(forward.get("sha1", {}).get(modality, "")).upper()
        reverse_hash = str(reverse.get("sha1", {}).get(modality, "")).upper()
        if not forward_hash or forward_hash != reverse_hash:
            raise ValueError(
                f"forward/reverse {modality} grid differs at frame {frame_id}"
            )
    scene_state_exact = forward.get("sceneStateSha1") == reverse.get("sceneStateSha1")
    fixture_enabled = bool(
        forward.get("semanticValidationFixture", {}).get("enabled")
        and reverse.get("semanticValidationFixture", {}).get("enabled")
    )
    if not scene_state_exact and not fixture_enabled:
        raise ValueError(
            f"forward/reverse full scene state differs at production frame {frame_id}"
        )
    return scene_state_exact


def assemble(
    endpoint_dir: Path,
    reverse_endpoint_dir: Path,
    intermediate_dir: Path,
    output_dir: Path,
) -> None:
    endpoint, endpoint_report = require_capture(endpoint_dir, ENDPOINT_ROLE)
    reverse_endpoint, reverse_endpoint_report = require_capture(
        reverse_endpoint_dir, REVERSE_ENDPOINT_ROLE
    )
    intermediate, intermediate_report = require_capture(intermediate_dir, INTERMEDIATE_ROLE)
    wpo_fixture_validated = all(
        (
            require_wpo_fixture_gate(endpoint, endpoint_report, ENDPOINT_ROLE),
            require_wpo_fixture_gate(
                reverse_endpoint, reverse_endpoint_report, REVERSE_ENDPOINT_ROLE
            ),
            require_wpo_fixture_gate(
                intermediate, intermediate_report, INTERMEDIATE_ROLE
            ),
        )
    )
    compatibility = compatible(endpoint, reverse_endpoint, intermediate)
    endpoint_job = endpoint["job"]
    if output_dir.exists():
        raise ValueError(f"output already exists; choose a new directory: {output_dir}")

    endpoint_frames = sorted(endpoint.get("frames", []), key=lambda frame: int(frame["logicalFrameId"]))
    reverse_by_id = {
        int(frame["logicalFrameId"]): frame
        for frame in reverse_endpoint.get("frames", [])
    }
    intermediate_by_id = {
        int(frame["logicalFrameId"]): frame for frame in intermediate.get("frames", [])
    }
    if len(endpoint_frames) < 2:
        raise ValueError("at least two endpoint frames are required")

    staging = output_dir.with_name(output_dir.name + ".building")
    if staging.exists():
        raise ValueError(f"stale staging directory exists: {staging}")
    staging.mkdir(parents=True)
    pairs: list[dict[str, Any]] = []
    try:
        for pair_index, (frame0, frame1) in enumerate(zip(endpoint_frames, endpoint_frames[1:])):
            t0 = int(frame0["logicalFrameId"])
            t1 = int(frame1["logicalFrameId"])
            if int(frame1.get("motionPreviousLogicalFrameId", -1)) != t0:
                raise ValueError(f"endpoint motion at frame {t1} does not point to frame {t0}")
            tau_candidates = [frame_id for frame_id in intermediate_by_id if t0 < frame_id < t1]
            if len(tau_candidates) != 1:
                raise ValueError(f"pair {t0}->{t1} requires exactly one intermediate, found {tau_candidates}")
            tau_frame_id = tau_candidates[0]
            frame_tau = intermediate_by_id[tau_frame_id]
            reverse_frame0 = reverse_by_id.get(t0)
            reverse_frame1 = reverse_by_id.get(t1)
            if reverse_frame0 is None or reverse_frame1 is None:
                raise ValueError(f"reverse endpoints are missing pair {t0}->{t1}")
            if int(reverse_frame0.get("motionPreviousLogicalFrameId", -1)) != t1:
                raise ValueError(
                    f"reverse endpoint motion at frame {t0} does not point to frame {t1}"
                )
            for role_name, endpoint_frame in (
                ("forward", frame1),
                ("reverse", reverse_frame0),
            ):
                skipped_skeletal = endpoint_frame.get(
                    "endpointPreviousSkeletalBoneSkippedComponents", []
                )
                if (
                    endpoint_frame.get("endpointPreviousSkeletalBoneOverride") is not True
                    or int(
                        endpoint_frame.get(
                            "endpointPreviousSkeletalBoneComponentCount", 0
                        )
                    )
                    <= 0
                    or int(endpoint_frame.get("endpointPreviousSkeletalBoneCount", 0)) <= 0
                    or not isinstance(skipped_skeletal, list)
                    or skipped_skeletal
                ):
                    raise ValueError(
                        f"{role_name} endpoint lacks complete cached skeletal-bone history: "
                        f"override={endpoint_frame.get('endpointPreviousSkeletalBoneOverride')} "
                        f"components={endpoint_frame.get('endpointPreviousSkeletalBoneComponentCount')} "
                        f"bones={endpoint_frame.get('endpointPreviousSkeletalBoneCount')} "
                        f"skipped={skipped_skeletal}"
                    )
            scene_state_exact_t0 = require_matching_endpoint_grid(frame0, reverse_frame0)
            scene_state_exact_t1 = require_matching_endpoint_grid(frame1, reverse_frame1)
            tau = (tau_frame_id - t0) / (t1 - t0)
            if abs(tau - 0.5) > 1e-9:
                raise ValueError(f"v1 requires tau=0.5, found {tau}")

            pair_name = f"pair_{pair_index:06d}"
            files: dict[str, str] = {}
            hashes: dict[str, str] = {}
            mappings = (
                ("scene_color_hudless_t0", endpoint_dir, frame0, "color_main_view_hudless_after_tonemap"),
                ("scene_color_hudless_tau", intermediate_dir, frame_tau, "color_main_view_hudless_after_tonemap"),
                ("scene_color_hudless_t1", endpoint_dir, frame1, "color_main_view_hudless_after_tonemap"),
                ("depth_t0", endpoint_dir, frame0, "depth_view_linear_meters"),
                ("depth_tau", intermediate_dir, frame_tau, "depth_view_linear_meters"),
                ("depth_t1", endpoint_dir, frame1, "depth_view_linear_meters"),
                ("motion_1_to_0", endpoint_dir, frame1, "motion_full_current_to_previous"),
                ("motion_valid_1_to_0", endpoint_dir, frame1, "motion_valid"),
                ("motion_0_to_1", reverse_endpoint_dir, reverse_frame0, "motion_full_current_to_previous"),
                ("motion_valid_0_to_1", reverse_endpoint_dir, reverse_frame0, "motion_valid"),
                ("history_rejection_1_to_0", endpoint_dir, frame1, "history_rejection_mask"),
                ("history_rejection_valid_1_to_0", endpoint_dir, frame1, "history_rejection_valid"),
                ("history_rejection_0_to_1", reverse_endpoint_dir, reverse_frame0, "history_rejection_mask"),
                ("history_rejection_valid_0_to_1", reverse_endpoint_dir, reverse_frame0, "history_rejection_valid"),
                ("object_id_t0", endpoint_dir, frame0, "object_id"),
                ("object_id_tau", intermediate_dir, frame_tau, "object_id"),
                ("object_id_t1", endpoint_dir, frame1, "object_id"),
                ("reactive_mask_t0", endpoint_dir, frame0, "reactive_mask"),
                ("reactive_mask_tau", intermediate_dir, frame_tau, "reactive_mask"),
                ("reactive_mask_t1", endpoint_dir, frame1, "reactive_mask"),
                ("transparency_mask_t0", endpoint_dir, frame0, "transparency_mask"),
                ("transparency_mask_tau", intermediate_dir, frame_tau, "transparency_mask"),
                ("transparency_mask_t1", endpoint_dir, frame1, "transparency_mask"),
            )
            for output_modality, dataset, frame, source_modality in mappings:
                copy_modality(
                    staging, pair_name, output_modality, dataset, frame, source_modality, files, hashes
                )

            pairs.append({
                "pairId": pair_index,
                "baseFrameId": t0,
                "t0LogicalFrameId": t0,
                "tauLogicalFrameId": tau_frame_id,
                "t1LogicalFrameId": t1,
                "tau": tau,
                "deltaTimeS": float(frame1["simulationTimeS"]) - float(frame0["simulationTimeS"]),
                "endpointMotionDefinition": "previous_pixel = current_pixel + motion_1_to_0",
                "endpointMotionUnit": "display_pixel",
                "reverseEndpointMotionDefinition": "future_pixel = current_pixel + motion_0_to_1",
                "bidirectionalMotionIndependentProcesses": True,
                "intermediateHistoryIsolated": True,
                "endpointPreviousState": {
                    "t1PreviousLogicalFrameId": int(frame1["motionPreviousLogicalFrameId"]),
                    "timeSpanS": float(frame1["motionTimeSpanS"]),
                    "componentTransformOverride": bool(frame1["endpointPreviousTransformOverride"]),
                    "skeletalBoneOverride": bool(
                        frame1["endpointPreviousSkeletalBoneOverride"]
                    ),
                    "skeletalBoneComponentCount": int(
                        frame1["endpointPreviousSkeletalBoneComponentCount"]
                    ),
                    "skeletalBoneCount": int(frame1["endpointPreviousSkeletalBoneCount"]),
                    "skeletalBoneSkippedComponents": frame1[
                        "endpointPreviousSkeletalBoneSkippedComponents"
                    ],
                    "wpoPreviousFrameSwitchFixtureValidated": wpo_fixture_validated,
                },
                "reverseEndpointPreviousState": {
                    "t0PreviousLogicalFrameId": int(reverse_frame0["motionPreviousLogicalFrameId"]),
                    "timeSpanS": float(reverse_frame0["motionTimeSpanS"]),
                    "componentTransformOverride": bool(reverse_frame0["endpointPreviousTransformOverride"]),
                    "skeletalBoneOverride": bool(
                        reverse_frame0["endpointPreviousSkeletalBoneOverride"]
                    ),
                    "skeletalBoneComponentCount": int(
                        reverse_frame0["endpointPreviousSkeletalBoneComponentCount"]
                    ),
                    "skeletalBoneCount": int(
                        reverse_frame0["endpointPreviousSkeletalBoneCount"]
                    ),
                    "skeletalBoneSkippedComponents": reverse_frame0[
                        "endpointPreviousSkeletalBoneSkippedComponents"
                    ],
                    "wpoPreviousFrameSwitchFixtureValidated": wpo_fixture_validated,
                },
                "wpoValidation": {
                    "previousFrameSwitch": True,
                    "objectId": int(
                        frame1["semanticValidationFixture"]["wpoObjectId"]
                    ),
                    "forward": {
                        "currentRightCm": float(
                            frame1["semanticValidationFixture"]["wpoCurrentRightCm"]
                        ),
                        "previousRightCm": float(
                            frame1["semanticValidationFixture"]["wpoPreviousRightCm"]
                        ),
                        "expectedMotionDisplayPixels": frame1[
                            "semanticValidationFixture"
                        ]["expectedWPOMotionDisplayPixels"],
                    },
                    "reverse": {
                        "currentRightCm": float(
                            reverse_frame0["semanticValidationFixture"][
                                "wpoCurrentRightCm"
                            ]
                        ),
                        "previousRightCm": float(
                            reverse_frame0["semanticValidationFixture"][
                                "wpoPreviousRightCm"
                            ]
                        ),
                        "expectedMotionDisplayPixels": reverse_frame0[
                            "semanticValidationFixture"
                        ]["expectedWPOMotionDisplayPixels"],
                    },
                },
                "reverseEndpointGridAlignment": {
                    "jitterCameraDepthObjectIdExact": True,
                    "sceneStateExactT0": scene_state_exact_t0,
                    "sceneStateExactT1": scene_state_exact_t1,
                    "semanticFixtureAllowsHiddenUncontrolledStateMismatch": bool(
                        frame0.get("semanticValidationFixture", {}).get("enabled")
                    ),
                },
                "cameraT0Unjittered": unjittered_camera(frame0),
                "cameraT1Unjittered": unjittered_camera(frame1),
                "exposure": {
                    "t0": frame0["temporalDiagnostics"]["exposure"],
                    "tau": frame_tau["temporalDiagnostics"]["exposure"],
                    "t1": frame1["temporalDiagnostics"]["exposure"],
                },
                "preExposure": {
                    "t0": frame0["temporalDiagnostics"]["preExposure"],
                    "tau": frame_tau["temporalDiagnostics"]["preExposure"],
                    "t1": frame1["temporalDiagnostics"]["preExposure"],
                },
                "streamingStateSha1": {
                    "t0": frame0["streamingStateSha1"],
                    "tau": frame_tau["streamingStateSha1"],
                    "t1": frame1["streamingStateSha1"],
                },
                "sceneStateSha1": {
                    "t0": frame0["sceneStateSha1"],
                    "tau": frame_tau["sceneStateSha1"],
                    "t1": frame1["sceneStateSha1"],
                },
                "sceneStateHashScope": frame1["sceneStateHashScope"],
                "reset": {
                    "t0": bool(frame0.get("reset", False)),
                    "tau": bool(frame_tau.get("reset", False)),
                    "t1": bool(frame1.get("reset", False)),
                },
                "resetReason": {
                    "t0": frame0.get("resetReason", "none"),
                    "tau": frame_tau.get("resetReason", "none"),
                    "t1": frame1.get("resetReason", "none"),
                },
                "files": files,
                "sha1": hashes,
            })

        manifest = {
            "schemaVersion": 1,
            "contractVersion": "nr-fg-data-v1",
            "certificationStatus": "experimental_uncertified",
            "frameGenerationCertified": False,
            "tau": 0.5,
            "renderSize": [endpoint_job["lRResolution"]["x"], endpoint_job["lRResolution"]["y"]],
            "displaySize": [endpoint_job["hRResolution"]["x"], endpoint_job["hRResolution"]["y"]],
            "baseFrameRate": {
                "numerator": endpoint_job["captureFrameRateNumerator"],
                "denominator": endpoint_job["captureFrameRateDenominator"],
            },
            "uiSeparated": False,
            "motionBlur": "off",
            "bufferSemantics": {
                "sceneColorHudless": {
                    "pipelineStage": "after_tonemap_before_ui",
                    "resolution": "display",
                    "uiIncluded": False,
                    "hudIncluded": False,
                    "encoding": "tonemapper_output_device_encoded",
                },
                "depth": {
                    "encoding": "linear_view_meters",
                    "resolution": "render",
                    "validity": "finite_positive_values_with_source_depth_valid_gate",
                },
                "motion1To0": {
                    "definition": "previous_pixel = current_pixel + motion_1_to_0",
                    "unit": "display_pixel",
                    "origin": "top_left",
                    "jitterRemoved": True,
                    "resolution": "render",
                    "endpointTransformScope": "scene_components_plus_double_buffered_skinned_component_space_bones_plus_explicit_previous_frame_switch_wpo_fixture",
                },
                "motion0To1": {
                    "definition": "future_pixel = current_pixel + motion_0_to_1",
                    "unit": "display_pixel",
                    "origin": "top_left",
                    "jitterRemoved": True,
                    "resolution": "render",
                    "independentlyCaptured": True,
                    "endpointTransformScope": "scene_components_plus_double_buffered_skinned_component_space_bones_plus_explicit_previous_frame_switch_wpo_fixture",
                },
                "historyRejection1To0": {
                    "definition": "one_rejects_t0_history_at_t1_motion_reprojected_pixel",
                    "source": "custom_stencil_identity_else_static_camera_depth_reprojection_v1",
                    "validity": "history_rejection_valid_1_to_0",
                    "resolution": "render",
                    "productionCertified": False,
                },
                "historyRejection0To1": {
                    "definition": "one_rejects_t1_history_at_t0_motion_reprojected_pixel",
                    "source": "custom_stencil_identity_else_static_camera_depth_reprojection_v1",
                    "validity": "history_rejection_valid_0_to_1",
                    "resolution": "render",
                    "productionCertified": False,
                },
                "objectId": {
                    "source": "custom_stencil_uint8_zero_unlabeled",
                    "resolution": "render",
                },
            },
            "sourceReplays": {
                "endpoints": str(endpoint_dir.resolve()),
                "reverseEndpoints": str(reverse_endpoint_dir.resolve()),
                "intermediate": str(intermediate_dir.resolve()),
                "endpointValidation": {
                    "validatorVersion": endpoint_report.get("validatorVersion"),
                    "checksPassed": endpoint_report.get("checksPassed"),
                    "checksTotal": endpoint_report.get("checksTotal"),
                },
                "intermediateValidation": {
                    "validatorVersion": intermediate_report.get("validatorVersion"),
                    "checksPassed": intermediate_report.get("checksPassed"),
                    "checksTotal": intermediate_report.get("checksTotal"),
                },
                "reverseEndpointValidation": {
                    "validatorVersion": reverse_endpoint_report.get("validatorVersion"),
                    "checksPassed": reverse_endpoint_report.get("checksPassed"),
                    "checksTotal": reverse_endpoint_report.get("checksTotal"),
                },
            },
            "validationCoverage": {
                "semanticFixtureRigidComponentMotion": True,
                "semanticFixtureDoubleBufferedSkeletalBoneMotion": True,
                "semanticFixturePreviousFrameSwitchWPOMotion": wpo_fixture_validated,
            },
            "provenance": {
                **compatibility,
                "endpointReplay": endpoint.get("provenance", {}),
                "reverseEndpointReplay": reverse_endpoint.get("provenance", {}),
                "intermediateReplay": intermediate.get("provenance", {}),
            },
            "missingRequirements": [
                "ui_color_alpha_t0_tau_t1",
                "non_fixture_skeletal_animation_endpoint_motion_validation",
                "non_fixture_wpo_and_animated_material_endpoint_motion_validation",
                "production_disocclusion_masks",
                "hudless_in_world_ui_residue_validation",
            ],
            "pairs": pairs,
        }
        manifest_path = staging / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        os.replace(staging, output_dir)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoints", type=Path, required=True)
    parser.add_argument("--reverse-endpoints", type=Path, required=True)
    parser.add_argument("--intermediate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        assemble(
            args.endpoints.resolve(),
            args.reverse_endpoints.resolve(),
            args.intermediate.resolve(),
            args.output.resolve(),
        )
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"Assembled experimental FG dataset: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
