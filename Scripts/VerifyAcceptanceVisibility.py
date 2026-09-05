"""Check revealed background against authored geometry, without output motion/IDs.

The previous sample location comes from the known background plane and camera
translation. A conservative interior of the eight-vertex panel silhouette is
used to avoid assigning ground truth to subpixel silhouette boundaries.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from ValidateTemporalAcceptance import color_masks, erode, jitter_from_projection, panel_bounds, read


def verify(root):
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    previous = None
    frames = []
    for frame in manifest["frames"]:
        if previous is None or frame["reset"]:
            previous = frame
            continue
        rgb = read(root, frame, "color_lr_scene_hdr")
        _, background = color_masks(rgb)
        height, width = rgb.shape[:2]
        y, x = np.mgrid[:height, :width]
        jitter = jitter_from_projection(frame["temporalDiagnostics"], (width, height))
        prev_jitter = jitter_from_projection(previous["temporalDiagnostics"], (width, height))
        camera_delta = frame["camera"]["locationCm"][1] - previous["camera"]["locationCm"][1]
        px = np.floor(x + camera_delta*width/2/995 + prev_jitter[0]-jitter[0]+.5)+.5
        py = np.floor(y + prev_jitter[1]-jitter[1]+.5)+.5
        panel_y = -120+240*previous["logicalFrameId"]/63
        bounds = panel_bounds(previous["camera"]["locationCm"][1], panel_y, (width, height), prev_jitter)
        revealed = erode(background, 1) & (px > bounds[0]+.75) & (px < bounds[1]-.75) & (py > bounds[2]+.75) & (py < bounds[3]-.75)
        reject = read(root, frame, "history_rejection_mask", "R")[..., 0]
        valid = read(root, frame, "history_rejection_valid", "R")[..., 0]
        frames.append(dict(frame=frame["frame"], expectedRevealedPixels=int(revealed.sum()),
                           validRejections=int((revealed & (reject > .5) & (valid > .5)).sum())))
        previous = frame
    count = sum(f["expectedRevealedPixels"] for f in frames)
    correct = sum(f["validRejections"] for f in frames)
    return dict(dataset=str(root.resolve()), expectedRevealedPixels=count, validRejections=correct,
                passed=count > 100 and count == correct, frames=frames,
                scope="authored ObjectOnly/Mixed panel; previous silhouette interior; excludes ambiguous boundary pixels")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.dataset)
    args.report.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "frames"}))
    raise SystemExit(0 if result["passed"] else 1)
