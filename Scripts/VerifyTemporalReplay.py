"""Byte-compare all saved modalities and verify actual auxiliary submission order."""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    result = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b""):
            result.update(chunk)
    return result.hexdigest().upper()


def verify(base_path, comparison_path, reverse_order):
    base = json.loads((base_path/"manifest.json").read_text(encoding="utf-8"))
    comparison = json.loads((comparison_path/"manifest.json").read_text(encoding="utf-8"))
    mismatches = []
    pairs = 0
    if base["state"] != "Completed" or comparison["state"] != "Completed":
        mismatches.append(dict(field="completed_state"))
    if len(base["frames"]) != len(comparison["frames"]):
        mismatches.append(dict(field="frame_count"))
    for a, b in zip(base["frames"], comparison["frames"]):
        if set(a["files"]) != set(b["files"]):
            mismatches.append(dict(frame=a["frame"], field="modality_set"))
            continue
        for modality, relative in a["files"].items():
            first = digest(base_path/relative)
            second = digest(comparison_path/b["files"][modality])
            pairs += 1
            if not (first == second == a["sha1"][modality] == b["sha1"][modality]):
                mismatches.append(dict(frame=a["frame"], modality=modality))
        for field in ("logicalFrameId", "camera", "sceneStateSha1", "reset", "rendererCameraCut"):
            if a[field] != b[field]:
                mismatches.append(dict(frame=a["frame"], field=field))
        for field in ("renderSize", "displaySize", "preExposure", "jitterCurrentNDC", "jitterPreviousNDC",
                      "viewToClipCurrentJittered", "viewToClipPrevious", "translatedWorldToViewCurrent",
                      "translatedWorldToViewPrevious", "worldViewOriginHighCurrent", "worldViewOriginLowCurrent",
                      "worldViewOriginHighPrevious", "worldViewOriginLowPrevious"):
            if a["temporalDiagnostics"][field] != b["temporalDiagnostics"][field]:
                mismatches.append(dict(frame=a["frame"], temporalField=field))
        first_order = [s["modality"] for s in a["renderSubmissions"]]
        second_order = [s["modality"] for s in b["renderSubmissions"]]
        reference = [name for name in first_order if name.startswith("hr_reference")]
        other = [name for name in first_order if name not in {"hr", "lr", "depth"} and name not in reference]
        expected = ["lr", "depth", "hr"] + reference + other if reverse_order else first_order
        if second_order != expected or (reverse_order and first_order == second_order):
            mismatches.append(dict(frame=a["frame"], field="actual_submission_order"))
    return dict(base=str(base_path.resolve()), comparison=str(comparison_path.resolve()),
                frames=len(comparison["frames"]), modalityPairsChecked=pairs, mismatches=mismatches,
                reverseAuxiliaryOrderRequired=reverse_order, passed=not mismatches)


def write_replay_heatmaps(base_path, comparison_path, output):
    """Save actual HDR/depth/motion differences; preserve both source captures."""
    import numpy as np
    from ValidateTemporalAcceptance import read
    from ValidateDataset import write_heatmap
    output.mkdir(parents=True, exist_ok=False)
    first = json.loads((base_path / "manifest.json").read_text(encoding="utf-8"))
    second = json.loads((comparison_path / "manifest.json").read_text(encoding="utf-8"))
    rows = []
    for a, b in zip(first["frames"], second["frames"]):
        for modality, channels in (("color_lr_scene_hdr", "RGB"), ("depth_view_linear_meters", "R"), ("motion_full_current_to_previous", "RG")):
            left, right = read(base_path, a, modality, channels), read(comparison_path, b, modality, channels)
            if channels == "R": left, right = np.repeat(left, 3, axis=-1), np.repeat(right, 3, axis=-1)
            if channels == "RG": left, right = np.pad(left, ((0,0),(0,0),(0,1))), np.pad(right, ((0,0),(0,0),(0,1)))
            path = output / f"frame_{a['frame']:06d}_{modality}.png"
            write_heatmap(left, right, path)
            rows.append({"frame":a["frame"], "modality":modality, "maxAbs":float(np.max(np.abs(left-right))), "file":path.name, "sha1":digest(path)})
    result = {"colorScale":"black is exact zero; nonzero error normalized by each image's p99 error", "frames":len(first["frames"]), "images":rows}
    (output / "heatmaps.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return str(output.resolve())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("comparison", type=Path)
    parser.add_argument("--reverse-order", action="store_true")
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--heatmaps", type=Path, help="New directory for per-frame HDR/depth/motion error images")
    args = parser.parse_args()
    result = verify(args.base, args.comparison, args.reverse_order)
    if args.heatmaps:
        result["heatmaps"] = write_replay_heatmaps(args.base, args.comparison, args.heatmaps)
    args.report.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(result))
    raise SystemExit(0 if result["passed"] else 1)
