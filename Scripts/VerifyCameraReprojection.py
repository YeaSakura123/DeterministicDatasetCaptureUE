"""Validate camera fallback via depth -> world point -> previous camera projection."""
import argparse
import json
from pathlib import Path

import numpy as np

from ValidateTemporalAcceptance import read


def verify(root):
    manifest = json.loads((root/"manifest.json").read_text(encoding="utf-8"))
    results = []
    for previous, frame in zip(manifest["frames"], manifest["frames"][1:]):
        if frame["reset"]:
            continue
        current, prev = frame["temporalDiagnostics"], previous["temporalDiagnostics"]
        depth = read(root, frame, "depth_device_raw", "R")[..., 0]
        motion = read(root, frame, "motion_full_current_to_previous", "RG")
        coverage = read(root, frame, "velocity_coverage", "R")[..., 0]
        height, width = depth.shape
        y, x = np.mgrid[:height, :width]
        clip = np.stack(((x+.5)/width*2-1, 1-(y+.5)/height*2, depth, np.ones_like(depth)), axis=-1)
        view = clip @ np.linalg.inv(np.array(current["viewToClipCurrentJittered"]).reshape(4, 4))
        view /= view[..., 3:4]
        world = view @ np.array(current["viewToTranslatedWorldCurrent"]).reshape(4, 4)
        world[..., :3] += np.array(current["worldViewOriginHighCurrent"])+current["worldViewOriginLowCurrent"]
        world[..., :3] -= np.array(prev["worldViewOriginHighCurrent"])+prev["worldViewOriginLowCurrent"]
        old_view = world @ np.array(prev["translatedWorldToViewCurrent"]).reshape(4, 4)
        old_clip = old_view @ np.array(prev["viewToClipCurrentUnjittered"]).reshape(4, 4)
        current_clip = view @ np.array(current["viewToClipCurrentUnjittered"]).reshape(4, 4)
        display = current["displaySize"]
        expected = (old_clip[..., :2]/old_clip[..., 3:4]-current_clip[..., :2]/current_clip[..., 3:4])*np.array([display[0]/2, -display[1]/2])
        valid = (depth > 0) & (coverage < .5)
        error = np.max(np.abs(motion-expected), axis=-1)[valid]
        previous_depth = read(root, frame, "depth_previous_reprojected_device", "R")[..., 0]
        depth_error = np.abs(previous_depth-old_clip[..., 2]/old_clip[..., 3])[valid]
        results.append(dict(frame=frame["frame"], fallbackPixels=int(valid.sum()), motionMaxDisplayPixels=float(error.max()), previousDepthMax=float(depth_error.max())))
    return dict(frames=results, passed=bool(results) and all(f["fallbackPixels"] > 100 and f["motionMaxDisplayPixels"] < .001 and f["previousDepthMax"] < 1e-6 for f in results))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.dataset)
    args.report.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(result))
    raise SystemExit(0 if result["passed"] else 1)
