import copy
import gzip
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capture_health
from capture_manifest import file_identity
from capture_schema import SchemaError, canonical_sha256, validate_health, WORKLOAD_KEYS


def make_workload(manifest, trace_name):
    totals = dict.fromkeys(WORKLOAD_KEYS, 0)
    totals.update(requested_draw_operations=2, host_draw_commands=2, host_commands_completed=2)
    totals["operation_status.complete"] = 2
    rows = []
    for index, (first, last) in enumerate(((0, 16_000_000), (16_000_000, 40_000_000))):
        counts = {key: value // 2 for key, value in totals.items()}
        rows.append({"present": manifest["capture"]["start"] + index, "begin_ns": 1_000_000_000 + first,
            "end_ns": 1_000_000_000 + last, "counts": counts, "pending_at_begin": [0, 0, 0], "pending_at_end": [0, 0, 0]})
    return {"schema_version": 1, "document_type": "anyps5.measurement-workload", "run_id": manifest["run_id"],
        "producer": {"pid": 7, "revision": "a" * 40, "dirty": False}, "trace": trace_name,
        "clock": {"domain": "steady_clock", "unit": "ns", "origin": "implementation_defined_steady_epoch"},
        "capture": {"spec": f"frames {manifest['capture']['start']}:2", "open_present": manifest["capture"]["start"],
            "close_present": manifest["capture"]["start"] + 2, "open_ns": rows[0]["begin_ns"], "close_ns": rows[-1]["end_ns"]},
        "counts": totals, "frames": rows, "coverage": {"closed": True, "maximum_intervals": 65536,
            "lost_intervals": 0, "overflow": False, "lifetime_error": False,
            "pending_at_open": [0, 0, 0], "pending_at_close": [0, 0, 0]}}


class CaptureHealthTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source_root = self.root / "source"
        self.source_root.mkdir()
        self.run = self.root / "run"
        self.run.mkdir()
        self.binary = self.root / "synthetic-runtime.bin"
        self.binary.write_bytes(b"synthetic runtime identity, not an executable")
        self.trace_name = "aps5-trace-" + "a" * 40 + "-7.json.gz"
        self.summary = {
            "commit": "a" * 40, "dirty": False, "env": {"APS5_TRACE_FRAMES": "3:2"},
            "gpu": "GPU", "driver": "driver", "os": "OS", "resolution": "1920x1080",
            "window": {"spec": "frames 3:2", "first_present": 3, "ms": 40.0}, "presents": 2,
            "frame_ms": {"p50": 24.0, "p90": 24.0, "p99": 24.0, "max": 24.0},
            "gpu_busy_ms_per_present": 2.0, "worker_busy_pct": {"queue worker 0": 30.0},
            "top_phases_ms_per_frame": {"Driver.Packet": 6.0}, "trace_dropped": False,
            "trace": self.trace_name,
        }
        self.trace = {"traceEvents": [
            {"name": "process_name", "ph": "M", "ts": 0, "pid": 7, "tid": 0, "args": {"name": "AnyPS5"}},
            {"name": "thread_name", "ph": "M", "ts": 0, "pid": 7, "tid": 1, "args": {"name": "presenter"}},
            {"name": "present", "ph": "i", "ts": 0, "pid": 7, "tid": 1, "args": {"present": 3}},
            {"name": "present", "ph": "i", "ts": 16000, "pid": 7, "tid": 1, "args": {"present": 4}},
        ]}
        before_verified = {"verified": True, "findings": [], "checked_at": "2026-10-15T12:00:00Z"}
        after_verified = {"verified": True, "findings": [], "checked_at": "2026-10-15T12:01:01Z"}
        self.manifest = {
            "schema_version": 1, "document_type": "anyps5.run-manifest",
            "run_id": "e5eeef31-5fb3-4ad9-9d6a-29bbf38f178d", "created_at": "2026-10-15T12:00:00Z",
            "started_at": "2026-10-15T12:00:01Z", "ended_at": "2026-10-15T12:01:00Z",
            "state": "completed", "exit_code": 0,
            "source": {"root": str(self.source_root), "commit": "a" * 40, "dirty": False,
                       "diff_sha256": "b" * 64, "untracked": [], "submodules": []},
            "build": {"compiler": "compiler", "build_type": "Release", "options": {},
                      "provenance": "declared_configuration", "source_binary_binding": "unverified"},
            "runtime": {"inventory_kind": "declared_files", "loaded_modules_verified": False,
                        "files": [{"role": "executable", "path": str(self.binary), **file_identity(self.binary)}]},
            "machine": {"id": "machine", "cpu": "CPU", "gpu": "GPU", "driver": "driver", "os": "OS",
                        "provenance": "declared_configuration"},
            "title": {"id": "synthetic", "version": "1"},
            "scene": {"id": "scene", "recipe": {}, "recipe_sha256": canonical_sha256({})},
            "settings": {"resolution": "1920x1080", "presentation": "windowed",
                         "cache": {"shader": "warm", "pipeline": "warm", "driver": "warm"},
                         "diagnostics": {"mode": "measurement", "options": {}}, "relinker": {"flags": []}},
            "environment": {"effective": {"APS5_TRACE_FRAMES": "3:2"}, "removed_inherited": []},
            "capture": {"unit": "frames", "start": 3, "length": 2}, "artifacts": [],
            "execution": {"command": [str(self.binary)], "cwd": str(self.root), "pid": 7,
                          "inventory_verification": {"before": before_verified, "after": after_verified}},
        }
        self.write_evidence()

    def descriptor(self, kind, name):
        return {"kind": kind, "path": name, **file_identity(self.run / name)}

    def write_evidence(self, summaries=None):
        records = [self.summary] if summaries is None else summaries
        (self.run / "summary.jsonl").write_text("\n".join(json.dumps(record) for record in records) + "\n", encoding="utf-8")
        payload = json.dumps(self.trace).encode("utf-8")
        if self.trace_name.endswith(".gz"):
            (self.run / self.trace_name).write_bytes(gzip.compress(payload, mtime=0))
        else:
            (self.run / self.trace_name).write_bytes(payload)
        self.manifest["artifacts"] = [self.descriptor("summary", "summary.jsonl"), self.descriptor("trace", self.trace_name)]

    def assess(self):
        report = capture_health.assess_capture(self.run, self.manifest)
        validate_health(report)
        return report

    def codes(self, report):
        return {issue["code"] for issue in report["issues"]}

    def test_completed_frames_do_not_claim_complete_attribution_or_loaded_modules(self):
        before = {path.name: path.read_bytes() for path in self.run.iterdir()}
        report = self.assess()
        self.assertEqual(report["frame_metrics"]["status"], "valid")
        self.assertEqual(report["frame_metrics"]["metrics"]["mean_frame_ms"], 20)
        self.assertEqual(report["frame_metrics"]["metrics"]["frame_ms.p90"], 24)
        self.assertEqual(report["detailed_attribution"]["status"], "unknown")
        self.assertEqual(report["provenance"]["status"], "unknown")
        self.assertFalse(report["provenance"]["loaded_modules_verified"])
        self.assertTrue(report["provenance"]["runtime_boundaries_verified"])
        self.assertEqual(report["coverage"]["all_artifacts_complete"], "unknown")
        self.assertIn("gpu_track_missing", self.codes(report))
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.run.iterdir()})

    def test_plain_json_trace_is_supported(self):
        self.trace_name = self.trace_name[:-3]
        self.summary["trace"] = self.trace_name
        self.write_evidence()
        self.assertEqual(self.assess()["frame_metrics"]["status"], "valid")

    def test_unrelated_summary_is_never_chosen_by_file_order(self):
        other = dict(self.summary, trace="unrelated.json.gz")
        for summaries in ([other, self.summary], [self.summary, other]):
            self.write_evidence(summaries)
            self.assertEqual(self.assess()["frame_metrics"]["status"], "valid")
        self.write_evidence([self.summary, self.summary])
        self.assertEqual(self.assess()["frame_metrics"]["status"], "invalid")

    def test_missing_or_extra_artifact_selection_is_invalid(self):
        for artifacts in ([], [self.manifest["artifacts"][0]], self.manifest["artifacts"] + [
                {"kind": "trace", "path": "other.json.gz", "size": 0, "sha256": "0" * 64}]):
            with self.subTest(count=len(artifacts)):
                manifest = copy.deepcopy(self.manifest)
                manifest["artifacts"] = artifacts
                self.assertEqual(capture_health.assess_capture(self.run, manifest)["status"], "invalid")

    def test_artifact_hashes_are_checked_and_historical_runtime_evidence_survives_rebuild(self):
        (self.run / self.trace_name).write_bytes(b"changed artifact")
        self.assertIn("artifact_integrity", self.codes(self.assess()))
        self.write_evidence()
        self.binary.write_bytes(b"changed declared runtime")
        report = self.assess()
        self.assertEqual(report["frame_metrics"]["status"], "valid")
        self.assertNotEqual(report["status"], "invalid")
        self.assertTrue(report["provenance"]["runtime_files_verified"])
        self.binary.unlink()
        self.assertNotEqual(self.assess()["status"], "invalid")

    def test_runtime_verification_failure_and_missing_boundaries_stay_visible(self):
        self.manifest["execution"]["inventory_verification"]["after"]["verified"] = False
        self.assertIn("runtime_verification_failed", self.codes(self.assess()))
        self.manifest["execution"]["inventory_verification"] = {}
        report = self.assess()
        self.assertFalse(report["provenance"]["runtime_boundaries_verified"])
        self.assertIn("runtime_verification_missing", self.codes(report))

    def test_loss_does_not_erase_valid_frame_timing(self):
        self.summary["trace_dropped"] = True
        self.summary["trace_dropped_threads"] = {"presenter": {"events_lost": 3, "totals_full": False}}
        self.write_evidence()
        report = self.assess()
        self.assertEqual(report["frame_metrics"]["status"], "valid")
        self.assertEqual(report["detailed_attribution"]["status"], "invalid")
        self.assertIn("trace_data_loss", self.codes(report))
        self.summary["trace_dropped"] = False
        self.write_evidence()
        self.assertIn("loss_flag_inconsistent", self.codes(self.assess()))

    def test_trace_loss_marker_is_checked_even_with_false_summary_flag(self):
        self.trace["traceEvents"].append({"name": "trace buffer full", "ph": "i", "ts": 12000,
                                          "pid": 7, "tid": 1, "args": {"events_lost": 4}})
        self.write_evidence()
        self.assertIn("loss_flag_inconsistent", self.codes(self.assess()))

    def test_scoped_work_can_begin_before_the_window_origin(self):
        self.trace["traceEvents"].append({"name": "Driver.Present", "ph": "X", "ts": -500,
                                          "dur": 1000, "pid": 7, "tid": 1})
        self.write_evidence()
        self.assertEqual(self.assess()["frame_metrics"]["status"], "valid")

    def test_missing_queries_are_not_treated_as_zero_work(self):
        self.summary["gpu_queries_missing"] = 3
        self.write_evidence()
        report = self.assess()
        self.assertIn("gpu_query_loss", self.codes(report))
        self.assertEqual(report["detailed_attribution"]["status"], "invalid")

    def test_log_missing_completions_have_unknown_window_overlap(self):
        path = self.run / "run.log"
        path.write_text("[present] GPU busy 1.0 ms (3 batches without a completion record)\n", encoding="utf-8")
        self.manifest["artifacts"].append(self.descriptor("log", path.name))
        report = self.assess()
        self.assertEqual(report["coverage"]["run_log_missing_completion_records"], 3)
        self.assertIn("gpu_completion_loss_observed", self.codes(report))
        self.assertEqual(report["detailed_attribution"]["status"], "unknown")

    def test_incomplete_lifecycle_keeps_evidence_but_invalidates_comparison(self):
        for state, code in (("failed", 1), ("interrupted", None)):
            self.manifest.update(state=state, exit_code=code)
            report = self.assess()
            self.assertEqual(report["frame_metrics"]["status"], "invalid")
            self.assertTrue(report["artifacts_selected"])
            self.assertIn("run_incomplete", self.codes(report))

    def test_requested_frames_and_present_ids_must_match(self):
        original = copy.deepcopy(self.trace)
        for change in ("count", "sequence", "pid", "timestamp"):
            self.trace = copy.deepcopy(original)
            if change == "count":
                self.trace["traceEvents"].pop()
            elif change == "sequence":
                self.trace["traceEvents"][-1]["args"]["present"] = 8
            elif change == "pid":
                for event in self.trace["traceEvents"]:
                    event["pid"] = 8
            else:
                self.trace["traceEvents"][-1]["ts"] = 45000
            self.write_evidence()
            self.assertEqual(self.assess()["frame_metrics"]["status"], "invalid")
        self.trace = original
        self.manifest["capture"]["length"] = 3
        self.write_evidence()
        self.assertEqual(self.assess()["frame_metrics"]["status"], "invalid")

    def test_seconds_window_start_and_closing_overshoot_are_unknown(self):
        self.manifest["capture"] = {"unit": "seconds", "start": 2, "length": 1}
        self.manifest["environment"]["effective"] = {"APS5_TRACE_SECONDS": "2:1"}
        self.summary["window"]["spec"] = "seconds 2:1"
        self.summary["window"]["ms"] = 1040.0
        self.summary["frame_ms"] = {key: 1024.0 for key in ("p50", "p90", "p99", "max")}
        self.summary["env"] = {"APS5_TRACE_SECONDS": "2:1"}
        self.write_evidence()
        report = self.assess()
        self.assertEqual(report["frame_metrics"]["status"], "unknown")
        self.assertIn("seconds_start_unverified", self.codes(report))
        self.summary["window"]["ms"] = 900.0
        self.summary["frame_ms"] = {key: 884.0 for key in ("p50", "p90", "p99", "max")}
        self.write_evidence()
        self.assertEqual(self.assess()["status"], "invalid")

    def test_nonfinite_ordered_boolean_and_missing_metrics_are_rejected(self):
        original = copy.deepcopy(self.summary)
        for value in (float("nan"), float("inf"), True, -1, 50, 1, None):
            self.summary = copy.deepcopy(original)
            self.summary["frame_ms"]["p50"] = value
            self.write_evidence()
            self.assertEqual(self.assess()["frame_metrics"]["status"], "invalid")
        self.summary = copy.deepcopy(original)
        del self.summary["dirty"]
        self.write_evidence()
        self.assertEqual(self.assess()["frame_metrics"]["status"], "invalid")

    def test_duplicate_json_keys_and_truncated_gzip_are_rejected(self):
        path = self.run / "summary.jsonl"
        path.write_text('{"trace":"x","trace":"y"}\n', encoding="utf-8")
        self.manifest["artifacts"][0] = self.descriptor("summary", "summary.jsonl")
        self.assertEqual(self.assess()["status"], "invalid")
        self.write_evidence()
        path = self.run / self.trace_name
        path.write_bytes(path.read_bytes()[:-6])
        self.manifest["artifacts"][1] = self.descriptor("trace", self.trace_name)
        self.assertEqual(self.assess()["status"], "invalid")

    def test_artifact_run_id_schema_and_source_identity_must_agree(self):
        for key, value in (("run_id", "wrong-session"), ("schema_version", 2), ("commit", "c" * 40)):
            summary = copy.deepcopy(self.summary)
            summary[key] = value
            self.write_evidence([summary])
            self.assertEqual(self.assess()["status"], "invalid")
        self.trace["run_id"] = "wrong-session"
        self.write_evidence()
        self.assertEqual(self.assess()["status"], "invalid")

    def test_path_escape_and_symlink_escape_are_refused(self):
        self.manifest["artifacts"][0]["path"] = "../summary.jsonl"
        with self.assertRaises(SchemaError):
            self.assess()
        self.write_evidence()
        link = self.run / "outside.jsonl"
        try:
            link.symlink_to(self.binary)
        except OSError as error:
            self.skipTest(f"symlink creation unavailable: {error}")
        self.manifest["artifacts"][0]["path"] = link.name
        with self.assertRaises(SchemaError):
            self.assess()

    def test_path_sanitized_producer_environment_is_understood(self):
        self.manifest["environment"]["effective"].update(APS5_TRACE_DIR=str(self.run), VK_DRIVER_FILES="explicit-driver")
        self.summary["env"]["APS5_TRACE_DIR"] = "<path>"
        self.write_evidence()
        self.assertNotIn("environment_mismatch", self.codes(self.assess()))

    def test_missing_or_extra_observed_environment_is_rejected(self):
        self.summary["env"]["APS5_UNEXPECTED"] = "1"
        self.write_evidence()
        self.assertIn("environment_mismatch", self.codes(self.assess()))
        self.summary["env"].pop("APS5_UNEXPECTED")
        self.manifest["environment"]["effective"]["APS5_EXPECTED"] = "1"
        self.write_evidence()
        self.assertIn("environment_mismatch", self.codes(self.assess()))

    def test_short_producer_revision_is_matched_without_claiming_full_identity(self):
        self.summary["commit"] = "a" * 12
        self.write_evidence()
        report = self.assess()
        self.assertTrue(report["provenance"]["producer_revision_verified"])
        self.assertIn("producer_revision_abbreviated", self.codes(report))
        self.assertEqual(report["observed_context"]["commit"], "a" * 12)
        self.summary["commit"] = "unknown"
        self.write_evidence()
        report = self.assess()
        self.assertFalse(report["provenance"]["producer_revision_verified"])
        self.assertIn("producer_revision_unknown", self.codes(report))

    def test_malformed_boundary_receipts_are_invalid(self):
        for key, value in (("checked_at", "not-time"), ("findings", "none"), ("verified", 1)):
            inventory = copy.deepcopy(self.manifest["execution"]["inventory_verification"])
            inventory["after"][key] = value
            manifest = copy.deepcopy(self.manifest)
            manifest["execution"]["inventory_verification"] = inventory
            self.assertEqual(capture_health.assess_capture(self.run, manifest)["status"], "invalid")

    def test_manifest_reader_and_cli_only_report_json(self):
        (self.run / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")
        with patch.object(sys, "stdout", new_callable=io.StringIO) as output:
            self.assertEqual(capture_health.main([str(self.run), "--json"]), 0)
            self.assertEqual(json.loads(output.getvalue())["frame_metrics"]["status"], "valid")

    def test_observer_failure_is_visible_without_erasing_independent_frame_timings(self):
        self.manifest["execution"]["observation_errors"] = [
            {"phase": "evidence_attachment", "type": "ValueError", "message": "profile process identity differs"}]
        report = self.assess()
        self.assertIn("observation_collection_failed", self.codes(report))
        self.assertEqual(report["frame_metrics"]["status"], "valid")
        self.assertFalse(report["provenance"]["process_creation_identity_verified"])

    def test_malformed_condition_receipt_is_reported_without_aborting_health(self):
        (self.run / "conditions.json").write_text('{}', encoding="utf-8")
        self.manifest["artifacts"].append(self.descriptor("condition_receipt", "conditions.json"))
        report = self.assess()
        self.assertEqual(report["conditions"]["status"], "invalid")
        self.assertEqual(report["status"], "invalid")
        self.assertIn("condition_evidence_invalid", self.codes(report))
        self.assertEqual(report["frame_metrics"]["status"], "valid")

    def attach_workload(self, document=None):
        self.manifest["environment"]["effective"].update(APS5_TRACE_WORKLOAD="1", APS5_CAPTURE_RUN_ID=self.manifest["run_id"])
        self.summary["env"] = dict(self.manifest["environment"]["effective"])
        (self.run / "summary.jsonl").write_text(json.dumps(self.summary) + "\n", encoding="utf-8")
        self.manifest["artifacts"][0] = self.descriptor("summary", "summary.jsonl")
        name = f"aps5-workload-{self.manifest['run_id']}-7.json.gz"
        document = make_workload(self.manifest, self.trace_name) if document is None else document
        (self.run / name).write_bytes(gzip.compress(json.dumps(document).encode("utf-8"), mtime=0))
        self.manifest["artifacts"].append({**self.descriptor("measurement_workload", name), "trace": self.trace_name})

    def test_measurement_ledger_proves_counts_without_shader_or_scene_claims(self):
        self.attach_workload()
        report = self.assess()
        self.assertEqual(report["measurement_workload"]["status"], "valid")
        self.assertTrue(report["measurement_workload"]["correlation_valid"])
        self.assertEqual(report["evidence"]["metrics"]["workload.host_draw_commands"], 2)
        self.assertEqual(report["evidence"]["dimensions"]["scene"]["status"], "unknown")
        self.assertEqual(report["frame_metrics"]["status"], "valid")

    def test_workload_pending_outer_boundary_retains_observations_but_not_metrics(self):
        document = make_workload(self.manifest, self.trace_name)
        document["coverage"]["pending_at_open"] = [1, 0, 0]
        document["coverage"]["pending_at_close"] = [1, 0, 0]
        for row in document["frames"]:
            row["pending_at_begin"] = row["pending_at_end"] = [1, 0, 0]
        self.attach_workload(document)
        report = self.assess()
        self.assertEqual(report["measurement_workload"]["status"], "unknown")
        self.assertEqual(report["measurement_workload"]["metrics"], {})
        self.assertEqual(report["measurement_workload"]["observed_counts"]["host_draw_commands"], 2)

    def test_workload_binding_and_total_contradictions_are_invalid(self):
        for mutation in ("pid", "totals", "balance", "boundary"):
            with self.subTest(mutation=mutation):
                document = make_workload(self.manifest, self.trace_name)
                if mutation == "pid":
                    document["producer"]["pid"] = 8
                elif mutation == "totals":
                    document["counts"]["host_draw_commands"] += 1
                elif mutation == "balance":
                    document["frames"][0]["pending_at_end"] = [0, 1, 0]
                else:
                    document["frames"][0]["end_ns"] += 1
                self.manifest["artifacts"] = self.manifest["artifacts"][:2]
                self.attach_workload(document)
                report = self.assess()
                self.assertEqual(report["measurement_workload"]["status"], "invalid")
                self.assertEqual(report["frame_metrics"]["status"], "valid")

    def test_requested_collector_failure_is_error_without_erasing_frame_timing(self):
        self.manifest["execution"]["collector_errors"] = [{"phase": "start", "type": "OSError", "message": "collector failed"}]
        report = self.assess()
        self.assertEqual(report["status"], "invalid")
        self.assertEqual(report["frame_metrics"]["status"], "valid")
        self.assertEqual(report["coverage"]["collectors"]["status"], "invalid")


if __name__ == "__main__":
    unittest.main()
