import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_recover
from capture_schema import sha256_file
from test_capture_schema import make_manifest
from test_capture_draws import event


class CaptureRecoveryTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.manifest = make_manifest(self.root)
        self.manifest["execution"]["pid"] = 7
        self.path = self.root / f"aps5-draw-{self.manifest['run_id']}-7.journal.jsonl"
        self.capture_id = "11111111-1111-4111-8111-111111111111"
        self.rows = [self.record("header", {"time_unit": "ns", "clock_domain": "steady_clock", "origin_steady_ns": "1",
            **self.manifest["capture"], "maximum_bytes": capture_recover.MAX_JOURNAL_BYTES, "batch_events": 4096}, 0),
            self.record("thread", {"thread_id": "1", "name": "worker", "registered_ns": 0,
                "native_thread_id": "77", "native_thread_id_source": "windows_thread_id"}, 1),
            self.record("events", {"thread_id": "1", "first_index": 0, "count": 1,
                "events": [event(1, "span", "10", name="work", start=1, end=4)]}, 2)]

    def record(self, kind, data, sequence):
        return {"schema_version": 1, "document_type": "anyps5.draw-journal", "run_id": self.manifest["run_id"],
                "capture_id": self.capture_id, "pid": 7, "sequence": sequence, "kind": kind, "data": data}

    def write(self, tail=b""):
        self.path.write_bytes(b"".join(json.dumps(row).encode() + b"\n" for row in self.rows) + tail)

    def recover(self, **kwargs):
        self.write()
        return capture_recover.recover_journal(self.path, self.manifest, **kwargs)

    def test_committed_prefix_never_claims_complete_capture(self):
        report = self.recover()
        self.assertEqual(report["status"], "unknown")
        self.assertFalse(report["complete"])
        self.assertEqual(report["capture_eligibility"], "invalid")
        self.assertEqual(report["retained_events"][0]["source"]["line"], 3)

    def test_truncated_uncommitted_tail_is_discarded(self):
        self.write(b'{"sequence":3')
        report = capture_recover.recover_journal(self.path, self.manifest)
        self.assertEqual(report["status"], "unknown")
        self.assertGreater(report["truncated_tail_bytes"], 0)
        self.assertEqual(report["event_count"], 1)

    def test_reordered_sequence_is_invalid(self):
        self.rows[2]["sequence"] = 1
        self.assertEqual(self.recover()["status"], "invalid")

    def test_changed_capture_identity_is_invalid(self):
        self.rows[2]["capture_id"] = "22222222-2222-4222-8222-222222222222"
        self.assertEqual(self.recover()["status"], "invalid")

    def test_header_window_is_checked(self):
        self.rows[0]["data"]["start"] += 1
        self.assertEqual(self.recover()["status"], "invalid")

    def test_smaller_native_journal_budget_is_valid_partial_evidence(self):
        self.rows[0]["data"]["maximum_bytes"] = 1024 * 1024
        report = self.recover()
        self.assertEqual(report["status"], "unknown", report["issues"])
        self.assertEqual(report["declared_maximum_bytes"], 1024 * 1024)
        self.assertEqual(report["event_count"], 1)
        self.assertFalse(report["complete"])

    def test_declared_journal_budget_must_be_positive_and_bounded(self):
        for budget in (0, capture_recover.MAX_JOURNAL_BYTES + 1, True):
            with self.subTest(budget=budget):
                self.rows[0]["data"]["maximum_bytes"] = budget
                self.assertEqual(self.recover()["status"], "invalid")

    def test_observed_file_including_tail_cannot_exceed_declared_budget(self):
        self.rows[0]["data"]["maximum_bytes"] = 1024
        self.write(b"x" * 2048)
        report = capture_recover.recover_journal(self.path, self.manifest)
        self.assertEqual(report["status"], "invalid")
        self.assertTrue(any("declared byte budget" in issue["message"] for issue in report["issues"]))

    def test_event_before_thread_definition_is_invalid(self):
        self.rows.pop(1)
        self.rows[1]["sequence"] = 1
        self.assertEqual(self.recover()["status"], "invalid")

    def test_duplicate_event_is_invalid(self):
        duplicate = copy.deepcopy(self.rows[2])
        duplicate["sequence"], duplicate["data"]["first_index"] = 3, 1
        self.rows.append(duplicate)
        self.assertEqual(self.recover()["status"], "invalid")

    def test_footer_count_must_match_committed_events(self):
        self.rows.append(self.record("footer", {"emitted_events": 2, "stop_reason": "complete", "complete": False,
                                                "final_sidecar_required": True}, 3))
        self.assertEqual(self.recover()["status"], "invalid")

    def test_complete_footer_still_requires_final_sidecar(self):
        self.rows.append(self.record("footer", {"emitted_events": 1, "stop_reason": "complete", "complete": False,
                                                "final_sidecar_required": True}, 3))
        self.assertEqual(self.recover()["status"], "unknown")

    def test_retained_events_are_bounded_without_changing_observed_count(self):
        report = self.recover(max_retained_events=0)
        self.assertEqual(report["retained_events"], [])
        self.assertEqual(report["omitted_event_count"], 1)

    def test_descriptor_tamper_is_invalid(self):
        self.write()
        descriptor = {"kind": "draw_journal", "size": self.path.stat().st_size, "sha256": "0" * 64}
        self.assertNotEqual(sha256_file(self.path), descriptor["sha256"])
        self.assertEqual(capture_recover.recover_journal(self.path, self.manifest, descriptor=descriptor)["status"], "invalid")

    def test_explicit_contained_journal_supports_unfinalized_inventory(self):
        self.write()
        self.manifest["artifacts"] = []
        report = capture_recover.recover_run(self.root, manifest=self.manifest, journal=self.path.name)
        self.assertEqual(report["event_count"], 1)
        self.assertEqual(report["status"], "unknown")


if __name__ == "__main__":
    unittest.main()
