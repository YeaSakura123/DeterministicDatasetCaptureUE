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
PROJECT_SKELETAL_EVIDENCE_VERSION = 1
PROJECT_SKELETAL_EVIDENCE_FILES = {
    "forwardManifest": "project_skeletal_forward_manifest.json",
    "forwardValidation": "project_skeletal_forward_validation.json",
    "reverseManifest": "project_skeletal_reverse_manifest.json",
    "reverseValidation": "project_skeletal_reverse_validation.json",
    "reverseComparison": "project_skeletal_reverse_comparison.json",
    "poseCacheArtifact": "project_skeletal_pose_cache.srcache",
}
PROJECT_ANIMATED_MATERIAL_EVIDENCE_VERSION = 1
PROJECT_ANIMATED_MATERIAL_EVIDENCE_FILES = {
    "forwardManifest": "project_animated_material_forward_manifest.json",
    "forwardValidation": "project_animated_material_forward_validation.json",
    "reverseManifest": "project_animated_material_reverse_manifest.json",
    "reverseValidation": "project_animated_material_reverse_validation.json",
    "reverseComparison": "project_animated_material_reverse_comparison.json",
}
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
    "r.SkipRedundantTransformUpdate": {
        "endpoints": "0",
        "reverseEndpoints": "0",
        "intermediate": "1",
        "reason": (
            "Endpoint replay must submit a simulated previous transform even when the current "
            "transform equals a renderer-primed intermediate state; isolated midpoint replay "
            "uses the engine default redundant-update optimization."
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
    source_hash = hashlib.sha256(Path(__file__).with_name("ValidateDataset.py").read_bytes() + Path(__file__).with_name("TemporalGeometry.py").read_bytes()).hexdigest()
    if report.get("manifestSha256") != hashlib.sha256(manifest_path.read_bytes()).hexdigest() or report.get("validatorSourceSha256") != source_hash:
        raise ValueError(f"Stale capture validation; rerun ValidateDataset.py: {dataset}")
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
    if report.get("disocclusionGate") != "pass" or any(f.get("temporalDiagnostics", {}).get("historyRejectionSource") != "component_identity_and_static_camera_depth_on_jittered_rasters_v3" for f in manifest.get("frames", [])):
        raise ValueError(f"Current jitter-aware history validation is required: {dataset}")
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


def has_world_space_widget_rejection_gate(
    manifest: dict[str, Any], report: dict[str, Any]
) -> bool:
    if (
        manifest.get("job", {}).get("bRejectVisibleWidgetComponents") is not True
        or int(report.get("validatorVersion", 0)) < 9
    ):
        return False
    checks = passing_check_map(report)
    frame_checks = {
        name: passed
        for name, passed in checks.items()
        if name.endswith(".world_ui_zero_widget_component_residue")
    }
    return bool(
        checks.get("world_ui.job_and_contract") is True
        and frame_checks
        and all(frame_checks.values())
    )


def passing_check_map(report: dict[str, Any]) -> dict[str, bool]:
    return {
        str(check.get("name")): bool(check.get("passed", False))
        for check in report.get("checks", [])
        if isinstance(check, dict)
    }


def require_report_pass(
    report: dict[str, Any], label: str, gate: str = "formatAndIntegrityGate"
) -> dict[str, bool]:
    passed = int(report.get("checksPassed", -1))
    total = int(report.get("checksTotal", -2))
    required_failures = [
        check
        for check in report.get("checks", [])
        if isinstance(check, dict)
        and check.get("required", True)
        and not check.get("passed", False)
    ]
    if (
        int(report.get("validatorVersion", 0)) < 7
        or report.get(gate) != "pass"
        or total <= 0
        or passed != total
        or required_failures
    ):
        raise ValueError(
            f"{label} requires a complete validatorVersion >= 7 passing report"
        )
    return passing_check_map(report)


def require_named_checks(
    checks: dict[str, bool], exact: tuple[str, ...], prefixes: tuple[str, ...], label: str
) -> None:
    missing_exact = [name for name in exact if checks.get(name) is not True]
    missing_prefixes = [
        prefix
        for prefix in prefixes
        if not any(name.startswith(prefix) and passed for name, passed in checks.items())
    ]
    if missing_exact or missing_prefixes:
        raise ValueError(
            f"{label} lacks required proof checks: exact={missing_exact} "
            f"prefixes={missing_prefixes}"
        )


def resolve_project_relative_artifact(dataset: Path, declared: Any) -> Path:
    if not isinstance(declared, str) or not declared:
        raise ValueError("skeletal pose cache artifact path is missing")
    relative = Path(declared)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("skeletal pose cache artifact must be project-relative")
    for root in (dataset, *dataset.parents):
        candidate = (root / relative).resolve()
        try:
            candidate.relative_to(root.resolve())
        except ValueError:
            continue
        if candidate.is_file():
            return candidate
    raise ValueError(f"skeletal pose cache artifact not found: {declared}")


def require_project_skeletal_evidence(
    forward_dir: Path, reverse_dir: Path
) -> dict[str, Any]:
    forward_manifest_path = forward_dir / "manifest.json"
    forward_report_path = forward_dir / "validation_report.json"
    reverse_manifest_path = reverse_dir / "manifest.json"
    reverse_report_path = reverse_dir / "validation_report.json"
    comparison_report_path = forward_dir / "validation_report_skeletal_reverse.json"
    required_paths = (
        forward_manifest_path,
        forward_report_path,
        reverse_manifest_path,
        reverse_report_path,
        comparison_report_path,
    )
    missing = [str(path) for path in required_paths if not path.is_file()]
    if missing:
        raise ValueError(f"project skeletal evidence files are missing: {missing}")

    forward_manifest = load_json(forward_manifest_path)
    forward_report = load_json(forward_report_path)
    reverse_manifest = load_json(reverse_manifest_path)
    reverse_report = load_json(reverse_report_path)
    comparison_report = load_json(comparison_report_path)
    if forward_manifest.get("state") != "Completed" or reverse_manifest.get("state") != "Completed":
        raise ValueError("project skeletal evidence captures must be complete")
    if forward_manifest.get("replayPass") != ENDPOINT_ROLE:
        raise ValueError("project skeletal forward evidence has the wrong replay role")
    if reverse_manifest.get("replayPass") != REVERSE_ENDPOINT_ROLE:
        raise ValueError("project skeletal reverse evidence has the wrong replay role")

    standalone_exact = (
        "skeletal_replay.job_and_contract",
        "provenance.skeletal_pose_cache_artifact",
        "skeletal_replay.project_anim_blueprint_pose_changes",
        "skeletal_replay.project_disocclusion_transition_coverage",
    )
    standalone_prefixes = (
        "frame_000000.project_anim_blueprint_probe",
        "frame_000000.project_skeletal_probe_visible",
    )
    for label, manifest, report in (
        ("project skeletal forward", forward_manifest, forward_report),
        ("project skeletal reverse", reverse_manifest, reverse_report),
    ):
        job = manifest.get("job", {})
        if (
            job.get("bValidateNonFixtureSkeletalAnimation") is not True
            or job.get("bCacheSkeletalAnimationPosesForReplay") is not True
            or job.get("bUseDeterministicCameraTransform") is not True
            or not job.get("nonFixtureSkeletalValidationActorClass")
        ):
            raise ValueError(f"{label} is not a deterministic project AnimBP probe")
        checks = require_report_pass(report, label)
        require_named_checks(checks, standalone_exact, standalone_prefixes, label)
        if not any(
            name.endswith(".project_skeletal_probe_endpoint_motion") and passed
            for name, passed in checks.items()
        ):
            raise ValueError(f"{label} lacks endpoint skeletal motion proof")
        if not any(
            name.endswith(".project_skeletal_bidirectional_visibility") and passed
            for name, passed in checks.items()
        ):
            raise ValueError(f"{label} lacks bidirectional visibility proof")

    comparison_checks = require_report_pass(
        comparison_report, "project skeletal reverse comparison"
    )
    if comparison_report.get("skeletalReverseReplayGate") != "pass":
        raise ValueError("project skeletal reverse comparison gate did not pass")
    require_named_checks(
        comparison_checks,
        (
            "skeletal_reverse.opposite_endpoint_roles",
            "skeletal_reverse.jobs_equal_except_identity_output_and_role",
            "skeletal_reverse.provenance_equal_except_config_hash",
            "skeletal_reverse.static_grids_compared",
            "skeletal_replay.project_disocclusion_transition_coverage",
        ),
        (
            "skeletal_reverse.frame_000000.project_pose_and_identity_exact",
            "skeletal_reverse.frame_000000.camera_exact",
            "skeletal_reverse.frame_000000.left_independent_motion_evidence",
            "skeletal_reverse.frame_000000.right_independent_motion_evidence",
        ),
        "project skeletal reverse comparison",
    )

    forward_job = forward_manifest.get("job", {})
    reverse_job = reverse_manifest.get("job", {})
    if (
        forward_job.get("nonFixtureSkeletalValidationActorClass")
        != reverse_job.get("nonFixtureSkeletalValidationActorClass")
    ):
        raise ValueError("project skeletal evidence actor classes differ")
    forward_frame_ids = sorted(
        int(frame["logicalFrameId"]) for frame in forward_manifest.get("frames", [])
    )
    reverse_frame_ids = sorted(
        int(frame["logicalFrameId"]) for frame in reverse_manifest.get("frames", [])
    )
    if len(forward_frame_ids) < 2 or forward_frame_ids != reverse_frame_ids:
        raise ValueError("project skeletal evidence logical frame grids differ")

    forward_sha1 = str(
        forward_manifest.get("provenance", {}).get("skeletalPoseCacheArtifactSha1", "")
    ).upper()
    reverse_sha1 = str(
        reverse_manifest.get("provenance", {}).get("skeletalPoseCacheArtifactSha1", "")
    ).upper()
    if len(forward_sha1) != 40 or forward_sha1 != reverse_sha1:
        raise ValueError("project skeletal evidence pose-cache hashes differ")
    artifact_declared = forward_job.get("skeletalPoseCacheOutputFile")
    artifact_path = resolve_project_relative_artifact(forward_dir, artifact_declared)
    if sha1(artifact_path) != forward_sha1:
        raise ValueError("project skeletal pose-cache artifact hash does not match provenance")

    return {
        "paths": {
            "forwardManifest": forward_manifest_path,
            "forwardValidation": forward_report_path,
            "reverseManifest": reverse_manifest_path,
            "reverseValidation": reverse_report_path,
            "reverseComparison": comparison_report_path,
            "poseCacheArtifact": artifact_path,
        },
        "artifactSha1": forward_sha1,
        "actorClass": forward_job["nonFixtureSkeletalValidationActorClass"],
        "logicalFrameIds": forward_frame_ids,
        "validatorVersion": int(comparison_report["validatorVersion"]),
        "checksPassed": int(comparison_report["checksPassed"]),
        "checksTotal": int(comparison_report["checksTotal"]),
    }


def require_project_animated_material_evidence(
    forward_dir: Path, reverse_dir: Path
) -> dict[str, Any]:
    paths = {
        "forwardManifest": forward_dir / "manifest.json",
        "forwardValidation": forward_dir / "validation_report.json",
        "reverseManifest": reverse_dir / "manifest.json",
        "reverseValidation": reverse_dir / "validation_report.json",
        "reverseComparison": forward_dir / "validation_report_material_reverse.json",
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise ValueError(f"project animated-material evidence files are missing: {missing}")
    loaded = {key: load_json(path) for key, path in paths.items()}
    forward_manifest = loaded["forwardManifest"]
    reverse_manifest = loaded["reverseManifest"]
    forward_report = loaded["forwardValidation"]
    reverse_report = loaded["reverseValidation"]
    comparison_report = loaded["reverseComparison"]
    if forward_manifest.get("state") != "Completed" or reverse_manifest.get("state") != "Completed":
        raise ValueError("project animated-material evidence captures must be complete")
    if forward_manifest.get("replayPass") != ENDPOINT_ROLE:
        raise ValueError("project animated-material forward evidence has the wrong replay role")
    if reverse_manifest.get("replayPass") != REVERSE_ENDPOINT_ROLE:
        raise ValueError("project animated-material reverse evidence has the wrong replay role")

    standalone_exact = (
        "material_time.logical_frame_contract",
        "material_replay.project_validation_contract",
        "material_replay.project_animated_material_changes_with_logical_time",
    )
    standalone_prefixes = (
        "frame_000000.project_animated_material_schema",
        "frame_000000.project_animated_material_visible",
    )
    material_paths: set[str] = set()
    for label, manifest, report in (
        ("project animated-material forward", forward_manifest, forward_report),
        ("project animated-material reverse", reverse_manifest, reverse_report),
    ):
        job = manifest.get("job", {})
        material_path = str(job.get("projectAnimatedMaterialValidationMaterial", ""))
        material_paths.add(material_path)
        if (
            job.get("bValidateProjectAnimatedMaterial") is not True
            or job.get("bLockMaterialTimeToLogicalFrame") is not True
            or job.get("bUseDeterministicCameraTransform") is not True
            or not material_path.startswith("/Game/")
        ):
            raise ValueError(f"{label} is not a deterministic project material probe")
        checks = require_report_pass(report, label)
        if int(report.get("validatorVersion", 0)) < 8:
            raise ValueError(f"{label} requires ValidateDataset.py validatorVersion >= 8")
        require_named_checks(checks, standalone_exact, standalone_prefixes, label)

    if len(material_paths) != 1:
        raise ValueError("project animated-material evidence material interfaces differ")
    comparison_checks = require_report_pass(
        comparison_report, "project animated-material reverse comparison"
    )
    if (
        int(comparison_report.get("validatorVersion", 0)) < 8
        or comparison_report.get("materialReverseReplayGate") != "pass"
    ):
        raise ValueError("project animated-material reverse comparison gate did not pass")
    require_named_checks(
        comparison_checks,
        (
            "material_reverse.opposite_endpoint_roles",
            "material_reverse.jobs_equal_except_identity_output_and_role",
            "material_reverse.provenance_equal_except_config_hash",
            "material_reverse.static_identity_grids_compared",
        ),
        (
            "material_reverse.frame_000000.gpu_logical_game_time_and_signed_direction",
            "material_reverse.frame_000000.project_material_identity_exact",
            "material_reverse.frame_000000.project_material_current_color_exact",
        ),
        "project animated-material reverse comparison",
    )
    forward_frame_ids = sorted(
        int(frame["logicalFrameId"]) for frame in forward_manifest.get("frames", [])
    )
    reverse_frame_ids = sorted(
        int(frame["logicalFrameId"]) for frame in reverse_manifest.get("frames", [])
    )
    if len(forward_frame_ids) < 2 or forward_frame_ids != reverse_frame_ids:
        raise ValueError("project animated-material evidence logical frame grids differ")
    return {
        "paths": paths,
        "materialInterface": next(iter(material_paths)),
        "logicalFrameIds": forward_frame_ids,
        "validatorVersion": int(comparison_report["validatorVersion"]),
        "checksPassed": int(comparison_report["checksPassed"]),
        "checksTotal": int(comparison_report["checksTotal"]),
    }


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
        "bLockMaterialTimeToLogicalFrame",
        "bRejectVisibleWidgetComponents",
        "bEnableSemanticValidationFixture",
        "bCaptureUIColorAlpha",
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
        "pluginBinarySha1",
        "loadedContentSha1",
        "materialShadersReadyAfterWarmup",
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
    if not path.resolve().is_relative_to(dataset.resolve()):
        raise ValueError(f"source path escapes dataset: {relative}")
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
        "ui_color_alpha",
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
    project_skeletal_forward_dir: Path | None = None,
    project_skeletal_reverse_dir: Path | None = None,
    project_animated_material_forward_dir: Path | None = None,
    project_animated_material_reverse_dir: Path | None = None,
    project_wpo_dirs: list[Path] | None = None,
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
    world_space_widget_residue_rejected = all(
        (
            has_world_space_widget_rejection_gate(endpoint, endpoint_report),
            has_world_space_widget_rejection_gate(
                reverse_endpoint, reverse_endpoint_report
            ),
            has_world_space_widget_rejection_gate(
                intermediate, intermediate_report
            ),
        )
    )
    compatibility = compatible(endpoint, reverse_endpoint, intermediate)
    endpoint_job = endpoint["job"]
    project_wpo_report = None
    if project_wpo_dirs is not None:
        from VerifyProjectWPO import verify
        for directory, role in zip(project_wpo_dirs, (ENDPOINT_ROLE, REVERSE_ENDPOINT_ROLE, INTERMEDIATE_ROLE)):
            proof_manifest, _ = require_capture(directory, role)
            if proof_manifest["provenance"]["pluginBinarySha1"] != endpoint["provenance"]["pluginBinarySha1"]:
                raise ValueError("Project WPO proof uses a different plugin binary")
        project_wpo_report = verify(project_wpo_dirs)
        if not project_wpo_report["passed"]:
            raise ValueError("Project WPO physical proof failed")
    if (project_skeletal_forward_dir is None) != (project_skeletal_reverse_dir is None):
        raise ValueError(
            "--project-skeletal-forward and --project-skeletal-reverse must be supplied together"
        )
    project_skeletal_evidence = (
        require_project_skeletal_evidence(
            project_skeletal_forward_dir, project_skeletal_reverse_dir
        )
        if project_skeletal_forward_dir is not None
        and project_skeletal_reverse_dir is not None
        else None
    )
    if (project_animated_material_forward_dir is None) != (
        project_animated_material_reverse_dir is None
    ):
        raise ValueError(
            "--project-animated-material-forward and --project-animated-material-reverse "
            "must be supplied together"
        )
    project_animated_material_evidence = (
        require_project_animated_material_evidence(
            project_animated_material_forward_dir,
            project_animated_material_reverse_dir,
        )
        if project_animated_material_forward_dir is not None
        and project_animated_material_reverse_dir is not None
        else None
    )
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
        validation_evidence: dict[str, Any] = {}
        if project_wpo_dirs is not None:
            from VerifyProjectWPO import PROOF_MODALITIES
            proof_files, proof_hashes = {}, {}
            for role, directory in zip(("forward", "reverse", "midpoint"), project_wpo_dirs):
                proof_manifest = load_json(directory / "manifest.json")
                relative_files = {"manifest.json", "validation_report.json"}
                for frame in proof_manifest["frames"]:
                    for modality in PROOF_MODALITIES:
                        source_file(directory, frame, modality)
                        relative_files.add(frame["files"][modality])
                for relative in sorted(relative_files):
                    destination = staging / "evidence" / "project_wpo" / role / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(directory / relative, destination)
                    key = role + "/" + relative
                    proof_files[key] = destination.relative_to(staging).as_posix()
                    proof_hashes[key] = sha1(destination)
            validation_evidence["projectWPO"] = {"schemaVersion": 1, "files": proof_files, "sha1": proof_hashes, "physicalReport": project_wpo_report, "scope": "original_generated_Gallery_sinusoidal_panel; real forward/reverse motion and midpoint silhouette; proof modalities only"}
        if project_skeletal_evidence is not None:
            evidence_dir = staging / "validation_evidence" / "project_skeletal"
            evidence_dir.mkdir(parents=True)
            evidence_files: dict[str, str] = {}
            evidence_hashes: dict[str, str] = {}
            for key, filename in PROJECT_SKELETAL_EVIDENCE_FILES.items():
                source = project_skeletal_evidence["paths"][key]
                destination = evidence_dir / filename
                shutil.copy2(source, destination)
                relative = destination.relative_to(staging).as_posix()
                evidence_files[key] = relative
                evidence_hashes[key] = sha1(destination)
            validation_evidence["projectSkeletalAnimation"] = {
                "schemaVersion": PROJECT_SKELETAL_EVIDENCE_VERSION,
                "proofScope": (
                    "project_authored_anim_blueprint_shared_pose_cache_exact_forward_reverse_"
                    "identity_camera_depth_and_bidirectional_disocclusion"
                ),
                "actorClass": project_skeletal_evidence["actorClass"],
                "logicalFrameIds": project_skeletal_evidence["logicalFrameIds"],
                "poseCacheArtifactSha1": project_skeletal_evidence["artifactSha1"],
                "validatorVersion": project_skeletal_evidence["validatorVersion"],
                "checksPassed": project_skeletal_evidence["checksPassed"],
                "checksTotal": project_skeletal_evidence["checksTotal"],
                "files": evidence_files,
                "sha1": evidence_hashes,
            }
        if project_animated_material_evidence is not None:
            evidence_dir = staging / "validation_evidence" / "project_animated_material"
            evidence_dir.mkdir(parents=True)
            evidence_files: dict[str, str] = {}
            evidence_hashes: dict[str, str] = {}
            for key, filename in PROJECT_ANIMATED_MATERIAL_EVIDENCE_FILES.items():
                source = project_animated_material_evidence["paths"][key]
                destination = evidence_dir / filename
                shutil.copy2(source, destination)
                evidence_files[key] = destination.relative_to(staging).as_posix()
                evidence_hashes[key] = sha1(destination)
            validation_evidence["projectAnimatedMaterial"] = {
                "schemaVersion": PROJECT_ANIMATED_MATERIAL_EVIDENCE_VERSION,
                "proofScope": (
                    "project_authored_surface_material_gpu_logical_game_time_visible_change_"
                    "and_forward_reverse_current_color_tolerance"
                ),
                "materialInterface": project_animated_material_evidence[
                    "materialInterface"
                ],
                "logicalFrameIds": project_animated_material_evidence[
                    "logicalFrameIds"
                ],
                "validatorVersion": project_animated_material_evidence[
                    "validatorVersion"
                ],
                "checksPassed": project_animated_material_evidence["checksPassed"],
                "checksTotal": project_animated_material_evidence["checksTotal"],
                "files": evidence_files,
                "sha1": evidence_hashes,
            }

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
                if endpoint_frame.get("reset") or endpoint_frame.get("motionTrainingUsable") is not True:
                    raise ValueError(f"{role_name} endpoint motion is a reset or unusable")
                skipped_skeletal = endpoint_frame.get(
                    "endpointPreviousSkeletalBoneSkippedComponents", []
                )
                has_skeleton = int(endpoint_frame.get("sceneSkeletalComponentCount", 0)) > 0
                bone_components = int(endpoint_frame.get("endpointPreviousSkeletalBoneComponentCount", 0))
                bone_count = int(endpoint_frame.get("endpointPreviousSkeletalBoneCount", 0))
                if (
                    endpoint_frame.get("endpointPreviousSkeletalBoneOverride") is not has_skeleton
                    or (has_skeleton and (bone_components <= 0 or bone_count <= 0))
                    or (not has_skeleton and (bone_components != 0 or bone_count != 0))
                    or not isinstance(skipped_skeletal, list) or skipped_skeletal
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
                ("ui_color_alpha_t0", endpoint_dir, frame0, "ui_color_alpha"),
                ("ui_color_alpha_tau", intermediate_dir, frame_tau, "ui_color_alpha"),
                ("ui_color_alpha_t1", endpoint_dir, frame1, "ui_color_alpha"),
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
                    "sceneSkeletalComponentCount": int(frame1["sceneSkeletalComponentCount"]),
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
                    "sceneSkeletalComponentCount": int(reverse_frame0["sceneSkeletalComponentCount"]),
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
                } if wpo_fixture_validated else {},
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
                "rasterGrids": {"t0": current_grid_signature(frame0), "tau": current_grid_signature(frame_tau), "t1": current_grid_signature(frame1)},
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
                "uiColorAlphaDiagnostics": {
                    "t0": frame0["uiColorAlphaDiagnostics"],
                    "tau": frame_tau["uiColorAlphaDiagnostics"],
                    "t1": frame1["uiColorAlphaDiagnostics"],
                },
                "worldSpaceWidgetPolicy": {
                    "t0": frame0["worldSpaceWidgetPolicy"],
                    "tau": frame_tau["worldSpaceWidgetPolicy"],
                    "t1": frame1["worldSpaceWidgetPolicy"],
                },
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
            "certificationStatus": "pending_dataset_validation" if project_wpo_report is not None and project_skeletal_evidence is not None and project_animated_material_evidence is not None and world_space_widget_residue_rejected else "experimental_uncertified",
            "frameGenerationCertified": False,
            "tau": 0.5,
            "renderSize": [endpoint_job["lRResolution"]["x"], endpoint_job["lRResolution"]["y"]],
            "displaySize": [endpoint_job["hRResolution"]["x"], endpoint_job["hRResolution"]["y"]],
            "baseFrameRate": {
                "numerator": endpoint_job["captureFrameRateNumerator"],
                "denominator": endpoint_job["captureFrameRateDenominator"],
            },
            "uiSeparated": True,
            "motionBlur": "off",
            "bufferSemantics": {
                "sceneColorHudless": {
                    "pipelineStage": "after_tonemap_before_ui",
                    "resolution": "display",
                    "uiIncluded": False,
                    "hudIncluded": False,
                    "encoding": "tonemapper_output_device_encoded",
                },
                "uiColorAlpha": {
                    "pipelineStage": "independent_slate_game_layer_before_scene_composite",
                    "resolution": "display",
                    "encoding": "display_referred_srgb_png_unorm8",
                    "alphaSemantic": "straight_coverage_zero_is_transparent_one_is_opaque",
                    "rgbSemantic": "premultiplied_by_coverage_alpha",
                    "sceneIncluded": False,
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
                    "source": "component_identity_and_static_camera_depth_on_jittered_rasters_v3",
                    "validity": "history_rejection_valid_1_to_0",
                    "resolution": "render",
                    "productionCertified": False,
                },
                "historyRejection0To1": {
                    "definition": "one_rejects_t1_history_at_t0_motion_reprojected_pixel",
                    "source": "component_identity_and_static_camera_depth_on_jittered_rasters_v3",
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
                    "manifestSha256": endpoint_report["manifestSha256"],
                    "validatorSourceSha256": endpoint_report["validatorSourceSha256"],
                    "validatorVersion": endpoint_report.get("validatorVersion"),
                    "checksPassed": endpoint_report.get("checksPassed"),
                    "checksTotal": endpoint_report.get("checksTotal"),
                },
                "intermediateValidation": {
                    "manifestSha256": intermediate_report["manifestSha256"],
                    "validatorSourceSha256": intermediate_report["validatorSourceSha256"],
                    "validatorVersion": intermediate_report.get("validatorVersion"),
                    "checksPassed": intermediate_report.get("checksPassed"),
                    "checksTotal": intermediate_report.get("checksTotal"),
                },
                "reverseEndpointValidation": {
                    "manifestSha256": reverse_endpoint_report["manifestSha256"],
                    "validatorSourceSha256": reverse_endpoint_report["validatorSourceSha256"],
                    "validatorVersion": reverse_endpoint_report.get("validatorVersion"),
                    "checksPassed": reverse_endpoint_report.get("checksPassed"),
                    "checksTotal": reverse_endpoint_report.get("checksTotal"),
                },
            },
            "validationCoverage": {
                "semanticFixtureRigidComponentMotion": wpo_fixture_validated,
                "semanticFixtureDoubleBufferedSkeletalBoneMotion": wpo_fixture_validated,
                "semanticFixturePreviousFrameSwitchWPOMotion": wpo_fixture_validated,
                "semanticFixtureUIColorAlpha": wpo_fixture_validated,
                "ordinarySceneCaptureValidated": not endpoint_job.get("bEnableSemanticValidationFixture", False),
                "projectAuthoredWPOEndpointMotion": project_wpo_report is not None,
                "projectAuthoredAnimBlueprintEndpointMotion": project_skeletal_evidence
                is not None,
                "projectAuthoredAnimBlueprintExactForwardReverseReplay": project_skeletal_evidence
                is not None,
                "projectSkeletalBidirectionalDisocclusion": project_skeletal_evidence
                is not None,
                "projectAnimatedMaterialLogicalGameTime": project_animated_material_evidence
                is not None,
                "projectAnimatedMaterialVisibleTimeVariation": project_animated_material_evidence
                is not None,
                "projectAnimatedMaterialForwardReverseCurrentColor": project_animated_material_evidence
                is not None,
                "hudlessWorldSpaceWidgetResidueRejected": world_space_widget_residue_rejected,
            },
            "validationEvidence": validation_evidence,
            "sourceCaptureControls": {
                role: {"job": source["job"], "sceneControlPreflight": source.get("sceneControlPreflight", {})}
                for role, source in (("forward", endpoint), ("reverse", reverse_endpoint), ("midpoint", intermediate))
            },
            "provenance": {
                **compatibility,
                "endpointReplay": endpoint.get("provenance", {}),
                "reverseEndpointReplay": reverse_endpoint.get("provenance", {}),
                "intermediateReplay": intermediate.get("provenance", {}),
            },
            "missingRequirements": [
                *(
                    []
                    if project_skeletal_evidence is not None
                    else [
                        "non_fixture_skeletal_animation_endpoint_motion_validation",
                        "production_disocclusion_masks",
                    ]
                ),
                *(
                    []
                    if project_animated_material_evidence is not None
                    else ["project_animated_material_logical_time_validation"]
                ),
                *([] if project_wpo_report is not None else ["project_authored_wpo_endpoint_motion_validation"]),
                *(
                    []
                    if world_space_widget_residue_rejected
                    else ["hudless_in_world_ui_residue_validation"]
                ),
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
    parser.add_argument("--project-wpo-replays", nargs=3, type=Path, metavar=("FORWARD", "REVERSE", "MIDPOINT"), help="Optional matching-binary generated Gallery WPO captures; physical proof and its images are included in the assembled output")
    parser.add_argument(
        "--project-skeletal-forward",
        type=Path,
        help=(
            "Optional validated project-asset AnimBP forward capture. Must be paired with "
            "--project-skeletal-reverse."
        ),
    )
    parser.add_argument(
        "--project-skeletal-reverse",
        type=Path,
        help="Optional validated project-asset AnimBP reverse capture.",
    )
    parser.add_argument(
        "--project-animated-material-forward",
        type=Path,
        help=(
            "Optional validated project-authored animated-material forward capture. "
            "Must be paired with --project-animated-material-reverse."
        ),
    )
    parser.add_argument(
        "--project-animated-material-reverse",
        type=Path,
        help="Optional validated project-authored animated-material reverse capture.",
    )
    args = parser.parse_args()
    try:
        assemble(
            args.endpoints.resolve(),
            args.reverse_endpoints.resolve(),
            args.intermediate.resolve(),
            args.output.resolve(),
            args.project_skeletal_forward.resolve()
            if args.project_skeletal_forward
            else None,
            args.project_skeletal_reverse.resolve()
            if args.project_skeletal_reverse
            else None,
            args.project_animated_material_forward.resolve()
            if args.project_animated_material_forward
            else None,
            args.project_animated_material_reverse.resolve()
            if args.project_animated_material_reverse
            else None,
            [path.resolve() for path in args.project_wpo_replays] if args.project_wpo_replays else None,
        )
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"Assembled FG dataset; run ValidateFrameGenerationDataset.py before admission: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
