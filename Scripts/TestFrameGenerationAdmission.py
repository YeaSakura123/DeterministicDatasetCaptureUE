"""Exercise FG admission failures using an actual fully validated assembled dataset.

The output retains each report. Image files are hard-linked read-only by convention;
only a separately copied manifest is edited. The source dataset is never changed.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil

from ValidateFrameGenerationDataset import validate


def test(source, output):
    source, output = source.resolve(), output.resolve()
    if output.exists() or output.is_relative_to(source):
        raise ValueError("Choose a new output outside the source dataset")
    original_bytes = (source / "manifest.json").read_bytes()
    baseline, passed = validate(source)
    if not passed or baseline["certificationGate"] != "supported_scope_pass":
        raise ValueError("The baseline must pass supported-scope admission")
    shutil.copytree(source, output, copy_function=os.link)
    manifest_path = output / "manifest.json"
    manifest_path.unlink()  # Remove this link before writing an independent copy.
    original = json.loads(original_bytes)
    cases = {}
    changed = copy.deepcopy(original)
    del changed["validationEvidence"]["projectWPO"]
    cases["missing_physical_evidence"] = changed
    changed = copy.deepcopy(original)
    changed["validationEvidence"]["projectWPO"]["physicalReport"]["passed"] = False
    cases["forged_stored_physical_result"] = changed
    changed = copy.deepcopy(original)
    changed["validationEvidence"]["projectWPO"]["sha1"]["forward/manifest.json"] = "0" * 40
    cases["changed_proof_file"] = changed
    changed = copy.deepcopy(original)
    changed["sourceCaptureControls"]["forward"]["job"]["bRequireSceneControlPreflight"] = False
    cases["strict_control_disabled"] = changed
    changed = copy.deepcopy(original)
    changed["frameGenerationCertified"] = True
    cases["producer_self_certification"] = changed
    changed = copy.deepcopy(original)
    del changed["pairs"][0]["rasterGrids"]
    cases["missing_raster_grid"] = changed
    results = []
    for name, changed in cases.items():
        manifest_path.write_text(json.dumps(changed, indent=2), encoding="utf-8")
        report, accepted = validate(output)
        rejected = not accepted and not report["frameGenerationCertified"] and report["certificationGate"] == "not_certified"
        (output / (name + ".json")).write_text(json.dumps(report, indent=2), encoding="utf-8")
        results.append({"case": name, "rejected": rejected, "failedChecks": [c["name"] for c in report["checks"] if not c["passed"]]})
    manifest_path.write_bytes(original_bytes)
    unchanged = (source / "manifest.json").read_bytes() == original_bytes
    result = {"schema": "sr-fg-admission-negative-v1", "sourceManifestSha256": hashlib.sha256(original_bytes).hexdigest(), "sourceUnchanged": unchanged, "cases": results, "passed": unchanged and all(r["rejected"] for r in results)}
    (output / "admission_negative_results.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = test(args.dataset, args.output)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
