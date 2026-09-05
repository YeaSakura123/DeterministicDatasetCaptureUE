#!/usr/bin/env python3
"""Write the 3-train-map/1-unseen-test-map capture plan for generated assets.

The default is thirty 20-second 1080p clips (10 minutes at 30 fps). Every clip
captures native HR; the first training/test trajectory per map additionally
captures 16 frozen reference samples. Validation uses separate trajectories.
"""
import argparse
import json
from pathlib import Path


def generate(destination, dataset_root, version, frames=600, reference_samples=16):
    if not 1 <= frames <= 600 or reference_samples not in (16, 32, 64):
        raise ValueError("Frames must be 1..600 and reference samples 16/32/64")
    destination.mkdir(parents=True, exist_ok=False)
    base = json.loads(Path(__file__).parent.parent.joinpath("Config/TemporalAcceptance/Mixed.json").read_text(encoding="utf-8"))
    base.update(contractVersion="nr-sr-data-v2", endFrame=frames - 1, referenceTemporalSamples=reference_samples, referenceHRScale=2, bAsyncImageWrites=True, maxPendingImageWriteMB=512, bRejectVisibleWidgetComponents=True)
    plan = {"schema": "sr-capture-plan-v1", "datasetVersion": version, "jobs": []}
    for index, name in enumerate(("Gallery", "Courtyard", "Workshop", "AtriumTest")):
        tracks = [(f"Train{i:02d}", "train") for i in range(1, 9)] + [("Validation01", "validation")] if index < 3 else [(f"Test{i:02d}", "test") for i in range(1, 4)]
        for phase, (track, split) in enumerate(tracks):
            clip = name + "_" + track
            sequence = "/Game/SRFormalV1/" + clip
            job = {**base, "jobName": clip, "expectedMap": "/Game/SRFormalV1/" + name, "sequence": sequence + "." + clip, "outputDirectory": (Path(dataset_root) / version / clip).as_posix(), "temporalJitterPhaseOffset": (phase + index * 3) % 8, "bCaptureReferenceHR": phase == 0}
            filename = clip + ".json"
            (destination / filename).write_text(json.dumps(job, indent=2) + "\n", encoding="utf-8")
            plan["jobs"].append({"id": clip, "split": split, "jobFile": filename})
    path = destination / "plan.json"
    path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New directory for plan and expanded job files")
    parser.add_argument("--dataset-root", default="Saved/SRDataset", help="Absolute or host-project-relative output root")
    parser.add_argument("--version", required=True)
    parser.add_argument("--frames-per-clip", type=int, default=600)
    parser.add_argument("--reference-samples", type=int, choices=(16, 32, 64), default=16)
    args = parser.parse_args()
    print(generate(args.output, args.dataset_root, args.version, args.frames_per_clip, args.reference_samples))
