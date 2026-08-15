#!/usr/bin/env python3
"""Validate DeterministicDatasetCaptureUE output without modifying source data."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any

try:
    import numpy as np
    import OpenEXR
    from PIL import Image
except ImportError as exc:
    raise SystemExit(
        "Validation dependencies are missing. Run: "
        "python -m pip install -r Scripts/requirements-validation.txt"
    ) from exc


TEMPORAL_MODALITIES = (
    "color_hr_native_scene_hdr",
    "color_lr_scene_hdr",
    "velocity_raw",
    "velocity_coverage",
    "motion_full_current_to_previous",
    "motion_valid",
    "depth_device_raw",
    "depth_view_linear_meters",
    "depth_valid",
    "depth_previous_reprojected_device",
    "history_rejection_mask",
    "history_rejection_valid",
    "translucency_after_dof_raw",
    "transparency_mask",
    "reactive_mask",
    "object_id",
)
REFERENCE_MODALITIES = ("color_hr_reference_scene_hdr",)
HUDLESS_MODALITIES = ("color_main_view_hudless_after_tonemap",)
UI_MODALITIES = ("ui_color_alpha",)
MASK_MODALITIES = {
    "velocity_coverage",
    "motion_valid",
    "depth_valid",
    "history_rejection_mask",
    "history_rejection_valid",
}
COLOR_MODALITIES = {"hr", "lr", "color_lr_scene_hdr"}
COLOR_MODALITIES.add("color_hr_native_scene_hdr")
COLOR_MODALITIES.add("color_hr_reference_scene_hdr")
COLOR_MODALITIES.add("color_main_view_hudless_after_tonemap")
COLOR_MODALITIES.add("ui_color_alpha")
HR_TEMPORAL_MODALITIES = {
    "color_hr_native_scene_hdr",
    "color_hr_reference_scene_hdr",
    "color_main_view_hudless_after_tonemap",
}
HR_DISPLAY_MODALITIES = HR_TEMPORAL_MODALITIES | {"hr", "ui_color_alpha"}
REPLAY_ROLES = {
    "Standard",
    "FrameGenerationEndpoints",
    "FrameGenerationReverseEndpoints",
    "FrameGenerationIntermediate",
}
AUXILIARY_CAPTURE_ORDERS = {
    "HighResolutionFirst",
    "LowResolutionFirst",
}
REPLAY_METADATA_FIELDS = (
    "logicalFrameId",
    "simulationTick",
    "simulationTimeS",
    "deltaTimeS",
    "simulationAdvance",
    "historyAdvance",
    "reset",
    "resetReason",
    "timeSeconds",
    "resumed",
    "streamingStateSha1",
    "streamingTextureCount",
    "pendingStreamingTextureCount",
    "streamingRequestsWanting",
    "sceneStateSha1",
    "sceneActorCount",
    "sceneComponentCount",
    "sceneSkeletalComponentCount",
    "sceneBoneCount",
    "sceneFXComponentCount",
    "sceneNiagaraComponentCount",
    "sceneNiagaraEmitterCount",
    "sceneNiagaraCPUEmitterCount",
    "sceneNiagaraGPUEmitterCount",
    "sceneNiagaraParticleCount",
    "sceneNiagaraTotalSpawnedParticleCount",
    "niagaraFrameStates",
    "sceneControllableActorCount",
    "sceneUncontrolledTickingActorCount",
    "sceneControllableActors",
    "sceneUncontrolledTickingActors",
    "sceneStateHashScope",
    "renderSubmissions",
    "semanticValidationFixture",
    "camera",
    "temporalDiagnostics",
    "nativeHRDiagnostics",
    "referenceHRDiagnostics",
    "hudlessColorDiagnostics",
    "uiColorAlphaDiagnostics",
    "replayPass",
    "previousCapturedLogicalFrameId",
    "motionPreviousLogicalFrameId",
    "motionTimeSpanFrames",
    "motionTimeSpanS",
    "motionTrainingUsable",
    "materialTimeLogicalFrameLocked",
    "materialTimeSeconds",
    "materialPreviousLogicalFrameId",
    "materialPreviousTimeSeconds",
    "materialDeltaTimeSeconds",
    "endpointPreviousTransformOverride",
    "endpointPreviousSkeletalBoneOverride",
    "endpointPreviousSkeletalBoneComponentCount",
    "endpointPreviousSkeletalBoneCount",
    "endpointPreviousSkeletalBoneSkippedComponents",
    "auxiliaryCaptureOrder",
    "logicalEvaluationDirection",
    "skeletalPoseCacheReplayEnabled",
    "skeletalPoseCacheApplied",
    "skeletalPoseCacheAppliedComponentCount",
    "skeletalPoseCacheAppliedBoneCount",
    "skeletalPoseCacheSkippedComponents",
    "skeletalPoseCacheSource",
    "skeletalPoseCacheArtifactSha1",
    "projectAnimatedMaterialValidation",
    "worldSpaceWidgetPolicy",
    "nonFixtureSkeletalComponents",
)


def expected_submission_modalities(job: dict[str, Any]) -> list[str]:
    high_resolution = ["hr"]
    if job.get("bCaptureReferenceHR", False):
        high_resolution.append("hr_reference")
    low_resolution: list[str] = []
    if job.get("lRMode") == "NativeRender":
        low_resolution.append("lr")
    if job.get("bCaptureDepth", False):
        low_resolution.append("depth")
    auxiliary = (
        low_resolution + high_resolution
        if job.get("auxiliaryCaptureOrder") == "LowResolutionFirst"
        else high_resolution + low_resolution
    )
    if job.get("bCaptureMainViewTemporalDiagnostics", False):
        if job.get("bCaptureUIColorAlpha", False):
            auxiliary.append("ui_layer")
        auxiliary.append("main_view_temporal")
    return auxiliary


def normalized_capture_order_job(job: dict[str, Any]) -> dict[str, Any]:
    normalized = dict(job)
    for field in ("jobName", "outputDirectory", "auxiliaryCaptureOrder"):
        normalized.pop(field, None)
    return normalized


def normalized_capture_order_provenance(provenance: dict[str, Any]) -> dict[str, Any]:
    normalized = dict(provenance)
    normalized.pop("captureConfigSha1", None)
    return normalized


def normalized_vfx_reverse_job(job: dict[str, Any]) -> dict[str, Any]:
    normalized = dict(job)
    for field in (
        "jobName",
        "outputDirectory",
        "replayPass",
        "skeletalPoseCacheInputFile",
        "skeletalPoseCacheOutputFile",
    ):
        normalized.pop(field, None)
    return normalized


def normalized_vfx_reverse_provenance(provenance: dict[str, Any]) -> dict[str, Any]:
    normalized = dict(provenance)
    normalized.pop("captureConfigSha1", None)
    return normalized


def niagara_fixture_state(frame: dict[str, Any]) -> dict[str, Any] | None:
    expected_asset = str(
        frame.get("semanticValidationFixture", {}).get("niagaraFixtureAsset", "")
    )
    for state in frame.get("niagaraFrameStates", []):
        if isinstance(state, dict) and str(state.get("assetPath", "")) == expected_asset:
            return state
    return None


def nonfixture_project_probe_states(
    frame: dict[str, Any],
) -> dict[str, dict[str, Any]]:
    states = frame.get("nonFixtureSkeletalComponents", [])
    if not isinstance(states, list):
        return {}
    return {
        str(state.get("componentPath")): state
        for state in states
        if isinstance(state, dict)
        and state.get("isProjectValidationProbe") is True
        and isinstance(state.get("componentPath"), str)
        and state.get("componentPath")
    }


def sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def png_size(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError("invalid PNG header")
    return struct.unpack(">II", header[16:24])


def exr_rgba(path: Path) -> np.ndarray:
    channels = OpenEXR.File(str(path)).channels()
    if "RGBA" in channels:
        pixels = channels["RGBA"].pixels
    else:
        names = ("R", "G", "B", "A")
        if not all(name in channels for name in names):
            raise ValueError(f"expected RGBA channels, found {sorted(channels)}")
        pixels = np.stack([channels[name].pixels for name in names], axis=-1)
    if pixels.ndim != 3 or pixels.shape[2] != 4:
        raise ValueError(f"expected HxWx4 pixels, found {pixels.shape}")
    return pixels.astype(np.float32, copy=False)


def png_rgba(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGBA"), dtype=np.float32) / 255.0


def image_rgba(path: Path) -> np.ndarray:
    return png_rgba(path) if path.suffix.lower() == ".png" else exr_rgba(path)


def safe_dataset_file(dataset: Path, relative: Any) -> Path:
    if not isinstance(relative, str) or not relative:
        raise ValueError("file path must be a non-empty relative string")
    relative_path = Path(relative)
    if relative_path.is_absolute():
        raise ValueError("absolute file paths are forbidden")
    root = dataset.resolve()
    candidate = (root / relative_path).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise ValueError("file path escapes the dataset root") from exc
    return candidate


def numeric_comparison(left: np.ndarray, right: np.ndarray) -> dict[str, float | bool]:
    if left.shape != right.shape:
        return {"valid": False, "shapeMatch": False}
    difference = np.abs(left.astype(np.float64) - right.astype(np.float64))
    rgb_difference = difference[..., :3]
    changed_pixels = np.max(rgb_difference, axis=-1) > 1e-6
    mse = float(np.mean(np.square(rgb_difference)))
    peak = max(float(np.max(np.abs(left[..., :3]))), float(np.max(np.abs(right[..., :3]))), 1.0)
    return {
        "valid": bool(np.isfinite(difference).all()),
        "shapeMatch": True,
        "meanAbs": float(np.mean(rgb_difference)),
        "p99Abs": float(np.quantile(rgb_difference, 0.99)),
        "maxAbs": float(np.max(rgb_difference)),
        "changedPixelFraction": float(np.mean(changed_pixels)),
        "psnrDb": math.inf if mse == 0.0 else float(10.0 * math.log10((peak * peak) / mse)),
    }


def write_heatmap(left: np.ndarray, right: np.ndarray, path: Path) -> None:
    difference = np.max(np.abs(left.astype(np.float64)[..., :3] - right.astype(np.float64)[..., :3]), axis=-1)
    robust_scale = max(float(np.quantile(difference, 0.99)), float(np.max(difference)) * 0.1, 1e-12)
    normalized = np.clip(difference / robust_scale, 0.0, 1.0)
    pixels = np.zeros((*normalized.shape, 4), dtype=np.uint8)
    pixels[..., 0] = np.rint(255.0 * normalized).astype(np.uint8)
    pixels[..., 1] = np.rint(64.0 * np.sqrt(normalized)).astype(np.uint8)
    pixels[..., 3] = 255
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(pixels, mode="RGBA").save(path)


def matrix_stats(values: Any) -> dict[str, Any]:
    array = np.asarray(values, dtype=np.float64)
    if array.shape != (16,):
        return {"valid": False, "reason": f"expected 16 values, found {array.shape}"}
    matrix = array.reshape(4, 4)
    finite = bool(np.isfinite(matrix).all())
    determinant = float(np.linalg.det(matrix)) if finite else math.nan
    return {
        "valid": finite and math.isfinite(determinant) and abs(determinant) > 1e-12,
        "finite": finite,
        "determinant": determinant,
    }


def add_check(
    checks: list[dict[str, Any]], name: str, passed: bool, detail: str, *, required: bool = True
) -> None:
    checks.append({"name": name, "passed": bool(passed), "required": required, "detail": detail})


def matrix(values: Any) -> np.ndarray:
    array = np.asarray(values, dtype=np.float64)
    return array.reshape(4, 4) if array.shape == (16,) else np.full((4, 4), np.nan)


def expected_automatic_view_mip_bias(resolution_fraction: float, cvars: dict[str, Any]) -> float:
    """Mirror UE's automatic material texture Mip-bias calculation for spatial upscale views."""
    if not math.isfinite(resolution_fraction) or resolution_fraction <= 0.0:
        return math.nan
    offset = float(cvars.get("r.ViewTextureMipBias.Offset", 0.0))
    minimum = float(cvars.get("r.ViewTextureMipBias.Min", -4.0))
    quantization = float(cvars.get("r.ViewTextureMipBias.Quantization", 0.0))
    bias = -max(-math.log2(resolution_fraction), 0.0) + offset
    bias = min(max(max(bias, minimum), -4.0), 4.0)
    if quantization > 0.0:
        step = 8.0 / quantization
        bias = math.ceil(bias / step) * step
    return bias


def validate_mip_bias_metadata(
    checks: list[dict[str, Any]],
    prefix: str,
    metadata: dict[str, Any],
    expected_automatic: float,
    expected_global: float,
) -> None:
    automatic = float(metadata.get("automaticViewMipBias", math.nan))
    global_bias = float(metadata.get("globalMipMapLODBias", math.nan))
    effective = float(metadata.get("effectiveMaterialTextureMipBias", math.nan))
    add_check(
        checks,
        f"{prefix}.automatic_view_mip_bias",
        math.isfinite(automatic)
        and math.isclose(automatic, expected_automatic, rel_tol=0.0, abs_tol=2e-4),
        f"expected={expected_automatic:.9g} actual={automatic:.9g}",
    )
    add_check(
        checks,
        f"{prefix}.global_mipmap_lod_bias",
        math.isfinite(global_bias)
        and math.isclose(global_bias, expected_global, rel_tol=0.0, abs_tol=1e-6),
        f"expected={expected_global:.9g} actual={global_bias:.9g}",
    )
    add_check(
        checks,
        f"{prefix}.effective_material_texture_mip_bias",
        math.isfinite(effective)
        and math.isclose(effective, automatic + global_bias, rel_tol=0.0, abs_tol=1e-6),
        f"automatic={automatic:.9g} global={global_bias:.9g} effective={effective:.9g}",
    )


def validate_temporal_frame(
    checks: list[dict[str, Any]],
    frame: dict[str, Any],
    temporal: dict[str, Any],
    pixels: dict[str, np.ndarray],
    lr_size: tuple[int, int],
    hr_size: tuple[int, int],
    main_view: bool,
    cvars: dict[str, Any],
) -> None:
    frame_id = int(frame["logicalFrameId"])
    prefix = f"frame_{frame_id:06d}"
    expected_fraction = lr_size[0] / hr_size[0]
    expected_main_mip_bias = expected_automatic_view_mip_bias(expected_fraction, cvars)
    # The isolated native/reference SceneCapture views are rendered at their full
    # target extent and do not opt into the Main View's automatic spatial-upscale
    # texture bias. Their View Uniform must therefore report zero here.
    expected_full_resolution_mip_bias = 0.0
    global_mip_bias = float(cvars.get("r.MipMapLODBias", 0.0))
    motion_previous_frame = int(frame.get("motionPreviousLogicalFrameId", frame_id))

    add_check(checks, f"{prefix}.render_size_contract", tuple(temporal.get("renderSize", ())) == lr_size, str(temporal.get("renderSize")))
    add_check(checks, f"{prefix}.display_size_contract", tuple(temporal.get("displaySize", ())) == hr_size, str(temporal.get("displaySize")))
    add_check(
        checks,
        f"{prefix}.resolution_fraction",
        math.isclose(float(temporal.get("resolutionFraction", math.nan)), expected_fraction, rel_tol=0.0, abs_tol=1e-6),
        f"expected={expected_fraction} actual={temporal.get('resolutionFraction')}",
    )
    validate_mip_bias_metadata(
        checks,
        f"{prefix}.temporal",
        temporal,
        expected_main_mip_bias,
        global_mip_bias,
    )
    add_check(
        checks,
        f"{prefix}.exposure_reciprocal",
        math.isclose(float(temporal.get("preExposure", math.nan)) * float(temporal.get("exposure", math.nan)), 1.0, rel_tol=1e-5, abs_tol=1e-5),
        f"preExposure={temporal.get('preExposure')} exposure={temporal.get('exposure')}",
    )
    add_check(
        checks,
        f"{prefix}.motion_endpoint_metadata",
        int(temporal.get("motionPreviousLogicalFrameId", -1)) == motion_previous_frame,
        f"frame_previous={motion_previous_frame} temporal_previous={temporal.get('motionPreviousLogicalFrameId')}",
    )
    expected_motion_span = float(frame.get("motionTimeSpanS", math.nan))
    add_check(
        checks,
        f"{prefix}.motion_time_span",
        math.isclose(float(temporal.get("motionTimeSpanS", math.nan)), expected_motion_span, rel_tol=0.0, abs_tol=1e-9),
        f"frame={expected_motion_span} temporal={temporal.get('motionTimeSpanS')}",
    )

    native_hr = frame.get("nativeHRDiagnostics")
    add_check(checks, f"{prefix}.native_hr_metadata", isinstance(native_hr, dict), "present")
    if isinstance(native_hr, dict):
        validate_mip_bias_metadata(
            checks,
            f"{prefix}.native_hr",
            native_hr,
            expected_full_resolution_mip_bias,
            global_mip_bias,
        )
        native_jitter = np.asarray(native_hr.get("jitterCurrentNDC", (math.nan, math.nan)), dtype=np.float64)
        native_jittered = matrix(native_hr.get("viewToClipCurrentJittered"))
        native_unjittered = matrix(native_hr.get("viewToClipCurrentUnjittered"))
        add_check(checks, f"{prefix}.native_hr_size", tuple(native_hr.get("renderSize", ())) == hr_size, str(native_hr.get("renderSize")))
        add_check(
            checks,
            f"{prefix}.native_hr_fixed_grid",
            native_hr.get("fixedOutputGrid") is True
            and bool(np.allclose(native_jitter, 0.0, rtol=0.0, atol=1e-7))
            and bool(np.allclose(native_jittered, native_unjittered, rtol=0.0, atol=1e-7)),
            f"jitter={native_jitter.tolist()} fixed={native_hr.get('fixedOutputGrid')}",
        )
        add_check(
            checks,
            f"{prefix}.native_hr_exposure_match",
            math.isclose(float(native_hr.get("preExposure", math.nan)), float(temporal.get("preExposure", math.nan)), rel_tol=0.0, abs_tol=1e-7),
            f"native={native_hr.get('preExposure')} lr={temporal.get('preExposure')}",
        )
        native_view = matrix(native_hr.get("translatedWorldToViewCurrent"))
        main_view_matrix = matrix(temporal.get("translatedWorldToViewCurrent"))
        add_check(
            checks,
            f"{prefix}.native_hr_main_view_alignment",
            bool(np.allclose(native_unjittered, matrix(temporal.get("viewToClipCurrentUnjittered")), rtol=0.0, atol=1e-6))
            and bool(np.allclose(native_view, main_view_matrix, rtol=0.0, atol=1e-6)),
            f"projection_max_error={float(np.max(np.abs(native_unjittered - matrix(temporal.get('viewToClipCurrentUnjittered'))))):.9g} view_max_error={float(np.max(np.abs(native_view - main_view_matrix))):.9g}",
        )

    reference_enabled = bool(frame.get("referenceHRDiagnostics"))
    if reference_enabled:
        reference = frame["referenceHRDiagnostics"]
        validate_mip_bias_metadata(
            checks,
            f"{prefix}.reference_hr",
            reference,
            expected_full_resolution_mip_bias,
            global_mip_bias,
        )
        scale = int(reference.get("spatialScalePerAxis", 0))
        source_size = (hr_size[0] * scale, hr_size[1] * scale)
        reference_jitter = np.asarray(reference.get("jitterCurrentNDC", (math.nan, math.nan)), dtype=np.float64)
        reference_jittered = matrix(reference.get("viewToClipCurrentJittered"))
        reference_unjittered = matrix(reference.get("viewToClipCurrentUnjittered"))
        add_check(checks, f"{prefix}.reference_hr_source_size", tuple(reference.get("sourceRenderSize", ())) == source_size, str(reference.get("sourceRenderSize")))
        add_check(checks, f"{prefix}.reference_hr_output_size", tuple(reference.get("outputSize", ())) == hr_size, str(reference.get("outputSize")))
        add_check(
            checks,
            f"{prefix}.reference_hr_fixed_grid",
            reference.get("fixedOutputGrid") is True
            and bool(np.allclose(reference_jitter, 0.0, rtol=0.0, atol=1e-7))
            and bool(np.allclose(reference_jittered, reference_unjittered, rtol=0.0, atol=1e-7)),
            f"jitter={reference_jitter.tolist()} fixed={reference.get('fixedOutputGrid')}",
        )
        add_check(
            checks,
            f"{prefix}.reference_hr_exposure_match",
            math.isclose(float(reference.get("preExposure", math.nan)), float(temporal.get("preExposure", math.nan)), rel_tol=0.0, abs_tol=1e-7),
            f"reference={reference.get('preExposure')} lr={temporal.get('preExposure')}",
        )
        add_check(
            checks,
            f"{prefix}.reference_hr_isolation_contract",
            reference.get("viewState") == "isolated_reference_scene_capture"
            and reference.get("historyAdvance") is False
            and reference.get("simulationAdvance") is False,
            json.dumps({key: reference.get(key) for key in ("viewState", "historyAdvance", "simulationAdvance")}, sort_keys=True),
        )
        reference_view = matrix(reference.get("translatedWorldToViewCurrent"))
        main_projection = matrix(temporal.get("viewToClipCurrentUnjittered"))
        main_view_matrix = matrix(temporal.get("translatedWorldToViewCurrent"))
        add_check(
            checks,
            f"{prefix}.reference_hr_main_view_alignment",
            bool(np.allclose(reference_unjittered, main_projection, rtol=0.0, atol=1e-6))
            and bool(np.allclose(reference_view, main_view_matrix, rtol=0.0, atol=1e-6)),
            f"projection_max_error={float(np.max(np.abs(reference_unjittered - main_projection))):.9g} view_max_error={float(np.max(np.abs(reference_view - main_view_matrix))):.9g}",
        )

    if main_view:
        submissions = frame.get("renderSubmissions", [])
        matching = [item for item in submissions if item.get("modality") == "main_view_temporal"]
        add_check(
            checks,
            f"{prefix}.main_view_submission",
            len(matching) == 1 and matching[0].get("viewState") == "player_main_view",
            json.dumps(matching, sort_keys=True),
        )
        add_check(
            checks,
            f"{prefix}.dynamic_resolution_disabled",
            temporal.get("dynamicResolutionEnabled") is False,
            str(temporal.get("dynamicResolutionEnabled")),
        )

    hudless = frame.get("hudlessColorDiagnostics")
    if isinstance(hudless, dict):
        validate_mip_bias_metadata(
            checks,
            f"{prefix}.hudless",
            hudless,
            expected_main_mip_bias,
            global_mip_bias,
        )
        add_check(
            checks,
            f"{prefix}.hudless_display_size",
            tuple(hudless.get("size", ())) == hr_size and hudless.get("displayResolution") is True,
            str(hudless.get("size")),
        )
        add_check(
            checks,
            f"{prefix}.hudless_pipeline_stage",
            hudless.get("pipelineStage") == "after_tonemap_before_ui"
            and hudless.get("uiIncluded") is False
            and hudless.get("hudIncluded") is False,
            json.dumps({key: hudless.get(key) for key in ("pipelineStage", "uiIncluded", "hudIncluded")}, sort_keys=True),
        )
        add_check(
            checks,
            f"{prefix}.hudless_pre_exposure_match",
            math.isclose(float(hudless.get("preExposureBeforeTonemap", math.nan)), float(temporal.get("preExposure", math.nan)), rel_tol=0.0, abs_tol=1e-7),
            f"hudless={hudless.get('preExposureBeforeTonemap')} temporal={temporal.get('preExposure')}",
        )

    matrix_names = (
        "viewToClipCurrentJittered",
        "viewToClipCurrentUnjittered",
        "viewToClipPrevious",
        "viewToClipPreviousUnjittered",
        "clipToPreviousClipUnjittered",
        "clipToPreviousClipJittered",
        "translatedWorldToViewCurrent",
        "viewToTranslatedWorldCurrent",
        "translatedWorldToClipCurrentJittered",
        "translatedWorldToClipCurrentUnjittered",
        "clipToTranslatedWorldCurrentJittered",
        "translatedWorldToViewPrevious",
        "viewToTranslatedWorldPrevious",
        "translatedWorldToClipPreviousJittered",
        "translatedWorldToClipPreviousUnjittered",
    )
    for name in matrix_names:
        result = matrix_stats(temporal.get(name))
        add_check(checks, f"{prefix}.{name}.invertible", bool(result["valid"]), json.dumps(result, sort_keys=True))

    current_projection = matrix(temporal.get("viewToClipCurrentJittered"))
    current_projection_noaa = matrix(temporal.get("viewToClipCurrentUnjittered"))
    previous_projection = matrix(temporal.get("viewToClipPrevious"))
    previous_projection_noaa = matrix(temporal.get("viewToClipPreviousUnjittered"))
    current_jitter = np.asarray(temporal.get("jitterCurrentNDC", (math.nan, math.nan)), dtype=np.float64)
    previous_jitter = np.asarray(temporal.get("jitterPreviousNDC", (math.nan, math.nan)), dtype=np.float64)
    add_check(
        checks,
        f"{prefix}.current_jitter_matrix_sign",
        bool(np.allclose(current_projection[2, :2] - current_projection_noaa[2, :2], current_jitter, rtol=0.0, atol=1e-7)),
        f"matrixDelta={(current_projection[2, :2] - current_projection_noaa[2, :2]).tolist()} metadata={current_jitter.tolist()}",
    )
    add_check(
        checks,
        f"{prefix}.previous_jitter_matrix_sign",
        bool(np.allclose(previous_projection[2, :2] - previous_projection_noaa[2, :2], previous_jitter, rtol=0.0, atol=1e-7)),
        f"matrixDelta={(previous_projection[2, :2] - previous_projection_noaa[2, :2]).tolist()} metadata={previous_jitter.tolist()}",
    )

    world_to_view = matrix(temporal.get("translatedWorldToViewCurrent"))
    view_to_world = matrix(temporal.get("viewToTranslatedWorldCurrent"))
    world_to_clip = matrix(temporal.get("translatedWorldToClipCurrentJittered"))
    clip_to_world = matrix(temporal.get("clipToTranslatedWorldCurrentJittered"))
    identity = np.eye(4)
    add_check(
        checks,
        f"{prefix}.view_matrix_inverse",
        bool(np.allclose(world_to_view @ view_to_world, identity, rtol=1e-5, atol=1e-5)),
        f"max_error={float(np.max(np.abs(world_to_view @ view_to_world - identity))):.9g}",
    )
    add_check(
        checks,
        f"{prefix}.clip_matrix_inverse",
        bool(np.allclose(world_to_clip @ clip_to_world, identity, rtol=1e-5, atol=1e-5)),
        f"max_error={float(np.max(np.abs(world_to_clip @ clip_to_world - identity))):.9g}",
    )
    add_check(
        checks,
        f"{prefix}.world_view_projection_composition",
        bool(np.allclose(world_to_view @ current_projection, world_to_clip, rtol=1e-5, atol=1e-5)),
        f"max_error={float(np.max(np.abs(world_to_view @ current_projection - world_to_clip))):.9g}",
    )

    needed = set(TEMPORAL_MODALITIES) - {"color_lr_scene_hdr"}
    if not needed.issubset(pixels):
        return
    velocity_raw = pixels["velocity_raw"]
    motion_full = pixels["motion_full_current_to_previous"]
    velocity_coverage = pixels["velocity_coverage"][..., 0]
    motion_valid = pixels["motion_valid"][..., 0]
    depth_device = pixels["depth_device_raw"][..., 0]
    depth_linear = pixels["depth_view_linear_meters"][..., 0]
    depth_valid = pixels["depth_valid"][..., 0]
    depth_previous_reprojected = pixels["depth_previous_reprojected_device"][..., 0]
    history_rejection = pixels["history_rejection_mask"][..., 0]
    history_rejection_valid = pixels["history_rejection_valid"][..., 0]
    add_check(checks, f"{prefix}.velocity_coverage_channel", bool(np.array_equal(velocity_raw[..., 2], velocity_coverage)), "velocity_raw.B == velocity_coverage.R")
    add_check(checks, f"{prefix}.motion_coverage_channel", bool(np.array_equal(motion_full[..., 2], velocity_coverage)), "motion_full.B == velocity_coverage.R")
    add_check(checks, f"{prefix}.motion_valid_channel", bool(np.array_equal(motion_full[..., 3], motion_valid)), "motion_full.A == motion_valid.R")
    add_check(checks, f"{prefix}.velocity_valid_channel", bool(np.array_equal(velocity_raw[..., 3], motion_valid)), "velocity_raw.A == motion_valid.R")
    add_check(checks, f"{prefix}.invalid_depth_zero", bool(np.all(depth_linear[depth_valid == 0.0] == 0.0)), "linear depth is zero where invalid")
    add_check(
        checks,
        f"{prefix}.previous_reprojected_depth_finite_nonnegative",
        bool(np.isfinite(depth_previous_reprojected).all() and np.min(depth_previous_reprojected) >= 0.0),
        f"min={float(np.min(depth_previous_reprojected)):.9g} max={float(np.max(depth_previous_reprojected)):.9g}",
    )
    add_check(
        checks,
        f"{prefix}.history_rejection_contract",
        temporal.get("historyRejectionDefinition")
        == "one_rejects_previous_history_at_motion_reprojected_pixel"
        and temporal.get("historyRejectionSource")
        == "custom_stencil_identity_else_static_camera_depth_reprojection_v1"
        and temporal.get("historyRejectionTrainingUsable") is True
        and temporal.get("historyRejectionRequiresValidityMask") is True
        and temporal.get("historyRejectionProductionCertified") is False,
        json.dumps(
            {
                key: temporal.get(key)
                for key in (
                    "historyRejectionDefinition",
                    "historyRejectionSource",
                    "historyRejectionTrainingUsable",
                    "historyRejectionRequiresValidityMask",
                    "historyRejectionProductionCertified",
                )
            },
            sort_keys=True,
        ),
    )
    if frame.get("reset") is True:
        add_check(
            checks,
            f"{prefix}.history_rejection_reset",
            bool(np.all(history_rejection == 1.0) and np.all(history_rejection_valid == 1.0)),
            f"reject_mean={float(np.mean(history_rejection)):.9g} valid_mean={float(np.mean(history_rejection_valid)):.9g}",
        )
    else:
        add_check(
            checks,
            f"{prefix}.history_rejection_nontrivial",
            bool(
                np.any((history_rejection == 0.0) & (history_rejection_valid == 1.0))
                and np.any(history_rejection == 1.0)
            ),
            f"reject_fraction={float(np.mean(history_rejection)):.9g} valid_fraction={float(np.mean(history_rejection_valid)):.9g}",
        )

    translucency = pixels["translucency_after_dof_raw"]
    transparency = pixels["transparency_mask"][..., 0]
    reactive = pixels["reactive_mask"][..., 0]
    object_id = pixels["object_id"][..., 0]
    add_check(
        checks,
        f"{prefix}.transparency_range",
        bool(np.all((transparency >= 0.0) & (transparency <= 1.0))),
        f"min={float(np.min(transparency)):.9g} max={float(np.max(transparency)):.9g}",
    )
    add_check(
        checks,
        f"{prefix}.reactive_range",
        bool(np.all((reactive >= 0.0) & (reactive <= 1.0))),
        f"min={float(np.min(reactive)):.9g} max={float(np.max(reactive)):.9g}",
    )
    transparency_error = np.abs(transparency - np.clip(1.0 - translucency[..., 3], 0.0, 1.0))
    add_check(
        checks,
        f"{prefix}.transparency_source_semantic",
        bool(np.max(transparency_error) <= 1e-4),
        f"max_error={float(np.max(transparency_error)):.9g}",
    )
    add_check(
        checks,
        f"{prefix}.reactive_conservative_v1",
        bool(np.array_equal(reactive, transparency)),
        "reactive mask equals conservative transparency coverage",
    )
    object_id_error = np.abs(object_id - np.rint(object_id))
    add_check(
        checks,
        f"{prefix}.object_id_uint8",
        bool(np.all((object_id >= 0.0) & (object_id <= 255.0)) and np.max(object_id_error) <= 1e-6),
        f"min={float(np.min(object_id)):.9g} max={float(np.max(object_id)):.9g} max_integer_error={float(np.max(object_id_error)):.9g}",
    )
    add_check(
        checks,
        f"{prefix}.object_id_contract",
        temporal.get("objectIdSource") == "custom_stencil_uint8_zero_unlabeled",
        str(temporal.get("objectIdSource")),
    )

    valid = (depth_valid == 1.0) & (depth_device > 0.0)
    add_check(checks, f"{prefix}.valid_depth_present", bool(np.any(valid)), f"valid_fraction={float(np.mean(valid)):.9f}")
    if np.any(valid) and frame.get("camera", {}).get("projection") == "Perspective":
        near_meters = float(temporal.get("nearPlane", math.nan)) * float(temporal.get("viewSpaceToMeters", 0.01))
        predicted_linear = near_meters / depth_device[valid]
        linear_error = np.abs(predicted_linear - depth_linear[valid])
        # At long reversed-Z distances, GPU reciprocal/projection precision
        # produces an error proportional to depth (about 1 cm at 100 m on the
        # D3D12 validation device). Keep a strict 1 mm floor plus 0.02% bound.
        linear_tolerance = np.maximum(1e-3, np.abs(depth_linear[valid]) * 2e-4)
        add_check(
            checks,
            f"{prefix}.reversed_z_linearization",
            bool(np.all(linear_error <= linear_tolerance)),
            f"max_error_m={float(np.max(linear_error)):.9g} max_tolerance_m={float(np.max(linear_tolerance)):.9g} mean_error_m={float(np.mean(linear_error)):.9g}",
        )

        height, width = depth_device.shape
        yy, xx = np.mgrid[:height, :width]
        screen_x = 2.0 * (xx + 0.5) / width - 1.0
        screen_y = 2.0 * (1.0 - (yy + 0.5) / height) - 1.0
        clip = np.stack((screen_x, screen_y, depth_device, np.ones_like(depth_device)), axis=-1)
        clip_to_view = np.linalg.inv(current_projection)
        view_h = clip @ clip_to_view
        view_z = np.zeros_like(view_h[..., 2])
        np.divide(view_h[..., 2], view_h[..., 3], out=view_z, where=view_h[..., 3] != 0.0)
        view_z_meters = view_z * float(temporal.get("viewSpaceToMeters", 0.01))
        reconstruction_error = np.abs(view_z_meters[valid] - depth_linear[valid])
        reconstruction_tolerance = np.maximum(1e-3, np.abs(depth_linear[valid]) * 2e-4)
        add_check(
            checks,
            f"{prefix}.view_position_reconstruction",
            bool(np.all(reconstruction_error <= reconstruction_tolerance)),
            f"max_error_m={float(np.max(reconstruction_error)):.9g} max_tolerance_m={float(np.max(reconstruction_tolerance)):.9g} mean_error_m={float(np.mean(reconstruction_error)):.9g}",
        )


def validate_semantic_fixture_frame(
    checks: list[dict[str, Any]],
    frame: dict[str, Any],
    fixture: dict[str, Any],
    pixels: dict[str, np.ndarray],
) -> None:
    frame_id = int(frame["logicalFrameId"])
    prefix = f"frame_{frame_id:06d}.semantic_fixture"
    required_modalities = {
        "object_id",
        "motion_full_current_to_previous",
        "velocity_coverage",
        "depth_view_linear_meters",
        "depth_valid",
        "transparency_mask",
    }
    if not required_modalities.issubset(pixels):
        add_check(
            checks,
            f"{prefix}.modalities",
            False,
            f"missing={sorted(required_modalities - set(pixels))}",
        )
        return

    object_id = np.rint(pixels["object_id"][..., 0]).astype(np.int32)
    velocity_coverage = pixels["velocity_coverage"][..., 0]
    intermediate_replay = frame.get("replayPass") == "FrameGenerationIntermediate"

    def validate_motion_object(
        label: str,
        stencil_id: int,
        expected_motion: np.ndarray,
        minimum_pixels: int,
        reset_tolerance: float,
        motion_tolerance: float,
    ) -> None:
        mask = object_id == stencil_id
        visible_pixels = int(np.count_nonzero(mask))
        add_check(
            checks,
            f"{prefix}.{label}_object_visible",
            visible_pixels >= minimum_pixels,
            f"id={stencil_id} pixels={visible_pixels}",
        )
        if not visible_pixels:
            return

        covered = mask & (velocity_coverage == 1.0)
        covered_count = int(np.count_nonzero(covered))
        covered_fraction = covered_count / visible_pixels
        expected_is_finite = expected_motion.shape == (2,) and bool(
            np.all(np.isfinite(expected_motion))
        )
        expected_is_moving = expected_is_finite and bool(
            np.max(np.abs(expected_motion)) > reset_tolerance
        )
        if (not frame.get("reset", False) or intermediate_replay) and expected_is_moving:
            add_check(
                checks,
                f"{prefix}.{label}_velocity_coverage",
                covered_fraction >= 0.75,
                f"covered={covered_count} fraction={covered_fraction:.9f}",
            )
        if frame.get("reset", False) and not intermediate_replay:
            reset_motion = pixels["motion_full_current_to_previous"][mask, :2]
            add_check(
                checks,
                f"{prefix}.{label}_reset_motion_zero",
                bool(np.max(np.abs(reset_motion)) <= reset_tolerance),
                f"max_abs={float(np.max(np.abs(reset_motion))):.9g}",
            )
        elif not expected_is_moving:
            static_motion = pixels["motion_full_current_to_previous"][mask, :2]
            add_check(
                checks,
                f"{prefix}.{label}_static_motion_zero",
                expected_is_finite
                and bool(np.max(np.abs(static_motion)) <= reset_tolerance),
                f"expected={expected_motion.tolist()} "
                f"max_abs={float(np.max(np.abs(static_motion))):.9g} "
                f"covered={covered_count}",
            )
        elif covered_count:
            measured_motion = np.median(
                pixels["motion_full_current_to_previous"][covered, :2].astype(np.float64),
                axis=0,
            )
            error = np.abs(measured_motion - expected_motion)
            add_check(
                checks,
                f"{prefix}.{label}_motion_direction_magnitude",
                bool(np.all(error <= motion_tolerance)),
                f"expected={expected_motion.tolist()} measured={measured_motion.tolist()} "
                f"abs_error={error.tolist()}",
            )

    validate_motion_object(
        "moving",
        int(fixture.get("movingObjectId", -1)),
        np.asarray(
            fixture.get("expectedMovingMotionDisplayPixels", (math.nan, math.nan)),
            dtype=np.float64,
        ),
        8,
        1e-4,
        1.5,
    )
    validate_motion_object(
        "skeletal",
        int(fixture.get("skeletalObjectId", -1)),
        np.asarray(
            fixture.get("expectedSkeletalMotionDisplayPixels", (math.nan, math.nan)),
            dtype=np.float64,
        ),
        32,
        0.01,
        2.0,
    )
    validate_motion_object(
        "wpo",
        int(fixture.get("wpoObjectId", -1)),
        np.asarray(
            fixture.get("expectedWPOMotionDisplayPixels", (math.nan, math.nan)),
            dtype=np.float64,
        ),
        32,
        0.01,
        1.5,
    )

    linear_depth = pixels["depth_view_linear_meters"][..., 0]
    depth_valid = pixels["depth_valid"][..., 0]
    expected_depths = fixture.get("expectedFrontDepthMetersByObjectId", {})
    for object_id_text, expected_value in sorted(expected_depths.items(), key=lambda item: int(item[0])):
        stencil_id = int(object_id_text)
        mask = (object_id == stencil_id) & (depth_valid == 1.0)
        pixel_count = int(np.count_nonzero(mask))
        add_check(
            checks,
            f"{prefix}.depth_{stencil_id}.visible",
            pixel_count >= 2,
            f"pixels={pixel_count}",
        )
        if pixel_count:
            measured = float(np.median(linear_depth[mask]))
            expected = float(expected_value)
            tolerance = max(0.002, expected * 0.0002)
            add_check(
                checks,
                f"{prefix}.depth_{stencil_id}.known_distance",
                abs(measured - expected) <= tolerance,
                f"expected_m={expected:.9g} measured_m={measured:.9g} tolerance_m={tolerance:.9g}",
            )

    transparency = pixels["transparency_mask"][..., 0]
    reactive_pixels = int(np.count_nonzero(transparency > 0.01))
    add_check(
        checks,
        f"{prefix}.nonzero_after_dof_transparency",
        reactive_pixels >= 8 and float(np.max(transparency)) >= 0.05,
        f"pixels_gt_0.01={reactive_pixels} max={float(np.max(transparency)):.9g}",
    )

    if fixture.get("niagaraVisibleProbeExpected") is True:
        display_size = np.asarray(
            frame.get("temporalDiagnostics", {}).get("displaySize", (0, 0)),
            dtype=np.float64,
        )
        anchor = np.asarray(
            fixture.get("niagaraAnchorDisplayPixels", (math.nan, math.nan)),
            dtype=np.float64,
        )
        radius = float(fixture.get("niagaraValidationRadiusDisplayPixels", math.nan))
        valid_probe = (
            display_size.shape == (2,)
            and np.all(display_size > 0)
            and anchor.shape == (2,)
            and np.all(np.isfinite(anchor))
            and math.isfinite(radius)
            and radius > 0
        )
        roi_pixels = np.empty((0,), dtype=transparency.dtype)
        if valid_probe:
            height, width = transparency.shape
            scale = np.asarray((width, height), dtype=np.float64) / display_size
            center = anchor * scale
            radius_render = radius * scale[0]
            yy, xx = np.ogrid[:height, :width]
            roi = (xx - center[0]) ** 2 + (yy - center[1]) ** 2 <= radius_render**2
            roi_pixels = transparency[roi]
        visible_pixels = int(np.count_nonzero(roi_pixels > 0.01))
        maximum = float(np.max(roi_pixels)) if roi_pixels.size else 0.0
        add_check(
            checks,
            f"{prefix}.niagara_visible_after_dof_probe",
            valid_probe and visible_pixels >= 8 and maximum >= 0.5,
            f"pixels_gt_0.01={visible_pixels} max={maximum:.9g} anchor={anchor.tolist()} radius={radius}",
        )


def validate(
    dataset: Path,
    compare: Path | None,
    compare_mode: str = "exact-replay",
) -> tuple[dict[str, Any], bool]:
    dataset = dataset.resolve()
    manifest_path = dataset / "manifest.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"manifest not found: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    checks: list[dict[str, Any]] = []
    stats: dict[str, list[dict[str, Any]]] = {}

    add_check(checks, "capture_completed", manifest.get("state") == "Completed", str(manifest.get("state")))
    frames = manifest.get("frames", [])
    add_check(checks, "frames_present", bool(frames), f"count={len(frames)}")
    job = manifest.get("job", {})
    hr_size = (int(job.get("hRResolution", {}).get("x", 0)), int(job.get("hRResolution", {}).get("y", 0)))
    lr_size = (int(job.get("lRResolution", {}).get("x", 0)), int(job.get("lRResolution", {}).get("y", 0)))
    temporal_enabled = bool(job.get("bCaptureTemporalDiagnostics", False))
    main_view = bool(job.get("bCaptureMainViewTemporalDiagnostics", False))
    auxiliary_capture_order = str(job.get("auxiliaryCaptureOrder") or "HighResolutionFirst")
    add_check(
        checks,
        "capture_order.job_contract",
        auxiliary_capture_order in AUXILIARY_CAPTURE_ORDERS
        and not (
            auxiliary_capture_order == "LowResolutionFirst"
            and job.get("lRMode") != "NativeRender"
        ),
        f"order={auxiliary_capture_order} lrMode={job.get('lRMode')}",
    )
    replay_role = str(manifest.get("replayPass") or job.get("replayPass") or "Standard")
    job_replay_role = str(job.get("replayPass") or "Standard")
    add_check(
        checks,
        "replay.role_contract",
        replay_role in REPLAY_ROLES and replay_role == job_replay_role,
        f"manifest={replay_role} job={job_replay_role}",
    )
    frame_rate_numerator = int(job.get("captureFrameRateNumerator", 0))
    frame_rate_denominator = int(job.get("captureFrameRateDenominator", 0))
    valid_frame_rate = frame_rate_numerator > 0 and frame_rate_denominator > 0
    add_check(
        checks,
        "replay.frame_rate",
        valid_frame_rate,
        f"numerator={frame_rate_numerator} denominator={frame_rate_denominator}",
    )
    if replay_role in (
        "FrameGenerationEndpoints",
        "FrameGenerationReverseEndpoints",
    ):
        add_check(
            checks,
            "replay.endpoint_job_contract",
            job.get("bSuppressMainViewOnUncapturedFrames") is True
            and job.get("bUseLastCapturedEndpointTransforms") is True
            and job.get("bLockTemporalJitterToLogicalFrame") is True
            and int(job.get("frameStep", 1)) > 1,
            json.dumps(
                {
                    key: job.get(key)
                    for key in (
                        "bSuppressMainViewOnUncapturedFrames",
                        "bUseLastCapturedEndpointTransforms",
                        "bLockTemporalJitterToLogicalFrame",
                        "frameStep",
                    )
                },
                sort_keys=True,
            ),
        )
    elif replay_role == "FrameGenerationIntermediate":
        add_check(
            checks,
            "replay.intermediate_job_contract",
            job.get("bSuppressMainViewOnUncapturedFrames") is True
            and job.get("bUseLastCapturedEndpointTransforms") is not True
            and job.get("bLockTemporalJitterToLogicalFrame") is True
            and int(job.get("frameStep", 1)) > 1
            and 0 < int(job.get("captureFrameOffset", 0)) < int(job.get("frameStep", 1)),
            json.dumps(
                {
                    key: job.get(key)
                    for key in (
                        "bSuppressMainViewOnUncapturedFrames",
                        "bUseLastCapturedEndpointTransforms",
                        "bLockTemporalJitterToLogicalFrame",
                        "frameStep",
                        "captureFrameOffset",
                    )
                },
                sort_keys=True,
            ),
        )
    determinism_contract = manifest.get("determinismContract", {})
    if job.get("bLockMaterialTimeToLogicalFrame", False):
        add_check(
            checks,
            "material_time.logical_frame_contract",
            determinism_contract.get("materialTimeEvaluation")
            == "scene_view_family_game_time_current_and_signed_previous_from_logical_frame_ids_real_time_frozen",
            str(determinism_contract.get("materialTimeEvaluation")),
        )
    if job.get("bRejectVisibleWidgetComponents", False):
        add_check(
            checks,
            "world_ui.job_and_contract",
            determinism_contract.get("worldSpaceWidgetPolicy")
            == "reject_any_visible_registered_UWidgetComponent_before_and_during_capture",
            str(determinism_contract.get("worldSpaceWidgetPolicy")),
        )
    if job.get("bControlNiagara", False):
        add_check(
            checks,
            "niagara.absolute_age_contract",
            determinism_contract.get("niagaraAbsoluteAge") is True
            and determinism_contract.get("niagaraAgeEvaluation")
            == "solo_absolute_fixed_step_advance_wait_for_concurrent_tick_and_finalize_before_capture"
            and determinism_contract.get("niagaraInitialAgeWarmup")
            == "progressive_fixed_age_ramp_normal_single_tick_plus_one_ulp_when_initial_age_nonzero"
            and determinism_contract.get("niagaraPayloadEvidence")
            == "cpu_emitter_particle_counts_and_visible_semantic_fixture_pixels_gpu_payload_not_read_back",
            json.dumps(
                {
                    key: determinism_contract.get(key)
                    for key in (
                        "niagaraAbsoluteAge",
                        "niagaraAgeEvaluation",
                        "niagaraInitialAgeWarmup",
                        "niagaraPayloadEvidence",
                    )
                },
                sort_keys=True,
            ),
        )
        add_check(
            checks,
            "niagara.forced_determinism_contract",
            determinism_contract.get("niagaraForcedDeterminism")
            is bool(job.get("bForceNiagaraDeterminism", False)),
            str(determinism_contract.get("niagaraForcedDeterminism")),
        )
    if job.get("bValidateNonFixtureSkeletalAnimation", False):
        add_check(
            checks,
            "skeletal_replay.job_and_contract",
            job.get("bCacheSkeletalAnimationPosesForReplay") is True
            and determinism_contract.get("skeletalAnimationReplay")
            == "shared_or_forward_baked_component_space_pose_cache_by_logical_frame"
            and determinism_contract.get("skeletalPoseCacheArtifact")
            == "engine_versioned_binary_component_path_asset_bones_visibility_sha1"
            and determinism_contract.get("nonFixtureSkeletalValidationEnabled") is True
            and determinism_contract.get("nonFixtureSkeletalValidationActorClass")
            == job.get("nonFixtureSkeletalValidationActorClass")
            and str(job.get("nonFixtureSkeletalValidationActorClass", "")).startswith(
                "/Game/"
            ),
            json.dumps(
                {
                    key: determinism_contract.get(key)
                    for key in (
                        "skeletalAnimationReplay",
                        "skeletalPoseCacheArtifact",
                        "nonFixtureSkeletalValidationEnabled",
                        "nonFixtureSkeletalValidationActorClass",
                    )
                },
                sort_keys=True,
            ),
        )
    if job.get("bValidateProjectAnimatedMaterial", False):
        add_check(
            checks,
            "material_replay.project_validation_contract",
            job.get("bLockMaterialTimeToLogicalFrame") is True
            and determinism_contract.get(
                "projectAnimatedMaterialValidationEnabled"
            )
            is True
            and determinism_contract.get(
                "projectAnimatedMaterialValidationInterface"
            )
            == job.get("projectAnimatedMaterialValidationMaterial")
            and determinism_contract.get(
                "projectAnimatedMaterialValidationCarrier"
            )
            == "transient_labeled_engine_cube_with_project_material_interface"
            and str(job.get("projectAnimatedMaterialValidationMaterial", "")).startswith(
                "/Game/"
            ),
            json.dumps(
                {
                    key: determinism_contract.get(key)
                    for key in (
                        "projectAnimatedMaterialValidationEnabled",
                        "projectAnimatedMaterialValidationInterface",
                        "projectAnimatedMaterialValidationCarrier",
                    )
                },
                sort_keys=True,
            ),
        )
    if job.get("bUseDeterministicCameraTransform", False):
        add_check(
            checks,
            "camera.deterministic_override_contract",
            determinism_contract.get("cameraEvaluation")
            == "explicit_transient_player_view_target_locked_each_tick",
            str(determinism_contract.get("cameraEvaluation")),
        )
    if job.get("bSuppressMainViewOnUncapturedFrames", False):
        add_check(
            checks,
            "replay.uncaptured_renderer_prime_contract",
            determinism_contract.get("uncapturedRendererPrime")
            == "offscreen_scene_capture_64_pixel_long_edge_without_player_main_view_history",
            str(determinism_contract.get("uncapturedRendererPrime")),
        )
    provenance = manifest.get("provenance", {})
    add_check(checks, "provenance_present", isinstance(provenance, dict) and bool(provenance), "present")
    for name in ("captureConfigSha1", "cvarProfileSha1", "contentMapSha1", "shaderSourceSha1"):
        value = str(provenance.get(name, ""))
        valid_hash = len(value) == 40 and all(character in "0123456789ABCDEFabcdef" for character in value)
        add_check(checks, f"provenance.{name}", valid_hash, value)
    if job.get("bCacheSkeletalAnimationPosesForReplay", False) and (
        job.get("skeletalPoseCacheInputFile")
        or job.get("skeletalPoseCacheOutputFile")
    ):
        artifact_hash = str(provenance.get("skeletalPoseCacheArtifactSha1", ""))
        add_check(
            checks,
            "provenance.skeletal_pose_cache_artifact",
            len(artifact_hash) == 40
            and all(
                character in "0123456789ABCDEFabcdef"
                for character in artifact_hash
            ),
            artifact_hash,
        )
    streaming_hash = str(provenance.get("streamingStateAfterBarrierSha1", ""))
    valid_streaming_hash = len(streaming_hash) == 40 and all(
        character in "0123456789ABCDEFabcdef" for character in streaming_hash
    )
    streaming_barrier_enabled = job.get("bBlockOnStreamingBeforeCapture", True) is True
    streaming_requests = int(provenance.get("streamingRequestsAfterBarrier", -1))
    streaming_texture_count = int(provenance.get("streamingTextureCountAfterBarrier", -1))
    pending_streaming_texture_count = int(
        provenance.get("pendingStreamingTextureCountAfterBarrier", -1)
    )
    add_check(
        checks,
        "provenance.streaming_barrier",
        provenance.get("streamingBarrierEnabled") is streaming_barrier_enabled
        and provenance.get("streamingBarrierComplete") is True
        and (not streaming_barrier_enabled or streaming_requests == 0),
        json.dumps(
            {
                "enabled": provenance.get("streamingBarrierEnabled"),
                "complete": provenance.get("streamingBarrierComplete"),
                "requests": streaming_requests,
            },
            sort_keys=True,
        ),
    )
    add_check(
        checks,
        "provenance.streaming_state",
        valid_streaming_hash
        and streaming_texture_count > 0
        and pending_streaming_texture_count >= 0,
        f"sha1={streaming_hash} textures={streaming_texture_count} pending={pending_streaming_texture_count}",
    )
    cvars = provenance.get("cvars", {})
    cvar_lines = [f"{name}={value}" for name, value in sorted(cvars.items())]
    canonical_cvar_profile = str(provenance.get("cvarProfileCanonical", ""))
    calculated_cvar_hash = hashlib.sha1(canonical_cvar_profile.encode("utf-8")).hexdigest().upper()
    add_check(
        checks,
        "provenance.cvar_profile_hash",
        calculated_cvar_hash == str(provenance.get("cvarProfileSha1", "")).upper(),
        f"expected={provenance.get('cvarProfileSha1')} actual={calculated_cvar_hash}",
    )
    add_check(
        checks,
        "provenance.cvar_profile_canonical",
        set(canonical_cvar_profile.splitlines()) == set(cvar_lines),
        f"canonical_lines={len(canonical_cvar_profile.splitlines())} cvars={len(cvar_lines)}",
    )
    if job.get("bLockExposure", False):
        add_check(checks, "provenance.exposure_locked", cvars.get("r.EyeAdaptationQuality") == "0", str(cvars.get("r.EyeAdaptationQuality")))
    if job.get("bLockTemporalJitterToLogicalFrame", False):
        add_check(
            checks,
            "provenance.logical_frame_jitter_override",
            cvars.get("r.TemporalAA.Debug.OverrideTemporalIndex") == "0",
            str(cvars.get("r.TemporalAA.Debug.OverrideTemporalIndex")),
        )
    if job.get("bCaptureMainViewTemporalDiagnostics", False):
        add_check(
            checks,
            "provenance.vertex_deformation_velocity_enabled",
            cvars.get("r.Velocity.EnableVertexDeformation") == "1",
            str(cvars.get("r.Velocity.EnableVertexDeformation")),
        )
    if temporal_enabled:
        add_check(
            checks,
            "provenance.custom_stencil_enabled",
            cvars.get("r.CustomDepth") == "3",
            str(cvars.get("r.CustomDepth")),
        )
    if job.get("bForceSynchronousRendering", False):
        synchronous_names = (
            "r.RDG.ParallelExecute",
            "r.RDG.AsyncCompute",
            "r.OneFrameThreadLag",
            "r.Lumen.AsyncCompute",
            "r.LumenScene.ParallelUpdate",
        )
        add_check(
            checks,
            "provenance.synchronous_render_profile",
            all(cvars.get(name) == "0" for name in synchronous_names),
            json.dumps({name: cvars.get(name) for name in synchronous_names}, sort_keys=True),
        )
    if job.get("bUseLastCapturedEndpointTransforms", False):
        add_check(
            checks,
            "provenance.endpoint_motion_vector_simulation",
            cvars.get("r.MotionVectorSimulation") == "1",
            str(cvars.get("r.MotionVectorSimulation")),
        )
    elif replay_role in ("Standard", "FrameGenerationIntermediate"):
        add_check(
            checks,
            "provenance.motion_vector_simulation_disabled",
            cvars.get("r.MotionVectorSimulation") == "0",
            str(cvars.get("r.MotionVectorSimulation")),
        )

    frame_hashes: dict[tuple[int, str], str] = {}
    frame_paths: dict[tuple[int, str], Path] = {}
    temporal_records: list[tuple[int, dict[str, Any], bool]] = []
    semantic_records: list[tuple[int, dict[str, Any], dict[str, np.ndarray]]] = []
    nonfixture_skeletal_records: list[
        tuple[int, dict[str, dict[str, Any]], bool, dict[str, np.ndarray]]
    ] = []
    project_animated_material_records: list[
        tuple[int, dict[str, Any], np.ndarray]
    ] = []
    all_render_submission_ids: list[int] = []
    for frame in frames:
        frame_id = int(frame["logicalFrameId"])
        frame_role = str(frame.get("replayPass") or "Standard")
        add_check(
            checks,
            f"frame_{frame_id:06d}.replay_role",
            frame_role == replay_role,
            f"expected={replay_role} actual={frame_role}",
        )
        widget_policy = frame.get("worldSpaceWidgetPolicy", {})
        if job.get("bRejectVisibleWidgetComponents", False):
            widget_policy_ok = bool(
                isinstance(widget_policy, dict)
                and widget_policy.get("policy")
                == "reject_visible_registered_widget_components"
                and int(
                    widget_policy.get(
                        "activeVisibleRegisteredComponentCount", -1
                    )
                )
                == 0
                and widget_policy.get(
                    "activeVisibleRegisteredComponentPaths"
                )
                == []
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.world_ui_zero_widget_component_residue",
                widget_policy_ok,
                json.dumps(widget_policy, sort_keys=True),
            )
        if job.get("bUseDeterministicCameraTransform", False):
            camera = frame.get("camera", {})
            configured_location = job.get("deterministicCameraLocationCm", {})
            configured_rotation = job.get(
                "deterministicCameraRotationDegrees", {}
            )
            expected_location = np.asarray(
                [
                    configured_location.get("x", math.nan),
                    configured_location.get("y", math.nan),
                    configured_location.get("z", math.nan),
                ],
                dtype=np.float64,
            )
            expected_rotation = np.asarray(
                [
                    configured_rotation.get("pitch", math.nan),
                    configured_rotation.get("yaw", math.nan),
                    configured_rotation.get("roll", math.nan),
                ],
                dtype=np.float64,
            )
            actual_location = np.asarray(
                camera.get("locationCm", (math.nan,) * 3), dtype=np.float64
            )
            actual_rotation = np.asarray(
                camera.get("rotationDeg", (math.nan,) * 3), dtype=np.float64
            )
            expected_fov = float(
                job.get("deterministicCameraFOVDegrees", math.nan)
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.deterministic_camera_exact",
                actual_location.shape == (3,)
                and actual_rotation.shape == (3,)
                and bool(
                    np.allclose(
                        actual_location,
                        expected_location,
                        rtol=0.0,
                        atol=1e-5,
                    )
                )
                and bool(
                    np.allclose(
                        actual_rotation,
                        expected_rotation,
                        rtol=0.0,
                        atol=1e-5,
                    )
                )
                and math.isclose(
                    float(camera.get("fovDegrees", math.nan)),
                    expected_fov,
                    rel_tol=0.0,
                    abs_tol=1e-6,
                ),
                json.dumps(
                    {
                        "expectedLocation": expected_location.tolist(),
                        "actualLocation": actual_location.tolist(),
                        "expectedRotation": expected_rotation.tolist(),
                        "actualRotation": actual_rotation.tolist(),
                        "expectedFov": expected_fov,
                        "actualFov": camera.get("fovDegrees"),
                    },
                    sort_keys=True,
                ),
            )
        motion_training_usable = frame.get("motionTrainingUsable")
        expected_motion_training_usable = replay_role != "FrameGenerationIntermediate"
        add_check(
            checks,
            f"frame_{frame_id:06d}.motion_training_role",
            motion_training_usable is expected_motion_training_usable,
            f"expected={expected_motion_training_usable} actual={motion_training_usable}",
        )
        endpoint_override = frame.get("endpointPreviousTransformOverride")
        expected_endpoint_override = replay_role in (
            "FrameGenerationEndpoints",
            "FrameGenerationReverseEndpoints",
        )
        add_check(
            checks,
            f"frame_{frame_id:06d}.endpoint_transform_role",
            endpoint_override is expected_endpoint_override,
            f"expected={expected_endpoint_override} actual={endpoint_override}",
        )
        expected_skeletal_override = (
            expected_endpoint_override and int(frame.get("motionTimeSpanFrames", 0)) > 0
        )
        skeletal_override = frame.get("endpointPreviousSkeletalBoneOverride")
        skeletal_component_count = int(
            frame.get("endpointPreviousSkeletalBoneComponentCount", -1)
        )
        skeletal_bone_count = int(frame.get("endpointPreviousSkeletalBoneCount", -1))
        skeletal_skipped = frame.get("endpointPreviousSkeletalBoneSkippedComponents")
        add_check(
            checks,
            f"frame_{frame_id:06d}.endpoint_skeletal_bone_override",
            skeletal_override is expected_skeletal_override
            and isinstance(skeletal_skipped, list)
            and not skeletal_skipped
            and (
                (skeletal_component_count > 0 and skeletal_bone_count > 0)
                if expected_skeletal_override
                else (skeletal_component_count == 0 and skeletal_bone_count == 0)
            ),
            json.dumps(
                {
                    "expected": expected_skeletal_override,
                    "actual": skeletal_override,
                    "components": skeletal_component_count,
                    "bones": skeletal_bone_count,
                    "skipped": skeletal_skipped,
                },
                sort_keys=True,
            ),
        )
        expected_evaluation_direction = (
            "decreasing_frame_id"
            if replay_role == "FrameGenerationReverseEndpoints"
            else "increasing_frame_id"
        )
        add_check(
            checks,
            f"frame_{frame_id:06d}.logical_evaluation_direction",
            frame.get("logicalEvaluationDirection") == expected_evaluation_direction,
            f"expected={expected_evaluation_direction} actual={frame.get('logicalEvaluationDirection')}",
        )
        frame_capture_order = str(frame.get("auxiliaryCaptureOrder") or "")
        submissions = frame.get("renderSubmissions", [])
        expected_modalities = expected_submission_modalities(job)
        actual_modalities = [str(item.get("modality")) for item in submissions]
        submission_ids = [int(item.get("renderSubmissionId", -1)) for item in submissions]
        expected_view_states = [
            (
                "player_main_view"
                if modality == "main_view_temporal"
                else (
                    "independent_slate_game_layer"
                    if modality == "ui_layer"
                    else f"{modality}_scene_capture"
                )
            )
            for modality in actual_modalities
        ]
        actual_view_states = [str(item.get("viewState")) for item in submissions]
        submission_contract = (
            frame_capture_order == auxiliary_capture_order
            and actual_modalities == ([] if frame.get("resumed") is True else expected_modalities)
            and submission_ids == sorted(submission_ids)
            and len(set(submission_ids)) == len(submission_ids)
            and all(submission_id >= 0 for submission_id in submission_ids)
            and actual_view_states == expected_view_states
            and all(item.get("simulationAdvance") is False for item in submissions)
        )
        add_check(
            checks,
            f"frame_{frame_id:06d}.capture_order_submissions",
            submission_contract,
            json.dumps(
                {
                    "order": frame_capture_order,
                    "expectedModalities": expected_modalities,
                    "actualModalities": actual_modalities,
                    "submissionIds": submission_ids,
                    "viewStates": actual_view_states,
                },
                sort_keys=True,
            ),
        )
        all_render_submission_ids.extend(submission_ids)
        frame_streaming_hash = str(frame.get("streamingStateSha1", ""))
        valid_frame_streaming_hash = len(frame_streaming_hash) == 40 and all(
            character in "0123456789ABCDEFabcdef" for character in frame_streaming_hash
        )
        add_check(
            checks,
            f"frame_{frame_id:06d}.streaming_state",
            valid_frame_streaming_hash
            and int(frame.get("streamingTextureCount", -1)) > 0
            and int(frame.get("pendingStreamingTextureCount", -1)) >= 0
            and int(frame.get("streamingRequestsWanting", -1)) >= 0,
            json.dumps(
                {
                    key: frame.get(key)
                    for key in (
                        "streamingStateSha1",
                        "streamingTextureCount",
                        "pendingStreamingTextureCount",
                        "streamingRequestsWanting",
                    )
                },
                sort_keys=True,
            ),
        )
        scene_state_hash = str(frame.get("sceneStateSha1", ""))
        valid_scene_state_hash = len(scene_state_hash) == 40 and all(
            character in "0123456789ABCDEFabcdef" for character in scene_state_hash
        )
        actor_count = int(frame.get("sceneActorCount", -1))
        component_count = int(frame.get("sceneComponentCount", -1))
        skeletal_count = int(frame.get("sceneSkeletalComponentCount", -1))
        bone_count = int(frame.get("sceneBoneCount", -1))
        fx_count = int(frame.get("sceneFXComponentCount", -1))
        niagara_count = int(frame.get("sceneNiagaraComponentCount", -1))
        niagara_emitter_count = int(frame.get("sceneNiagaraEmitterCount", -1))
        niagara_cpu_emitter_count = int(frame.get("sceneNiagaraCPUEmitterCount", -1))
        niagara_gpu_emitter_count = int(frame.get("sceneNiagaraGPUEmitterCount", -1))
        niagara_particle_count = int(frame.get("sceneNiagaraParticleCount", -1))
        niagara_total_spawned = int(frame.get("sceneNiagaraTotalSpawnedParticleCount", -1))
        niagara_states = frame.get("niagaraFrameStates", [])
        controllable_count = int(frame.get("sceneControllableActorCount", -1))
        uncontrolled_ticking_count = int(frame.get("sceneUncontrolledTickingActorCount", -1))
        controllable_actors = frame.get("sceneControllableActors", [])
        uncontrolled_ticking_actors = frame.get("sceneUncontrolledTickingActors", [])
        add_check(
            checks,
            f"frame_{frame_id:06d}.scene_state_provenance",
            valid_scene_state_hash
            and actor_count > 0
            and component_count > 0
            and 0 <= skeletal_count <= component_count
            and bone_count >= 0
            and (skeletal_count > 0 or bone_count == 0)
            and 0 <= niagara_count <= fx_count <= component_count
            and niagara_emitter_count >= 0
            and niagara_cpu_emitter_count >= 0
            and niagara_gpu_emitter_count >= 0
            and niagara_cpu_emitter_count + niagara_gpu_emitter_count == niagara_emitter_count
            and niagara_particle_count >= 0
            and niagara_total_spawned >= niagara_particle_count
            and isinstance(niagara_states, list)
            and len(niagara_states) == niagara_count
            and 0 <= controllable_count <= actor_count
            and 0 <= uncontrolled_ticking_count <= actor_count
            and isinstance(controllable_actors, list)
            and len(controllable_actors) == controllable_count
            and len(set(controllable_actors)) == controllable_count
            and isinstance(uncontrolled_ticking_actors, list)
            and len(uncontrolled_ticking_actors) == uncontrolled_ticking_count
            and len(set(uncontrolled_ticking_actors)) == uncontrolled_ticking_count
            and frame.get("sceneStateHashScope")
            == "sorted_actor_component_transforms_visibility_tick_controllable_skeletal_component_space_bones_niagara_component_and_finalized_cpu_particle_counts_cascade_component_state_gpu_payload_not_read_back",
            json.dumps(
                {
                    "sha1": scene_state_hash,
                    "actors": actor_count,
                    "components": component_count,
                    "skeletalComponents": skeletal_count,
                    "bones": bone_count,
                    "fxComponents": fx_count,
                    "niagaraComponents": niagara_count,
                    "niagaraEmitters": niagara_emitter_count,
                    "niagaraCPUEmitters": niagara_cpu_emitter_count,
                    "niagaraGPUEmitters": niagara_gpu_emitter_count,
                    "niagaraParticles": niagara_particle_count,
                    "niagaraTotalSpawned": niagara_total_spawned,
                    "controllableActors": controllable_count,
                    "uncontrolledTickingActors": uncontrolled_ticking_count,
                    "uncontrolledTickingActorPaths": uncontrolled_ticking_actors,
                },
                sort_keys=True,
            ),
        )
        if job.get("bControlNiagara", False):
            state_totals_valid = all(
                isinstance(state, dict)
                and int(state.get("emitterCount", -1))
                == int(state.get("cpuEmitterCount", -2)) + int(state.get("gpuEmitterCount", -2))
                and int(state.get("particleCount", -1)) >= 0
                and int(state.get("totalSpawnedParticleCount", -1))
                >= int(state.get("particleCount", 0))
                and state.get("soloInstanceObservable") is True
                and len(state.get("emitterDeterminism", []))
                == int(state.get("emitterCount", -1))
                and len(state.get("emitterRandomSeeds", []))
                == int(state.get("emitterCount", -1))
                and (
                    not job.get("bForceNiagaraDeterminism", False)
                    or all(value is True for value in state.get("emitterDeterminism", []))
                )
                for state in niagara_states
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.niagara_state_totals",
                state_totals_valid
                and sum(int(state["emitterCount"]) for state in niagara_states)
                == niagara_emitter_count
                and sum(int(state["cpuEmitterCount"]) for state in niagara_states)
                == niagara_cpu_emitter_count
                and sum(int(state["gpuEmitterCount"]) for state in niagara_states)
                == niagara_gpu_emitter_count
                and sum(int(state["particleCount"]) for state in niagara_states)
                == niagara_particle_count
                and sum(int(state["totalSpawnedParticleCount"]) for state in niagara_states)
                == niagara_total_spawned,
                json.dumps(niagara_states, sort_keys=True),
            )
            if job.get("bEnableSemanticValidationFixture", False):
                fixture_state = niagara_fixture_state(frame)
                fixed_delta = np.float32(
                    frame_rate_denominator / frame_rate_numerator
                )
                expected_age = float(np.float32(frame_id) * fixed_delta)
                age_error = (
                    abs(float(fixture_state.get("simulationAgeS", math.nan)) - expected_age)
                    if fixture_state is not None
                    else math.inf
                )
                desired_age_error = (
                    float(fixture_state.get("desiredAgeS", math.nan)) - expected_age
                    if fixture_state is not None
                    else math.inf
                )
                expected_asset = "/SuperResolutionDataset/Validation/NS_SRDatasetVFXFixture.NS_SRDatasetVFXFixture"
                add_check(
                    checks,
                    f"frame_{frame_id:06d}.niagara_fixture_absolute_state",
                    fixture_state is not None
                    and fixture_state.get("assetPath") == expected_asset
                    and fixture_state.get("soloInstanceObservable") is True
                    and (
                        not job.get("bForceNiagaraDeterminism", False)
                        or fixture_state.get("systemDeterminism") is True
                    )
                    and fixture_state.get("systemFixedTick") is True
                    and abs(float(fixture_state.get("systemFixedTickS", math.nan)) - float(fixed_delta)) <= 1e-7
                    and age_error <= 1e-6
                    and 0.0 <= desired_age_error <= 1e-6
                    and int(fixture_state.get("emitterCount", -1)) == 1
                    and int(fixture_state.get("cpuEmitterCount", -1)) == 1
                    and int(fixture_state.get("gpuEmitterCount", -1)) == 0
                    and int(fixture_state.get("rendererCount", -1)) == 1
                    and fixture_state.get("emitterDeterminism") == [True]
                    and len(fixture_state.get("emitterRandomSeeds", [])) == 1,
                    f"expectedAge={expected_age:.9g} ageError={age_error:.9g} desiredAgeError={desired_age_error:.9g} state={json.dumps(fixture_state, sort_keys=True)}",
                )
                if frame.get("semanticValidationFixture", {}).get("niagaraVisibleProbeExpected") is True:
                    add_check(
                        checks,
                        f"frame_{frame_id:06d}.niagara_fixture_nonzero_particle_payload",
                        fixture_state is not None
                        and int(fixture_state.get("particleCount", 0)) >= 100
                        and int(fixture_state.get("totalSpawnedParticleCount", 0)) >= 100,
                        json.dumps(fixture_state, sort_keys=True),
                    )
        motion_span_frames = int(frame.get("motionTimeSpanFrames", -1))
        motion_span_seconds = float(frame.get("motionTimeSpanS", math.nan))
        expected_motion_seconds = (
            motion_span_frames * frame_rate_denominator / frame_rate_numerator
            if valid_frame_rate and motion_span_frames >= 0
            else math.nan
        )
        add_check(
            checks,
            f"frame_{frame_id:06d}.motion_span_units",
            motion_span_frames >= 0
            and math.isclose(motion_span_seconds, expected_motion_seconds, rel_tol=0.0, abs_tol=1e-9),
            f"frames={motion_span_frames} seconds={motion_span_seconds} expected={expected_motion_seconds}",
        )
        if job.get("bLockMaterialTimeToLogicalFrame", False) and valid_frame_rate:
            material_current_time = (
                frame_id * frame_rate_denominator / frame_rate_numerator
            )
            material_previous_frame = int(
                frame.get("materialPreviousLogicalFrameId", frame_id)
            )
            material_previous_time = (
                material_previous_frame
                * frame_rate_denominator
                / frame_rate_numerator
            )
            material_delta_time = material_current_time - material_previous_time
            material_frame_contract = bool(
                frame.get("materialTimeLogicalFrameLocked") is True
                and math.isclose(
                    float(frame.get("materialTimeSeconds", math.nan)),
                    material_current_time,
                    rel_tol=0.0,
                    abs_tol=1e-7,
                )
                and math.isclose(
                    float(frame.get("materialPreviousTimeSeconds", math.nan)),
                    material_previous_time,
                    rel_tol=0.0,
                    abs_tol=1e-7,
                )
                and math.isclose(
                    float(frame.get("materialDeltaTimeSeconds", math.nan)),
                    material_delta_time,
                    rel_tol=0.0,
                    abs_tol=1e-7,
                )
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.material_logical_time",
                material_frame_contract,
                json.dumps(
                    {
                        "expectedCurrent": material_current_time,
                        "expectedPrevious": material_previous_time,
                        "expectedDelta": material_delta_time,
                        "actualCurrent": frame.get("materialTimeSeconds"),
                        "actualPreviousFrame": frame.get(
                            "materialPreviousLogicalFrameId"
                        ),
                        "actualPrevious": frame.get("materialPreviousTimeSeconds"),
                        "actualDelta": frame.get("materialDeltaTimeSeconds"),
                    },
                    sort_keys=True,
                ),
            )
            for metadata_name in (
                "temporalDiagnostics",
                "nativeHRDiagnostics",
                "referenceHRDiagnostics",
                "hudlessColorDiagnostics",
            ):
                metadata = frame.get(metadata_name)
                if not isinstance(metadata, dict):
                    continue
                render_current = float(metadata.get("renderGameTimeS", math.nan))
                render_delta = float(metadata.get("renderDeltaTimeS", math.nan))
                render_time_ok = bool(
                    math.isfinite(render_current)
                    and math.isfinite(render_delta)
                    and math.isclose(
                        render_current,
                        material_current_time,
                        rel_tol=0.0,
                        abs_tol=1e-6,
                    )
                    and math.isclose(
                        render_delta,
                        material_delta_time,
                        rel_tol=0.0,
                        abs_tol=1e-6,
                    )
                )
                add_check(
                    checks,
                    f"frame_{frame_id:06d}.{metadata_name}.material_logical_time",
                    render_time_ok,
                    f"current={render_current} expectedCurrent={material_current_time} "
                    f"delta={render_delta} expectedDelta={material_delta_time}",
                )
        files = frame.get("files", {})
        hashes = frame.get("sha1", {})
        frame_pixels: dict[str, np.ndarray] = {}
        required = ["hr", "lr"] + (["depth"] if job.get("bCaptureDepth", False) else [])
        if temporal_enabled:
            required.extend(TEMPORAL_MODALITIES)
        if job.get("bCaptureReferenceHR", False):
            required.extend(REFERENCE_MODALITIES)
        if job.get("bCaptureMainViewHUDlessColor", False):
            required.extend(HUDLESS_MODALITIES)
        if job.get("bCaptureUIColorAlpha", False):
            required.extend(UI_MODALITIES)
        for modality in required:
            relative = files.get(modality)
            try:
                path = safe_dataset_file(dataset, relative)
                path_error = ""
            except Exception as exc:
                path = dataset / "__missing__"
                path_error = str(exc)
            exists = not path_error and path.is_file()
            add_check(
                checks,
                f"frame_{frame_id:06d}.{modality}.exists",
                exists,
                f"path={relative} error={path_error}",
            )
            if not exists:
                continue
            actual_hash = sha1(path)
            expected_hash = str(hashes.get(modality, "")).upper()
            add_check(
                checks,
                f"frame_{frame_id:06d}.{modality}.sha1",
                actual_hash == expected_hash,
                f"expected={expected_hash} actual={actual_hash}",
            )
            frame_hashes[(frame_id, modality)] = actual_hash
            frame_paths[(frame_id, modality)] = path

            if path.suffix.lower() == ".png":
                size = png_size(path)
                expected_size = hr_size if modality in HR_DISPLAY_MODALITIES else lr_size
                add_check(checks, f"frame_{frame_id:06d}.{modality}.size", size == expected_size, f"{size}")
                if modality in UI_MODALITIES:
                    pixels = png_rgba(path)
                    frame_pixels[modality] = pixels
                    finite = np.isfinite(pixels)
                    add_check(
                        checks,
                        f"frame_{frame_id:06d}.{modality}.finite",
                        bool(finite.all()),
                        f"finite_fraction={finite.mean():.9f}",
                    )
                    add_check(
                        checks,
                        f"frame_{frame_id:06d}.{modality}.unorm_range",
                        bool((pixels >= 0.0).all() and (pixels <= 1.0).all()),
                        f"min={float(pixels.min()):.9f} max={float(pixels.max()):.9f}",
                    )
                    channel_min = np.min(pixels, axis=(0, 1))
                    channel_max = np.max(pixels, axis=(0, 1))
                    channel_mean = np.mean(pixels, axis=(0, 1))
                    stats.setdefault(modality, []).append(
                        {
                            "logicalFrameId": frame_id,
                            "minRGBA": channel_min.tolist(),
                            "maxRGBA": channel_max.tolist(),
                            "meanRGBA": channel_mean.tolist(),
                        }
                    )
                continue

            pixels = exr_rgba(path)
            frame_pixels[modality] = pixels
            height, width, _ = pixels.shape
            expected_size = hr_size if modality in HR_TEMPORAL_MODALITIES else (lr_size if modality in TEMPORAL_MODALITIES else hr_size)
            add_check(
                checks,
                f"frame_{frame_id:06d}.{modality}.size",
                (width, height) == expected_size,
                f"{width}x{height}",
            )
            finite = np.isfinite(pixels)
            add_check(
                checks,
                f"frame_{frame_id:06d}.{modality}.finite",
                bool(finite.all()),
                f"finite_fraction={finite.mean():.9f}",
            )
            channel_min = np.nanmin(pixels, axis=(0, 1))
            channel_max = np.nanmax(pixels, axis=(0, 1))
            channel_mean = np.nanmean(pixels, axis=(0, 1))
            entry = {
                "logicalFrameId": frame_id,
                "minRGBA": channel_min.tolist(),
                "maxRGBA": channel_max.tolist(),
                "meanRGBA": channel_mean.tolist(),
            }
            stats.setdefault(modality, []).append(entry)
            if modality in MASK_MODALITIES:
                unique = np.unique(pixels[..., 0])
                binary = bool(np.all((unique == 0.0) | (unique == 1.0)))
                add_check(
                    checks,
                    f"frame_{frame_id:06d}.{modality}.binary",
                    binary,
                    f"values={unique[:16].tolist()}",
                )

        temporal = frame.get("temporalDiagnostics")
        if temporal_enabled:
            add_check(checks, f"frame_{frame_id:06d}.temporal_metadata", isinstance(temporal, dict), "present")
            if isinstance(temporal, dict):
                for name in (
                    "viewToClipCurrentJittered",
                    "viewToClipCurrentUnjittered",
                    "viewToClipPrevious",
                    "viewToClipPreviousUnjittered",
                    "clipToPreviousClipUnjittered",
                    "clipToPreviousClipJittered",
                ):
                    result = matrix_stats(temporal.get(name))
                    add_check(
                        checks,
                        f"frame_{frame_id:06d}.{name}.invertible",
                        bool(result["valid"]),
                        json.dumps(result, sort_keys=True),
                    )
                for name in ("preExposure", "exposure"):
                    value = temporal.get(name)
                    valid = isinstance(value, (int, float)) and math.isfinite(value) and value > 0
                    add_check(checks, f"frame_{frame_id:06d}.{name}.positive", valid, str(value))
                render_delta = temporal.get("renderDeltaTimeS")
                valid_render_delta = isinstance(render_delta, (int, float)) and math.isfinite(
                    render_delta
                )
                add_check(
                    checks,
                    f"frame_{frame_id:06d}.renderDeltaTimeS.finite_signed",
                    valid_render_delta,
                    str(render_delta),
                )

                if job.get("bLockTemporalJitterToLogicalFrame", False):
                    sequence_length = int(job.get("temporalJitterSequenceLength", 0))
                    phase_offset = int(job.get("temporalJitterPhaseOffset", 0))
                    expected_jitter_index = (
                        (frame_id + phase_offset) % sequence_length
                        if sequence_length > 0
                        else -1
                    )
                    add_check(
                        checks,
                        f"frame_{frame_id:06d}.logical_frame_jitter_phase",
                        1 <= sequence_length <= 8
                        and temporal.get("jitterLogicalFrameLocked") is True
                        and temporal.get("jitterIndexSource") == "logical_frame_debug_override"
                        and int(temporal.get("jitterIndex", -1)) == expected_jitter_index
                        and int(temporal.get("jitterSequenceLength", -1)) == sequence_length
                        and int(temporal.get("jitterPhaseOffset", 0)) == phase_offset,
                        json.dumps(
                            {
                                "expectedIndex": expected_jitter_index,
                                "actualIndex": temporal.get("jitterIndex"),
                                "sequenceLength": temporal.get("jitterSequenceLength"),
                                "phaseOffset": temporal.get("jitterPhaseOffset"),
                                "source": temporal.get("jitterIndexSource"),
                            },
                            sort_keys=True,
                        ),
                    )

                validate_temporal_frame(
                    checks, frame, temporal, frame_pixels, lr_size, hr_size, main_view, cvars
                )
                temporal_records.append((frame_id, temporal, bool(frame.get("reset", False))))
                if job.get("bEnableSemanticValidationFixture", False):
                    fixture = frame.get("semanticValidationFixture")
                    add_check(
                        checks,
                        f"frame_{frame_id:06d}.semantic_fixture_metadata",
                        isinstance(fixture, dict) and fixture.get("enabled") is True,
                        "present",
                    )
                    if isinstance(fixture, dict):
                        validate_semantic_fixture_frame(checks, frame, fixture, frame_pixels)
                        semantic_records.append((frame_id, fixture, frame_pixels))
                if job.get("bCaptureReferenceHR", False):
                    reference = frame.get("referenceHRDiagnostics")
                    add_check(
                        checks,
                        f"frame_{frame_id:06d}.reference_hr_metadata",
                        isinstance(reference, dict),
                        "present",
                    )
                if job.get("bCaptureMainViewHUDlessColor", False):
                    hudless = frame.get("hudlessColorDiagnostics")
                    add_check(
                        checks,
                        f"frame_{frame_id:06d}.hudless_color_metadata",
                        isinstance(hudless, dict),
                        "present",
                    )
                    if isinstance(hudless, dict):
                        expected_output_device = int(float(cvars.get("r.HDR.Display.OutputDevice", -1)))
                        expected_color_gamut = int(float(cvars.get("r.HDR.Display.ColorGamut", -1)))
                        add_check(
                            checks,
                            f"frame_{frame_id:06d}.hudless_output_encoding_provenance",
                            int(hudless.get("outputDevice", -2)) == expected_output_device
                            and int(hudless.get("colorGamut", -2)) == expected_color_gamut,
                            f"metadata_device={hudless.get('outputDevice')} cvar_device={expected_output_device} metadata_gamut={hudless.get('colorGamut')} cvar_gamut={expected_color_gamut}",
                        )
                if job.get("bCaptureUIColorAlpha", False):
                    ui = frame.get("uiColorAlphaDiagnostics")
                    ui_pixels = frame_pixels.get("ui_color_alpha")
                    add_check(
                        checks,
                        f"frame_{frame_id:06d}.ui_color_alpha_metadata",
                        isinstance(ui, dict),
                        "present",
                    )
                    if isinstance(ui, dict) and ui_pixels is not None:
                        alpha = ui_pixels[..., 3]
                        nonzero = int(np.count_nonzero(alpha > (0.5 / 255.0)))
                        fractional = int(
                            np.count_nonzero(
                                (alpha > (0.5 / 255.0)) & (alpha < (254.5 / 255.0))
                            )
                        )
                        metadata_ok = (
                            ui.get("pipelineStage")
                            == "independent_slate_game_layer_before_scene_composite"
                            and ui.get("colorEncoding")
                            == "display_referred_srgb_png_unorm8"
                            and ui.get("alphaSemantic")
                            == "straight_coverage_zero_is_transparent_one_is_opaque"
                            and ui.get("rgbSemantic") == "premultiplied_by_coverage_alpha"
                            and ui.get("source")
                            == "SGameLayerManager_without_enclosing_SViewport_scene_backbuffer"
                            and ui.get("sceneIncluded") is False
                            and ui.get("screenSpaceGameLayersIncluded") is True
                            and ui.get("displayResolution") is True
                            and tuple(ui.get("size", ())) == hr_size
                            and int(ui.get("nonzeroAlphaPixelCount", -1)) == nonzero
                            and int(ui.get("fractionalAlphaPixelCount", -1)) == fractional
                            and math.isclose(
                                float(ui.get("minAlpha", math.nan)),
                                float(alpha.min()),
                                rel_tol=0.0,
                                abs_tol=0.5 / 255.0,
                            )
                            and math.isclose(
                                float(ui.get("maxAlpha", math.nan)),
                                float(alpha.max()),
                                rel_tol=0.0,
                                abs_tol=0.5 / 255.0,
                            )
                        )
                        add_check(
                            checks,
                            f"frame_{frame_id:06d}.ui_color_alpha_contract",
                            metadata_ok,
                            json.dumps(
                                {
                                    "size": ui.get("size"),
                                    "nonzero": [ui.get("nonzeroAlphaPixelCount"), nonzero],
                                    "fractional": [ui.get("fractionalAlphaPixelCount"), fractional],
                                    "minAlpha": [ui.get("minAlpha"), float(alpha.min())],
                                    "maxAlpha": [ui.get("maxAlpha"), float(alpha.max())],
                                },
                                sort_keys=True,
                            ),
                        )
                        rgb_le_alpha = bool(
                            (ui_pixels[..., :3] <= alpha[..., None] + (1.0 / 255.0)).all()
                        )
                        add_check(
                            checks,
                            f"frame_{frame_id:06d}.ui_premultiplied_rgb_bound",
                            rgb_le_alpha,
                            f"max_rgb_minus_alpha={float(np.max(ui_pixels[..., :3] - alpha[..., None])):.9f}",
                        )
                        if ui.get("semanticValidationFixture") is True:
                            probes = ui.get("validationProbes", [])
                            probe_results: dict[str, Any] = {}
                            probes_ok = isinstance(probes, list) and len(probes) == 3
                            height, width, _ = ui_pixels.shape
                            for probe in probes if isinstance(probes, list) else []:
                                try:
                                    name = str(probe["name"])
                                    minimum = np.asarray(probe["normalizedMin"], dtype=np.float64)
                                    extent = np.asarray(probe["normalizedExtent"], dtype=np.float64)
                                    straight = np.asarray(probe["straightRGBA"], dtype=np.float64)
                                    x = min(width - 1, max(0, int(round((minimum[0] + 0.5 * extent[0]) * width))))
                                    y = min(height - 1, max(0, int(round((minimum[1] + 0.5 * extent[1]) * height))))
                                    actual = ui_pixels[y, x].astype(np.float64)
                                    expected = straight.copy()
                                    expected[:3] *= expected[3]
                                    matched = bool(np.allclose(actual, expected, rtol=0.0, atol=1.5 / 255.0))
                                    probes_ok = probes_ok and matched
                                    probe_results[name] = {
                                        "actual": actual.tolist(),
                                        "expectedPremultiplied": expected.tolist(),
                                    }
                                except Exception as exc:
                                    probes_ok = False
                                    probe_results[str(probe)] = {"error": str(exc)}
                            add_check(
                                checks,
                                f"frame_{frame_id:06d}.ui_validation_probes",
                                probes_ok,
                                json.dumps(probe_results, sort_keys=True),
                            )

        if job.get("bValidateNonFixtureSkeletalAnimation", False):
            states = frame.get("nonFixtureSkeletalComponents", [])
            cache_skips = frame.get("skeletalPoseCacheSkippedComponents")
            cache_components = int(
                frame.get("skeletalPoseCacheAppliedComponentCount", -1)
            )
            cache_bones = int(frame.get("skeletalPoseCacheAppliedBoneCount", -1))
            add_check(
                checks,
                f"frame_{frame_id:06d}.skeletal_pose_cache_applied",
                frame.get("skeletalPoseCacheReplayEnabled") is True
                and frame.get("skeletalPoseCacheApplied") is True
                and cache_components > 0
                and cache_bones > 0
                and isinstance(cache_skips, list)
                and not cache_skips,
                json.dumps(
                    {
                        "enabled": frame.get("skeletalPoseCacheReplayEnabled"),
                        "applied": frame.get("skeletalPoseCacheApplied"),
                        "components": cache_components,
                        "bones": cache_bones,
                        "skipped": cache_skips,
                    },
                    sort_keys=True,
                ),
            )
            expected_cache_source = (
                "shared_artifact"
                if job.get("skeletalPoseCacheInputFile")
                else "forward_warmup_bake"
            )
            frame_artifact_hash = str(
                frame.get("skeletalPoseCacheArtifactSha1", "")
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.skeletal_pose_cache_artifact",
                frame.get("skeletalPoseCacheSource") == expected_cache_source
                and len(frame_artifact_hash) == 40
                and frame_artifact_hash
                == str(provenance.get("skeletalPoseCacheArtifactSha1", "")),
                json.dumps(
                    {
                        "source": frame.get("skeletalPoseCacheSource"),
                        "expectedSource": expected_cache_source,
                        "frameSha1": frame_artifact_hash,
                        "provenanceSha1": provenance.get(
                            "skeletalPoseCacheArtifactSha1"
                        ),
                    },
                    sort_keys=True,
                ),
            )
            state_list = states if isinstance(states, list) else []
            component_paths = [
                str(state.get("componentPath", ""))
                for state in state_list
                if isinstance(state, dict)
            ]
            object_ids = [
                int(state.get("objectId", -1))
                for state in state_list
                if isinstance(state, dict)
            ]
            state_schema_ok = (
                bool(state_list)
                and len(component_paths) == len(state_list)
                and all(component_paths)
                and len(set(component_paths)) == len(component_paths)
                and len(object_ids) == len(state_list)
                and len(set(object_ids)) == len(object_ids)
                and all(1 <= object_id <= 254 for object_id in object_ids)
                and all(
                    isinstance(state, dict)
                    and state.get("projectAsset") is True
                    and str(state.get("skinnedAssetPath", "")).startswith("/Game/")
                    and int(state.get("boneCount", 0)) > 0
                    and len(str(state.get("poseSha1", ""))) == 40
                    and all(
                        character in "0123456789ABCDEFabcdef"
                        for character in str(state.get("poseSha1", ""))
                    )
                    and state.get("registered") is True
                    for state in state_list
                )
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.nonfixture_skeletal_state_schema",
                state_schema_ok,
                f"states={len(state_list)} components={component_paths} objectIds={object_ids}",
            )
            probe_states = nonfixture_project_probe_states(frame)
            configured_actor_class = str(
                job.get("nonFixtureSkeletalValidationActorClass", "")
            )
            probe_schema_ok = bool(probe_states) and all(
                state.get("ownerClass") == configured_actor_class
                and state.get("animationMode") == "AnimationBlueprint"
                and str(state.get("animationInstanceClass", "")).startswith("/Game/")
                and state.get("visible") is True
                and state.get("poseSha1") == state.get("cachedPoseSha1")
                and float(state.get("currentToCachedPoseMaxMatrixAbs", math.inf))
                <= 1e-9
                for state in probe_states.values()
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.project_anim_blueprint_probe",
                probe_schema_ok,
                json.dumps(probe_states, sort_keys=True),
            )

            object_id_pixels = frame_pixels.get("object_id")
            motion_pixels = frame_pixels.get("motion_full_current_to_previous")
            motion_valid_pixels = frame_pixels.get("motion_valid")
            probe_metrics: dict[str, Any] = {}
            visible_probe = False
            moving_probe = False
            if (
                object_id_pixels is not None
                and motion_pixels is not None
                and motion_valid_pixels is not None
            ):
                ids = np.rint(object_id_pixels[..., 0]).astype(np.int32)
                motion_valid = motion_valid_pixels[..., 0] == 1.0
                motion_magnitude = np.linalg.norm(motion_pixels[..., :2], axis=-1)
                for component_path, state in probe_states.items():
                    object_id = int(state.get("objectId", -1))
                    visible_mask = ids == object_id
                    covered_mask = visible_mask & motion_valid
                    visible_count = int(np.count_nonzero(visible_mask))
                    covered_count = int(np.count_nonzero(covered_mask))
                    p95_motion = (
                        float(np.percentile(motion_magnitude[covered_mask], 95.0))
                        if covered_count
                        else 0.0
                    )
                    visible_probe = visible_probe or visible_count >= 20
                    moving_probe = moving_probe or (
                        covered_count >= 20 and p95_motion > 0.05
                    )
                    probe_metrics[component_path] = {
                        "objectId": object_id,
                        "visiblePixels": visible_count,
                        "motionCoveredPixels": covered_count,
                        "motionP95DisplayPixels": p95_motion,
                    }
            add_check(
                checks,
                f"frame_{frame_id:06d}.project_skeletal_probe_visible",
                visible_probe,
                json.dumps(probe_metrics, sort_keys=True),
            )
            if frame.get("reset") is not True:
                add_check(
                    checks,
                    f"frame_{frame_id:06d}.project_skeletal_probe_endpoint_motion",
                    moving_probe,
                    json.dumps(probe_metrics, sort_keys=True),
                )
            nonfixture_skeletal_records.append(
                (
                    frame_id,
                    probe_states,
                    frame.get("reset") is True,
                    frame_pixels,
                )
            )

        if job.get("bValidateProjectAnimatedMaterial", False):
            material_state = frame.get("projectAnimatedMaterialValidation", {})
            configured_interface = str(
                job.get("projectAnimatedMaterialValidationMaterial", "")
            )
            schema_ok = bool(
                isinstance(material_state, dict)
                and material_state.get("enabled") is True
                and material_state.get("materialInterfacePath")
                == configured_interface
                and str(material_state.get("baseMaterialPath", "")).startswith(
                    "/Game/"
                )
                and material_state.get("projectAuthoredInterface") is True
                and material_state.get("projectAuthoredBaseMaterial") is True
                and material_state.get("registered") is True
                and material_state.get("visible") is True
                and int(material_state.get("receiverObjectId", -1)) == 150
                and math.isclose(
                    float(
                        material_state.get("logicalGameTimeSeconds", math.nan)
                    ),
                    float(frame.get("materialTimeSeconds", math.nan)),
                    rel_tol=0.0,
                    abs_tol=1e-7,
                )
                and math.isclose(
                    float(
                        material_state.get(
                            "previousLogicalGameTimeSeconds", math.nan
                        )
                    ),
                    float(frame.get("materialPreviousTimeSeconds", math.nan)),
                    rel_tol=0.0,
                    abs_tol=1e-7,
                )
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.project_animated_material_schema",
                schema_ok,
                json.dumps(material_state, sort_keys=True),
            )
            object_id_pixels = frame_pixels.get("object_id")
            color_pixels = frame_pixels.get("color_lr_scene_hdr")
            visible_count = 0
            receiver_color = np.asarray((math.nan, math.nan, math.nan))
            if object_id_pixels is not None and color_pixels is not None:
                receiver_mask = (
                    np.rint(object_id_pixels[..., 0]).astype(np.int32) == 150
                )
                visible_count = int(np.count_nonzero(receiver_mask))
                if visible_count:
                    receiver_color = np.mean(
                        color_pixels[..., :3][receiver_mask], axis=0
                    )
            receiver_visible = bool(
                visible_count >= 20
                and receiver_color.shape == (3,)
                and np.all(np.isfinite(receiver_color))
                and float(np.max(np.abs(receiver_color))) > 0.001
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.project_animated_material_visible",
                receiver_visible,
                f"pixels={visible_count} meanRgb={receiver_color.tolist()}",
            )
            project_animated_material_records.append(
                (frame_id, material_state, receiver_color)
            )

    add_check(
        checks,
        "capture_order.submission_ids_global",
        not all_render_submission_ids
        or all_render_submission_ids == list(range(len(all_render_submission_ids))),
        f"count={len(all_render_submission_ids)} first={all_render_submission_ids[:4]} last={all_render_submission_ids[-4:]}",
    )

    if job.get("bValidateProjectAnimatedMaterial", False):
        material_colors = [
            color
            for _, _, color in project_animated_material_records
            if color.shape == (3,) and np.all(np.isfinite(color))
        ]
        max_color_change = 0.0
        for left_index in range(len(material_colors)):
            for right_index in range(left_index + 1, len(material_colors)):
                max_color_change = max(
                    max_color_change,
                    float(
                        np.max(
                            np.abs(
                                material_colors[left_index]
                                - material_colors[right_index]
                            )
                        )
                    ),
                )
        add_check(
            checks,
            "material_replay.project_animated_material_changes_with_logical_time",
            len(material_colors) >= 2 and max_color_change > 0.01,
            f"records={len(material_colors)} maxMeanRgbChange={max_color_change}",
        )

    if job.get("bValidateNonFixtureSkeletalAnimation", False):
        pose_hashes_by_component: dict[str, set[str]] = {}
        for _, probe_states, _, _ in nonfixture_skeletal_records:
            for component_path, state in probe_states.items():
                pose_hashes_by_component.setdefault(component_path, set()).add(
                    str(state.get("poseSha1", ""))
                )
        changed_components = sorted(
            component_path
            for component_path, pose_hashes in pose_hashes_by_component.items()
            if len(pose_hashes) > 1
        )
        add_check(
            checks,
            "skeletal_replay.project_anim_blueprint_pose_changes",
            len(nonfixture_skeletal_records) >= 2 and bool(changed_components),
            f"records={len(nonfixture_skeletal_records)} changedComponents={changed_components}",
        )
        validated_project_disocclusion_transitions = 0
        for index in range(1, len(nonfixture_skeletal_records)):
            (
                frame_id,
                current_probe_states,
                reset,
                current_pixels,
            ) = nonfixture_skeletal_records[index]
            _, previous_probe_states, _, previous_pixels = (
                nonfixture_skeletal_records[index - 1]
            )
            if reset:
                continue
            required_modalities_present = all(
                modality in current_pixels
                for modality in (
                    "object_id",
                    "history_rejection_mask",
                    "history_rejection_valid",
                )
            ) and "object_id" in previous_pixels
            if not required_modalities_present:
                add_check(
                    checks,
                    f"frame_{frame_id:06d}.project_skeletal_disocclusion_buffers",
                    False,
                    "required Object ID/history-rejection buffers are missing",
                )
                continue
            project_ids = {
                int(state.get("objectId", -1))
                for state in current_probe_states.values()
            } | {
                int(state.get("objectId", -1))
                for state in previous_probe_states.values()
            }
            current_ids = np.rint(
                current_pixels["object_id"][..., 0]
            ).astype(np.int32)
            previous_ids = np.rint(
                previous_pixels["object_id"][..., 0]
            ).astype(np.int32)
            previous_probe = np.isin(previous_ids, list(project_ids))
            current_probe = np.isin(current_ids, list(project_ids))
            newly_revealed = previous_probe & ~current_probe
            newly_occluded = ~previous_probe & current_probe
            rejection = current_pixels["history_rejection_mask"][..., 0]
            rejection_valid = current_pixels["history_rejection_valid"][..., 0]
            transition_detail: dict[str, Any] = {}
            transition_ok = True
            for transition_name, transition_mask in (
                ("revealed", newly_revealed),
                ("occluded", newly_occluded),
            ):
                count = int(np.count_nonzero(transition_mask))
                rejected_valid = int(
                    np.count_nonzero(
                        transition_mask
                        & (rejection == 1.0)
                        & (rejection_valid == 1.0)
                    )
                )
                ratio = rejected_valid / count if count else 0.0
                transition_detail[transition_name] = {
                    "pixels": count,
                    "rejectedValidPixels": rejected_valid,
                    "ratio": ratio,
                }
                transition_ok = transition_ok and count >= 4 and ratio >= 0.95
            add_check(
                checks,
                f"frame_{frame_id:06d}.project_skeletal_bidirectional_visibility",
                transition_ok,
                json.dumps(transition_detail, sort_keys=True),
            )
            validated_project_disocclusion_transitions += int(transition_ok)
        add_check(
            checks,
            "skeletal_replay.project_disocclusion_transition_coverage",
            validated_project_disocclusion_transitions >= 1,
            f"validatedTransitions={validated_project_disocclusion_transitions}",
        )

    # Preserve manifest/process order. Reverse endpoint replay intentionally
    # captures t1 before t0 so the retained View State yields motion_0_to_1.
    for index in range(1, len(temporal_records)):
        frame_id, current, reset = temporal_records[index]
        _, previous, _ = temporal_records[index - 1]
        if reset:
            continue
        previous_jitter = np.asarray(current.get("jitterPreviousNDC", (math.nan, math.nan)), dtype=np.float64)
        prior_current_jitter = np.asarray(previous.get("jitterCurrentNDC", (math.nan, math.nan)), dtype=np.float64)
        add_check(
            checks,
            f"frame_{frame_id:06d}.jitter_history_continuity",
            bool(np.allclose(previous_jitter, prior_current_jitter, rtol=0.0, atol=1e-7)),
            f"previous={previous_jitter.tolist()} priorCurrent={prior_current_jitter.tolist()}",
        )
        add_check(
            checks,
            f"frame_{frame_id:06d}.state_frame_monotonic",
            int(current.get("stateFrameIndex", -1)) > int(previous.get("stateFrameIndex", -1)),
            f"previous={previous.get('stateFrameIndex')} current={current.get('stateFrameIndex')}",
        )

    if job.get("bSuppressMainViewOnUncapturedFrames", False):
        start = int(job.get("startFrame", 0))
        end = int(job.get("endFrame", -1))
        step = int(job.get("frameStep", 1))
        offset = int(job.get("captureFrameOffset", 0))
        expected_frame_ids = list(range(start + offset, end + 1, step))
        expected_process_order = (
            list(reversed(expected_frame_ids))
            if replay_role == "FrameGenerationReverseEndpoints"
            else expected_frame_ids
        )
        actual_frame_ids = [int(frame["logicalFrameId"]) for frame in frames]
        add_check(
            checks,
            "replay.captured_frame_phase",
            actual_frame_ids == expected_process_order,
            f"expectedProcessOrder={expected_process_order} actual={actual_frame_ids}",
        )
        if replay_role == "FrameGenerationIntermediate":
            add_check(
                checks,
                "replay.intermediate_single_submission_process",
                len(actual_frame_ids) == 1,
                (
                    f"captured={actual_frame_ids}; v1 requires one isolated intermediate per process "
                    "so a prior intermediate cannot become the retained Main View history"
                ),
            )
        for frame_index, frame in enumerate(frames):
            frame_id = int(frame["logicalFrameId"])
            expected_previous = (
                frame_id
                if frame_index == 0
                else int(frames[frame_index - 1]["logicalFrameId"])
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.endpoint_previous_frame",
                int(frame.get("previousCapturedLogicalFrameId", -1)) == expected_previous,
                f"expected={expected_previous} actual={frame.get('previousCapturedLogicalFrameId')}",
            )
            if replay_role in (
                "FrameGenerationEndpoints",
                "FrameGenerationReverseEndpoints",
            ):
                expected_motion_previous = expected_previous
                expected_span_frames = 0 if frame_index == 0 else step
            else:
                expected_motion_previous = frame_id - 1
                expected_span_frames = 1
            add_check(
                checks,
                f"frame_{frame_id:06d}.role_motion_previous_frame",
                int(frame.get("motionPreviousLogicalFrameId", -1)) == expected_motion_previous,
                f"expected={expected_motion_previous} actual={frame.get('motionPreviousLogicalFrameId')}",
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.role_motion_span_frames",
                int(frame.get("motionTimeSpanFrames", -1)) == expected_span_frames,
                f"expected={expected_span_frames} actual={frame.get('motionTimeSpanFrames')}",
            )
    elif replay_role == "Standard":
        ordered_frames = sorted(frames, key=lambda item: int(item["logicalFrameId"]))
        for index, frame in enumerate(ordered_frames):
            frame_id = int(frame["logicalFrameId"])
            previous_frame_id = frame_id if index == 0 else int(ordered_frames[index - 1]["logicalFrameId"])
            expected_span_frames = 0 if index == 0 else frame_id - previous_frame_id
            add_check(
                checks,
                f"frame_{frame_id:06d}.standard_motion_previous_frame",
                int(frame.get("motionPreviousLogicalFrameId", -1)) == previous_frame_id,
                f"expected={previous_frame_id} actual={frame.get('motionPreviousLogicalFrameId')}",
            )
            add_check(
                checks,
                f"frame_{frame_id:06d}.standard_motion_span_frames",
                int(frame.get("motionTimeSpanFrames", -1)) == expected_span_frames,
                f"expected={expected_span_frames} actual={frame.get('motionTimeSpanFrames')}",
            )

    # Preserve process order for the same reason as temporal history above.
    if len(semantic_records) > 1:
        semantic_frame_ids = {frame_id for frame_id, _, _ in semantic_records}
        semantic_scene_hashes = {
            str(frame.get("sceneStateSha1", ""))
            for frame in frames
            if int(frame.get("logicalFrameId", -1)) in semantic_frame_ids
        }
        add_check(
            checks,
            "semantic_fixture.scene_state_changes_across_frames",
            len(semantic_scene_hashes) == len(semantic_records),
            f"frames={len(semantic_records)} uniqueSceneStateHashes={len(semantic_scene_hashes)}",
        )
    validated_moving_transitions = 0
    for index in range(1, len(semantic_records)):
        frame_id, fixture, current_pixels = semantic_records[index]
        _, previous_fixture, previous_pixels = semantic_records[index - 1]
        current_right_cm = float(fixture.get("movingCurrentRightCm", math.nan))
        previous_right_cm = float(
            previous_fixture.get("movingCurrentRightCm", math.nan)
        )
        # Occlusion/disocclusion are transition properties. Once the validation
        # cube reaches its endpoint it intentionally stays still while Niagara
        # continues aging; requiring fresh geometry changes on those frames is
        # a false positive, not a stronger temporal contract.
        if (
            not math.isfinite(current_right_cm)
            or not math.isfinite(previous_right_cm)
            or math.isclose(current_right_cm, previous_right_cm, abs_tol=1e-5)
        ):
            continue
        validated_moving_transitions += 1
        current_ids = np.rint(current_pixels["object_id"][..., 0]).astype(np.int32)
        previous_ids = np.rint(previous_pixels["object_id"][..., 0]).astype(np.int32)
        moving_id = int(fixture.get("movingObjectId", -1))
        background_id = int(fixture.get("backgroundObjectId", -1))
        newly_revealed = (previous_ids == moving_id) & (current_ids == background_id)
        newly_occluded = (previous_ids == background_id) & (current_ids == moving_id)
        add_check(
            checks,
            f"frame_{frame_id:06d}.semantic_fixture.disocclusion_geometry",
            int(np.count_nonzero(newly_revealed)) >= 4,
            f"newly_revealed_pixels={int(np.count_nonzero(newly_revealed))}",
        )
        add_check(
            checks,
            f"frame_{frame_id:06d}.semantic_fixture.occlusion_geometry",
            int(np.count_nonzero(newly_occluded)) >= 4,
            f"newly_occluded_pixels={int(np.count_nonzero(newly_occluded))}",
        )
        history_rejection = current_pixels["history_rejection_mask"][..., 0]
        history_rejection_valid = current_pixels["history_rejection_valid"][..., 0]
        stable_background = (previous_ids == background_id) & (current_ids == background_id)
        revealed_count = int(np.count_nonzero(newly_revealed))
        revealed_rejected = int(
            np.count_nonzero(
                newly_revealed
                & (history_rejection == 1.0)
                & (history_rejection_valid == 1.0)
            )
        )
        stable_kept = int(
            np.count_nonzero(
                stable_background
                & (history_rejection == 0.0)
                & (history_rejection_valid == 1.0)
            )
        )
        add_check(
            checks,
            f"frame_{frame_id:06d}.semantic_fixture.revealed_history_rejected",
            revealed_count >= 4 and revealed_rejected / revealed_count >= 0.8,
            f"revealed={revealed_count} rejected_valid={revealed_rejected}",
        )
        add_check(
            checks,
            f"frame_{frame_id:06d}.semantic_fixture.stable_history_retained",
            stable_kept >= 8,
            f"stable_background_kept_valid={stable_kept}",
        )
    if len(semantic_records) > 1:
        add_check(
            checks,
            "semantic_fixture.moving_transition_coverage",
            validated_moving_transitions >= 1,
            f"validatedMovingTransitions={validated_moving_transitions}",
        )

    if compare is not None:
        other_manifest_path = compare / "manifest.json" if compare.is_dir() else compare
        other_root = (compare if compare.is_dir() else compare.parent).resolve()
        other = json.loads(other_manifest_path.read_text(encoding="utf-8"))
        other_job = other.get("job", {})
        other_role = str(other.get("replayPass") or other.get("job", {}).get("replayPass") or "Standard")
        if compare_mode in ("vfx-reverse", "skeletal-reverse", "material-reverse"):
            valid_role_pair = {replay_role, other_role} == {
                "FrameGenerationEndpoints",
                "FrameGenerationReverseEndpoints",
            }
            reverse_prefix = (
                "vfx_reverse"
                if compare_mode == "vfx-reverse"
                else "skeletal_reverse"
                if compare_mode == "skeletal-reverse"
                else "material_reverse"
            )
            add_check(
                checks,
                f"{reverse_prefix}.opposite_endpoint_roles",
                valid_role_pair,
                f"left={replay_role} right={other_role}",
            )
        else:
            add_check(
                checks,
                "replay.role_exact",
                other_role == replay_role,
                f"left={replay_role} right={other_role}",
            )
        if compare_mode == "capture-order":
            other_order = str(other_job.get("auxiliaryCaptureOrder") or "HighResolutionFirst")
            order_pair_valid = {
                auxiliary_capture_order,
                other_order,
            } == AUXILIARY_CAPTURE_ORDERS
            normalized_jobs_match = (
                normalized_capture_order_job(job)
                == normalized_capture_order_job(other_job)
            )
            normalized_provenance_match = (
                normalized_capture_order_provenance(provenance)
                == normalized_capture_order_provenance(other.get("provenance", {}))
            )
            add_check(
                checks,
                "capture_order.opposite_orders",
                order_pair_valid,
                f"left={auxiliary_capture_order} right={other_order}",
            )
            add_check(
                checks,
                "capture_order.jobs_equal_except_identity_output_and_order",
                normalized_jobs_match,
                "exact after normalization" if normalized_jobs_match else "normalized jobs differ",
            )
            add_check(
                checks,
                "capture_order.provenance_equal_except_config_hash",
                normalized_provenance_match,
                "exact after normalization" if normalized_provenance_match else "normalized provenance differs",
            )
        elif compare_mode == "vfx-reverse":
            normalized_jobs_match = normalized_vfx_reverse_job(job) == normalized_vfx_reverse_job(other_job)
            normalized_provenance_match = normalized_vfx_reverse_provenance(
                provenance
            ) == normalized_vfx_reverse_provenance(other.get("provenance", {}))
            add_check(
                checks,
                "vfx_reverse.jobs_equal_except_identity_output_and_role",
                normalized_jobs_match,
                "exact after normalization" if normalized_jobs_match else "normalized jobs differ",
            )
            add_check(
                checks,
                "vfx_reverse.provenance_equal_except_config_hash",
                normalized_provenance_match,
                "exact after normalization" if normalized_provenance_match else "normalized provenance differs",
            )
        elif compare_mode == "skeletal-reverse":
            normalized_jobs_match = normalized_vfx_reverse_job(job) == normalized_vfx_reverse_job(other_job)
            normalized_provenance_match = normalized_vfx_reverse_provenance(
                provenance
            ) == normalized_vfx_reverse_provenance(other.get("provenance", {}))
            add_check(
                checks,
                "skeletal_reverse.jobs_equal_except_identity_output_and_role",
                normalized_jobs_match,
                "exact after normalization" if normalized_jobs_match else "normalized jobs differ",
            )
            add_check(
                checks,
                "skeletal_reverse.provenance_equal_except_config_hash",
                normalized_provenance_match,
                "exact after normalization" if normalized_provenance_match else "normalized provenance differs",
            )
        elif compare_mode == "material-reverse":
            normalized_jobs_match = normalized_vfx_reverse_job(job) == normalized_vfx_reverse_job(other_job)
            normalized_provenance_match = normalized_vfx_reverse_provenance(
                provenance
            ) == normalized_vfx_reverse_provenance(other.get("provenance", {}))
            add_check(
                checks,
                "material_reverse.jobs_equal_except_identity_output_and_role",
                normalized_jobs_match,
                "exact after normalization" if normalized_jobs_match else "normalized jobs differ",
            )
            add_check(
                checks,
                "material_reverse.provenance_equal_except_config_hash",
                normalized_provenance_match,
                "exact after normalization" if normalized_provenance_match else "normalized provenance differs",
            )
        else:
            add_check(
                checks,
                "replay.provenance_exact",
                provenance == other.get("provenance", {}),
                "exact" if provenance == other.get("provenance", {}) else "provenance differs",
            )
        other_hashes = {
            (int(frame["logicalFrameId"]), modality): str(value).upper()
            for frame in other.get("frames", [])
            for modality, value in frame.get("sha1", {}).items()
        }
        all_keys = set(frame_hashes) | set(other_hashes)
        hash_mismatches = sum(frame_hashes.get(key) != other_hashes.get(key) for key in all_keys)
        add_check(
            checks,
            "replay.byte_exact_all_modalities",
            hash_mismatches == 0,
            f"left={len(frame_hashes)} right={len(other_hashes)} mismatches={hash_mismatches}",
            required=False,
        )
        if compare_mode == "capture-order":
            non_color_keys = {
                key for key in all_keys if key[1] not in COLOR_MODALITIES
            }
            non_color_hash_mismatches = sum(
                frame_hashes.get(key) != other_hashes.get(key)
                for key in non_color_keys
            )
            add_check(
                checks,
                "capture_order.non_color_modalities_byte_exact",
                bool(non_color_keys) and non_color_hash_mismatches == 0,
                f"modalities={len(non_color_keys)} mismatches={non_color_hash_mismatches}",
            )

        other_frames = {int(frame["logicalFrameId"]): frame for frame in other.get("frames", [])}
        add_check(checks, "replay.frame_ids", set(other_frames) == {int(frame["logicalFrameId"]) for frame in frames}, f"left={len(frames)} right={len(other_frames)}")
        left_frames = {int(frame["logicalFrameId"]): frame for frame in frames}
        if compare_mode == "vfx-reverse":
            for frame_id in sorted(set(left_frames) & set(other_frames)):
                left_frame = left_frames[frame_id]
                right_frame = other_frames[frame_id]
                left_state = niagara_fixture_state(left_frame)
                right_state = niagara_fixture_state(right_frame)
                add_check(
                    checks,
                    f"vfx_reverse.frame_{frame_id:06d}.fixture_particle_state_exact",
                    left_state is not None and left_state == right_state,
                    "exact" if left_state is not None and left_state == right_state else
                    f"left={json.dumps(left_state, sort_keys=True)} right={json.dumps(right_state, sort_keys=True)}",
                )
                add_check(
                    checks,
                    f"vfx_reverse.frame_{frame_id:06d}.scene_state_hash_exact",
                    left_frame.get("sceneStateSha1") == right_frame.get("sceneStateSha1"),
                    f"left={left_frame.get('sceneStateSha1')} right={right_frame.get('sceneStateSha1')}",
                    required=False,
                )
                scene_structure_fields = (
                    "sceneActorCount",
                    "sceneComponentCount",
                    "sceneSkeletalComponentCount",
                    "sceneBoneCount",
                    "sceneFXComponentCount",
                    "sceneNiagaraComponentCount",
                    "sceneNiagaraEmitterCount",
                    "sceneNiagaraCPUEmitterCount",
                    "sceneNiagaraGPUEmitterCount",
                    "sceneControllableActorCount",
                    "sceneUncontrolledTickingActorCount",
                )
                add_check(
                    checks,
                    f"vfx_reverse.frame_{frame_id:06d}.scene_state_structure_exact",
                    all(left_frame.get(field) == right_frame.get(field) for field in scene_structure_fields),
                    json.dumps(
                        {
                            field: [left_frame.get(field), right_frame.get(field)]
                            for field in scene_structure_fields
                        },
                        sort_keys=True,
                    ),
                )
        elif compare_mode == "skeletal-reverse":
            for frame_id in sorted(set(left_frames) & set(other_frames)):
                left_frame = left_frames[frame_id]
                right_frame = other_frames[frame_id]
                left_probes = nonfixture_project_probe_states(left_frame)
                right_probes = nonfixture_project_probe_states(right_frame)
                probe_keys = set(left_probes) | set(right_probes)
                pose_and_identity_exact = bool(probe_keys) and all(
                    key in left_probes
                    and key in right_probes
                    and left_probes[key].get("poseSha1")
                    == right_probes[key].get("poseSha1")
                    and left_probes[key].get("objectId")
                    == right_probes[key].get("objectId")
                    and left_probes[key].get("skinnedAssetPath")
                    == right_probes[key].get("skinnedAssetPath")
                    and left_probes[key].get("animationInstanceClass")
                    == right_probes[key].get("animationInstanceClass")
                    for key in probe_keys
                )
                add_check(
                    checks,
                    f"skeletal_reverse.frame_{frame_id:06d}.project_pose_and_identity_exact",
                    pose_and_identity_exact,
                    json.dumps(
                        {
                            key: [
                                left_probes.get(key, {}).get("poseSha1"),
                                right_probes.get(key, {}).get("poseSha1"),
                            ]
                            for key in sorted(probe_keys)
                        },
                        sort_keys=True,
                    ),
                )
                add_check(
                    checks,
                    f"skeletal_reverse.frame_{frame_id:06d}.camera_exact",
                    left_frame.get("camera") == right_frame.get("camera"),
                    "exact"
                    if left_frame.get("camera") == right_frame.get("camera")
                    else "camera metadata differs",
                )
                for side, root, side_frame in (
                    ("left", dataset, left_frame),
                    ("right", other_root, right_frame),
                ):
                    motion_evidence = False
                    visible_evidence = False
                    evidence_detail: dict[str, Any] = {}
                    try:
                        ids = np.rint(
                            image_rgba(
                                safe_dataset_file(
                                    root, side_frame.get("files", {}).get("object_id")
                                )
                            )[..., 0]
                        ).astype(np.int32)
                        motion = image_rgba(
                            safe_dataset_file(
                                root,
                                side_frame.get("files", {}).get(
                                    "motion_full_current_to_previous"
                                ),
                            )
                        )
                        motion_valid = (
                            image_rgba(
                                safe_dataset_file(
                                    root,
                                    side_frame.get("files", {}).get("motion_valid"),
                                )
                            )[..., 0]
                            == 1.0
                        )
                        magnitudes = np.linalg.norm(motion[..., :2], axis=-1)
                        for key, state in nonfixture_project_probe_states(
                            side_frame
                        ).items():
                            mask = ids == int(state.get("objectId", -1))
                            covered = mask & motion_valid
                            visible_count = int(np.count_nonzero(mask))
                            covered_count = int(np.count_nonzero(covered))
                            p95 = (
                                float(np.percentile(magnitudes[covered], 95.0))
                                if covered_count
                                else 0.0
                            )
                            visible_evidence = visible_evidence or visible_count >= 20
                            motion_evidence = motion_evidence or (
                                covered_count >= 20 and p95 > 0.05
                            )
                            evidence_detail[key] = {
                                "visiblePixels": visible_count,
                                "motionCoveredPixels": covered_count,
                                "motionP95DisplayPixels": p95,
                            }
                    except Exception as exc:
                        evidence_detail["error"] = str(exc)
                    add_check(
                        checks,
                        f"skeletal_reverse.frame_{frame_id:06d}.{side}_independent_motion_evidence",
                        visible_evidence
                        and (
                            side_frame.get("reset") is True or motion_evidence
                        ),
                        json.dumps(evidence_detail, sort_keys=True),
                    )
        elif compare_mode == "material-reverse":
            for frame_id in sorted(set(left_frames) & set(other_frames)):
                left_frame = left_frames[frame_id]
                right_frame = other_frames[frame_id]
                left_time = float(left_frame.get("materialTimeSeconds", math.nan))
                right_time = float(right_frame.get("materialTimeSeconds", math.nan))
                left_gpu_time = float(
                    left_frame.get("temporalDiagnostics", {}).get(
                        "renderGameTimeS", math.nan
                    )
                )
                right_gpu_time = float(
                    right_frame.get("temporalDiagnostics", {}).get(
                        "renderGameTimeS", math.nan
                    )
                )
                left_delta = float(
                    left_frame.get("materialDeltaTimeSeconds", math.nan)
                )
                right_delta = float(
                    right_frame.get("materialDeltaTimeSeconds", math.nan)
                )
                left_gpu_delta = float(
                    left_frame.get("temporalDiagnostics", {}).get(
                        "renderDeltaTimeS", math.nan
                    )
                )
                right_gpu_delta = float(
                    right_frame.get("temporalDiagnostics", {}).get(
                        "renderDeltaTimeS", math.nan
                    )
                )
                expected_time = (
                    frame_id * frame_rate_denominator / frame_rate_numerator
                    if valid_frame_rate
                    else math.nan
                )
                role_deltas = {
                    replay_role: left_delta,
                    other_role: right_delta,
                }
                time_contract_ok = bool(
                    valid_frame_rate
                    and all(
                        math.isfinite(value)
                        for value in (
                            left_time,
                            right_time,
                            left_gpu_time,
                            right_gpu_time,
                            left_delta,
                            right_delta,
                            left_gpu_delta,
                            right_gpu_delta,
                        )
                    )
                    and all(
                        math.isclose(
                            value, expected_time, rel_tol=0.0, abs_tol=1e-6
                        )
                        for value in (
                            left_time,
                            right_time,
                            left_gpu_time,
                            right_gpu_time,
                        )
                    )
                    and math.isclose(
                        left_delta, left_gpu_delta, rel_tol=0.0, abs_tol=1e-6
                    )
                    and math.isclose(
                        right_delta, right_gpu_delta, rel_tol=0.0, abs_tol=1e-6
                    )
                    and role_deltas.get("FrameGenerationEndpoints", 0.0) > 0.0
                    and role_deltas.get("FrameGenerationReverseEndpoints", 0.0) < 0.0
                )
                add_check(
                    checks,
                    f"material_reverse.frame_{frame_id:06d}.gpu_logical_game_time_and_signed_direction",
                    time_contract_ok,
                    json.dumps(
                        {
                            "expectedTime": expected_time,
                            "leftTime": left_time,
                            "rightTime": right_time,
                            "leftGpuTime": left_gpu_time,
                            "rightGpuTime": right_gpu_time,
                            "leftDelta": left_delta,
                            "rightDelta": right_delta,
                            "leftGpuDelta": left_gpu_delta,
                            "rightGpuDelta": right_gpu_delta,
                            "roleDeltas": role_deltas,
                        },
                        sort_keys=True,
                    ),
                )
                project_material_probe = bool(
                    job.get("bValidateProjectAnimatedMaterial", False)
                )
                left_material_state = left_frame.get(
                    "projectAnimatedMaterialValidation", {}
                )
                right_material_state = right_frame.get(
                    "projectAnimatedMaterialValidation", {}
                )
                material_identity_exact = bool(
                    isinstance(left_material_state, dict)
                    and isinstance(right_material_state, dict)
                    and left_material_state.get("enabled") is True
                    and right_material_state.get("enabled") is True
                    and left_material_state.get("materialInterfacePath")
                    == right_material_state.get("materialInterfacePath")
                    and left_material_state.get("baseMaterialPath")
                    == right_material_state.get("baseMaterialPath")
                    and int(left_material_state.get("receiverObjectId", -1)) == 150
                    and int(right_material_state.get("receiverObjectId", -1)) == 150
                )
                add_check(
                    checks,
                    f"material_reverse.frame_{frame_id:06d}.project_material_identity_exact",
                    material_identity_exact,
                    json.dumps(
                        {
                            "left": left_material_state,
                            "right": right_material_state,
                        },
                        sort_keys=True,
                    ),
                    required=project_material_probe,
                )
                receiver_metrics: dict[str, Any] = {"valid": False}
                receiver_pixels = 0
                try:
                    left_ids = np.rint(
                        image_rgba(
                            safe_dataset_file(
                                dataset,
                                left_frame.get("files", {}).get("object_id"),
                            )
                        )[..., 0]
                    ).astype(np.int32)
                    right_ids = np.rint(
                        image_rgba(
                            safe_dataset_file(
                                other_root,
                                right_frame.get("files", {}).get("object_id"),
                            )
                        )[..., 0]
                    ).astype(np.int32)
                    left_color = image_rgba(
                        safe_dataset_file(
                            dataset,
                            left_frame.get("files", {}).get(
                                "color_lr_scene_hdr"
                            ),
                        )
                    )
                    right_color = image_rgba(
                        safe_dataset_file(
                            other_root,
                            right_frame.get("files", {}).get(
                                "color_lr_scene_hdr"
                            ),
                        )
                    )
                    receiver_mask = (left_ids == 150) & (right_ids == 150)
                    receiver_pixels = int(np.count_nonzero(receiver_mask))
                    receiver_metrics = numeric_comparison(
                        left_color[..., :3][receiver_mask],
                        right_color[..., :3][receiver_mask],
                    )
                except Exception as exc:
                    receiver_metrics = {"valid": False, "error": str(exc)}
                add_check(
                    checks,
                    f"material_reverse.frame_{frame_id:06d}.project_material_current_color_exact",
                    receiver_pixels >= 20
                    and bool(receiver_metrics.get("valid"))
                    and float(receiver_metrics.get("maxAbs", math.inf)) <= 0.025
                    and float(receiver_metrics.get("meanAbs", math.inf)) <= 0.0012
                    and float(receiver_metrics.get("p99Abs", math.inf)) <= 0.015,
                    f"pixels={receiver_pixels} metrics={json.dumps(receiver_metrics, sort_keys=True)}",
                    required=project_material_probe,
                )
        replay_metrics: dict[str, dict[str, Any]] = {}
        heatmap_root = dataset / "validation_heatmaps"
        if heatmap_root.is_dir():
            for stale_heatmap in heatmap_root.glob("frame_*.png"):
                stale_heatmap.unlink()
        vfx_reverse_visible_probe_count = 0
        skeletal_reverse_static_modality_count = 0
        material_reverse_static_modality_count = 0
        for key in sorted(all_keys):
            frame_id, modality = key
            left_path = frame_paths.get(key)
            other_frame = other_frames.get(frame_id, {})
            other_relative = other_frame.get("files", {}).get(modality)
            try:
                right_path = safe_dataset_file(other_root, other_relative)
            except Exception:
                right_path = None
            present = left_path is not None and right_path is not None and right_path.is_file()
            add_check(checks, f"replay.frame_{frame_id:06d}.{modality}.present", present, f"left={left_path} right={right_path}")
            if not present:
                continue

            if compare_mode == "vfx-reverse":
                if (
                    modality != "translucency_after_dof_raw"
                    or other_frame.get("semanticValidationFixture", {}).get(
                        "niagaraVisibleProbeExpected"
                    )
                    is not True
                ):
                    continue
                left_pixels = image_rgba(left_path)
                right_pixels = image_rgba(right_path)
                fixture = other_frame["semanticValidationFixture"]
                display_size = np.asarray(
                    other_frame.get("temporalDiagnostics", {}).get(
                        "displaySize",
                        other_frame.get("temporalDiagnostics", {}).get(
                            "displayResolution", (0, 0)
                        ),
                    ),
                    dtype=np.float64,
                )
                anchor = np.asarray(
                    fixture.get("niagaraAnchorDisplayPixels", (math.nan, math.nan)),
                    dtype=np.float64,
                )
                radius = float(
                    fixture.get("niagaraValidationRadiusDisplayPixels", math.nan)
                )
                valid_roi = (
                    left_pixels.shape == right_pixels.shape
                    and left_pixels.ndim == 3
                    and display_size.shape == (2,)
                    and np.all(display_size > 0)
                    and anchor.shape == (2,)
                    and np.all(np.isfinite(anchor))
                    and math.isfinite(radius)
                    and radius > 0
                )
                if valid_roi:
                    height, width = left_pixels.shape[:2]
                    scale = np.asarray((width, height), dtype=np.float64) / display_size
                    center = anchor * scale
                    radius_render = radius * scale[0]
                    yy, xx = np.ogrid[:height, :width]
                    roi = (xx - center[0]) ** 2 + (yy - center[1]) ** 2 <= radius_render**2
                    left_roi = left_pixels[roi]
                    right_roi = right_pixels[roi]
                    metrics = numeric_comparison(left_roi, right_roi)
                    visible_left = int(np.count_nonzero(np.max(np.abs(left_roi[:, :3]), axis=1) > 0.01))
                    visible_right = int(np.count_nonzero(np.max(np.abs(right_roi[:, :3]), axis=1) > 0.01))
                else:
                    metrics = {"valid": False, "reason": "invalid ROI metadata or shape"}
                    visible_left = 0
                    visible_right = 0
                replay_metrics[f"vfx_reverse.{frame_id:06d}.{modality}.roi"] = metrics
                add_check(
                    checks,
                    f"vfx_reverse.frame_{frame_id:06d}.visible_translucency_roi",
                    bool(metrics.get("valid"))
                    and visible_left >= 8
                    and visible_right >= 8
                    and float(metrics.get("maxAbs", math.inf)) <= 1e-3,
                    f"leftVisible={visible_left} rightVisible={visible_right} metrics={json.dumps(metrics, sort_keys=True)}",
                )
                vfx_reverse_visible_probe_count += 1
                continue

            if compare_mode == "skeletal-reverse":
                if modality not in {
                    "depth",
                    "object_id",
                    "depth_device_raw",
                    "depth_view_linear_meters",
                    "depth_valid",
                }:
                    continue
                left_pixels = image_rgba(left_path)
                right_pixels = image_rgba(right_path)
                metrics = numeric_comparison(left_pixels, right_pixels)
                replay_metrics[
                    f"skeletal_reverse.{frame_id:06d}.{modality}"
                ] = metrics
                changed_fraction = float(
                    metrics.get("changedPixelFraction", math.inf)
                )
                if modality == "object_id":
                    static_grid_within_tolerance = (
                        bool(metrics.get("valid"))
                        and float(metrics.get("maxAbs", math.inf)) <= 1e-6
                    )
                elif modality == "depth_device_raw":
                    static_grid_within_tolerance = (
                        bool(metrics.get("valid"))
                        and changed_fraction <= 5e-4
                        and float(metrics.get("meanAbs", math.inf)) <= 1e-5
                        and float(metrics.get("p99Abs", math.inf)) <= 1e-5
                    )
                elif modality == "depth_view_linear_meters":
                    static_grid_within_tolerance = (
                        bool(metrics.get("valid"))
                        and changed_fraction <= 5e-4
                        and float(metrics.get("meanAbs", math.inf)) <= 5e-3
                        and float(metrics.get("p99Abs", math.inf)) <= 1e-4
                    )
                elif modality == "depth_valid":
                    static_grid_within_tolerance = (
                        bool(metrics.get("valid"))
                        and changed_fraction <= 5e-4
                    )
                else:
                    static_grid_within_tolerance = (
                        bool(metrics.get("valid"))
                        and changed_fraction <= 5e-4
                        and float(metrics.get("p99Abs", math.inf)) <= 1e-3
                    )
                add_check(
                    checks,
                    f"skeletal_reverse.frame_{frame_id:06d}.{modality}.static_grid_tolerance",
                    static_grid_within_tolerance,
                    json.dumps(metrics, sort_keys=True),
                )
                skeletal_reverse_static_modality_count += 1
                continue

            if compare_mode == "material-reverse":
                if modality not in {
                    "object_id",
                    "ui_color_alpha",
                }:
                    continue
                material_reverse_static_modality_count += 1

            byte_exact = frame_hashes.get(key) == other_hashes.get(key)
            add_check(checks, f"replay.frame_{frame_id:06d}.{modality}.byte_exact", byte_exact, str(byte_exact), required=False)
            left_pixels = image_rgba(left_path)
            right_pixels = image_rgba(right_path)
            metrics = numeric_comparison(left_pixels, right_pixels)
            replay_metrics[f"{frame_id:06d}.{modality}"] = metrics
            if not byte_exact and metrics.get("shapeMatch"):
                write_heatmap(left_pixels, right_pixels, heatmap_root / f"frame_{frame_id:06d}_{modality}.png")

            if modality in COLOR_MODALITIES:
                if modality == "color_lr_scene_hdr":
                    within_tolerance = (
                        bool(metrics.get("valid"))
                        and float(metrics.get("psnrDb", -math.inf)) >= 45.0
                        and float(metrics.get("meanAbs", math.inf)) <= 0.002
                        and float(metrics.get("p99Abs", math.inf)) <= 0.02
                    )
                else:
                    within_tolerance = (
                        bool(metrics.get("valid"))
                        and float(metrics.get("psnrDb", -math.inf)) >= 45.0
                        and float(metrics.get("meanAbs", math.inf)) <= 2.0 / 255.0
                    )
            else:
                within_tolerance = bool(metrics.get("valid")) and float(metrics.get("maxAbs", math.inf)) <= 1e-6
            add_check(
                checks,
                f"replay.frame_{frame_id:06d}.{modality}.numeric_tolerance",
                within_tolerance,
                json.dumps(metrics, sort_keys=True),
            )

        if compare_mode == "vfx-reverse":
            add_check(
                checks,
                "vfx_reverse.visible_probe_compared",
                vfx_reverse_visible_probe_count >= 1,
                f"count={vfx_reverse_visible_probe_count}",
            )
        elif compare_mode == "skeletal-reverse":
            add_check(
                checks,
                "skeletal_reverse.static_grids_compared",
                skeletal_reverse_static_modality_count
                >= 5 * len(set(left_frames) & set(other_frames)),
                f"count={skeletal_reverse_static_modality_count}",
            )
        elif compare_mode == "material-reverse":
            add_check(
                checks,
                "material_reverse.static_identity_grids_compared",
                material_reverse_static_modality_count
                >= len(set(left_frames) & set(other_frames)),
                f"count={material_reverse_static_modality_count}",
            )

        for frame_id, right_frame in other_frames.items():
            left_frame = left_frames.get(frame_id)
            if left_frame is None:
                continue
            for field in REPLAY_METADATA_FIELDS:
                if compare_mode in ("vfx-reverse", "skeletal-reverse", "material-reverse"):
                    continue
                if compare_mode == "capture-order" and field in {
                    "renderSubmissions",
                    "auxiliaryCaptureOrder",
                }:
                    continue
                left_value = left_frame.get(field)
                right_value = right_frame.get(field)
                add_check(
                    checks,
                    f"replay.frame_{frame_id:06d}.{field}_exact",
                    left_value == right_value,
                    "exact" if left_value == right_value else "metadata differs",
                )
    else:
        replay_metrics = {}

    required_checks = [check for check in checks if check["required"]]
    passed = all(check["passed"] for check in required_checks)
    report = {
        "validatorVersion": 9,
        "comparisonMode": compare_mode if compare is not None else "none",
        "captureOrderInvarianceGate": (
            "pass" if compare is not None and compare_mode == "capture-order" and passed
            else "fail" if compare is not None and compare_mode == "capture-order"
            else "not_run"
        ),
        "vfxReverseReplayGate": (
            "pass" if compare is not None and compare_mode == "vfx-reverse" and passed
            else "fail" if compare is not None and compare_mode == "vfx-reverse"
            else "not_run"
        ),
        "skeletalReverseReplayGate": (
            "pass" if compare is not None and compare_mode == "skeletal-reverse" and passed
            else "fail" if compare is not None and compare_mode == "skeletal-reverse"
            else "not_run"
        ),
        "materialReverseReplayGate": (
            "pass" if compare is not None and compare_mode == "material-reverse" and passed
            else "fail" if compare is not None and compare_mode == "material-reverse"
            else "not_run"
        ),
        "dataset": str(dataset.resolve()),
        "contractVersion": manifest.get("contractVersion"),
        "certificationStatus": manifest.get("certificationStatus"),
        "temporalTrainingCertified": bool(manifest.get("temporalTrainingCertified", False)),
        "formatAndIntegrityGate": "pass" if passed else "fail",
        "checksPassed": sum(check["passed"] for check in required_checks),
        "checksTotal": len(required_checks),
        "informationalChecks": len(checks) - len(required_checks),
        "checks": checks,
        "statistics": stats,
        "replayMetrics": replay_metrics,
        "note": "This gate validates buffer integrity, replay-role isolation, motion time-span metadata, endpoint skeletal-bone override coverage, matrix/jitter consistency, reversed-Z/view-position reconstruction and tolerance-based replay. The semantic fixture additionally validates rigid, pure-skinning and explicit PreviousFrameSwitch WPO motion, 1/10/100 m depth, transparency, disocclusion, finalized CPU Niagara particle counts and a visible AfterDOF VFX probe. The non-fixture skeletal gate requires a visible project-authored Actor and AnimBP, exact application of the shared cached pose and covered endpoint motion; skeletal-reverse requires exact absolute project poses, camera metadata and object-ID grids, tightly bounded depth-grid differences, and independent directional motion evidence. GPU particle payload readback, arbitrary Niagara data interfaces, non-fixture animated-material/WPO motion, production disocclusion and Main View/reference-HR pixel equivalence remain separate gates.",
    }
    return report, passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path, help="Dataset output directory containing manifest.json")
    parser.add_argument("--compare", type=Path, help="Second dataset directory or manifest for deterministic hash comparison")
    parser.add_argument(
        "--compare-mode",
        choices=("exact-replay", "capture-order", "vfx-reverse", "skeletal-reverse", "material-reverse"),
        default="exact-replay",
        help="Comparison contract. Reverse modes compare forward/reverse endpoint roles at identical absolute times.",
    )
    parser.add_argument("--report", type=Path, help="Output report path (default: <dataset>/validation_report.json)")
    args = parser.parse_args()

    if args.compare_mode in (
        "capture-order",
        "vfx-reverse",
        "skeletal-reverse",
        "material-reverse",
    ) and args.compare is None:
        parser.error(f"--compare-mode {args.compare_mode} requires --compare")

    report, passed = validate(args.dataset, args.compare, args.compare_mode)
    report_path = args.report or (args.dataset / "validation_report.json")
    temporary = report_path.with_suffix(report_path.suffix + ".part")
    temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(report_path)
    print(f"{report['formatAndIntegrityGate'].upper()}: {report['checksPassed']}/{report['checksTotal']} checks; report={report_path}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
