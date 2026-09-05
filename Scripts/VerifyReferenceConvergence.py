#!/usr/bin/env python3
"""Compare native/1/16-sample HR to 64-sample HR on the same frozen scene.

This measures raster-sampling convergence, not physical path-tracing accuracy.
Run ValidateDataset.py on all three inputs before using this report as evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from ValidateDataset import exr_rgba, safe_dataset_file


def verify(roots):
    manifests = {n: json.loads((root / "manifest.json").read_text(encoding="utf-8")) for n, root in roots.items()}
    excluded = {"jobName", "outputDirectory", "referenceTemporalSamples"}
    jobs = [{k: v for k, v in m["job"].items() if k not in excluded} for m in manifests.values()]
    if any(job != jobs[0] for job in jobs):
        raise ValueError("Only identity/output and reference sample count may differ")
    provenance = [{k: v for k, v in m["provenance"].items() if k != "captureConfigSha1"} for m in manifests.values()]
    if any(p != provenance[0] for p in provenance):
        raise ValueError("Engine, source, scene assets, streaming or render settings changed")
    ids = [[f["logicalFrameId"] for f in m["frames"]] for m in manifests.values()]
    if not ids[0] or any(value != ids[0] for value in ids):
        raise ValueError("Logical frames differ or are empty")
    report = {"schema": "sr-reference-convergence-v1", "manifestSha256": {str(n): hashlib.sha256((p / "manifest.json").read_bytes()).hexdigest() for n, p in roots.items()}, "reference": "64 frozen-time raster samples; no path-tracing claim", "frames": []}
    for index, frame_id in enumerate(ids[0]):
        frames = {n: m["frames"][index] for n, m in manifests.items()}
        target = exr_rgba(safe_dataset_file(roots[64], frames[64]["files"]["color_hr_reference_scene_hdr"]))[..., :3].astype(np.float64)
        row = {"logicalFrameId": frame_id, "errorsTo64": {}, "mainViewHashesEqual": {k: len({frames[n]["sha1"][k] for n in roots}) == 1 for k in ("color_lr_scene_hdr", "depth_device_raw", "motion_full_current_to_previous", "object_id")}}
        for name, n, modality in (("native", 16, "color_hr_native_scene_hdr"), ("reference1", 1, "color_hr_reference_scene_hdr"), ("reference16", 16, "color_hr_reference_scene_hdr")):
            actual = exr_rgba(safe_dataset_file(roots[n], frames[n]["files"][modality]))[..., :3].astype(np.float64)
            error = actual - target
            row["errorsTo64"][name] = {"mse": float(np.mean(error * error)), "mae": float(np.mean(np.abs(error)))}
        errors = row["errorsTo64"]
        row["passed"] = bool(all(row["mainViewHashesEqual"].values()) and errors["reference16"]["mse"] < .25 * errors["native"]["mse"] and errors["reference16"]["mse"] < .5 * errors["reference1"]["mse"])
        report["frames"].append(row)
    report["passed"] = all(r["passed"] for r in report["frames"])
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for n in (1, 16, 64):
        parser.add_argument(f"--samples-{n}", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    result = verify({n: getattr(args, f"samples_{n}").resolve() for n in (1, 16, 64)})
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if result['passed'] else 'FAIL'}: {len(result['frames'])} reference convergence frames")
    raise SystemExit(0 if result["passed"] else 1)
