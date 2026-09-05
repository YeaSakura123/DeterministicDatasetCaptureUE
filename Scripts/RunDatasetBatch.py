#!/usr/bin/env python3
"""Run a finite scene/sequence plan in isolated Unreal processes.

Completed clips can be reused only with this batch's matching receipt and
unchanged source files. Failed/partial output is preserved for diagnosis;
recapture into a new output directory instead of overwriting its evidence.
"""
import argparse
import subprocess
import sys
from pathlib import Path

from DatasetDelivery import atomic_json, check_job, digest, load_plan, read_json, verify_manifest_files


def run(plan_path, project_path, editor, status_path, reuse, ddc):
    plan, jobs = load_plan(plan_path, project_path)
    fingerprint = {"planSha256": digest(plan_path), "project": str(project_path.resolve()), "editor": str(editor.resolve())}
    status = {"schema": "sr-batch-status-v1", "datasetVersion": plan["datasetVersion"], **fingerprint, "clips": {}}
    if status_path.exists():
        previous = read_json(status_path)
        if not reuse or any(previous.get(k) != v for k, v in fingerprint.items()):
            raise ValueError("Existing batch receipt requires --reuse-completed and unchanged plan/project/editor")
        status = previous
    for item in jobs:
        job_hash = digest(item["jobPath"])
        receipt = status["clips"].get(item["id"])
        manifest_path = item["root"] / "manifest.json"
        if receipt and receipt.get("state") == "Completed" and reuse:
            if receipt["jobSha256"] != job_hash or digest(manifest_path) != receipt["manifestSha256"]:
                raise ValueError(f"Completed clip has changed: {item['id']}")
            manifest = read_json(manifest_path)
            check_job(manifest, item["job"])
            verify_manifest_files(item["root"], manifest)
            print(f"Reusing verified completed clip: {item['id']}", flush=True)
            continue
        if item["root"].exists() and any(item["root"].iterdir()):
            raise ValueError(f"Output is not empty; choose a fresh outputDirectory for {item['id']}: {item['root']}")
        status["clips"][item["id"]] = {"state": "Running", "jobSha256": job_hash}
        atomic_json(status_path, status)
        command = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(Path(__file__).with_name("RunDatasetCapture.ps1")), "-Project", str(project_path), "-Editor", str(editor), "-Job", str(item["jobPath"])]
        if ddc:
            command += ["-UseWorkspaceLocalDDC"]
        print(f"Capturing {item['id']} ({item['split']})", flush=True)
        result = subprocess.run(command, check=False)
        try:
            if result.returncode != 0:
                raise RuntimeError(f"Capture process failed with exit code {result.returncode}")
            manifest = read_json(manifest_path)
            check_job(manifest, item["job"])
            verify_manifest_files(item["root"], manifest)
            status["clips"][item["id"]].update(state="Completed", manifestSha256=digest(manifest_path), frameCount=len(manifest["frames"]))
        except Exception as exc:
            status["clips"][item["id"]].update(state="Failed", error=str(exc))
            atomic_json(status_path, status)
            raise
        atomic_json(status_path, status)
    print(f"Completed {len(jobs)} clips. Run DatasetDelivery.py index to apply quality gates.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--status", type=Path, required=True)
    parser.add_argument("--reuse-completed", action="store_true")
    parser.add_argument("--workspace-ddc", action="store_true")
    args = parser.parse_args()
    try:
        run(args.plan.resolve(), args.project.resolve(), args.editor.resolve(), args.status.resolve(), args.reuse_completed, args.workspace_ddc)
    except Exception as exc:
        print(f"Batch failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
