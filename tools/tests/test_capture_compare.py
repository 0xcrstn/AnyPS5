import copy
import io
import json
import os
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock
from uuid import uuid4

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_compare
from capture_schema import canonical_sha256, sha256_file
from test_capture_schema import make_health, make_manifest


class CaptureCompareTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifests = {}
        self.health = {}
        self.sequence = 0
        self.assessor = mock.patch.object(capture_compare, "assess_capture", side_effect=self.assess).start()
        self.addCleanup(mock.patch.stopall)

    def assess(self, directory, manifest=None):
        self.assertIsNotNone(manifest)
        return copy.deepcopy(self.health[Path(directory)])

    def run_fixture(self, timing=20.0, presents=4):
        self.sequence += 1
        directory = self.root / f"run-{self.sequence}"
        directory.mkdir()
        manifest = make_manifest(self.root)
        manifest["run_id"] = str(uuid4())
        start = datetime(2026, 1, 1, tzinfo=timezone.utc) + timedelta(minutes=self.sequence * 2)
        manifest.update(started_at=start.isoformat().replace("+00:00", "Z"),
                        ended_at=(start + timedelta(minutes=1)).isoformat().replace("+00:00", "Z"),
                        state="completed", exit_code=0)
        manifest["environment"]["effective"]["APS5_TRACE_DIR"] = str(directory / "traces")
        for kind, filename in (("trace", "trace.json.gz"), ("summary", "summary.jsonl")):
            path = directory / filename
            path.write_bytes(f"{kind} synthetic evidence {self.sequence}".encode("utf-8"))
            manifest["artifacts"].append({"kind": kind, "path": filename, "sha256": sha256_file(path),
                                          "size": path.stat().st_size})
        health = make_health()
        health.update(run_id=manifest["run_id"], status="unknown")
        health["provenance"]["runtime_boundaries_verified"] = True
        health["provenance"]["producer_revision_verified"] = True
        health["provenance"]["process_identity_verified"] = True
        health["observed_context"] = {"gpu": "observed GPU", "driver": "observed driver", "os": "observed OS",
                                      "resolution": "640x480", "commit": manifest["source"]["commit"],
                                      "dirty": False, "env": {}}
        health["frame_metrics"] = {"status": "valid", "metrics": {"frame_ms.p50": timing, "mean_frame_ms": timing},
                                   "presents": presents, "duration_ms": timing * presents}
        health["artifacts_selected"] = copy.deepcopy(manifest["artifacts"])
        self.health[directory] = health
        self.manifests[directory] = manifest
        self.save(directory)
        return directory

    def save(self, directory):
        manifest = self.manifests[directory]
        self.health[directory]["run_id"] = manifest["run_id"]
        self.health[directory]["observed_context"].update(commit=manifest["source"]["commit"],
                                                         dirty=manifest["source"]["dirty"])
        self.health[directory]["artifacts_selected"] = copy.deepcopy(manifest["artifacts"])
        (directory / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    def cohorts(self):
        return [self.run_fixture(10, 1000), self.run_fixture(30, 4)], [self.run_fixture(12), self.run_fixture(16)]

    def test_run_level_medians_do_not_pool_frames_and_report_baseline_variation(self):
        baseline, candidate = self.cohorts()
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertEqual(report["status"], "comparable", report["reasons"])
        metric = report["metrics"]["mean_frame_ms"]
        self.assertEqual(metric["baseline"]["values"], [10, 30])
        self.assertEqual(metric["baseline"]["median"], 20)
        self.assertEqual(metric["baseline"]["range"], 20)
        self.assertEqual(metric["candidate"]["median"], 14)
        self.assertEqual(metric["change"], -6)
        self.assertEqual(metric["change_pct"], -30)
        self.assertEqual(metric["baseline"]["count"], 2)
        self.assertEqual(self.assessor.call_count, 4)
        self.assertEqual(report["pairing"], "unpaired")
        self.assertEqual(report["pairs"], [])
        limitations = " ".join(report["limitations"])
        self.assertIn("source-to-binary binding is unverified", limitations)
        self.assertIn("loaded modules are unverified", limitations)
        self.assertIn("not confidence intervals", limitations)
        self.assertIn("false dropped flag", limitations)

    def test_one_pair_is_insufficient_but_retains_descriptive_metrics(self):
        report = capture_compare.compare_runs([self.run_fixture(20)], [self.run_fixture(10)])
        self.assertEqual(report["status"], "insufficient")
        self.assertEqual(report["metrics"]["frame_ms.p50"]["change"], -10)
        self.assertIsNone(report["metrics"]["frame_ms.p50"]["baseline"]["stdev"])
        self.assertTrue(any("one pair" in reason for reason in report["reasons"]))

    def test_source_and_runtime_differences_are_allowed_only_between_cohorts(self):
        baseline, candidate = self.cohorts()
        for directory in candidate:
            manifest = self.manifests[directory]
            manifest["source"]["commit"] = "b" * 40
            manifest["source"].update(dirty=True, diff_sha256="c" * 64)
            manifest["runtime"]["files"][0]["sha256"] = "d" * 64
            self.save(directory)
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertEqual(report["status"], "comparable", report["reasons"])
        self.assertIn("source_sha256", report["identity_changes"])
        self.assertIn("runtime_sha256", report["identity_changes"])
        self.manifests[candidate[1]]["source"]["diff_sha256"] = "e" * 64
        self.save(candidate[1])
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertEqual(report["status"], "incomparable")
        self.assertEqual(report["metrics"], {})

    def test_mismatched_conditions_are_incomparable(self):
        cases = [("title", "version", "2"), ("machine", "cpu", "other CPU"),
                 ("machine", "driver", "other driver"), ("settings", "resolution", "320x240"),
                 ("settings", "cache", {"shader": "cold", "pipeline": "warm", "driver": "warm"}),
                 ("settings", "diagnostics", {"mode": "forensic", "options": {}}),
                 ("build", "options", {"option": True})]
        for section, key, value in cases:
            with self.subTest(section=section, key=key):
                before, after = self.run_fixture(), self.run_fixture()
                self.manifests[after][section][key] = value
                self.save(after)
                report = capture_compare.compare_runs([before], [after])
                self.assertEqual(report["status"], "incomparable", report["reasons"])
                self.assertIn(f"{section}.{key}" if not isinstance(value, dict) else section,
                              " ".join(report["configuration_differences"]))

    def test_scene_recipe_window_and_meaningful_path_environment_changes_fail(self):
        for change in ("recipe", "window", "cache_path"):
            with self.subTest(change=change):
                before, after = self.run_fixture(), self.run_fixture()
                manifest = self.manifests[after]
                if change == "recipe":
                    manifest["scene"]["recipe"] = {"steps": ["other"]}
                    manifest["scene"]["recipe_sha256"] = canonical_sha256(manifest["scene"]["recipe"])
                elif change == "window":
                    manifest["capture"]["start"] = 20
                else:
                    manifest["environment"]["effective"]["APS5_SHADER_CACHE_PATH"] = "/different/cache"
                self.save(after)
                report = capture_compare.compare_runs([before], [after])
                self.assertEqual(report["status"], "incomparable", report["reasons"])

    def test_duplicate_directory_uuid_and_trace_bytes_are_not_independent_runs(self):
        for change in ("directory", "uuid", "trace"):
            with self.subTest(change=change):
                before, after = self.run_fixture(), self.run_fixture()
                if change == "directory":
                    after = before
                elif change == "uuid":
                    self.manifests[after]["run_id"] = self.manifests[before]["run_id"]
                    self.save(after)
                else:
                    path = after / "trace.json.gz"
                    path.write_bytes((before / "trace.json.gz").read_bytes())
                    artifact = self.manifests[after]["artifacts"][0]
                    artifact.update(sha256=sha256_file(path), size=path.stat().st_size)
                    self.save(after)
                report = capture_compare.compare_runs([before], [after])
                self.assertEqual(report["status"], "incomparable", report["reasons"])
                self.assertTrue(any("Reused" in reason or "reuse byte-identical" in reason for reason in report["reasons"]))

    def test_overlapping_lifecycles_cannot_be_independent_observations(self):
        before, after = self.run_fixture(), self.run_fixture()
        for key in ("started_at", "ended_at"):
            self.manifests[after][key] = self.manifests[before][key]
        self.save(after)
        report = capture_compare.compare_runs([before], [after])
        self.assertEqual(report["status"], "incomparable")
        self.assertTrue(any("overlap" in reason for reason in report["reasons"]))

    def test_explicit_pairing_uses_identity_and_checks_sets(self):
        baseline, candidate = self.cohorts()
        for directories in (baseline, candidate):
            for index, directory in enumerate(directories):
                self.manifests[directory]["experiment"] = {"id": "experiment", "pair_id": f"pair-{index}"}
                self.save(directory)
        report = capture_compare.compare_runs(baseline, list(reversed(candidate)))
        self.assertEqual(report["status"], "comparable", report["reasons"])
        self.assertEqual(report["pairing"], "explicit_pairs")
        self.assertEqual(report["pairs"][0]["candidate_run_id"], self.manifests[candidate[0]]["run_id"])
        self.manifests[candidate[0]]["experiment"]["pair_id"] = "missing-pair"
        self.save(candidate[0])
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertEqual(report["status"], "incomparable")
        self.assertTrue(any("pair identities do not match" in reason for reason in report["reasons"]))

    def test_partial_and_duplicate_pair_identities_fail(self):
        baseline, candidate = self.cohorts()
        self.manifests[baseline[0]]["experiment"] = {"id": "experiment", "pair_id": "pair"}
        self.save(baseline[0])
        self.assertEqual(capture_compare.compare_runs(baseline, candidate)["status"], "incomparable")
        for directory in baseline + candidate:
            self.manifests[directory]["experiment"] = {"id": "experiment", "pair_id": "pair"}
            self.save(directory)
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertEqual(report["status"], "incomparable")
        self.assertTrue(any("Duplicate" in reason for reason in report["reasons"]))

    def test_missing_metric_is_unavailable_and_never_zero(self):
        baseline, candidate = self.cohorts()
        self.health[candidate[0]]["frame_metrics"]["metrics"]["mean_frame_ms"] = None
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertEqual(report["status"], "comparable")
        metric = report["metrics"]["mean_frame_ms"]
        self.assertFalse(metric["available"])
        self.assertEqual(metric["candidate"]["missing"], 1)
        self.assertIsNone(metric["change"])
        self.assertIsNone(metric["change_pct"])
        self.assertIn("n/a", capture_compare.text_report(report))

    def test_unknown_cache_or_unverified_provenance_is_insufficient(self):
        for change in ("cache", "runtime", "revision", "process", "observed"):
            with self.subTest(change=change):
                baseline, candidate = self.cohorts()
                if change == "cache":
                    for directory in baseline + candidate:
                        self.manifests[directory]["settings"]["cache"]["shader"] = "unknown"
                        self.save(directory)
                elif change == "runtime":
                    self.health[candidate[0]]["provenance"]["runtime_boundaries_verified"] = False
                elif change == "revision":
                    self.health[candidate[0]]["provenance"]["producer_revision_verified"] = False
                elif change == "process":
                    self.health[candidate[0]]["provenance"]["process_identity_verified"] = False
                else:
                    for directory in baseline + candidate:
                        self.health[directory]["observed_context"]["gpu"] = ""
                report = capture_compare.compare_runs(baseline, candidate)
                self.assertEqual(report["status"], "insufficient", report["reasons"])

    def test_observed_conditions_must_match_despite_unchanged_declarations(self):
        for key in ("gpu", "driver", "os", "resolution"):
            with self.subTest(key=key):
                baseline, candidate = self.cohorts()
                for directory in candidate:
                    self.health[directory]["observed_context"][key] = "different observation"
                report = capture_compare.compare_runs(baseline, candidate)
                self.assertEqual(report["status"], "incomparable")
                self.assertIn(f"observed_context.{key}", report["configuration_differences"])
                self.health[candidate[0]]["observed_context"][key] = "another observation"
                report = capture_compare.compare_runs(baseline, candidate)
                self.assertEqual(report["status"], "incomparable")
                self.assertTrue(any("within the cohort" in reason for reason in report["reasons"]))

    def test_local_checkout_paths_do_not_change_source_identity(self):
        baseline, candidate = self.cohorts()
        for index, directory in enumerate(baseline):
            self.manifests[directory]["source"]["root"] = str(self.root / f"checkout-{index}")
            self.save(directory)
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertEqual(report["status"], "comparable", report["reasons"])
        self.assertNotIn("source_sha256", report["identity_changes"])

    def test_different_working_directories_are_incomparable(self):
        baseline, candidate = self.cohorts()
        for directory in candidate:
            self.manifests[directory]["execution"]["cwd"] = str(self.root / "another-runtime-layout")
            self.save(directory)
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertEqual(report["status"], "incomparable")
        self.assertIn("cwd", report["configuration_differences"])

    def test_stale_health_json_is_ignored_and_health_is_recomputed(self):
        baseline, candidate = self.cohorts()
        (candidate[0] / "health.json").write_text('{"status":"valid"}', encoding="utf-8")
        self.health[candidate[0]]["status"] = "invalid"
        self.health[candidate[0]]["frame_metrics"]["status"] = "invalid"
        self.health[candidate[0]]["issues"] = [{"code": "lost-present", "severity": "error", "message": "Missing present"}]
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertEqual(report["status"], "invalid")
        self.assertEqual(self.assessor.call_count, 4)
        self.assertEqual(report["metrics"], {})

    def test_artifact_mutation_and_escape_fail_before_health(self):
        for change in ("mutation", "escape"):
            with self.subTest(change=change):
                before, after = self.run_fixture(), self.run_fixture()
                if change == "mutation":
                    (after / "trace.json.gz").write_bytes(b"mutated")
                else:
                    self.manifests[after]["artifacts"][0]["path"] = "../escape.json"
                    self.save(after)
                report = capture_compare.compare_runs([before], [after])
                self.assertEqual(report["status"], "invalid", report["reasons"])
                self.assertEqual(report["metrics"], {})

    def test_malformed_manifest_and_empty_inputs_return_invalid_reports(self):
        before, after = self.run_fixture(), self.run_fixture()
        (after / "manifest.json").write_text('{"run_id":"a","run_id":"b"}', encoding="utf-8")
        self.assertEqual(capture_compare.compare_runs([before], [after])["status"], "invalid")
        self.assertEqual(capture_compare.compare_runs([], [before])["status"], "invalid")

    def test_cli_accepts_repeated_inputs_and_returns_status_without_execution(self):
        baseline, candidate = self.cohorts()
        arguments = [item for directory in baseline for item in ("--baseline", str(directory))]
        arguments += [item for directory in candidate for item in ("--candidate", str(directory))]
        with mock.patch("sys.stdout", new_callable=io.StringIO) as output:
            result = capture_compare.main(arguments + ["--json"])
        self.assertEqual(result, 0)
        report = json.loads(output.getvalue())
        self.assertEqual(report["baseline"]["run_count"], 2)
        self.assertEqual(report["candidate"]["run_count"], 2)
        with mock.patch("sys.stdout", new_callable=io.StringIO) as output:
            result = capture_compare.main(["--baseline", str(baseline[0]), "--candidate", str(candidate[0])])
        self.assertEqual(result, 1)
        self.assertIn("Capture comparison: insufficient", output.getvalue())

    def test_shared_hardlink_artifact_is_not_an_independent_capture(self):
        before, after = self.run_fixture(), self.run_fixture()
        path = after / "summary.jsonl"
        path.unlink()
        try:
            os.link(before / "summary.jsonl", path)
        except OSError as error:
            self.skipTest(f"Hardlinks unavailable: {error}")
        artifact = self.manifests[after]["artifacts"][1]
        artifact.update(sha256=sha256_file(path), size=path.stat().st_size)
        self.save(after)
        report = capture_compare.compare_runs([before], [after])
        self.assertEqual(report["status"], "incomparable")
        self.assertTrue(any("same selected artifact file" in reason for reason in report["reasons"]))


    def test_same_metric_name_with_different_units_never_produces_a_delta(self):
        baseline = [self.run_fixture(), self.run_fixture()]
        candidate = [self.run_fixture(), self.run_fixture()]
        for directory in baseline + candidate:
            self.health[directory]["evidence"] = {
                "schema_version": 1, "document_type": "anyps5.evidence-report",
                "run_id": self.manifests[directory]["run_id"], "capture_id": None,
                "status": "unknown", "issues": [], "dimensions": {},
                "metrics": {"external.duration": 10}, "metric_availability": {"external.duration": "valid"},
                "metric_units": {"external.duration": "ms" if directory in baseline else "ns"}}
        report = capture_compare.compare_runs(baseline, candidate)
        self.assertFalse(report["metrics"]["evidence.external.duration"]["available"])
        self.assertIsNone(report["metrics"]["evidence.external.duration"]["change"])
        self.assertTrue(report["metrics"]["mean_frame_ms"]["available"])


class EvidenceMetricTests(unittest.TestCase):
    def test_lightweight_workload_is_run_bound_typed_and_independent_of_draw_detail(self):
        health = {"run_id": "run", "measurement_workload": {"run_id": "run", "status": "valid",
                  "metrics": {name: 2 for name in ("requested_draw_operations", "dispatch_operations", "host_draw_commands",
                                                   "host_dispatch_commands", "indirect_record_operations")},
                  "metric_units": {name: "count" for name in ("requested_draw_operations", "dispatch_operations", "host_draw_commands",
                                                             "host_dispatch_commands", "indirect_record_operations")}}}
        metrics = capture_compare._draw_metrics(health)
        self.assertEqual(metrics["draw.workload.requested_draw_operations"], 2)
        self.assertEqual(capture_compare._metric_unit(health, "draw.workload.requested_draw_operations"), "count")
        self.assertFalse(any(name.startswith("draw.gpu.") for name in metrics))
        health["measurement_workload"]["run_id"] = "another run"
        self.assertEqual(capture_compare._draw_metrics(health), {})

    def test_lightweight_contradiction_removes_detailed_and_evidence_workload_metrics(self):
        health = {"run_id": "run", "draw_attribution": {"correlation_valid": True, "workload_status": "valid",
                  "metrics": {"workload": {"host_draw_commands": 2}}},
                  "measurement_workload": {"status": "invalid", "run_id": "run"}}
        self.assertEqual(capture_compare._draw_metrics(health), {})

    def test_direct_comparison_binds_actual_collectors_policy(self):
        before = make_manifest()
        after = copy.deepcopy(before)
        after["collectors"] = {"enabled": True, "kind": "wpr_cpu", "profile": "CPU"}
        self.assertNotEqual(capture_compare._context(before, {}), capture_compare._context(after, {}))
        before["collectors"] = {"enabled": False}
        self.assertNotEqual(capture_compare._context(before, {}), capture_compare._context(after, {}))

    def test_lightweight_binding_excludes_only_verified_generated_run_identity(self):
        manifest = make_manifest()
        manifest["environment"]["effective"]["APS5_CAPTURE_RUN_ID"] = manifest["run_id"]
        health = {"measurement_workload": {"run_id": manifest["run_id"], "status": "unknown", "correlation_valid": True}}
        self.assertNotIn("APS5_CAPTURE_RUN_ID", capture_compare._environment(manifest, health))
        health["measurement_workload"]["correlation_valid"] = False
        self.assertIn("APS5_CAPTURE_RUN_ID", capture_compare._environment(manifest, health))
        health["measurement_workload"].update(correlation_valid=True, run_id="wrong run")
        self.assertIn("APS5_CAPTURE_RUN_ID", capture_compare._environment(manifest, health))

    def test_same_collector_path_with_changed_verified_bytes_changes_context(self):
        manifest = make_manifest()
        manifest["collectors"] = {"enabled": True, "kind": "wpr_cpu", "wpr": "/synthetic/wpr.exe"}
        first = {"collector_observation": {"run_id": manifest["run_id"], "tool_identity_status": "valid",
                 "tool_identities": [{"role": "start", "path": "/synthetic/wpr.exe", "size": 20,
                                      "sha256": "a" * 64, "file_version": "1", "version_source": "pe_version_resource"}]}}
        second = copy.deepcopy(first)
        second["collector_observation"]["tool_identities"][0]["sha256"] = "b" * 64
        self.assertNotEqual(capture_compare._context(manifest, first), capture_compare._context(manifest, second))
        self.assertEqual(capture_compare._context(manifest, {})["collector_tools"]["status"], "unknown")

    def test_workload_availability_is_independent_of_shader_identity_with_legacy_fallback(self):
        detail = {"correlation_valid": True, "identity_status": "unknown", "workload_status": "valid",
                  "metrics": {"workload": {"requested_draw_operations": 2}, "shaders": {"shader.calls": 3}}}
        health = {"draw_attribution": detail}
        self.assertEqual(capture_compare._draw_metrics(health), {"draw.workload.requested_draw_operations": 2})
        detail["workload_status"] = "unknown"
        detail["identity_status"] = "valid"
        self.assertNotIn("draw.workload.requested_draw_operations", capture_compare._draw_metrics(health))
        del detail["workload_status"]
        self.assertEqual(capture_compare._draw_metrics(health)["draw.workload.requested_draw_operations"], 2)

    def test_attachment_locations_do_not_change_observation_policy(self):
        before = make_manifest()
        after = copy.deepcopy(before)
        before["observation"] = {"enabled": True, "interval_ms": 100, "profiling_evidence": [{"path": "one.json"}]}
        after["observation"] = {"enabled": True, "interval_ms": 100, "profiling_evidence": [{"path": "two.json"}]}
        self.assertEqual(capture_compare._context(before, {}), capture_compare._context(after, {}))
        after["observation"]["interval_ms"] = 200
        self.assertNotEqual(capture_compare._context(before, {}), capture_compare._context(after, {}))

    def test_available_measurements_survive_independent_unknown_dimensions(self):
        health = {"run_id": "run", "draw_attribution": {"correlation_valid": False},
                  "evidence": {"run_id": "run", "status": "unknown",
                               "metrics": {"cpu.elapsed_ms": 12, "cpu.scheduled_ms": None, "gpu.mapped_ms": 8},
                               "metric_availability": {"cpu.elapsed_ms": "valid", "cpu.scheduled_ms": "unknown",
                                                       "gpu.mapped_ms": "unknown"}}}
        self.assertEqual(capture_compare._draw_metrics(health), {"evidence.cpu.elapsed_ms": 12})
        health["evidence"]["run_id"] = "different"
        self.assertEqual(capture_compare._draw_metrics(health), {})
        health["evidence"].update(run_id="run", status="invalid")
        self.assertEqual(capture_compare._draw_metrics(health), {})

    def test_observer_overhead_policy_is_a_comparison_condition(self):
        before = make_manifest()
        after = copy.deepcopy(before)
        after["observation"] = {"enabled": True, "interval_ms": 10, "max_samples": 100}
        differences = capture_compare._differences(capture_compare._context(before, {}),
                                                  capture_compare._context(after, {}))
        self.assertIn("observation", differences)


if __name__ == "__main__":
    unittest.main()
