"""Independent saved-data consumer for GenerateTemporalAcceptanceAssets.py.

This is a bounded physical acceptance test, not a temporal SR implementation or
general scene certificate. It does not import the capture validator or use its
history-rejection output to choose photometric evaluation pixels.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import OpenEXR
from PIL import Image

# Fixed before the 1080p acceptance results are examined. Photometric warp scores
# use a 3x3 box prefilter to avoid treating point-sampled checkerboard aliasing as
# correspondence error. The original EXRs remain unchanged; raw scores are also
# reported. Flat-color, geometry, depth and motion checks use the original data.
LIMITS = dict(flat_mae=.002, flat_p99=.01, motion_p99_display_pixels=.05,
              depth_error_m=.002, reference_edge_error_pixels=.75,
              lr_edge_error_pixels=.75, warp_over_no_warp=.8,
              warp_over_wrong_motion=.8, warp_over_omitted_jitter=.9)


def read(root, frame, modality, channels="RGB"):
    with OpenEXR.File(str(root / frame["files"][modality]), separate_channels=True) as exr:
        return np.stack([exr.channels()[c].pixels.astype(np.float32) for c in channels], axis=-1)


def box3(a):
    p = np.pad(a, ((1, 1), (1, 1), (0, 0)), mode="edge")
    return sum(p[y:y+a.shape[0], x:x+a.shape[1]] for y in range(3) for x in range(3)) / 9


def erode(a, radius=2):
    p = np.pad(a, radius, constant_values=False)
    return np.logical_and.reduce([p[y:y+a.shape[0], x:x+a.shape[1]]
                                  for y in range(radius*2+1) for x in range(radius*2+1)])


def sample(a, x, y):
    h, w = a.shape[:2]
    x, y = np.clip(x, 0, w-1), np.clip(y, 0, h-1)
    x0, y0 = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
    x1, y1 = np.minimum(x0+1, w-1), np.minimum(y0+1, h-1)
    fx, fy = (x-x0)[..., None], (y-y0)[..., None]
    return (a[y0, x0]*(1-fx)+a[y0, x1]*fx)*(1-fy)+(a[y1, x0]*(1-fx)+a[y1, x1]*fx)*fy


def jitter_from_projection(metadata, size):
    p = np.array(metadata["viewToClipCurrentJittered"]).reshape(4, 4)
    q = np.array(metadata["viewToClipCurrentUnjittered"]).reshape(4, 4)
    # Project a view-space point. No serialized jitter fields are consulted.
    probe = np.array([0., 0., 100., 1.])
    a, b = probe @ p, probe @ q
    return (a[:2]/a[3]-b[:2]/b[3]) * np.array([size[0]/2, -size[1]/2])


def color_masks(rgb):
    red = (rgb[..., 0] > .2) & (rgb[..., 0] > rgb[..., 1]*3)
    background = (np.abs(rgb[..., 0]-rgb[..., 1]) < .01) & (rgb[..., 0] < .8)
    return red, background


def bounds(mask):
    y, x = np.nonzero(mask)
    if not len(x):
        raise ValueError("Expected acceptance panel is absent")
    # Pixel centers are index + .5; outer silhouette edges bracket the covered cells.
    return np.array([x.min(), x.max()+1, y.min(), y.max()+1], dtype=float)


def panel_bounds(camera_y, panel_y, size, jitter=(0, 0)):
    w, h = size
    focal = w/2
    # Silhouette is the union of all eight vertices, including the back corners
    # visible on the side of the ten-centimetre-thick panel.
    horizontal = [w/2+(panel_y+edge-camera_y)*focal/depth for edge in (-80, 80) for depth in (595, 605)]
    vertical = [h/2+edge*focal/depth for edge in (-110, 110) for depth in (595, 605)]
    return np.array([min(horizontal), max(horizontal), min(vertical), max(vertical)]) + np.array([jitter[0], jitter[0], jitter[1], jitter[1]])


def panel_ray_depth(camera_y, panel_y, size, jitter):
    w, h = size
    y, x = np.mgrid[:h, :w]
    dy = (x+.5-jitter[0]-w/2)/(w/2)
    dz = -(y+.5-jitter[1]-h/2)/(w/2)
    with np.errstate(divide="ignore", invalid="ignore"):
        enter_y = np.minimum((panel_y-80-camera_y)/dy, (panel_y+80-camera_y)/dy)
        enter_z = np.minimum(-110/dz, 110/dz)
    return np.maximum(595, np.maximum(enter_y, enter_z))/100


def summarize(values):
    values = np.asarray(values, dtype=float)
    return dict(mean=float(values.mean()), maximum=float(values.max()), minimum=float(values.min())) if values.size else None


def validate(root, output):
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    frames = manifest["frames"]
    name = Path(manifest["job"].get("sequence", "").split(".")[0]).name
    lab = manifest["world"] == "/Game/SRDatasetAcceptance/TemporalLab"
    camera_motion = name in ("CameraOnly", "CameraFast", "Mixed")
    object_motion = name in ("ObjectOnly", "Mixed")
    metrics = {key: [] for key in ("flatMae", "flatP99", "referenceEdgeErrorPixels", "lrEdgeErrorPixels", "thinLineCenterErrorPixels", "depthErrorM", "motionP99DisplayPixels")}
    scores = {region: {kind: [] for kind in ("noWarp", "correct", "wrongMotion", "omittedJitter", "wrongJitter", "rawNoWarp", "rawCorrect")}
              for region in ("background", "object", "all")}
    resets, cuts, jitters, accepted_history_frames = [], [], [], []
    checks = []
    previous = None
    output.mkdir(parents=True, exist_ok=True)
    for index, frame in enumerate(frames):
        t = frame["temporalDiagnostics"]
        rgb = read(root, frame, "color_lr_scene_hdr")
        h, w = rgb.shape[:2]
        display = t["displaySize"]
        j = jitter_from_projection(t, (w, h))
        jitters.append(j.tolist())
        ref = read(root, frame, "color_hr_reference_scene_hdr")
        y, x = np.mgrid[:h, :w].astype(np.float32)
        aligned = sample(ref, (x+.5-j[0])*display[0]/w-.5, (y+.5-j[1])*display[1]/h-.5)
        local_max = np.maximum.reduce([np.roll(aligned, (dy, dx), (0, 1)) for dy, dx in ((0, 0), (0, 2), (0, -2), (2, 0), (-2, 0))])
        local_min = np.minimum.reduce([np.roll(aligned, (dy, dx), (0, 1)) for dy, dx in ((0, 0), (0, 2), (0, -2), (2, 0), (-2, 0))])
        flat = erode(np.max(local_max-local_min, axis=-1) < .002)
        if lab:
            difference = np.abs(rgb-aligned).mean(axis=-1)[flat]
            metrics["flatMae"].append(float(difference.mean()))
            metrics["flatP99"].append(float(np.percentile(difference, 99)))
        red, background = color_masks(rgb)
        depth = read(root, frame, "depth_view_linear_meters", "R")[..., 0]
        cam_y = frame["camera"]["locationCm"][1]
        panel_y = -120+240*frame["logicalFrameId"]/63 if object_motion else -100
        if lab:
            ref_red, _ = color_masks(ref)
            metrics["referenceEdgeErrorPixels"].append(float(np.max(np.abs(bounds(ref_red)-panel_bounds(cam_y, panel_y, display)))))
            metrics["lrEdgeErrorPixels"].append(float(np.max(np.abs(bounds(red)-panel_bounds(cam_y, panel_y, (w, h), j)))))
            expected_panel_depth = panel_ray_depth(cam_y, panel_y, (w, h), j)
            for mask, expected in ((erode(red), expected_panel_depth), (erode(background), np.full_like(depth, 9.95))):
                metrics["depthErrorM"].append(float(np.max(np.abs(depth[mask]-expected[mask]))))
            for line in range(3):
                width = (.6, 1.2, 2.4)[line]
                corners = [display[0]/2+(150+45*line+edge-cam_y)*(display[0]/2)/distance
                           for edge in (-width/2, width/2) for distance in (499.5, 500.5)]
                expected = (min(corners)+max(corners))/2
                lo, hi = int(expected-5), int(expected+6)
                row = ref[display[1]//2-20:display[1]//2+20, lo:hi].mean(axis=(0, 2))
                # Bright line support; Lanczos side lobes are excluded.
                weights = np.maximum(row-.75, 0)
                if weights.sum() > 0:
                    centroid = float(np.dot(np.arange(lo, hi)+.5, weights)/weights.sum())
                    metrics["thinLineCenterErrorPixels"].append(abs(centroid-expected))
                else:
                    metrics["thinLineCenterErrorPixels"].append(float("inf"))
        if frame["reset"]:
            resets.append(index)
            reject = read(root, frame, "history_rejection_mask", "R")[..., 0]
            checks.append((f"reset_{index}_rejects_all_history", bool(np.all(reject > .5) and not frame["motionTrainingUsable"])))
        if frame.get("rendererCameraCut"):
            cuts.append(index)
        motion = read(root, frame, "motion_full_current_to_previous", "RG")
        ids = read(root, frame, "object_id", "R")[..., 0]
        smooth = box3(rgb)
        if previous is not None and not frame["reset"]:
            prev, prev_rgb, prev_smooth, prev_id, prev_j, prev_red, prev_bg = previous
            offset = prev_j-j
            mx, my = motion[..., 0]*w/display[0], motion[..., 1]*h/display[1]
            px, py = x+mx+offset[0], y+my+offset[1]
            nx, ny = np.clip(np.floor(px+.5).astype(int), 0, w-1), np.clip(np.floor(py+.5).astype(int), 0, h-1)
            inside = (px >= 2) & (px < w-3) & (py >= 2) & (py < h-3)
            same = inside & (ids == prev_id[ny, nx]) & (ids > 0)
            masks = dict(background=erode(background) & erode(prev_bg)[ny, nx] & same,
                         object=erode(red) & erode(prev_red)[ny, nx] & same,
                         all=erode(same))
            coordinates = dict(correct=(px, py), wrongMotion=(x-mx+offset[0], y-my+offset[1]),
                               omittedJitter=(x+mx, y+my), wrongJitter=(x+mx-offset[0], y+my-offset[1]))
            errors = {kind: np.abs(sample(prev_smooth, *coord)-smooth).mean(axis=-1) for kind, coord in coordinates.items()}
            errors["noWarp"] = np.abs(prev_smooth-smooth).mean(axis=-1)
            errors["rawNoWarp"] = np.abs(prev_rgb-rgb).mean(axis=-1)
            errors["rawCorrect"] = np.abs(sample(prev_rgb, px, py)-rgb).mean(axis=-1)
            for region, mask in masks.items():
                if mask.sum() > 100:
                    for kind, error in errors.items():
                        scores[region][kind].append(float(error[mask].mean()))
            accepted_history_frames.append(index)
            if lab:
                cam_delta = cam_y-prev["camera"]["locationCm"][1]
                obj_delta = 240/63 if object_motion else 0
                for region, distance, object_delta in (("background", 995, 0), ("object", 595, obj_delta)):
                    distance_field = expected_panel_depth*100 if region == "object" else np.full_like(depth, distance)
                    expected_x = (cam_delta-object_delta)*display[0]/2/distance_field
                    deviation = np.maximum(np.abs(motion[..., 0]-expected_x), np.abs(motion[..., 1]))[masks[region]]
                    metrics["motionP99DisplayPixels"].append(float(np.percentile(deviation, 99)))
            if index == 7:
                for kind in ("noWarp", "correct", "wrongMotion", "omittedJitter"):
                    heat = np.clip(errors[kind]*10, 0, 1)
                    heat = np.stack([heat, heat*.2, np.zeros_like(heat)], axis=-1)
                    Image.fromarray((heat*255).astype(np.uint8)).save(output/f"frame_000007_{kind}.png")
                Image.fromarray((np.clip(rgb, 0, 1)**(1/2.2)*255).astype(np.uint8)).save(output/"frame_000007_input.png")
        previous = frame, rgb, smooth, ids, j, red, background
        print(f"{root.name}: {index+1}/{len(frames)}", flush=True) if (index+1) % 16 == 0 else None
    checks.append(("eight_jitter_phases", len({tuple(np.round(j, 5)) for j in jitters}) == 8))
    if lab:
        checks += [("actual_reset_indices", resets == ([0, 12] if name == "CameraCut" else [0])),
                   ("renderer_reports_sequence_cut", 12 in cuts if name == "CameraCut" else not cuts)]
        for key, limit in (("flatMae", LIMITS["flat_mae"]), ("flatP99", LIMITS["flat_p99"]),
                           ("referenceEdgeErrorPixels", LIMITS["reference_edge_error_pixels"]),
                           ("lrEdgeErrorPixels", LIMITS["lr_edge_error_pixels"]),
                           ("thinLineCenterErrorPixels", LIMITS["reference_edge_error_pixels"]),
                           ("depthErrorM", LIMITS["depth_error_m"]),
                           ("motionP99DisplayPixels", LIMITS["motion_p99_display_pixels"])):
            checks.append((key, bool(metrics[key] and max(metrics[key]) <= limit)))
    aggregate = {region: {kind: summarize(values) for kind, values in kinds.items()} for region, kinds in scores.items()}
    primary = "object" if object_motion else "background"
    s = aggregate[primary]
    if lab and (camera_motion or object_motion):
        for other, threshold in (("noWarp", LIMITS["warp_over_no_warp"]), ("omittedJitter", LIMITS["warp_over_omitted_jitter"])):
            checks.append((f"{primary}_correct_better_than_{other}", s["correct"]["mean"] <= s[other]["mean"]*threshold))
        if camera_motion or object_motion:
            checks.append((f"{primary}_correct_better_than_wrong_motion", s["correct"]["mean"] <= s["wrongMotion"]["mean"]*LIMITS["warp_over_wrong_motion"]))
    if lab and not (camera_motion or object_motion):
        # Static/cut clips establish zero geometric motion, correct raster phase,
        # stable reference and reset isolation. A nearest-sampled step edge need
        # not lower L1 error under interpolation; do not turn that into a motion
        # failure. The opposite raster phase must still be visibly worse.
        checks.append(("static_correct_better_than_wrong_jitter", s["correct"]["mean"] < s["wrongJitter"]["mean"]*.8))
    report = dict(schemaVersion=2, dataset=str(root.resolve()), scope="controlled_pinhole_scene" if lab else "project_scene_diagnostics_only",
                  limits=LIMITS, frameCount=len(frames), resetIndices=resets, rendererCutIndices=cuts,
                  historyUsedForFrames=accepted_history_frames, checks=[dict(name=k, passed=bool(v)) for k, v in checks],
                  metrics={k: summarize(v) for k, v in metrics.items()}, photometricScores=aggregate,
                  photometricFilter="3x3_box_before_bilinear_warp; raw scores also included", passed=all(v for _, v in checks))
    (output/"acceptance.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(dict(passed=report["passed"], failed=[k for k, v in checks if not v])))
    return report["passed"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(0 if validate(args.dataset, args.output) else 1)
