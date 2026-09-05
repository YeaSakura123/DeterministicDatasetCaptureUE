"""Independent physical check for GenerateFormalDatasetAssets.py's WPO panel.

Requires the fixed camera (0,-250,170), FOV 90 and the original Gallery panel.
Checks the actual rendered green material, front-face depth, sinusoidal silhouette
position and signed endpoint velocity. This is a bounded asset test, not a claim
that arbitrary WPO materials are correct.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from ValidateTemporalAcceptance import erode, read


def verify(roots):
    rows, provenance, roles = [], [], set()
    for root in roots:
        path = root / "manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        roles.add(manifest["replayPass"])
        provenance.append({k: manifest["provenance"][k] for k in ("pluginBinarySha1", "loadedContentSha1")})
        for frame in manifest["frames"]:
            camera, diag = frame["camera"], frame["temporalDiagnostics"]
            camera_ok = camera["locationCm"] == [0, -250, 170] and camera["rotationDeg"] == [0, 0, 0] and abs(camera["fovDegrees"] - 90) < 1e-4
            depth = read(root, frame, "depth_view_linear_meters", "R")[..., 0]
            base = read(root, frame, "base_color_linear")
            panel = (np.abs(depth - 5.36) < .001) & (np.max(np.abs(base - [.06, .5, .12]), axis=-1) < .015)
            interior = erode(panel, 3)
            height, width = panel.shape
            y, x = np.where(panel)
            t, previous = frame["materialTimeSeconds"], frame["materialPreviousTimeSeconds"]
            expected_x = width / 2 + 40 * np.sin(2 * np.pi * t) * (width / 2) / 536 + diag["jitterCurrentRenderPixel"][0]
            center_error = abs((float(x.min() + x.max()) / 2 + .5) - expected_x) if x.size else float("inf")
            expected_motion = (40 * np.sin(2 * np.pi * previous) - 40 * np.sin(2 * np.pi * t)) * (diag["displaySize"][0] / 2) / 536
            motion = read(root, frame, "motion_full_current_to_previous", "RG")
            coverage = read(root, frame, "velocity_coverage", "R")[..., 0]
            error = float(np.max(np.abs(motion[interior] - [expected_motion, 0]))) if interior.any() else float("inf")
            coverage_ratio = float(coverage[interior].mean()) if interior.any() else 0
            # Reset buffers are explicitly unusable; UE may retain warmup velocity.
            motion_ok = not frame["motionTrainingUsable"] if frame["reset"] else error < .02 and coverage_ratio > .99
            rows.append(dict(role=manifest["replayPass"], frame=frame["frame"], manifestSha256=hashlib.sha256(path.read_bytes()).hexdigest(), panelPixels=int(interior.sum()), expectedDisplayMotionX=float(expected_motion), maxMotionErrorDisplayPixels=error, velocityCoverage=coverage_ratio, silhouetteCenterErrorRenderPixels=center_error, reset=frame["reset"], passed=bool(camera_ok and interior.sum() > 500 and center_error <= .55 and motion_ok)))
    complete = roles == {"FrameGenerationEndpoints", "FrameGenerationReverseEndpoints", "FrameGenerationIntermediate"}
    matched = bool(provenance) and all(p == provenance[0] for p in provenance)
    return dict(schema="sr-project-wpo-physical-v1", passed=bool(rows) and complete and matched and all(r["passed"] for r in rows), sameBinaryAndContent=matched, completeReplayRoles=complete, provenance=provenance, frames=rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("datasets", type=Path, nargs=3)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.datasets)
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))
    raise SystemExit(0 if result["passed"] else 1)
