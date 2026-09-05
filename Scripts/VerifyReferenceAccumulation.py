#!/usr/bin/env python3
"""Independently resolve exported reference samples; never modify source images.

Use bSaveReferenceSubsamples=true and referenceResizeFilter=Box for this audit.
Production may use Lanczos4; Box makes the resolve independently reproducible
with a pixel-area integral, without importing Unreal's resampling implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from ValidateDataset import exr_rgba, safe_dataset_file


def verify(root):
    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    job = manifest["job"]
    if not job.get("bSaveReferenceSubsamples") or job["referenceResizeFilter"] != "Box":
        raise ValueError("This independent arithmetic audit requires saved subsamples and Box resolve")
    count, scale = job["referenceTemporalSamples"], job["referenceHRScale"]
    checks = []
    for frame in manifest["frames"]:
        total = None
        for index in range(count):
            name = f"reference_subsample_{index:02d}"
            path = safe_dataset_file(root, frame["files"][name])
            if hashlib.sha1(path.read_bytes()).hexdigest().upper() != frame["sha1"][name].upper():
                raise ValueError(f"Source sample hash mismatch: {path}")
            pixels = exr_rgba(path)[..., :3].astype(np.float64)
            total = pixels if total is None else total + pixels
        mean = total / count
        height, width, channels = mean.shape
        expected = mean.reshape(height // scale, scale, width // scale, scale, channels).mean(axis=(1, 3))
        actual = exr_rgba(safe_dataset_file(root, frame["files"]["color_hr_reference_scene_hdr"]))[..., :3]
        error = np.abs(actual - expected)
        # Output EXR may round to float16; use its per-value ULP, not a
        # scene-specific error threshold. The summation itself is float32.
        tolerance = np.maximum(np.abs(expected) * 0.001, 2e-6)
        checks.append({"logicalFrameId": frame["logicalFrameId"], "passed": bool(np.isfinite(error).all() and (error <= tolerance).all()), "maxAbs": float(error.max()), "meanAbs": float(error.mean()), "samples": count})
    return {"schema": "sr-reference-arithmetic-v1", "manifestSha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(), "passed": bool(checks) and all(x["passed"] for x in checks), "checks": checks}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = verify(args.dataset.resolve())
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))
    raise SystemExit(0 if report["passed"] else 1)
