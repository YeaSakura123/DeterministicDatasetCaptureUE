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
HR_TEMPORAL_MODALITIES = {
    "color_hr_native_scene_hdr",
    "color_hr_reference_scene_hdr",
    "color_main_view_hudless_after_tonemap",
}
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
    "replayPass",
    "previousCapturedLogicalFrameId",
    "motionPreviousLogicalFrameId",
    "motionTimeSpanFrames",
    "motionTimeSpanS",
    "motionTrainingUsable",
    "endpointPreviousTransformOverride",
    "endpointPreviousSkeletalBoneOverride",
    "endpointPreviousSkeletalBoneComponentCount",
    "endpointPreviousSkeletalBoneCount",
    "endpointPreviousSkeletalBoneSkippedComponents",
    "auxiliaryCaptureOrder",
    "logicalEvaluationDirection",
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
    mse = float(np.mean(np.square(rgb_difference)))
    peak = max(float(np.max(np.abs(left[..., :3]))), float(np.max(np.abs(right[..., :3]))), 1.0)
    return {
        "valid": bool(np.isfinite(difference).all()),
        "shapeMatch": True,
        "meanAbs": float(np.mean(rgb_difference)),
        "p99Abs": float(np.quantile(rgb_difference, 0.99)),
        "maxAbs": float(np.max(rgb_difference)),
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
        if not frame.get("reset", False) or intermediate_replay:
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
    provenance = manifest.get("provenance", {})
    add_check(checks, "provenance_present", isinstance(provenance, dict) and bool(provenance), "present")
    for name in ("captureConfigSha1", "cvarProfileSha1", "contentMapSha1", "shaderSourceSha1"):
        value = str(provenance.get(name, ""))
        valid_hash = len(value) == 40 and all(character in "0123456789ABCDEFabcdef" for character in value)
        add_check(checks, f"provenance.{name}", valid_hash, value)
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
            "player_main_view" if modality == "main_view_temporal" else f"{modality}_scene_capture"
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
            and 0 <= controllable_count <= actor_count
            and 0 <= uncontrolled_ticking_count <= actor_count
            and isinstance(controllable_actors, list)
            and len(controllable_actors) == controllable_count
            and len(set(controllable_actors)) == controllable_count
            and isinstance(uncontrolled_ticking_actors, list)
            and len(uncontrolled_ticking_actors) == uncontrolled_ticking_count
            and len(set(uncontrolled_ticking_actors)) == uncontrolled_ticking_count
            and frame.get("sceneStateHashScope")
            == "sorted_actor_component_transforms_visibility_tick_controllable_skeletal_component_space_bones_niagara_component_state_cascade_component_state_not_particle_payload",
            json.dumps(
                {
                    "sha1": scene_state_hash,
                    "actors": actor_count,
                    "components": component_count,
                    "skeletalComponents": skeletal_count,
                    "bones": bone_count,
                    "fxComponents": fx_count,
                    "niagaraComponents": niagara_count,
                    "controllableActors": controllable_count,
                    "uncontrolledTickingActors": uncontrolled_ticking_count,
                    "uncontrolledTickingActorPaths": uncontrolled_ticking_actors,
                },
                sort_keys=True,
            ),
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
                expected_size = hr_size if modality == "hr" else lr_size
                add_check(checks, f"frame_{frame_id:06d}.{modality}.size", size == expected_size, f"{size}")
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
                for name in ("preExposure", "exposure", "renderDeltaTimeS"):
                    value = temporal.get(name)
                    valid = isinstance(value, (int, float)) and math.isfinite(value) and value > 0
                    add_check(checks, f"frame_{frame_id:06d}.{name}.positive", valid, str(value))

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

    add_check(
        checks,
        "capture_order.submission_ids_global",
        not all_render_submission_ids
        or all_render_submission_ids == list(range(len(all_render_submission_ids))),
        f"count={len(all_render_submission_ids)} first={all_render_submission_ids[:4]} last={all_render_submission_ids[-4:]}",
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
    for index in range(1, len(semantic_records)):
        frame_id, fixture, current_pixels = semantic_records[index]
        _, previous_fixture, previous_pixels = semantic_records[index - 1]
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

    if compare is not None:
        other_manifest_path = compare / "manifest.json" if compare.is_dir() else compare
        other_root = (compare if compare.is_dir() else compare.parent).resolve()
        other = json.loads(other_manifest_path.read_text(encoding="utf-8"))
        other_job = other.get("job", {})
        other_role = str(other.get("replayPass") or other.get("job", {}).get("replayPass") or "Standard")
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
        replay_metrics: dict[str, dict[str, Any]] = {}
        heatmap_root = dataset / "validation_heatmaps"
        if heatmap_root.is_dir():
            for stale_heatmap in heatmap_root.glob("frame_*.png"):
                stale_heatmap.unlink()
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

        left_frames = {int(frame["logicalFrameId"]): frame for frame in frames}
        for frame_id, right_frame in other_frames.items():
            left_frame = left_frames.get(frame_id)
            if left_frame is None:
                continue
            for field in REPLAY_METADATA_FIELDS:
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
        "validatorVersion": 5,
        "comparisonMode": compare_mode if compare is not None else "none",
        "captureOrderInvarianceGate": (
            "pass" if compare is not None and compare_mode == "capture-order" and passed
            else "fail" if compare is not None and compare_mode == "capture-order"
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
        "note": "This gate validates buffer integrity, replay-role isolation, motion time-span metadata, endpoint skeletal-bone override coverage, matrix/jitter consistency, reversed-Z/view-position reconstruction and tolerance-based replay. When the semantic fixture is enabled it also validates rigid, pure-skinning and explicit PreviousFrameSwitch WPO motion direction/magnitude, 1/10/100 m depth, nonzero After-DOF transparency, known occlusion/disocclusion geometry, and the experimental history-rejection signal. Exact replay requires matching provenance and metadata. Capture-order comparison requires opposite auxiliary submission orders, identical normalized jobs/provenance, exact non-color buffers and tolerance-gated color. Non-fixture animated-material/WPO motion, production disocclusion for unlabeled/deforming geometry and Main View/reference-HR pixel equivalence remain separate gates.",
    }
    return report, passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path, help="Dataset output directory containing manifest.json")
    parser.add_argument("--compare", type=Path, help="Second dataset directory or manifest for deterministic hash comparison")
    parser.add_argument(
        "--compare-mode",
        choices=("exact-replay", "capture-order"),
        default="exact-replay",
        help="Comparison contract. capture-order allows only job identity/output/order and captureConfigSha1 to differ.",
    )
    parser.add_argument("--report", type=Path, help="Output report path (default: <dataset>/validation_report.json)")
    args = parser.parse_args()

    if args.compare_mode == "capture-order" and args.compare is None:
        parser.error("--compare-mode capture-order requires --compare")

    report, passed = validate(args.dataset, args.compare, args.compare_mode)
    report_path = args.report or (args.dataset / "validation_report.json")
    temporary = report_path.with_suffix(report_path.suffix + ".part")
    temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(report_path)
    print(f"{report['formatAndIntegrityGate'].upper()}: {report['checksPassed']}/{report['checksTotal']} checks; report={report_path}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
