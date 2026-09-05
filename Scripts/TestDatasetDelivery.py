"""Boundary tests for split leakage, immutable evidence, and shard corruption."""
import json
import tempfile
import unittest
from pathlib import Path

from DatasetDelivery import atomic_json, digest, iter_samples, load_plan, pack


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "input.exr").write_bytes(b"original HDR payload\x00\xff")
        self.frame = {"logicalFrameId": 7, "reset": True, "files": {"color_lr_scene_hdr": "input.exr"}, "sha1": {"color_lr_scene_hdr": digest(self.source / "input.exr", "sha1")}}
        atomic_json(self.source / "manifest.json", {"state": "Completed", "contractVersion": "spatial-sr-data-v1", "job": {}, "frames": [self.frame]})
        self.manifest_hash = digest(self.source / "manifest.json")
        atomic_json(self.root / "report.json", {"manifestSha256": self.manifest_hash, "formatAndIntegrityGate": "pass", "checksPassed": 1, "checksTotal": 1})
        self.index = {"schema": "sr-dataset-index-v1", "datasetVersion": "test", "purpose": "diagnostic", "temporalTrainingCertified": False, "clips": [{"id": "clip", "split": "train", "map": "/Game/A", "sequence": "/Game/Seq", "root": "source", "report": "report.json", "reportSha256": digest(self.root / "report.json"), "manifestSha256": self.manifest_hash}]}
        self.index_path = self.root / "index.json"
        atomic_json(self.index_path, self.index)

    def test_round_trip_preserves_reset_and_payload(self):
        pack(self.index_path, self.root / "shards", 1)
        samples = list(iter_samples(self.root / "shards/shards.json", "train"))
        self.assertEqual(len(samples), 1)
        metadata, payload = samples[0]
        self.assertTrue(metadata["frame"]["reset"])
        self.assertEqual(metadata["frame"]["logicalFrameId"], 7)
        self.assertEqual(payload["color_lr_scene_hdr"], (self.source / "input.exr").read_bytes())
        self.assertEqual(list(iter_samples(self.root / "shards/shards.json", "test")), [])

    def test_failed_report_cannot_publish_shards(self):
        report = json.loads((self.root / "report.json").read_text())
        report.update(formatAndIntegrityGate="fail", checksPassed=0)
        atomic_json(self.root / "report.json", report)
        self.index["clips"][0]["reportSha256"] = digest(self.root / "report.json")
        atomic_json(self.index_path, self.index)
        with self.assertRaisesRegex(ValueError, "Stale or failed"):
            pack(self.index_path, self.root / "shards", 1)
        self.assertFalse((self.root / "shards/shards.json").exists())

    def test_changed_manifest_invalidates_report(self):
        with (self.source / "manifest.json").open("a") as stream:
            stream.write("\n")
        with self.assertRaisesRegex(ValueError, "source/report changed"):
            pack(self.index_path, self.root / "shards", 1)

    def test_changed_image_cannot_publish_shards(self):
        (self.source / "input.exr").write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "Source image changed"):
            pack(self.index_path, self.root / "shards", 1)
        self.assertFalse((self.root / "shards/shards.json").exists())

    def test_corrupt_tar_is_rejected_before_reading(self):
        pack(self.index_path, self.root / "shards", 1)
        path = next((self.root / "shards").glob("*.tar"))
        with path.open("ab") as stream:
            stream.write(b"tampered")
        with self.assertRaisesRegex(ValueError, "checksum failed"):
            list(iter_samples(self.root / "shards/shards.json"))

    def test_sequence_alias_cannot_hide_adjacent_split_leakage(self):
        for name, seq in (("a", "/Game/Shot"), ("b", "/game/shot.Shot")):
            atomic_json(self.root / (name + ".json"), {"expectedMap": "/Game/Level", "sequence": seq, "outputDirectory": name})
        plan = {"schema": "sr-capture-plan-v1", "datasetVersion": "test", "jobs": [{"id": "a", "jobFile": "a.json", "split": "train"}, {"id": "b", "jobFile": "b.json", "split": "validation"}]}
        atomic_json(self.root / "plan.json", plan)
        with self.assertRaisesRegex(ValueError, "Trajectory leaks"):
            load_plan(self.root / "plan.json", self.root / "host.uproject")


if __name__ == "__main__":
    unittest.main()
