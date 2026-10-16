import hashlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capture_issues
from capture_schema import SchemaError


class IssueEvidenceTests(unittest.TestCase):
    def summarize(self, payload, **kwargs):
        digest = hashlib.sha256()
        result = capture_issues.summarize_lines(
            capture_issues._bounded_lines(io.BytesIO(payload), digest, len(payload) or 1),
            source_path="run.log", **kwargs)
        self.assertEqual(digest.hexdigest(), hashlib.sha256(payload).hexdigest())
        return result

    def test_normalized_repeats_retain_distinct_source_lines(self):
        report = self.summarize(b"warning: missing object 123 at 0x120\ninfo\nwarning: missing object 456 at 0x300\n")
        self.assertEqual(len(report["signatures"]), 1)
        self.assertEqual(report["signatures"][0]["count"], 2)
        self.assertEqual(report["signatures"][0]["first_source"]["line"], 1)
        self.assertEqual(report["signatures"][0]["last_source"]["line"], 3)
        self.assertTrue(report["review_required"])
        self.assertEqual(report["correctness_status"], "unknown")

    def test_stats_prefix_does_not_hide_real_trace_failure(self):
        report = self.summarize(b"[trace] writing trace failed: permission denied\n[recorder] 7 batches without a completion record\n")
        self.assertEqual(report["counts"]["candidate_lines"], 1)
        self.assertIn("failed", report["signatures"][0]["signature"])

    def test_payload_and_paths_do_not_enter_signatures(self):
        payload = b"error: shader failed RecompileRequest:" + b"A" * 100000 + b"\nwarning: could not open D:\\private\\file.bin\n"
        report = self.summarize(payload)
        self.assertEqual(report["counts"]["truncated_lines"], 1)
        self.assertFalse(report["scan_complete"])
        self.assertEqual(report["counts"]["candidate_lines"], 2)
        text = str(report["signatures"])
        self.assertNotIn("AAAA", text)
        self.assertNotIn("private", text)
        self.assertIn("<path>", text)

    def test_empty_log_never_proves_correctness(self):
        report = self.summarize(b"")
        self.assertEqual(report["counts"]["candidate_lines"], 0)
        self.assertEqual(report["status"], "unknown")
        self.assertTrue(report["review_required"])

    def test_capacity_keeps_counting_known_signatures_and_unknown_tail(self):
        report = self.summarize(b"error alpha\nwarning beta\nerror alpha\n", maximum_signatures=1)
        self.assertEqual(report["signatures"][0]["count"], 2)
        self.assertEqual(report["counts"]["unindexed_candidate_lines"], 1)
        self.assertFalse(report["scan_complete"])

    def test_no_newline_and_long_line_preserve_following_line_number(self):
        payload = b"A" * (2 * 1024 * 1024) + b"\nerror after payload"
        report = self.summarize(payload)
        self.assertEqual(report["counts"]["lines"], 2)
        self.assertEqual(report["signatures"][0]["first_source"]["line"], 2)

    def test_log_integrity_and_containment_are_enforced(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = b"warning: missing record\n"
            (root / "run.log").write_bytes(data)
            descriptor = {"kind": "log", "path": "run.log", "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            self.assertEqual(capture_issues.assess_log(root, descriptor)["counts"]["candidate_lines"], 1)
            with self.assertRaises(SchemaError):
                capture_issues.assess_log(root, {**descriptor, "sha256": "0" * 64})
            with self.assertRaises(SchemaError):
                capture_issues.assess_log(root, {**descriptor, "path": "../run.log"})
            with self.assertRaises(SchemaError):
                capture_issues.assess_log(root, descriptor, maximum_bytes=2)


if __name__ == "__main__":
    unittest.main()
