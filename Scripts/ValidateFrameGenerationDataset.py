#!/usr/bin/env python3
"""Validate an assembled nr-fg-data-v1 dataset without claiming missing certification gates."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any

try:
    import numpy as np
    import OpenEXR
except ImportError as exc:
    raise SystemExit(
        "Validation dependencies are missing. Run: "
        "python -m pip install -r Scripts/requirements-validation.txt"
    ) from exc


REQUIRED_MODALITIES = {
    "scene_color_hudless_t0",
    "scene_color_hudless_tau",
    "scene_color_hudless_t1",
    "depth_t0",
    "depth_tau",
    "depth_t1",
    "motion_1_to_0",
    "motion_valid_1_to_0",
    "object_id_t0",
    "object_id_tau",
    "object_id_t1",
    "reactive_mask_t0",
    "reactive_mask_tau",
    "reactive_mask_t1",
    "transparency_mask_t0",
    "transparency_mask_tau",
    "transparency_mask_t1",
}

DISPLAY_MODALITIES = {
    "scene_color_hudless_t0",
    "scene_color_hudless_tau",
    "scene_color_hudless_t1",
}

MASK_MODALITIES = {
    "motion_valid_1_to_0",
    "reactive_mask_t0",
    "reactive_mask_tau",
    "reactive_mask_t1",
    "transparency_mask_t0",
    "transparency_mask_tau",
    "transparency_mask_t1",
}

OBJECT_ID_MODALITIES = {"object_id_t0", "object_id_tau", "object_id_t1"}

REQUIRED_UNCERTIFIED_GAPS = {
    "motion_0_to_1_independently_captured",
    "ui_color_alpha_t0_tau_t1",
    "skeletal_bone_endpoint_motion_validation",
    "wpo_and_animated_material_endpoint_motion_validation",
    "production_disocclusion_masks",
    "hudless_in_world_ui_residue_validation",
}

INTENTIONAL_CVAR_VALUES = {
    "r.MotionVectorSimulation": {"endpoints": "1", "intermediate": "0"}
}


def sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def canonical_sha1(value: Any) -> str:
    encoded = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha1(encoded).hexdigest().upper()


def add_check(
    checks: list[dict[str, Any]], name: str, passed: bool, detail: str
) -> None:
    checks.append({"name": name, "passed": bool(passed), "required": True, "detail": detail})


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


def finite_matrix(values: Any) -> bool:
    array = np.asarray(values, dtype=np.float64)
    return bool(array.shape == (16,) and np.isfinite(array).all())


def validate_camera(camera: Any) -> tuple[bool, str]:
    if not isinstance(camera, dict):
        return False, "camera record is not an object"
    matrix_names = (
        "viewToClip",
        "translatedWorldToView",
        "viewToTranslatedWorld",
        "translatedWorldToClip",
    )
    bad_matrices = [name for name in matrix_names if not finite_matrix(camera.get(name))]
    vector_names = ("worldViewOriginHigh", "worldViewOriginLow")
    bad_vectors = [
        name
        for name in vector_names
        if np.asarray(camera.get(name), dtype=np.float64).shape != (3,)
        or not np.isfinite(np.asarray(camera.get(name), dtype=np.float64)).all()
    ]
    conventions_ok = (
        camera.get("matrixLayout") == "row_major_flat_16"
        and camera.get("matrixVectorConvention") == "row_vector_mul_matrix"
        and camera.get("coordinateSystem") == "Unreal_left_handed_reversed_z"
        and camera.get("clipZRange") == "zero_to_one_reversed_z"
        and camera.get("reversedZ") is True
        and camera.get("infiniteFar") is True
        and math.isfinite(float(camera.get("nearPlane", math.nan)))
        and float(camera.get("nearPlane", 0.0)) > 0.0
    )
    valid = not bad_matrices and not bad_vectors and conventions_ok
    return valid, f"badMatrices={bad_matrices} badVectors={bad_vectors} conventions={conventions_ok}"


def validate_provenance(checks: list[dict[str, Any]], provenance: Any) -> None:
    if not isinstance(provenance, dict):
        add_check(checks, "provenance.object", False, "missing provenance object")
        return
    endpoint = provenance.get("endpointReplay", {})
    intermediate = provenance.get("intermediateReplay", {})
    endpoint_cvars = endpoint.get("cvars", {}) if isinstance(endpoint, dict) else {}
    intermediate_cvars = intermediate.get("cvars", {}) if isinstance(intermediate, dict) else {}
    add_check(
        checks,
        "provenance.source_objects",
        isinstance(endpoint_cvars, dict) and bool(endpoint_cvars)
        and isinstance(intermediate_cvars, dict) and bool(intermediate_cvars),
        f"endpointCVars={len(endpoint_cvars)} intermediateCVars={len(intermediate_cvars)}",
    )
    if not isinstance(endpoint_cvars, dict) or not isinstance(intermediate_cvars, dict):
        return

    unexpected = [
        name
        for name in sorted(set(endpoint_cvars) | set(intermediate_cvars))
        if name not in INTENTIONAL_CVAR_VALUES
        and endpoint_cvars.get(name) != intermediate_cvars.get(name)
    ]
    add_check(
        checks,
        "provenance.only_intentional_cvar_difference",
        not unexpected,
        f"unexpected={unexpected}",
    )
    expected_values_ok = all(
        str(endpoint_cvars.get(name)) == values["endpoints"]
        and str(intermediate_cvars.get(name)) == values["intermediate"]
        for name, values in INTENTIONAL_CVAR_VALUES.items()
    )
    add_check(
        checks,
        "provenance.intentional_cvar_values",
        expected_values_ok,
        json.dumps(
            {
                name: {
                    "endpoints": endpoint_cvars.get(name),
                    "intermediate": intermediate_cvars.get(name),
                }
                for name in INTENTIONAL_CVAR_VALUES
            },
            sort_keys=True,
        ),
    )
    endpoint_normalized = {
        name: endpoint_cvars[name]
        for name in sorted(endpoint_cvars)
        if name not in INTENTIONAL_CVAR_VALUES
    }
    intermediate_normalized = {
        name: intermediate_cvars[name]
        for name in sorted(intermediate_cvars)
        if name not in INTENTIONAL_CVAR_VALUES
    }
    normalized_hash = canonical_sha1(endpoint_normalized)
    add_check(
        checks,
        "provenance.normalized_cvar_profiles",
        endpoint_normalized == intermediate_normalized
        and provenance.get("normalizedCVarProfileSha1") == normalized_hash,
        f"computed={normalized_hash} declared={provenance.get('normalizedCVarProfileSha1')}",
    )

    invariant_fields = (
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
    mismatches = [name for name in invariant_fields if endpoint.get(name) != intermediate.get(name)]
    add_check(
        checks,
        "provenance.render_environment_exact",
        not mismatches,
        f"mismatches={mismatches}",
    )


def validate(dataset: Path) -> tuple[dict[str, Any], bool]:
    dataset = dataset.resolve()
    manifest_path = dataset / "manifest.json"
    if not manifest_path.is_file():
        raise ValueError(f"manifest.json not found: {dataset}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    checks: list[dict[str, Any]] = []

    add_check(checks, "contract.version", manifest.get("contractVersion") == "nr-fg-data-v1", str(manifest.get("contractVersion")))
    add_check(checks, "contract.schema", manifest.get("schemaVersion") == 1, str(manifest.get("schemaVersion")))
    declared_gaps = set(manifest.get("missingRequirements", []))
    add_check(
        checks,
        "contract.honest_uncertified_state",
        manifest.get("certificationStatus") == "experimental_uncertified"
        and manifest.get("frameGenerationCertified") is False
        and REQUIRED_UNCERTIFIED_GAPS <= declared_gaps,
        f"status={manifest.get('certificationStatus')} certified={manifest.get('frameGenerationCertified')} gaps={sorted(declared_gaps)}",
    )
    add_check(checks, "contract.tau", math.isclose(float(manifest.get("tau", math.nan)), 0.5, abs_tol=1e-12), str(manifest.get("tau")))
    add_check(checks, "contract.motion_blur_off", manifest.get("motionBlur") == "off", str(manifest.get("motionBlur")))
    add_check(checks, "contract.ui_not_fabricated", manifest.get("uiSeparated") is False, str(manifest.get("uiSeparated")))

    render_size = tuple(int(value) for value in manifest.get("renderSize", ()))
    display_size = tuple(int(value) for value in manifest.get("displaySize", ()))
    valid_sizes = (
        len(render_size) == 2
        and len(display_size) == 2
        and min(*render_size, *display_size) > 0
        and display_size[0] >= render_size[0]
        and display_size[1] >= render_size[1]
    )
    add_check(checks, "contract.resolutions", valid_sizes, f"render={render_size} display={display_size}")

    source_replays = manifest.get("sourceReplays", {})
    for role in ("endpointValidation", "intermediateValidation"):
        validation = source_replays.get(role, {}) if isinstance(source_replays, dict) else {}
        passed = int(validation.get("checksPassed", -1))
        total = int(validation.get("checksTotal", -2))
        add_check(
            checks,
            f"sources.{role}",
            total > 0 and passed == total,
            f"passed={passed} total={total} validator={validation.get('validatorVersion')}",
        )

    validate_provenance(checks, manifest.get("provenance"))

    semantics = manifest.get("bufferSemantics", {})
    motion_semantics = semantics.get("motion1To0", {}) if isinstance(semantics, dict) else {}
    color_semantics = semantics.get("sceneColorHudless", {}) if isinstance(semantics, dict) else {}
    depth_semantics = semantics.get("depth", {}) if isinstance(semantics, dict) else {}
    add_check(
        checks,
        "semantics.motion",
        motion_semantics.get("definition") == "previous_pixel = current_pixel + motion_1_to_0"
        and motion_semantics.get("unit") == "display_pixel"
        and motion_semantics.get("origin") == "top_left"
        and motion_semantics.get("jitterRemoved") is True,
        json.dumps(motion_semantics, sort_keys=True),
    )
    add_check(
        checks,
        "semantics.hudless_color",
        color_semantics.get("pipelineStage") == "after_tonemap_before_ui"
        and color_semantics.get("uiIncluded") is False
        and color_semantics.get("hudIncluded") is False
        and color_semantics.get("resolution") == "display",
        json.dumps(color_semantics, sort_keys=True),
    )
    add_check(
        checks,
        "semantics.depth",
        depth_semantics.get("encoding") == "linear_view_meters"
        and depth_semantics.get("resolution") == "render",
        json.dumps(depth_semantics, sort_keys=True),
    )

    pairs = manifest.get("pairs", [])
    add_check(checks, "pairs.nonempty", isinstance(pairs, list) and bool(pairs), f"count={len(pairs) if isinstance(pairs, list) else -1}")
    seen_ids: set[int] = set()
    seen_paths: set[str] = set()
    statistics: dict[str, Any] = {}
    if isinstance(pairs, list):
        for index, pair in enumerate(pairs):
            prefix = f"pair_{index:06d}"
            pair_id = int(pair.get("pairId", -1))
            add_check(checks, f"{prefix}.id", pair_id == index and pair_id not in seen_ids, f"pairId={pair_id}")
            seen_ids.add(pair_id)
            t0 = int(pair.get("t0LogicalFrameId", -1))
            tau_frame = int(pair.get("tauLogicalFrameId", -1))
            t1 = int(pair.get("t1LogicalFrameId", -1))
            tau = float(pair.get("tau", math.nan))
            add_check(
                checks,
                f"{prefix}.timeline",
                t0 < tau_frame < t1
                and math.isclose((tau_frame - t0) / (t1 - t0), tau, abs_tol=1e-12)
                and math.isclose(tau, 0.5, abs_tol=1e-12),
                f"t0={t0} tauFrame={tau_frame} t1={t1} tau={tau}",
            )
            delta_time = float(pair.get("deltaTimeS", math.nan))
            endpoint_state = pair.get("endpointPreviousState", {})
            time_span = float(endpoint_state.get("timeSpanS", math.nan))
            add_check(
                checks,
                f"{prefix}.endpoint_history_isolation",
                pair.get("intermediateHistoryIsolated") is True
                and int(endpoint_state.get("t1PreviousLogicalFrameId", -1)) == t0
                and endpoint_state.get("componentTransformOverride") is True
                and delta_time > 0.0
                and math.isclose(time_span, delta_time, rel_tol=1e-6, abs_tol=1e-7),
                f"previous={endpoint_state.get('t1PreviousLogicalFrameId')} delta={delta_time} span={time_span}",
            )

            for camera_name in ("cameraT0Unjittered", "cameraT1Unjittered"):
                camera_valid, camera_detail = validate_camera(pair.get(camera_name))
                add_check(checks, f"{prefix}.{camera_name}", camera_valid, camera_detail)

            exposure = pair.get("exposure", {})
            pre_exposure = pair.get("preExposure", {})
            exposure_ok = True
            exposure_detail: dict[str, Any] = {}
            for phase in ("t0", "tau", "t1"):
                value = float(exposure.get(phase, math.nan))
                pre_value = float(pre_exposure.get(phase, math.nan))
                exposure_detail[phase] = [value, pre_value]
                exposure_ok = exposure_ok and value > 0.0 and pre_value > 0.0 and math.isclose(
                    value * pre_value, 1.0, rel_tol=1e-5, abs_tol=1e-5
                )
            add_check(checks, f"{prefix}.exposure", exposure_ok, json.dumps(exposure_detail, sort_keys=True))

            streaming_hashes = pair.get("streamingStateSha1", {})
            streaming_values = [str(streaming_hashes.get(phase, "")) for phase in ("t0", "tau", "t1")]
            streaming_hashes_valid = all(
                len(value) == 40
                and all(character in "0123456789ABCDEFabcdef" for character in value)
                for value in streaming_values
            )
            add_check(
                checks,
                f"{prefix}.streaming_state_stable",
                streaming_hashes_valid and len(set(streaming_values)) == 1,
                json.dumps(streaming_hashes, sort_keys=True),
            )

            files = pair.get("files", {})
            hashes = pair.get("sha1", {})
            missing_modalities = sorted(REQUIRED_MODALITIES - set(files)) if isinstance(files, dict) else sorted(REQUIRED_MODALITIES)
            extra_modalities = sorted(set(files) - REQUIRED_MODALITIES) if isinstance(files, dict) else []
            add_check(checks, f"{prefix}.modality_set", not missing_modalities and not extra_modalities, f"missing={missing_modalities} extra={extra_modalities}")
            if not isinstance(files, dict):
                continue
            for modality in sorted(REQUIRED_MODALITIES & set(files)):
                try:
                    path = safe_dataset_file(dataset, files[modality])
                    path_error = ""
                except Exception as exc:
                    path = dataset / "__invalid__"
                    path_error = str(exc)
                relative = str(files[modality])
                unique = relative not in seen_paths
                seen_paths.add(relative)
                present = not path_error and path.is_file()
                add_check(checks, f"{prefix}.{modality}.path", present and unique, f"path={relative} unique={unique} error={path_error}")
                if not present:
                    continue
                actual_hash = sha1(path)
                declared_hash = str(hashes.get(modality, "")).upper()
                add_check(checks, f"{prefix}.{modality}.sha1", actual_hash == declared_hash, f"actual={actual_hash} declared={declared_hash}")
                try:
                    pixels = exr_rgba(path)
                    read_error = ""
                except Exception as exc:
                    pixels = np.zeros((0, 0, 4), dtype=np.float32)
                    read_error = str(exc)
                expected_size = display_size if modality in DISPLAY_MODALITIES else render_size
                actual_size = (pixels.shape[1], pixels.shape[0]) if pixels.ndim == 3 else (0, 0)
                finite = bool(pixels.size and np.isfinite(pixels).all())
                add_check(checks, f"{prefix}.{modality}.pixels", not read_error and finite and actual_size == expected_size, f"size={actual_size} expected={expected_size} finite={finite} error={read_error}")
                if not finite:
                    continue
                channel = pixels[..., 0]
                if modality in MASK_MODALITIES:
                    valid_values = bool(np.min(channel) >= -1e-6 and np.max(channel) <= 1.0 + 1e-6)
                    add_check(checks, f"{prefix}.{modality}.range", valid_values, f"min={float(np.min(channel))} max={float(np.max(channel))}")
                elif modality in OBJECT_ID_MODALITIES:
                    valid_values = bool(
                        np.min(channel) >= -1e-6
                        and np.max(channel) <= 255.0 + 1e-6
                        and np.max(np.abs(channel - np.rint(channel))) <= 1e-6
                    )
                    add_check(checks, f"{prefix}.{modality}.uint8_contract", valid_values, f"min={float(np.min(channel))} max={float(np.max(channel))}")
                elif modality.startswith("depth_"):
                    add_check(checks, f"{prefix}.{modality}.range", bool(np.min(channel) >= 0.0), f"min={float(np.min(channel))} max={float(np.max(channel))}")
                statistics[f"{prefix}.{modality}"] = {
                    "size": list(actual_size),
                    "min": float(np.min(pixels)),
                    "max": float(np.max(pixels)),
                    "mean": float(np.mean(pixels)),
                }

    passed = all(check["passed"] for check in checks)
    report = {
        "validatorVersion": 1,
        "dataset": str(dataset),
        "contractVersion": manifest.get("contractVersion"),
        "certificationStatus": manifest.get("certificationStatus"),
        "frameGenerationCertified": bool(manifest.get("frameGenerationCertified", False)),
        "formatAndIntegrityGate": "pass" if passed else "fail",
        "certificationGate": "not_certified",
        "checksPassed": sum(check["passed"] for check in checks),
        "checksTotal": len(checks),
        "checks": checks,
        "statistics": statistics,
        "missingRequirements": sorted(declared_gaps),
        "note": (
            "A passing integrity gate proves that the isolated endpoint/intermediate replay was "
            "assembled without path, hash, shape, numeric, timing, matrix, or declared-provenance "
            "violations. It does not certify nr-fg-data-v1 while the manifest lists missing requirements."
        ),
    }
    return report, passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path, help="Assembled dataset containing manifest.json")
    parser.add_argument("--report", type=Path, help="Output report (default: <dataset>/validation_report.json)")
    args = parser.parse_args()
    try:
        report, passed = validate(args.dataset)
        report_path = args.report or (args.dataset / "validation_report.json")
        temporary = report_path.with_suffix(report_path.suffix + ".part")
        temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        temporary.replace(report_path)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    state = "PASS (UNCERTIFIED)" if passed else "FAIL"
    print(f"{state}: {report['checksPassed']}/{report['checksTotal']} integrity checks; report={report_path}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
