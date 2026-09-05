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
    from PIL import Image
except ImportError as exc:
    raise SystemExit(
        "Validation dependencies are missing. Run: "
        "python -m pip install -r Scripts/requirements-validation.txt"
    ) from exc


REQUIRED_MODALITIES = {
    "scene_color_hudless_t0",
    "scene_color_hudless_tau",
    "scene_color_hudless_t1",
    "ui_color_alpha_t0",
    "ui_color_alpha_tau",
    "ui_color_alpha_t1",
    "depth_t0",
    "depth_tau",
    "depth_t1",
    "motion_1_to_0",
    "motion_valid_1_to_0",
    "motion_0_to_1",
    "motion_valid_0_to_1",
    "history_rejection_1_to_0",
    "history_rejection_valid_1_to_0",
    "history_rejection_0_to_1",
    "history_rejection_valid_0_to_1",
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
    "ui_color_alpha_t0",
    "ui_color_alpha_tau",
    "ui_color_alpha_t1",
}

UI_MODALITIES = {"ui_color_alpha_t0", "ui_color_alpha_tau", "ui_color_alpha_t1"}

MASK_MODALITIES = {
    "motion_valid_1_to_0",
    "motion_valid_0_to_1",
    "history_rejection_1_to_0",
    "history_rejection_valid_1_to_0",
    "history_rejection_0_to_1",
    "history_rejection_valid_0_to_1",
    "reactive_mask_t0",
    "reactive_mask_tau",
    "reactive_mask_t1",
    "transparency_mask_t0",
    "transparency_mask_tau",
    "transparency_mask_t1",
}

BINARY_MASK_MODALITIES = {
    "motion_valid_1_to_0",
    "motion_valid_0_to_1",
    "history_rejection_1_to_0",
    "history_rejection_valid_1_to_0",
    "history_rejection_0_to_1",
    "history_rejection_valid_0_to_1",
}

OBJECT_ID_MODALITIES = {"object_id_t0", "object_id_tau", "object_id_t1"}

BASE_REQUIRED_UNCERTIFIED_GAPS: set[str] = set()
WORLD_SPACE_WIDGET_GAP = "hudless_in_world_ui_residue_validation"

LEGACY_WPO_AND_MATERIAL_GAP = (
    "non_fixture_wpo_and_animated_material_endpoint_motion_validation"
)
PROJECT_ANIMATED_MATERIAL_GAP = "project_animated_material_logical_time_validation"
PROJECT_WPO_GAP = "project_authored_wpo_endpoint_motion_validation"

PROJECT_SKELETAL_GAPS = {
    "non_fixture_skeletal_animation_endpoint_motion_validation",
    "production_disocclusion_masks",
}

PROJECT_SKELETAL_EVIDENCE_VERSION = 1
PROJECT_SKELETAL_EVIDENCE_FILE_KEYS = {
    "forwardManifest",
    "forwardValidation",
    "reverseManifest",
    "reverseValidation",
    "reverseComparison",
    "poseCacheArtifact",
}

PROJECT_ANIMATED_MATERIAL_EVIDENCE_VERSION = 1
PROJECT_ANIMATED_MATERIAL_EVIDENCE_FILE_KEYS = {
    "forwardManifest",
    "forwardValidation",
    "reverseManifest",
    "reverseValidation",
    "reverseComparison",
}

EXPECTED_ENDPOINT_SCOPE = (
    "scene_components_plus_double_buffered_skinned_component_space_bones_"
    "plus_explicit_previous_frame_switch_wpo_fixture"
)

INTENTIONAL_CVAR_VALUES = {
    "r.MotionVectorSimulation": {
        "endpoints": "1",
        "reverseEndpoints": "1",
        "intermediate": "0",
    },
    "r.SkipRedundantTransformUpdate": {
        "endpoints": "0",
        "reverseEndpoints": "0",
        "intermediate": "1",
    },
}

EXPECTED_SCENE_STATE_HASH_SCOPE = (
    "sorted_actor_component_transforms_visibility_tick_controllable_canonical_state_hashes_skeletal_component_space_bones_"
    "niagara_component_and_finalized_cpu_particle_counts_cascade_component_state_gpu_payload_not_read_back"
)


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


def image_rgba(path: Path) -> np.ndarray:
    if path.suffix.lower() != ".png":
        return exr_rgba(path)
    with Image.open(path) as image:
        return np.asarray(image.convert("RGBA"), dtype=np.float32) / 255.0


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


def passing_check_map(report: Any) -> dict[str, bool]:
    if not isinstance(report, dict):
        return {}
    return {
        str(check.get("name")): bool(check.get("passed", False))
        for check in report.get("checks", [])
        if isinstance(check, dict)
    }


def report_complete(report: Any, gate: str = "formatAndIntegrityGate") -> bool:
    if not isinstance(report, dict):
        return False
    required_failures = [
        check
        for check in report.get("checks", [])
        if isinstance(check, dict)
        and check.get("required", True)
        and not check.get("passed", False)
    ]
    return bool(
        int(report.get("validatorVersion", 0)) >= 7
        and report.get(gate) == "pass"
        and int(report.get("checksTotal", -1)) > 0
        and int(report.get("checksPassed", -2))
        == int(report.get("checksTotal", -1))
        and not required_failures
    )


def named_proof_checks_pass(
    report: Any, exact: tuple[str, ...], prefixes: tuple[str, ...]
) -> tuple[bool, str]:
    check_map = passing_check_map(report)
    missing_exact = [name for name in exact if check_map.get(name) is not True]
    missing_prefixes = [
        prefix
        for prefix in prefixes
        if not any(name.startswith(prefix) and passed for name, passed in check_map.items())
    ]
    valid = not missing_exact and not missing_prefixes
    return valid, f"missingExact={missing_exact} missingPrefixes={missing_prefixes}"


def validate_project_skeletal_evidence(
    checks: list[dict[str, Any]], dataset: Path, evidence: Any
) -> bool:
    prefix = "evidence.project_skeletal"
    valid_record = isinstance(evidence, dict)
    add_check(checks, f"{prefix}.record", valid_record, type(evidence).__name__)
    if not valid_record:
        return False

    files = evidence.get("files", {})
    hashes = evidence.get("sha1", {})
    schema_ok = (
        evidence.get("schemaVersion") == PROJECT_SKELETAL_EVIDENCE_VERSION
        and evidence.get("proofScope")
        == "project_authored_anim_blueprint_shared_pose_cache_exact_forward_reverse_identity_camera_depth_and_bidirectional_disocclusion"
        and isinstance(files, dict)
        and isinstance(hashes, dict)
        and set(files) == PROJECT_SKELETAL_EVIDENCE_FILE_KEYS
        and set(hashes) == PROJECT_SKELETAL_EVIDENCE_FILE_KEYS
    )
    add_check(
        checks,
        f"{prefix}.schema",
        schema_ok,
        f"version={evidence.get('schemaVersion')} fileKeys={sorted(files) if isinstance(files, dict) else []}",
    )
    if not isinstance(files, dict) or not isinstance(hashes, dict):
        return False

    loaded: dict[str, Any] = {}
    resolved: dict[str, Path] = {}
    all_files_valid = schema_ok
    for key in sorted(PROJECT_SKELETAL_EVIDENCE_FILE_KEYS):
        try:
            path = safe_dataset_file(dataset, files.get(key))
            error = ""
        except Exception as exc:
            path = dataset / "__invalid__"
            error = str(exc)
        present = not error and path.is_file()
        actual_hash = sha1(path) if present else ""
        declared_hash = str(hashes.get(key, "")).upper()
        file_valid = present and len(declared_hash) == 40 and actual_hash == declared_hash
        add_check(
            checks,
            f"{prefix}.{key}.integrity",
            file_valid,
            f"path={files.get(key)} actual={actual_hash} declared={declared_hash} error={error}",
        )
        all_files_valid = all_files_valid and file_valid
        if not file_valid:
            continue
        resolved[key] = path
        if key != "poseCacheArtifact":
            try:
                loaded[key] = json.loads(path.read_text(encoding="utf-8"))
            except Exception as exc:
                add_check(checks, f"{prefix}.{key}.json", False, str(exc))
                all_files_valid = False

    if not all(key in loaded for key in PROJECT_SKELETAL_EVIDENCE_FILE_KEYS - {"poseCacheArtifact"}):
        return False

    forward_manifest = loaded["forwardManifest"]
    reverse_manifest = loaded["reverseManifest"]
    forward_report = loaded["forwardValidation"]
    reverse_report = loaded["reverseValidation"]
    comparison_report = loaded["reverseComparison"]
    manifests_are_objects = isinstance(forward_manifest, dict) and isinstance(
        reverse_manifest, dict
    )
    add_check(
        checks,
        f"{prefix}.manifest_objects",
        manifests_are_objects,
        f"forward={type(forward_manifest).__name__} reverse={type(reverse_manifest).__name__}",
    )
    if not manifests_are_objects:
        return False

    forward_job = forward_manifest.get("job", {})
    reverse_job = reverse_manifest.get("job", {})
    roles_ok = (
        forward_manifest.get("state") == "Completed"
        and reverse_manifest.get("state") == "Completed"
        and forward_manifest.get("replayPass") == "FrameGenerationEndpoints"
        and reverse_manifest.get("replayPass") == "FrameGenerationReverseEndpoints"
    )
    add_check(
        checks,
        f"{prefix}.capture_roles",
        roles_ok,
        f"forward={forward_manifest.get('replayPass')} reverse={reverse_manifest.get('replayPass')}",
    )
    jobs_ok = all(
        isinstance(job, dict)
        and job.get("bValidateNonFixtureSkeletalAnimation") is True
        and job.get("bCacheSkeletalAnimationPosesForReplay") is True
        and job.get("bUseDeterministicCameraTransform") is True
        and bool(job.get("nonFixtureSkeletalValidationActorClass"))
        for job in (forward_job, reverse_job)
    ) and (
        forward_job.get("nonFixtureSkeletalValidationActorClass")
        == reverse_job.get("nonFixtureSkeletalValidationActorClass")
        == evidence.get("actorClass")
    )
    add_check(
        checks,
        f"{prefix}.deterministic_project_anim_blueprint_jobs",
        jobs_ok,
        f"actor={evidence.get('actorClass')}",
    )

    try:
        forward_frame_ids = sorted(
            int(frame["logicalFrameId"]) for frame in forward_manifest.get("frames", [])
        )
        reverse_frame_ids = sorted(
            int(frame["logicalFrameId"]) for frame in reverse_manifest.get("frames", [])
        )
    except Exception:
        forward_frame_ids = []
        reverse_frame_ids = []
    declared_frame_ids = evidence.get("logicalFrameIds")
    grids_ok = (
        len(forward_frame_ids) >= 2
        and forward_frame_ids == reverse_frame_ids
        and forward_frame_ids == declared_frame_ids
    )
    add_check(
        checks,
        f"{prefix}.logical_frame_grid",
        grids_ok,
        f"forward={forward_frame_ids} reverse={reverse_frame_ids} declared={declared_frame_ids}",
    )

    forward_artifact_sha1 = str(
        forward_manifest.get("provenance", {}).get("skeletalPoseCacheArtifactSha1", "")
    ).upper()
    reverse_artifact_sha1 = str(
        reverse_manifest.get("provenance", {}).get("skeletalPoseCacheArtifactSha1", "")
    ).upper()
    copied_artifact_sha1 = sha1(resolved["poseCacheArtifact"]) if "poseCacheArtifact" in resolved else ""
    declared_artifact_sha1 = str(evidence.get("poseCacheArtifactSha1", "")).upper()
    artifact_ok = (
        len(declared_artifact_sha1) == 40
        and declared_artifact_sha1
        == forward_artifact_sha1
        == reverse_artifact_sha1
        == copied_artifact_sha1
    )
    add_check(
        checks,
        f"{prefix}.shared_pose_cache_artifact",
        artifact_ok,
        f"declared={declared_artifact_sha1} forward={forward_artifact_sha1} "
        f"reverse={reverse_artifact_sha1} copied={copied_artifact_sha1}",
    )

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
    standalone_reports_ok = True
    for label, report in (
        ("forward", forward_report),
        ("reverse", reverse_report),
    ):
        complete = report_complete(report)
        proof_ok, proof_detail = named_proof_checks_pass(
            report, standalone_exact, standalone_prefixes
        )
        check_map = passing_check_map(report)
        endpoint_motion = any(
            name.endswith(".project_skeletal_probe_endpoint_motion") and passed
            for name, passed in check_map.items()
        )
        visibility = any(
            name.endswith(".project_skeletal_bidirectional_visibility") and passed
            for name, passed in check_map.items()
        )
        valid = complete and proof_ok and endpoint_motion and visibility
        standalone_reports_ok = standalone_reports_ok and valid
        add_check(
            checks,
            f"{prefix}.{label}_standalone_gate",
            valid,
            f"complete={complete} proof={proof_detail} endpointMotion={endpoint_motion} "
            f"bidirectionalVisibility={visibility}",
        )

    comparison_proof_ok, comparison_detail = named_proof_checks_pass(
        comparison_report,
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
    )
    comparison_ok = bool(
        report_complete(comparison_report)
        and isinstance(comparison_report, dict)
        and comparison_report.get("skeletalReverseReplayGate") == "pass"
        and comparison_proof_ok
        and int(comparison_report.get("validatorVersion", 0))
        == int(evidence.get("validatorVersion", -1))
        and int(comparison_report.get("checksPassed", -1))
        == int(evidence.get("checksPassed", -2))
        and int(comparison_report.get("checksTotal", -1))
        == int(evidence.get("checksTotal", -2))
    )
    add_check(
        checks,
        f"{prefix}.exact_forward_reverse_gate",
        comparison_ok,
        f"gate={comparison_report.get('skeletalReverseReplayGate') if isinstance(comparison_report, dict) else None} "
        f"proof={comparison_detail}",
    )
    return bool(
        all_files_valid
        and roles_ok
        and jobs_ok
        and grids_ok
        and artifact_ok
        and standalone_reports_ok
        and comparison_ok
    )


def validate_project_animated_material_evidence(
    checks: list[dict[str, Any]], dataset: Path, evidence: Any
) -> bool:
    prefix = "evidence.project_animated_material"
    valid_record = isinstance(evidence, dict)
    add_check(checks, f"{prefix}.record", valid_record, type(evidence).__name__)
    if not valid_record:
        return False
    files = evidence.get("files", {})
    hashes = evidence.get("sha1", {})
    schema_ok = bool(
        evidence.get("schemaVersion")
        == PROJECT_ANIMATED_MATERIAL_EVIDENCE_VERSION
        and evidence.get("proofScope")
        == "project_authored_surface_material_gpu_logical_game_time_visible_change_and_forward_reverse_current_color_tolerance"
        and str(evidence.get("materialInterface", "")).startswith("/Game/")
        and isinstance(files, dict)
        and isinstance(hashes, dict)
        and set(files) == PROJECT_ANIMATED_MATERIAL_EVIDENCE_FILE_KEYS
        and set(hashes) == PROJECT_ANIMATED_MATERIAL_EVIDENCE_FILE_KEYS
    )
    add_check(
        checks,
        f"{prefix}.schema",
        schema_ok,
        f"version={evidence.get('schemaVersion')} material={evidence.get('materialInterface')} "
        f"fileKeys={sorted(files) if isinstance(files, dict) else []}",
    )
    if not isinstance(files, dict) or not isinstance(hashes, dict):
        return False

    loaded: dict[str, Any] = {}
    all_files_valid = schema_ok
    for key in sorted(PROJECT_ANIMATED_MATERIAL_EVIDENCE_FILE_KEYS):
        try:
            path = safe_dataset_file(dataset, files.get(key))
            error = ""
        except Exception as exc:
            path = dataset / "__invalid__"
            error = str(exc)
        present = not error and path.is_file()
        actual_hash = sha1(path) if present else ""
        declared_hash = str(hashes.get(key, "")).upper()
        valid = present and len(declared_hash) == 40 and actual_hash == declared_hash
        add_check(
            checks,
            f"{prefix}.{key}.integrity",
            valid,
            f"path={files.get(key)} actual={actual_hash} declared={declared_hash} error={error}",
        )
        all_files_valid = all_files_valid and valid
        if not valid:
            continue
        try:
            loaded[key] = json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:
            add_check(checks, f"{prefix}.{key}.json", False, str(exc))
            all_files_valid = False
    if set(loaded) != PROJECT_ANIMATED_MATERIAL_EVIDENCE_FILE_KEYS:
        return False

    forward_manifest = loaded["forwardManifest"]
    reverse_manifest = loaded["reverseManifest"]
    forward_report = loaded["forwardValidation"]
    reverse_report = loaded["reverseValidation"]
    comparison_report = loaded["reverseComparison"]
    manifests_ok = isinstance(forward_manifest, dict) and isinstance(
        reverse_manifest, dict
    )
    add_check(checks, f"{prefix}.manifest_objects", manifests_ok, str(manifests_ok))
    if not manifests_ok:
        return False
    roles_ok = bool(
        forward_manifest.get("state") == "Completed"
        and reverse_manifest.get("state") == "Completed"
        and forward_manifest.get("replayPass") == "FrameGenerationEndpoints"
        and reverse_manifest.get("replayPass")
        == "FrameGenerationReverseEndpoints"
    )
    add_check(
        checks,
        f"{prefix}.capture_roles",
        roles_ok,
        f"forward={forward_manifest.get('replayPass')} reverse={reverse_manifest.get('replayPass')}",
    )
    jobs = (forward_manifest.get("job", {}), reverse_manifest.get("job", {}))
    jobs_ok = all(
        isinstance(job, dict)
        and job.get("bValidateProjectAnimatedMaterial") is True
        and job.get("bLockMaterialTimeToLogicalFrame") is True
        and job.get("bUseDeterministicCameraTransform") is True
        and job.get("projectAnimatedMaterialValidationMaterial")
        == evidence.get("materialInterface")
        for job in jobs
    )
    add_check(
        checks,
        f"{prefix}.deterministic_project_material_jobs",
        jobs_ok,
        f"material={evidence.get('materialInterface')}",
    )
    try:
        forward_frame_ids = sorted(
            int(frame["logicalFrameId"]) for frame in forward_manifest.get("frames", [])
        )
        reverse_frame_ids = sorted(
            int(frame["logicalFrameId"]) for frame in reverse_manifest.get("frames", [])
        )
    except Exception:
        forward_frame_ids = []
        reverse_frame_ids = []
    grids_ok = bool(
        len(forward_frame_ids) >= 2
        and forward_frame_ids == reverse_frame_ids
        and forward_frame_ids == evidence.get("logicalFrameIds")
    )
    add_check(
        checks,
        f"{prefix}.logical_frame_grid",
        grids_ok,
        f"forward={forward_frame_ids} reverse={reverse_frame_ids} declared={evidence.get('logicalFrameIds')}",
    )

    standalone_exact = (
        "material_time.logical_frame_contract",
        "material_replay.project_validation_contract",
        "material_replay.project_animated_material_changes_with_logical_time",
    )
    standalone_prefixes = (
        "frame_000000.project_animated_material_schema",
        "frame_000000.project_animated_material_visible",
    )
    standalone_ok = True
    for label, report in (("forward", forward_report), ("reverse", reverse_report)):
        proof_ok, detail = named_proof_checks_pass(
            report, standalone_exact, standalone_prefixes
        )
        valid = bool(
            report_complete(report)
            and int(report.get("validatorVersion", 0)) >= 8
            and proof_ok
        )
        standalone_ok = standalone_ok and valid
        add_check(
            checks,
            f"{prefix}.{label}_standalone_gate",
            valid,
            f"proof={detail}",
        )

    comparison_proof_ok, comparison_detail = named_proof_checks_pass(
        comparison_report,
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
    )
    comparison_ok = bool(
        report_complete(comparison_report)
        and isinstance(comparison_report, dict)
        and int(comparison_report.get("validatorVersion", 0)) >= 8
        and comparison_report.get("materialReverseReplayGate") == "pass"
        and comparison_proof_ok
        and int(comparison_report.get("validatorVersion", 0))
        == int(evidence.get("validatorVersion", -1))
        and int(comparison_report.get("checksPassed", -1))
        == int(evidence.get("checksPassed", -2))
        and int(comparison_report.get("checksTotal", -1))
        == int(evidence.get("checksTotal", -2))
    )
    add_check(
        checks,
        f"{prefix}.exact_forward_reverse_gate",
        comparison_ok,
        f"gate={comparison_report.get('materialReverseReplayGate') if isinstance(comparison_report, dict) else None} "
        f"proof={comparison_detail}",
    )
    return bool(
        all_files_valid
        and roles_ok
        and jobs_ok
        and grids_ok
        and standalone_ok
        and comparison_ok
    )


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
    reverse_endpoint = provenance.get("reverseEndpointReplay", {})
    intermediate = provenance.get("intermediateReplay", {})
    endpoint_cvars = endpoint.get("cvars", {}) if isinstance(endpoint, dict) else {}
    reverse_endpoint_cvars = (
        reverse_endpoint.get("cvars", {}) if isinstance(reverse_endpoint, dict) else {}
    )
    intermediate_cvars = intermediate.get("cvars", {}) if isinstance(intermediate, dict) else {}
    add_check(
        checks,
        "provenance.source_objects",
        isinstance(endpoint_cvars, dict) and bool(endpoint_cvars)
        and isinstance(reverse_endpoint_cvars, dict) and bool(reverse_endpoint_cvars)
        and isinstance(intermediate_cvars, dict) and bool(intermediate_cvars),
        f"endpointCVars={len(endpoint_cvars)} "
        f"reverseEndpointCVars={len(reverse_endpoint_cvars)} "
        f"intermediateCVars={len(intermediate_cvars)}",
    )
    if (
        not isinstance(endpoint_cvars, dict)
        or not isinstance(reverse_endpoint_cvars, dict)
        or not isinstance(intermediate_cvars, dict)
    ):
        return

    unexpected = [
        name
        for name in sorted(
            set(endpoint_cvars) | set(reverse_endpoint_cvars) | set(intermediate_cvars)
        )
        if name not in INTENTIONAL_CVAR_VALUES
        and len(
            {
                str(endpoint_cvars.get(name)),
                str(reverse_endpoint_cvars.get(name)),
                str(intermediate_cvars.get(name)),
            }
        )
        != 1
    ]
    add_check(
        checks,
        "provenance.only_intentional_cvar_difference",
        not unexpected,
        f"unexpected={unexpected}",
    )
    expected_values_ok = all(
        str(endpoint_cvars.get(name)) == values["endpoints"]
        and str(reverse_endpoint_cvars.get(name)) == values["reverseEndpoints"]
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
                    "reverseEndpoints": reverse_endpoint_cvars.get(name),
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
    reverse_endpoint_normalized = {
        name: reverse_endpoint_cvars[name]
        for name in sorted(reverse_endpoint_cvars)
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
        endpoint_normalized == reverse_endpoint_normalized == intermediate_normalized
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
    mismatches = [
        name
        for name in invariant_fields
        if endpoint.get(name) != reverse_endpoint.get(name)
        or endpoint.get(name) != intermediate.get(name)
    ]
    add_check(
        checks,
        "provenance.render_environment_exact",
        not mismatches,
        f"mismatches={mismatches}",
    )


def skeletal_history_valid(state):
    count = int(state.get("sceneSkeletalComponentCount", state.get("skeletalBoneComponentCount", 0)))
    components, bones = int(state.get("skeletalBoneComponentCount", 0)), int(state.get("skeletalBoneCount", 0))
    return state.get("skeletalBoneSkippedComponents") == [] and ((count > 0 and state.get("skeletalBoneOverride") is True and components > 0 and bones > 0) or (count == 0 and state.get("skeletalBoneOverride") is False and components == 0 and bones == 0))


def validate_project_wpo_evidence(checks, dataset, evidence, binary_sha1):
    """Recompute the physical check from the included original proof images."""
    start = len(checks)
    try:
        from VerifyProjectWPO import verify
        files, hashes = evidence["files"], evidence["sha1"]
        if evidence.get("schemaVersion") != 1 or set(files) != set(hashes):
            raise ValueError("Invalid WPO evidence schema")
        for key, relative in files.items():
            path = safe_dataset_file(dataset, relative)
            if sha1(path) != str(hashes[key]).upper():
                raise ValueError(f"WPO proof hash mismatch: {key}")
        roots = []
        source_hash = hashlib.sha256(Path(__file__).with_name("ValidateDataset.py").read_bytes() + Path(__file__).with_name("TemporalGeometry.py").read_bytes()).hexdigest()
        for role in ("forward", "reverse", "midpoint"):
            path = safe_dataset_file(dataset, files[role + "/manifest.json"])
            source = json.loads(path.read_text(encoding="utf-8"))
            report = json.loads(safe_dataset_file(dataset, files[role + "/validation_report.json"]).read_text(encoding="utf-8"))
            valid = report_complete(report) and report.get("manifestSha256") == hashlib.sha256(path.read_bytes()).hexdigest() and report.get("validatorSourceSha256") == source_hash and source["provenance"]["pluginBinarySha1"] == binary_sha1
            add_check(checks, f"evidence.project_wpo.{role}.source_validation", valid, "Current validator, original manifest and matching capture binary")
            roots.append(path.parent)
        physical = verify(roots)
        add_check(checks, "evidence.project_wpo.physical_recomputed", physical["passed"] and physical == evidence.get("physicalReport"), "Recomputed signed forward/reverse motion, coverage and real midpoint silhouette")
    except Exception as exc:
        add_check(checks, "evidence.project_wpo.valid", False, str(exc))
    return len(checks) > start and all(c["passed"] for c in checks[start:])


def validate_evidence_binding(checks, dataset, evidence, binary_sha1, label):
    """Bind portable comparison reports to both source manifests and this build."""
    try:
        paths = {k: safe_dataset_file(dataset, v) for k, v in evidence["files"].items()}
        source_hash = hashlib.sha256(Path(__file__).with_name("ValidateDataset.py").read_bytes() + Path(__file__).with_name("TemporalGeometry.py").read_bytes()).hexdigest()
        for role in ("forward", "reverse"):
            path = paths[role + "Manifest"]
            source = json.loads(path.read_text(encoding="utf-8"))
            report = json.loads(paths[role + "Validation"].read_text(encoding="utf-8"))
            if report.get("manifestSha256") != hashlib.sha256(path.read_bytes()).hexdigest() or report.get("validatorSourceSha256") != source_hash or source["provenance"]["pluginBinarySha1"] != binary_sha1:
                raise ValueError(f"{role} evidence is stale or from a different binary")
        comparison = json.loads(paths["reverseComparison"].read_text(encoding="utf-8"))
        if comparison.get("manifestSha256") != hashlib.sha256(paths["forwardManifest"].read_bytes()).hexdigest() or comparison.get("compareManifestSha256") != hashlib.sha256(paths["reverseManifest"].read_bytes()).hexdigest() or comparison.get("validatorSourceSha256") != source_hash:
            raise ValueError("Comparison is not bound to these manifests and validator")
        add_check(checks, f"evidence.{label}.source_binding", True, "Both manifests, current validator and matching capture binary")
        return True
    except Exception as exc:
        add_check(checks, f"evidence.{label}.source_binding", False, str(exc))
        return False


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
    validation_coverage = manifest.get("validationCoverage", {})
    project_skeletal_claimed = bool(
        isinstance(validation_coverage, dict)
        and validation_coverage.get("projectAuthoredAnimBlueprintEndpointMotion") is True
        and validation_coverage.get(
            "projectAuthoredAnimBlueprintExactForwardReverseReplay"
        )
        is True
        and validation_coverage.get("projectSkeletalBidirectionalDisocclusion") is True
    )
    project_animated_material_claimed = bool(
        isinstance(validation_coverage, dict)
        and validation_coverage.get("projectAnimatedMaterialLogicalGameTime") is True
        and validation_coverage.get("projectAnimatedMaterialVisibleTimeVariation")
        is True
        and validation_coverage.get(
            "projectAnimatedMaterialForwardReverseCurrentColor"
        )
        is True
    )
    hudless_world_widget_rejection_claimed = bool(
        isinstance(validation_coverage, dict)
        and validation_coverage.get("hudlessWorldSpaceWidgetResidueRejected")
        is True
    )
    required_uncertified_gaps = set(BASE_REQUIRED_UNCERTIFIED_GAPS)
    if not project_skeletal_claimed:
        required_uncertified_gaps.update(PROJECT_SKELETAL_GAPS)
    if not hudless_world_widget_rejection_claimed:
        required_uncertified_gaps.add(WORLD_SPACE_WIDGET_GAP)
    project_wpo_claimed = validation_coverage.get("projectAuthoredWPOEndpointMotion") is True
    wpo_gap_ok = (project_wpo_claimed and PROJECT_WPO_GAP not in declared_gaps and LEGACY_WPO_AND_MATERIAL_GAP not in declared_gaps) or (not project_wpo_claimed and (PROJECT_WPO_GAP in declared_gaps or (
        not project_animated_material_claimed
        and LEGACY_WPO_AND_MATERIAL_GAP in declared_gaps
    )))
    animated_material_gap_ok = (
        project_animated_material_claimed
        and PROJECT_ANIMATED_MATERIAL_GAP not in declared_gaps
        and LEGACY_WPO_AND_MATERIAL_GAP not in declared_gaps
    ) or (
        not project_animated_material_claimed
        and (
            PROJECT_ANIMATED_MATERIAL_GAP in declared_gaps
            or LEGACY_WPO_AND_MATERIAL_GAP in declared_gaps
        )
    )
    add_check(
        checks,
        "contract.honest_pending_or_uncertified_state",
        (manifest.get("certificationStatus") == "experimental_uncertified" or (manifest.get("certificationStatus") == "pending_dataset_validation" and not declared_gaps))
        and manifest.get("frameGenerationCertified") is False
        and required_uncertified_gaps <= declared_gaps
        and wpo_gap_ok
        and animated_material_gap_ok
        and (
            not project_skeletal_claimed
            or not (PROJECT_SKELETAL_GAPS & declared_gaps)
        )
        and (
            not hudless_world_widget_rejection_claimed
            or WORLD_SPACE_WIDGET_GAP not in declared_gaps
        ),
        f"status={manifest.get('certificationStatus')} certified={manifest.get('frameGenerationCertified')} "
        f"gaps={sorted(declared_gaps)} required={sorted(required_uncertified_gaps)} "
        f"projectSkeletalClaimed={project_skeletal_claimed} "
        f"projectAnimatedMaterialClaimed={project_animated_material_claimed} "
        f"hudlessWorldWidgetRejectionClaimed={hudless_world_widget_rejection_claimed} "
        f"wpoGapOk={wpo_gap_ok} animatedMaterialGapOk={animated_material_gap_ok}",
    )
    add_check(checks, "contract.tau", math.isclose(float(manifest.get("tau", math.nan)), 0.5, abs_tol=1e-12), str(manifest.get("tau")))
    add_check(checks, "contract.motion_blur_off", manifest.get("motionBlur") == "off", str(manifest.get("motionBlur")))
    ui_semantics = manifest.get("bufferSemantics", {}).get("uiColorAlpha", {})
    add_check(
        checks,
        "contract.ui_separated",
        manifest.get("uiSeparated") is True
        and ui_semantics.get("pipelineStage")
        == "independent_slate_game_layer_before_scene_composite"
        and ui_semantics.get("resolution") == "display"
        and ui_semantics.get("encoding") == "display_referred_srgb_png_unorm8"
        and ui_semantics.get("alphaSemantic")
        == "straight_coverage_zero_is_transparent_one_is_opaque"
        and ui_semantics.get("rgbSemantic") == "premultiplied_by_coverage_alpha"
        and ui_semantics.get("sceneIncluded") is False,
        json.dumps(ui_semantics, sort_keys=True),
    )

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
    for role in (
        "endpointValidation",
        "reverseEndpointValidation",
        "intermediateValidation",
    ):
        validation = source_replays.get(role, {}) if isinstance(source_replays, dict) else {}
        passed = int(validation.get("checksPassed", -1))
        total = int(validation.get("checksTotal", -2))
        add_check(
            checks,
            f"sources.{role}",
            total > 0
            and passed == total
            and int(validation.get("validatorVersion", 0)) >= 5,
            f"passed={passed} total={total} validator={validation.get('validatorVersion')}",
        )

    fixture_coverage = validation_coverage.get("semanticFixturePreviousFrameSwitchWPOMotion") is True
    add_check(checks, "sources.applicable_scene_coverage", isinstance(validation_coverage, dict) and ((fixture_coverage and validation_coverage.get("semanticFixtureRigidComponentMotion") is True and validation_coverage.get("semanticFixtureDoubleBufferedSkeletalBoneMotion") is True) or validation_coverage.get("ordinarySceneCaptureValidated") is True), json.dumps(validation_coverage, sort_keys=True))
    add_check(
        checks,
        "sources.hudless_world_widget_rejection_coverage",
        not hudless_world_widget_rejection_claimed
        or (
            isinstance(validation_coverage, dict)
            and validation_coverage.get("hudlessWorldSpaceWidgetResidueRejected")
            is True
        ),
        json.dumps(validation_coverage, sort_keys=True),
    )
    validation_evidence = manifest.get("validationEvidence", {})
    binary_sha1 = manifest.get("provenance", {}).get("endpointReplay", {}).get("pluginBinarySha1")
    project_wpo_evidence = validation_evidence.get("projectWPO")
    project_wpo_passed = validate_project_wpo_evidence(checks, dataset, project_wpo_evidence, binary_sha1) if project_wpo_evidence is not None else False
    add_check(checks, "sources.project_wpo_claim_matches_portable_evidence", project_wpo_claimed == project_wpo_passed, f"claimed={project_wpo_claimed} evidencePassed={project_wpo_passed}")
    for key in ("projectSkeletalAnimation", "projectAnimatedMaterial"):
        if validation_evidence.get(key) is not None:
            validate_evidence_binding(checks, dataset, validation_evidence[key], binary_sha1, key)
    project_skeletal_evidence = (
        validation_evidence.get("projectSkeletalAnimation")
        if isinstance(validation_evidence, dict)
        else None
    )
    project_skeletal_evidence_passed = (
        validate_project_skeletal_evidence(
            checks, dataset, project_skeletal_evidence
        )
        if project_skeletal_evidence is not None
        else False
    )
    add_check(
        checks,
        "sources.project_skeletal_claim_matches_portable_evidence",
        project_skeletal_claimed == project_skeletal_evidence_passed,
        f"claimed={project_skeletal_claimed} evidencePassed={project_skeletal_evidence_passed}",
    )
    project_animated_material_evidence = (
        validation_evidence.get("projectAnimatedMaterial")
        if isinstance(validation_evidence, dict)
        else None
    )
    project_animated_material_evidence_passed = (
        validate_project_animated_material_evidence(
            checks, dataset, project_animated_material_evidence
        )
        if project_animated_material_evidence is not None
        else False
    )
    add_check(
        checks,
        "sources.project_animated_material_claim_matches_portable_evidence",
        project_animated_material_claimed
        == project_animated_material_evidence_passed,
        f"claimed={project_animated_material_claimed} "
        f"evidencePassed={project_animated_material_evidence_passed}",
    )
    add_check(
        checks,
        "sources.semantic_ui_color_alpha_coverage",
        isinstance(validation_coverage, dict)
        and (validation_coverage.get("semanticFixtureUIColorAlpha") is True or validation_coverage.get("ordinarySceneCaptureValidated") is True),
        json.dumps(validation_coverage, sort_keys=True),
    )

    validate_provenance(checks, manifest.get("provenance"))

    semantics = manifest.get("bufferSemantics", {})
    motion_semantics = semantics.get("motion1To0", {}) if isinstance(semantics, dict) else {}
    reverse_motion_semantics = semantics.get("motion0To1", {}) if isinstance(semantics, dict) else {}
    color_semantics = semantics.get("sceneColorHudless", {}) if isinstance(semantics, dict) else {}
    depth_semantics = semantics.get("depth", {}) if isinstance(semantics, dict) else {}
    rejection_semantics = semantics.get("historyRejection1To0", {}) if isinstance(semantics, dict) else {}
    reverse_rejection_semantics = (
        semantics.get("historyRejection0To1", {}) if isinstance(semantics, dict) else {}
    )
    add_check(
        checks,
        "semantics.motion",
        motion_semantics.get("definition") == "previous_pixel = current_pixel + motion_1_to_0"
        and motion_semantics.get("unit") == "display_pixel"
        and motion_semantics.get("origin") == "top_left"
        and motion_semantics.get("jitterRemoved") is True
        and motion_semantics.get("endpointTransformScope")
        == EXPECTED_ENDPOINT_SCOPE,
        json.dumps(motion_semantics, sort_keys=True),
    )
    add_check(
        checks,
        "semantics.reverse_motion",
        reverse_motion_semantics.get("definition")
        == "future_pixel = current_pixel + motion_0_to_1"
        and reverse_motion_semantics.get("unit") == "display_pixel"
        and reverse_motion_semantics.get("origin") == "top_left"
        and reverse_motion_semantics.get("jitterRemoved") is True
        and reverse_motion_semantics.get("independentlyCaptured") is True
        and reverse_motion_semantics.get("endpointTransformScope")
        == EXPECTED_ENDPOINT_SCOPE,
        json.dumps(reverse_motion_semantics, sort_keys=True),
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
    add_check(
        checks,
        "semantics.history_rejection",
        rejection_semantics.get("definition")
        == "one_rejects_t0_history_at_t1_motion_reprojected_pixel"
        and rejection_semantics.get("source")
        == "component_identity_and_static_camera_depth_on_jittered_rasters_v3"
        and rejection_semantics.get("validity") == "history_rejection_valid_1_to_0"
        and rejection_semantics.get("productionCertified") is False,
        json.dumps(rejection_semantics, sort_keys=True),
    )
    add_check(
        checks,
        "semantics.reverse_history_rejection",
        reverse_rejection_semantics.get("definition")
        == "one_rejects_t1_history_at_t0_motion_reprojected_pixel"
        and reverse_rejection_semantics.get("source")
        == "component_identity_and_static_camera_depth_on_jittered_rasters_v3"
        and reverse_rejection_semantics.get("validity")
        == "history_rejection_valid_0_to_1"
        and reverse_rejection_semantics.get("productionCertified") is False,
        json.dumps(reverse_rejection_semantics, sort_keys=True),
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
            reverse_endpoint_state = pair.get("reverseEndpointPreviousState", {})
            time_span = float(endpoint_state.get("timeSpanS", math.nan))
            reverse_time_span = float(reverse_endpoint_state.get("timeSpanS", math.nan))
            add_check(
                checks,
                f"{prefix}.endpoint_history_isolation",
                pair.get("intermediateHistoryIsolated") is True
                and pair.get("bidirectionalMotionIndependentProcesses") is True
                and int(endpoint_state.get("t1PreviousLogicalFrameId", -1)) == t0
                and endpoint_state.get("componentTransformOverride") is True
                and skeletal_history_valid(endpoint_state)
                and (not fixture_coverage or endpoint_state.get("wpoPreviousFrameSwitchFixtureValidated") is True)
                and delta_time > 0.0
                and math.isclose(time_span, delta_time, rel_tol=1e-6, abs_tol=1e-7),
                f"previous={endpoint_state.get('t1PreviousLogicalFrameId')} delta={delta_time} span={time_span}",
            )
            add_check(
                checks,
                f"{prefix}.reverse_endpoint_history_isolation",
                pair.get("bidirectionalMotionIndependentProcesses") is True
                and int(reverse_endpoint_state.get("t0PreviousLogicalFrameId", -1)) == t1
                and reverse_endpoint_state.get("componentTransformOverride") is True
                and skeletal_history_valid(reverse_endpoint_state)
                and (not fixture_coverage or reverse_endpoint_state.get("wpoPreviousFrameSwitchFixtureValidated") is True)
                and delta_time > 0.0
                and math.isclose(
                    reverse_time_span, delta_time, rel_tol=1e-6, abs_tol=1e-7
                ),
                f"previous={reverse_endpoint_state.get('t0PreviousLogicalFrameId')} "
                f"delta={delta_time} span={reverse_time_span}",
            )

            grid_alignment = pair.get("reverseEndpointGridAlignment", {})
            scene_state_exact = bool(grid_alignment.get("sceneStateExactT0")) and bool(
                grid_alignment.get("sceneStateExactT1")
            )
            fixture_hidden_state_allowance = (
                grid_alignment.get("semanticFixtureAllowsHiddenUncontrolledStateMismatch")
                is True
            )
            add_check(
                checks,
                f"{prefix}.reverse_endpoint_grid_alignment",
                grid_alignment.get("jitterCameraDepthObjectIdExact") is True
                and (scene_state_exact or fixture_hidden_state_allowance),
                json.dumps(grid_alignment, sort_keys=True),
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
            scene_hashes = pair.get("sceneStateSha1", {})
            grids = pair.get("rasterGrids", {})
            for phase, logical_id in (("t0", t0), ("tau", tau_frame), ("t1", t1)):
                grid = grids.get(phase, {})
                temporal = grid.get("temporal", {})
                jitter = np.asarray(temporal.get("jitterCurrentRenderPixel", []))
                projection = np.asarray(temporal.get("viewToClipCurrentJittered", []))
                add_check(checks, f"{prefix}.{phase}.raster_grid", grid.get("logicalFrameId") == logical_id and tuple(temporal.get("renderSize", [])) == render_size and tuple(temporal.get("displaySize", [])) == display_size and jitter.shape == (2,) and bool(np.isfinite(jitter).all()) and projection.shape == (16,) and bool(np.isfinite(projection).all()), "Recorded actual current jitter, projection and grid for each real sample")
            scene_values = [str(scene_hashes.get(phase, "")) for phase in ("t0", "tau", "t1")]
            scene_hashes_valid = all(
                len(value) == 40
                and all(character in "0123456789ABCDEFabcdef" for character in value)
                for value in scene_values
            )
            add_check(
                checks,
                f"{prefix}.scene_state_timeline",
                scene_hashes_valid
                and (not fixture_coverage or len(set(scene_values)) == 3)
                and pair.get("sceneStateHashScope")
                == EXPECTED_SCENE_STATE_HASH_SCOPE,
                json.dumps(scene_hashes, sort_keys=True),
            )

            wpo_validation = pair.get("wpoValidation", {})
            wpo_forward = (
                wpo_validation.get("forward", {})
                if isinstance(wpo_validation, dict)
                else {}
            )
            wpo_reverse = (
                wpo_validation.get("reverse", {})
                if isinstance(wpo_validation, dict)
                else {}
            )
            wpo_object_id = int(wpo_validation.get("objectId", -1))
            wpo_forward_expected = np.asarray(
                wpo_forward.get("expectedMotionDisplayPixels", (math.nan, math.nan)),
                dtype=np.float64,
            )
            wpo_reverse_expected = np.asarray(
                wpo_reverse.get("expectedMotionDisplayPixels", (math.nan, math.nan)),
                dtype=np.float64,
            )
            if fixture_coverage:
                add_check(
                    checks,
                    f"{prefix}.wpo_previous_frame_switch_metadata",
                    isinstance(wpo_validation, dict)
                    and wpo_validation.get("previousFrameSwitch") is True
                    and 0 < wpo_object_id <= 255
                    and wpo_forward_expected.shape == (2,)
                    and wpo_reverse_expected.shape == (2,)
                    and bool(np.isfinite(wpo_forward_expected).all())
                    and bool(np.isfinite(wpo_reverse_expected).all())
                    and float(wpo_forward.get("previousRightCm", math.nan))
                    != float(wpo_forward.get("currentRightCm", math.nan))
                    and float(wpo_reverse.get("previousRightCm", math.nan))
                    != float(wpo_reverse.get("currentRightCm", math.nan)),
                    json.dumps(wpo_validation, sort_keys=True),
                )

            files = pair.get("files", {})
            hashes = pair.get("sha1", {})
            missing_modalities = sorted(REQUIRED_MODALITIES - set(files)) if isinstance(files, dict) else sorted(REQUIRED_MODALITIES)
            extra_modalities = sorted(set(files) - REQUIRED_MODALITIES) if isinstance(files, dict) else []
            add_check(checks, f"{prefix}.modality_set", not missing_modalities and not extra_modalities, f"missing={missing_modalities} extra={extra_modalities}")
            if not isinstance(files, dict):
                continue
            pair_pixels: dict[str, np.ndarray] = {}
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
                    pixels = image_rgba(path)
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
                pair_pixels[modality] = pixels
                channel = pixels[..., 0]
                if modality in MASK_MODALITIES:
                    valid_values = bool(np.min(channel) >= -1e-6 and np.max(channel) <= 1.0 + 1e-6)
                    add_check(checks, f"{prefix}.{modality}.range", valid_values, f"min={float(np.min(channel))} max={float(np.max(channel))}")
                    if modality in BINARY_MASK_MODALITIES:
                        binary = bool(np.all((channel == 0.0) | (channel == 1.0)))
                        add_check(
                            checks,
                            f"{prefix}.{modality}.binary",
                            binary,
                            f"values={np.unique(channel)[:16].tolist()}",
                        )
                elif modality in OBJECT_ID_MODALITIES:
                    valid_values = bool(
                        np.min(channel) >= -1e-6
                        and np.max(channel) <= 255.0 + 1e-6
                        and np.max(np.abs(channel - np.rint(channel))) <= 1e-6
                    )
                    add_check(checks, f"{prefix}.{modality}.uint8_contract", valid_values, f"min={float(np.min(channel))} max={float(np.max(channel))}")
                elif modality.startswith("depth_"):
                    add_check(checks, f"{prefix}.{modality}.range", bool(np.min(channel) >= 0.0), f"min={float(np.min(channel))} max={float(np.max(channel))}")
                elif modality in UI_MODALITIES:
                    alpha = pixels[..., 3]
                    unorm_range = bool((pixels >= 0.0).all() and (pixels <= 1.0).all())
                    premultiplied = bool(
                        (pixels[..., :3] <= alpha[..., None] + (1.0 / 255.0)).all()
                    )
                    add_check(
                        checks,
                        f"{prefix}.{modality}.ui_contract",
                        unorm_range and premultiplied,
                        f"min={float(np.min(pixels))} max={float(np.max(pixels))} "
                        f"maxRgbMinusAlpha={float(np.max(pixels[..., :3] - alpha[..., None]))}",
                    )
                statistics[f"{prefix}.{modality}"] = {
                    "size": list(actual_size),
                    "min": float(np.min(pixels)),
                    "max": float(np.max(pixels)),
                    "mean": float(np.mean(pixels)),
                }

            ui_diagnostics = pair.get("uiColorAlphaDiagnostics", {})
            for phase, modality in (
                ("t0", "ui_color_alpha_t0"),
                ("tau", "ui_color_alpha_tau"),
                ("t1", "ui_color_alpha_t1"),
            ):
                ui = ui_diagnostics.get(phase, {}) if isinstance(ui_diagnostics, dict) else {}
                pixels = pair_pixels.get(modality)
                if pixels is None:
                    continue
                alpha = pixels[..., 3]
                nonzero = int(np.count_nonzero(alpha > (0.5 / 255.0)))
                fractional = int(
                    np.count_nonzero(
                        (alpha > (0.5 / 255.0)) & (alpha < (254.5 / 255.0))
                    )
                )
                ui_contract = (
                    isinstance(ui, dict)
                    and ui.get("pipelineStage")
                    == "independent_slate_game_layer_before_scene_composite"
                    and ui.get("colorEncoding") == "display_referred_srgb_png_unorm8"
                    and ui.get("alphaSemantic")
                    == "straight_coverage_zero_is_transparent_one_is_opaque"
                    and ui.get("rgbSemantic") == "premultiplied_by_coverage_alpha"
                    and ui.get("source")
                    == "SGameLayerManager_without_enclosing_SViewport_scene_backbuffer"
                    and ui.get("sceneIncluded") is False
                    and ui.get("screenSpaceGameLayersIncluded") is True
                    and ui.get("displayResolution") is True
                    and tuple(ui.get("size", ())) == display_size
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
                    f"{prefix}.{modality}.metadata",
                    ui_contract,
                    json.dumps(
                        {
                            "size": ui.get("size") if isinstance(ui, dict) else None,
                            "nonzero": [ui.get("nonzeroAlphaPixelCount"), nonzero]
                            if isinstance(ui, dict)
                            else [None, nonzero],
                            "fractional": [ui.get("fractionalAlphaPixelCount"), fractional]
                            if isinstance(ui, dict)
                            else [None, fractional],
                        },
                        sort_keys=True,
                    ),
                )
                if validation_coverage.get("semanticFixtureUIColorAlpha") is True:
                    add_check(
                        checks,
                        f"{prefix}.{modality}.fixture_alpha_coverage",
                        ui.get("semanticValidationFixture") is True
                        and nonzero > 0
                        and fractional > 0
                        and float(alpha.min()) == 0.0
                        and float(alpha.max()) == 1.0,
                        f"nonzero={nonzero} fractional={fractional} min={float(alpha.min())} max={float(alpha.max())}",
                    )

            widget_policy = pair.get("worldSpaceWidgetPolicy", {})
            for phase in ("t0", "tau", "t1"):
                phase_policy = (
                    widget_policy.get(phase, {})
                    if isinstance(widget_policy, dict)
                    else {}
                )
                policy_ok = bool(
                    not hudless_world_widget_rejection_claimed
                    or (
                        isinstance(phase_policy, dict)
                        and phase_policy.get("policy")
                        == "reject_visible_registered_widget_components"
                        and int(
                            phase_policy.get(
                                "activeVisibleRegisteredComponentCount", -1
                            )
                        )
                        == 0
                        and phase_policy.get(
                            "activeVisibleRegisteredComponentPaths"
                        )
                        == []
                    )
                )
                add_check(
                    checks,
                    f"{prefix}.world_ui_{phase}_zero_widget_component_residue",
                    policy_ok,
                    json.dumps(phase_policy, sort_keys=True),
                )

            if fixture_coverage:
                for direction, object_modality, motion_modality, valid_modality, expected in (
                    (
                        "forward",
                        "object_id_t1",
                        "motion_1_to_0",
                        "motion_valid_1_to_0",
                        wpo_forward_expected,
                    ),
                    (
                        "reverse",
                        "object_id_t0",
                        "motion_0_to_1",
                        "motion_valid_0_to_1",
                        wpo_reverse_expected,
                    ),
                ):
                    required_wpo_modalities = (
                        object_modality,
                        motion_modality,
                        valid_modality,
                    )
                    available = all(name in pair_pixels for name in required_wpo_modalities)
                    visible_count = 0
                    covered_count = 0
                    measured = np.asarray((math.nan, math.nan), dtype=np.float64)
                    error = np.asarray((math.inf, math.inf), dtype=np.float64)
                    if available:
                        ids = np.rint(pair_pixels[object_modality][..., 0]).astype(np.int32)
                        visible = ids == wpo_object_id
                        covered = visible & (pair_pixels[valid_modality][..., 0] > 0.5)
                        visible_count = int(np.count_nonzero(visible))
                        covered_count = int(np.count_nonzero(covered))
                        if covered_count:
                            measured = np.median(
                                pair_pixels[motion_modality][covered, :2].astype(np.float64),
                                axis=0,
                            )
                            error = np.abs(measured - expected)
                    covered_fraction = covered_count / visible_count if visible_count else 0.0
                    add_check(
                        checks,
                        f"{prefix}.wpo_{direction}_motion",
                        available
                        and visible_count >= 32
                        and covered_fraction >= 0.75
                        and bool(np.all(error <= 1.5)),
                        f"available={available} id={wpo_object_id} visible={visible_count} "
                        f"covered={covered_count} fraction={covered_fraction:.9f} "
                        f"expected={expected.tolist()} measured={measured.tolist()} "
                        f"abs_error={error.tolist()}",
                    )

            if fixture_hidden_state_allowance:
                required_motion_pixels = (
                    "motion_1_to_0",
                    "motion_valid_1_to_0",
                    "motion_0_to_1",
                    "motion_valid_0_to_1",
                )
                available = all(name in pair_pixels for name in required_motion_pixels)
                joint_valid_count = 0
                max_same_pixel_negation_error = 0.0
                if available:
                    joint_valid = (
                        pair_pixels["motion_valid_1_to_0"][..., 0] > 0.5
                    ) & (pair_pixels["motion_valid_0_to_1"][..., 0] > 0.5)
                    joint_valid_count = int(np.count_nonzero(joint_valid))
                    if joint_valid_count:
                        residual = (
                            pair_pixels["motion_1_to_0"][..., :2]
                            + pair_pixels["motion_0_to_1"][..., :2]
                        )[joint_valid]
                        max_same_pixel_negation_error = float(np.max(np.abs(residual)))
                add_check(
                    checks,
                    f"{prefix}.bidirectional_motion_not_fabricated_by_pixelwise_negation",
                    available
                    and joint_valid_count > 0
                    and max_same_pixel_negation_error > 1e-3,
                    f"available={available} jointValid={joint_valid_count} "
                    f"maxAbs(motion0To1+motion1To0)={max_same_pixel_negation_error}",
                )

    complete_coverage = not declared_gaps and project_wpo_claimed and project_skeletal_claimed and project_animated_material_claimed and hudless_world_widget_rejection_claimed
    if complete_coverage:
        required_controls = ("bRunSceneControlPreflight", "bRequireSceneControlPreflight", "bLockExposure", "bLockMaterialTimeToLogicalFrame", "bLockTemporalJitterToLogicalFrame", "bRejectVisibleWidgetComponents", "bDisableMotionBlur", "bBlockOnStreamingBeforeCapture")
        for role in ("forward", "reverse", "midpoint"):
            controls = manifest.get("sourceCaptureControls", {}).get(role, {})
            job, preflight = controls.get("job", {}), controls.get("sceneControlPreflight", {})
            valid = all(job.get(k) is True for k in required_controls) and job.get("bAllowDynamicInstanceIdTopology") is False and preflight.get("passed") is True and preflight.get("materialCompileErrorsChecked") is True
            add_check(checks, f"contract.supported_scope_controls.{role}", valid, "Strict preflight, fixed component identity, logical time/jitter/exposure and isolated UI")
    passed = all(check["passed"] for check in checks)
    admitted = passed and complete_coverage
    scope = {
        "contract": "nr-fg-data-v1",
        "engineWorkflow": "UE 5.7 Windows Editor Main View; independent forward/reverse/midpoint replays",
        "scenePolicy": "strict scene-control preflight, fixed topology, stable uint8 component identities",
        "trainingPixelPolicy": "use motion/visibility only where their validity is one; exclude reactive/transparency risk pixels from opaque correspondence losses",
        "visibilityLimit": "component correspondence and tested skeletal reveal; unknown same-component self-occlusion remains invalid",
        "replayCoverage": "project AnimBP pose cache, controlled-lighting animated material, original sinusoidal WPO panel",
        "exclusions": "arbitrary temporal lighting equality, external/random state and unvalidated custom simulation/WPO are not certified",
    }
    report = {
        "validatorVersion": 8,
        "manifestSha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
        "validatorSourceSha256": hashlib.sha256(b"".join(Path(__file__).with_name(name).read_bytes() for name in ("ValidateFrameGenerationDataset.py", "VerifyProjectWPO.py", "ValidateTemporalAcceptance.py", "TemporalGeometry.py", "ValidateDataset.py"))).hexdigest(),
        "dataset": str(dataset),
        "contractVersion": manifest.get("contractVersion"),
        "certificationStatus": "validated_supported_scope" if admitted else "experimental_uncertified",
        "frameGenerationCertified": admitted,
        "formatAndIntegrityGate": "pass" if passed else "fail",
        "certificationGate": "supported_scope_pass" if admitted else "not_certified",
        "certificationScope": scope if admitted else None,
        "checksPassed": sum(check["passed"] for check in checks),
        "checksTotal": len(checks),
        "checks": checks,
        "statistics": statistics,
        "missingRequirements": sorted(declared_gaps),
        "note": (
            "A passing integrity gate proves that the isolated forward-endpoint, reverse-endpoint, "
            "and intermediate replays were "
            "assembled without path, hash, shape, numeric, timing, matrix, or declared-provenance "
            "violations. Admission applies only to certificationScope when every evidence and integrity gate passes; the producer manifest never certifies itself."
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
    state = ("PASS (SUPPORTED SCOPE)" if report["frameGenerationCertified"] else "PASS (UNCERTIFIED)") if passed else "FAIL"
    print(f"{state}: {report['checksPassed']}/{report['checksTotal']} integrity checks; report={report_path}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
