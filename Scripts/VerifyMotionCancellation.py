"""Physical same-speed camera/object test for the generated SameSpeed sequence."""
import argparse
import json
from pathlib import Path

import numpy as np
from ValidateTemporalAcceptance import erode, read


def verify(root):
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    results = []
    for previous, frame in zip(manifest["frames"], manifest["frames"][1:]):
        depth = read(root, frame, "depth_view_linear_meters", "R")[..., 0]
        ids = read(root, frame, "object_id", "R")[..., 0]
        motion = read(root, frame, "motion_full_current_to_previous", "RG")
        coverage = read(root, frame, "velocity_coverage", "R")[..., 0]
        height, width = depth.shape
        panel_id = ids[height // 2, width // 2]
        panel = erode((ids == panel_id) & (np.abs(depth - 5.95) < .002), 3)
        background = erode((np.abs(depth - 9.95) < .002) & (coverage < .5), 3)
        delta = np.asarray(frame["camera"]["locationCm"]) - previous["camera"]["locationCm"]
        # Previous minus current projection: (Y-Cprev)-(Y-Ccurr) = Ccurr-Cprev.
        expected_background = delta[1] * frame["temporalDiagnostics"]["displaySize"][0] / 2 / 995
        panel_error = float(np.max(np.abs(motion[panel]))) if panel.any() else float("inf")
        background_error = float(np.max(np.abs(motion[background] - [expected_background, 0]))) if background.any() else float("inf")
        result = dict(frame=frame["frame"], panelPixels=int(panel.sum()), backgroundPixels=int(background.sum()), nativeVelocityCoverage=float(coverage[panel].mean()) if panel.any() else 0, panelMaxMotionDisplayPixels=panel_error, backgroundMaxErrorDisplayPixels=background_error, expectedBackgroundMotionDisplayPixels=float(expected_background), cameraDeltaCm=delta.tolist())
        result["passed"] = bool(not frame["reset"] and panel.sum() > 400 and background.sum() > 1000 and np.max(np.abs(delta - [0, 240 / 63, 0])) < .001 and result["nativeVelocityCoverage"] > .99 and panel_error < .02 and background_error < .002)
        results.append(result)
    return dict(schema="sr-motion-cancellation-v1", scope="same_speed_rigid_panel_and_camera_with_independent_static_background", passed=bool(results) and all(r["passed"] for r in results), frames=results)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.dataset)
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))
    raise SystemExit(0 if result["passed"] else 1)
