#!/usr/bin/env python3
"""Validate a scene-split capture plan, publish its index, and pack verified tar shards.

All paths in an index are relative to that index. A failed validation never
publishes index.json. Packing checks every source hash again and publishes a
shard catalog only after all files have been written and hashed.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import tarfile
from collections import defaultdict
from collections import deque
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path


def digest(path: Path, algorithm="sha256"):
    value = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".part")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def relative(path, parent):
    return Path(os.path.relpath(path.resolve(), parent.resolve())).as_posix()


def safe_file(root, name):
    if not isinstance(name, str) or not name or Path(name).is_absolute():
        raise ValueError(f"Invalid dataset file path: {name}")
    result = (root / name).resolve()
    if not result.is_relative_to(root.resolve()) or not result.is_file():
        raise ValueError(f"Missing or escaping dataset file: {name}")
    return result


def load_plan(plan_path, project_path):
    plan = read_json(plan_path)
    if plan.get("schema") != "sr-capture-plan-v1" or not re.fullmatch(r"[A-Za-z0-9_.-]+", str(plan.get("datasetVersion", ""))):
        raise ValueError("Plan requires schema sr-capture-plan-v1 and a portable datasetVersion")
    jobs, ids, roots, trajectory_splits, map_splits = [], set(), set(), {}, defaultdict(set)
    for item in plan.get("jobs", []):
        name, split = item.get("id", ""), item.get("split")
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name) or name in ids or split not in {"train", "validation", "test", "stress"}:
            raise ValueError(f"Invalid/duplicate job id or split: {item}")
        ids.add(name)
        job_path = (plan_path.parent / item["jobFile"]).resolve()
        job = read_json(job_path)
        if not job.get("expectedMap") or not job.get("sequence"):
            raise ValueError(f"{name}: batch clips require expectedMap and sequence asset paths")
        if job.get("bResume"):
            raise ValueError(f"{name}: use batch receipts to reuse completed clips; bResume must be false")
        root = Path(job["outputDirectory"])
        root = (root if root.is_absolute() else project_path.parent / root).resolve()
        if root in roots:
            raise ValueError(f"Two jobs write the same output: {root}")
        roots.add(root)
        # Sequence identity comes from the capture job, not a caller-supplied
        # alias which could hide neighboring clips in different splits.
        world = job["expectedMap"].split(".", 1)[0].casefold()
        trajectory = (world, job["sequence"].split(".", 1)[0].casefold())
        if trajectory in trajectory_splits and trajectory_splits[trajectory] != split:
            raise ValueError(f"Trajectory leaks across splits: {trajectory}")
        trajectory_splits[trajectory] = split
        map_splits[world].add(split)
        jobs.append({**item, "jobPath": job_path, "job": job, "root": root})
    if not jobs:
        raise ValueError("Empty capture plan")
    for world, splits in map_splits.items():
        if "test" in splits and splits & {"train", "validation"}:
            raise ValueError(f"Test map also appears in train/validation: {world}")
    return plan, jobs


def verify_manifest_files(root, manifest):
    if manifest.get("state") != "Completed" or not manifest.get("frames"):
        raise ValueError(f"Dataset is incomplete: {root}")
    for frame in manifest["frames"]:
        if set(frame["files"]) != set(frame["sha1"]):
            raise ValueError("Published file/hash keys differ")
        for name, filename in frame["files"].items():
            if digest(safe_file(root, filename), "sha1").upper() != frame["sha1"][name].upper():
                raise ValueError(f"Source file changed: {root / filename}")


def check_job(manifest, requested):
    # JSON's UE-normalized job contains defaults; compare every authored field.
    for name, value in requested.items():
        if manifest["job"].get(name) != value:
            raise ValueError(f"Captured job differs from requested {name}: {value}")


def validate_clip(item, report_path, purpose, reuse_reports):
    from ValidateDataset import validate, VALIDATOR_SOURCE_SHA256
    manifest_path = item["root"] / "manifest.json"
    manifest = read_json(manifest_path)
    check_job(manifest, item["job"])
    report = read_json(report_path) if reuse_reports and report_path.is_file() else None
    reusable = report is not None and report.get("manifestSha256") == digest(manifest_path) and report.get("validatorSourceSha256") == VALIDATOR_SOURCE_SHA256 and report.get("formatAndIntegrityGate") == "pass" and report.get("checksTotal", 0) > 0 and report.get("checksPassed") == report.get("checksTotal") and all(c.get("passed") for c in report.get("checks", []))
    if reusable:
        print(f"Reusing current validation: {item['id']}; rechecking every source hash", flush=True)
        passed = True
    else:
        print(f"Validating {item['id']} ({item['split']})", flush=True)
        report, passed = validate(item["root"], None)
        atomic_json(report_path, report)
    if not passed or report["manifestSha256"] != digest(manifest_path):
        raise ValueError(f"Validation failed or manifest changed: {item['id']}; {report_path}")
    verify_manifest_files(item["root"], manifest)
    if purpose == "temporal-sr" and not (manifest.get("contractVersion") == "nr-sr-data-v2" and report.get("temporalContractGate") == "pass"):
        raise ValueError(f"{item['id']}: temporal admission requires the complete nr-sr-data-v2 controls and validation gate")
    return manifest


def validated_jobs(jobs, output, purpose, workers, reuse_reports):
    def report_path(item):
        return output.parent / "reports" / (item["id"] + ".json")
    if workers == 1:
        for item in jobs:
            yield item, report_path(item), validate_clip(item, report_path(item), purpose, reuse_reports)
        return
    # Submit at most `workers` clips: large image/report data remains bounded.
    with ProcessPoolExecutor(max_workers=workers) as pool:
        pending = deque()
        iterator = iter(jobs)
        for _ in range(workers):
            item = next(iterator, None)
            if item is not None:
                pending.append((item, pool.submit(validate_clip, item, report_path(item), purpose, reuse_reports)))
        while pending:
            item, future = pending.popleft()
            yield item, report_path(item), future.result()
            item = next(iterator, None)
            if item is not None:
                pending.append((item, pool.submit(validate_clip, item, report_path(item), purpose, reuse_reports)))


def build_index(plan_path, project_path, output, purpose, workers=1, reuse_reports=False):
    if not 1 <= workers <= 4:
        raise ValueError("workers must be between 1 and 4")
    if output.exists():
        raise ValueError(f"An immutable index already exists: {output}")
    plan, jobs = load_plan(plan_path, project_path)
    entries, totals = [], defaultdict(lambda: {"frames": 0, "seconds": 0.0, "maps": set()})
    for item, report_path, manifest in validated_jobs(jobs, output, purpose, workers, reuse_reports):
        manifest_path = item["root"] / "manifest.json"
        count = len(manifest["frames"])
        fps = manifest["job"]["captureFrameRateNumerator"] / manifest["job"]["captureFrameRateDenominator"]
        seconds = count * manifest["job"]["frameStep"] / fps
        totals[item["split"]]["frames"] += count
        totals[item["split"]]["seconds"] += seconds
        totals[item["split"]]["maps"].add(item["job"]["expectedMap"])
        entries.append({"id": item["id"], "split": item["split"], "map": item["job"]["expectedMap"], "sequence": item["job"]["sequence"], "root": relative(item["root"], output.parent), "manifestSha256": digest(manifest_path), "report": relative(report_path, output.parent), "reportSha256": digest(report_path), "jobSha256": digest(item["jobPath"]), "frameCount": count, "sampledSeconds": seconds, "contractVersion": manifest["contractVersion"], "provenance": manifest["provenance"]})
    index = {"schema": "sr-dataset-index-v1", "datasetVersion": plan["datasetVersion"], "purpose": purpose, "temporalTrainingCertified": purpose == "temporal-sr", "planSha256": digest(plan_path), "splitPolicy": "sequence_disjoint; test_maps_unseen_in_train_and_validation", "totals": {k: {**v, "maps": sorted(v["maps"])} for k, v in totals.items()}, "clips": entries}
    atomic_json(output, index)
    return index


def checked_clips(index_path):
    index = read_json(index_path)
    if index.get("schema") != "sr-dataset-index-v1":
        raise ValueError("Unknown dataset index schema")
    for clip in index["clips"]:
        root = (index_path.parent / clip["root"]).resolve()
        manifest_path = root / "manifest.json"
        report_path = (index_path.parent / clip["report"]).resolve()
        if digest(manifest_path) != clip["manifestSha256"] or digest(report_path) != clip["reportSha256"]:
            raise ValueError(f"Index source/report changed: {clip['id']}")
        report = read_json(report_path)
        if report.get("manifestSha256") != clip["manifestSha256"] or report.get("formatAndIntegrityGate") != "pass" or report.get("checksPassed") != report.get("checksTotal"):
            raise ValueError(f"Stale or failed admission report: {clip['id']}")
        manifest = read_json(manifest_path)
        if index.get("purpose") == "temporal-sr" and not (manifest.get("contractVersion") == "nr-sr-data-v2" and report.get("temporalContractGate") == "pass"):
            raise ValueError(f"Temporal contract gate missing: {clip['id']}")
        yield index, clip, root, manifest


def pack(index_path, output_dir, samples_per_shard, profile="all"):
    if samples_per_shard < 1:
        raise ValueError("samples_per_shard must be positive")
    if profile not in {"all", "temporal-sr"}:
        raise ValueError("Unknown packing profile")
    output_dir.mkdir(parents=True, exist_ok=False)
    catalog = {"schema": "sr-training-shards-v1", "indexSha256": digest(index_path), "packingProfile": profile, "shards": []}
    writer, current_path, current_split, in_shard = None, None, None, 0

    def close_shard():
        nonlocal writer
        if writer is None:
            return
        writer.close()
        final = current_path.with_suffix("")
        current_path.replace(final)
        catalog["shards"].append({"file": final.name, "split": current_split, "samples": in_shard, "bytes": final.stat().st_size, "sha256": digest(final)})
        writer = None

    try:
        for index, clip, root, manifest in checked_clips(index_path):
            catalog.update(datasetVersion=index["datasetVersion"], purpose=index["purpose"], temporalTrainingCertified=index["temporalTrainingCertified"])
            mapping = manifest.get("trainingInputMapping", {})
            selected = None
            if profile == "temporal-sr":
                required = {"color_lr", "color_gt", "depth", "motion", "motion_valid", "depth_valid", "velocity_coverage", "reactive_mask", "transparency_mask"}
                if index["purpose"] != "temporal-sr" or manifest["contractVersion"] != "nr-sr-data-v2" or not required <= set(mapping):
                    raise ValueError("The temporal-sr profile requires admitted v2 inputs and their complete training mapping")
                selected = {mapping[k] for k in required} | {"history_rejection_mask", "history_rejection_valid", "disocclusion_mask", "disocclusion_valid", "object_id"}
            print(f"Packing {clip['id']} ({profile})", flush=True)
            for frame in manifest["frames"]:
                names = set(frame["files"]) if selected is None else selected
                if not names <= set(frame["files"]):
                    raise ValueError(f"Training modalities missing: {sorted(names - set(frame['files']))}")
                if writer is None or in_shard >= samples_per_shard or current_split != clip["split"]:
                    close_shard()
                    current_split, in_shard = clip["split"], 0
                    current_path = output_dir / f"{current_split}-{len(catalog['shards']):05d}.tar.part"
                    writer = tarfile.open(current_path, "w", format=tarfile.PAX_FORMAT)
                key = f"{clip['id']}_{int(frame['logicalFrameId']):08d}"
                packed_frame = {**frame, "files": {k: v for k, v in frame["files"].items() if k in names}, "sha1": {k: v for k, v in frame["sha1"].items() if k in names}}
                metadata = {"clipId": clip["id"], "split": clip["split"], "map": clip["map"], "sequence": clip["sequence"], "manifestSha256": clip["manifestSha256"], "contractVersion": manifest["contractVersion"], "trainingInputMapping": mapping, "packingProfile": profile, "sourceModalities": sorted(frame["files"]), "job": manifest["job"], "frame": packed_frame}
                for name, filename in sorted(frame["files"].items()):
                    if not re.fullmatch(r"[A-Za-z0-9_]+", name):
                        raise ValueError(f"Invalid modality name: {name}")
                    source = safe_file(root, filename)
                    payload = source.read_bytes()
                    if hashlib.sha1(payload).hexdigest().upper() != frame["sha1"][name].upper():
                        raise ValueError(f"Source image changed before packing: {source}")
                    if name not in names:
                        continue
                    info = tarfile.TarInfo(f"{key}.{name}{source.suffix}")
                    info.size = len(payload)
                    writer.addfile(info, io.BytesIO(payload))
                payload = json.dumps(metadata, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
                info = tarfile.TarInfo(key + ".json")
                info.size = len(payload)
                writer.addfile(info, io.BytesIO(payload))
                in_shard += 1
        close_shard()
        atomic_json(output_dir / "shards.json", catalog)
    finally:
        if writer is not None:
            writer.close()
    return catalog


def iter_samples(catalog_path, split=None):
    """Yield (metadata, modality->encoded bytes); one frame is retained at a time.

    Clip ID and reset remain explicit. Callers must not connect histories across
    clip IDs, reset frames, or split boundaries. No tar member is extracted.
    """
    catalog = read_json(catalog_path)
    if catalog.get("schema") != "sr-training-shards-v1":
        raise ValueError("Unknown shard catalog")
    for shard in catalog["shards"]:
        if split is not None and split != shard["split"]:
            continue
        path = safe_file(catalog_path.parent, shard["file"])
        if digest(path) != shard["sha256"]:
            raise ValueError(f"Shard checksum failed: {path}")
        samples, payloads, key = 0, {}, None
        with tarfile.open(path, "r|") as archive:
            for member in archive:
                if not member.isfile() or "/" in member.name or "\\" in member.name:
                    raise ValueError(f"Unexpected tar member: {member.name}")
                member_key, suffix = member.name.split(".", 1)
                if key is not None and member_key != key:
                    raise ValueError("Incomplete shard sample")
                key = member_key
                data = archive.extractfile(member).read()
                if suffix == "json":
                    metadata = json.loads(data)
                    if set(payloads) != set(metadata["frame"]["sha1"]) or metadata["split"] != shard["split"]:
                        raise ValueError("Shard sample metadata mismatch")
                    for modality, payload in payloads.items():
                        if hashlib.sha1(payload).hexdigest().upper() != metadata["frame"]["sha1"][modality].upper():
                            raise ValueError("Shard image checksum failed")
                    yield metadata, payloads
                    samples += 1
                    payloads, key = {}, None
                else:
                    modality = suffix.rsplit(".", 1)[0]
                    if modality in payloads:
                        raise ValueError("Duplicate shard modality")
                    payloads[modality] = data
        if key is not None or samples != shard["samples"]:
            raise ValueError("Shard sample count mismatch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    index = commands.add_parser("index")
    index.add_argument("plan", type=Path)
    index.add_argument("--project", required=True, type=Path)
    index.add_argument("--output", required=True, type=Path)
    index.add_argument("--purpose", choices=("diagnostic", "temporal-sr"), default="diagnostic")
    index.add_argument("--workers", type=int, default=1, help="Independent validation processes (1-4); each keeps one clip's working images")
    index.add_argument("--reuse-validation-reports", action="store_true", help="Reuse passing reports only for identical manifests/current validator; all source file hashes are checked again")
    packer = commands.add_parser("pack")
    packer.add_argument("index", type=Path)
    packer.add_argument("--output", required=True, type=Path)
    packer.add_argument("--samples-per-shard", type=int, default=128)
    packer.add_argument("--profile", choices=("all", "temporal-sr"), default="all", help="all retains diagnostics/previews; temporal-sr retains HDR training inputs, correspondence masks and all frame metadata")
    checker = commands.add_parser("verify-shards")
    checker.add_argument("catalog", type=Path)
    args = parser.parse_args()
    if args.command == "index":
        result = build_index(args.plan.resolve(), args.project.resolve(), args.output.resolve(), args.purpose, args.workers, args.reuse_validation_reports)
        print(json.dumps(result["totals"], indent=2))
    elif args.command == "pack":
        result = pack(args.index.resolve(), args.output.resolve(), args.samples_per_shard, args.profile)
        print(f"Packed {len(result['shards'])} verified shards")
    else:
        count = sum(1 for _ in iter_samples(args.catalog.resolve()))
        print(f"Read and verified {count} complete samples")


if __name__ == "__main__":
    main()
