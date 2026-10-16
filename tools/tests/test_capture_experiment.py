import copy
import gzip
import os
import tempfile
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_experiment as experiment
import capture_draws
import capture_evidence
import capture_health
from capture_schema import SchemaError, WORKLOAD_KEYS, atomic_write_json, canonical_json_bytes, canonical_sha256, read_json, sha256_file
from test_capture_draws import event, make_detail, recount


class ExperimentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.config = {
            "source_root": str(self.root / "source"),
            "runtime": {"executable": str(self.root / "runtime" / "synthetic.exe"), "libraries": []},
            "build": {"compiler": "synthetic", "build_type": "Release", "options": {}},
            "machine": {"id": "pc", "cpu": "cpu", "gpu": "gpu", "driver": "driver", "os": "os"},
            "title": {"id": "synthetic", "version": "1"},
            "scene": {"id": "scene", "recipe": {"checkpoint": "idle"}},
            "settings": {"resolution": "100x100", "presentation": "windowed",
                         "cache": {"shader": "warm", "pipeline": "warm", "driver": "warm"},
                         "diagnostics": {"mode": "measurement", "options": {}}, "relinker": {"flags": []}},
            "capture": {"unit": "frames", "start": 600, "length": 100},
            "execution": {"command": [str(self.root / "runtime" / "synthetic.exe")], "cwd": str(self.root / "runtime")},
            "environment": {"overrides": {}},
        }
        self.spec = {"id": "synthetic", "mode": "aa", "pairs": 5}
        self.plan = experiment.plan_experiment(self.spec, {"A": self.config, "B": self.config})
        self.runs = {}
        self.attempts = []
        for index, slot in enumerate(self.plan["schedule"]):
            directory = self.root / slot["slot_id"]
            directory.mkdir()
            manifest = self.make_manifest(slot, index)
            health = {"run_id": manifest["run_id"], "status": "unknown",
                      "frame_metrics": {"status": "valid", "metrics": {"mean_frame_ms": 10 + (slot["variant"] == "B")}},
                      "provenance": {key: True for key in ("runtime_boundaries_verified", "producer_revision_verified", "process_identity_verified")},
                      "observed_context": {"gpu": "gpu", "driver": "driver", "os": "os", "resolution": "100x100"},
                      "conditions": {"dimensions": {name: {"status": "valid", "basis": "reviewed_claim",
                                     "state": "warm" if name.startswith("cache.") else "matched", "policy_id": "policy"}
                                     for name in experiment.CONDITIONS}},
                      "evidence": {"dimensions": {"workload": {"status": "valid"}},
                                   "metrics": {"workload." + key: 100 for key in experiment.WORKLOAD_COUNTS},
                                   "metric_units": {"workload." + key: "count" for key in experiment.WORKLOAD_COUNTS},
                                   "metric_availability": {"workload." + key: "valid" for key in experiment.WORKLOAD_COUNTS}}}
            self.runs[directory] = manifest, health
            self.attempts.append({"slot_id": slot["slot_id"], "selection": "selected", "run_dir": str(directory)})
        self.directory = next(iter(self.runs))
        self.manifest = self.runs[self.directory][0]
        evidence_path = self.directory / "evidence.bin"
        evidence_path.write_bytes(b"synthetic reviewed evidence; not an image or game content")
        self.reference = {"path": evidence_path.name, "sha256": sha256_file(evidence_path), "size": evidence_path.stat().st_size,
                          "role": "actual", "media": "sequence", "coverage": copy.deepcopy(self.config["capture"])}
        self.manifest["artifacts"].append({key: self.reference[key] for key in ("path", "sha256", "size")} | {"kind": "review_evidence"})
        self.receipt = {"schema_version": 1, "document_type": "anyps5.capture-conditions", "run_id": self.manifest["run_id"],
                        "capture": self.config["capture"], "claims": {}}

    def make_manifest(self, slot, index):
        config = slot["config"]
        start = datetime(2026, 10, 16, tzinfo=timezone.utc) + timedelta(minutes=index * 2)
        return {"run_id": f"{index + 1:08x}-1234-4234-8234-123456789abc", "state": "completed", "exit_code": 0,
                "started_at": start.isoformat(), "ended_at": (start + timedelta(minutes=1)).isoformat(),
                "source": {"root": self.config["source_root"], "commit": "a" * 40, "dirty": False},
                "runtime": {"files": [{"role": "executable", "path": self.config["runtime"]["executable"], "size": 1, "sha256": "b" * 64}]},
                "artifacts": [{"kind": "trace", "path": "trace.json", "size": 1, "sha256": f"{index + 1:064x}"}],
                "machine": copy.deepcopy(config["machine"]), "build": copy.deepcopy(config["build"]),
                "title": config["title"], "scene": config["scene"], "settings": config["settings"],
                "capture": config["capture"], "experiment": config["experiment"], "execution": config["execution"],
                "environment": {"effective": {}}}

    def assess(self, plan=None, attempts=None):
        return experiment.assess_experiment(self.plan if plan is None else plan, self.attempts if attempts is None else attempts,
                                            assessor=lambda directory: self.runs[directory])

    def claim(self, name):
        value = {"state": "matched", "method": "artifact_review", "reviewer": "local reviewer", "reviewed_at": "2026-10-17T00:00:00Z",
                 "observations": "Explicit synthetic review observation", "scope": "whole_window",
                 "criteria": [{"id": "condition", "result": "passed", "observation": "Reviewer explicitly checked the criterion"}],
                 "artifacts": [copy.deepcopy(self.reference)]}
        if name == "scene":
            value.update(scene_id="scene", recipe_sha256=canonical_sha256(self.config["scene"]["recipe"]))
        elif name == "readiness":
            value.update(state="ready", checkpoint="idle")
        elif name == "correctness":
            value.update(state="preserved", reference_identity="known reference")
            path = self.directory / "reference.bin"
            path.write_bytes((self.directory / self.reference["path"]).read_bytes())
            reference = copy.deepcopy(self.reference) | {"role": "reference", "path": path.name}
            value["artifacts"].append(reference)
            self.manifest["artifacts"].append({key: reference[key] for key in ("path", "sha256", "size")} | {"kind": "review_evidence"})
        else:
            value.update(state="warm", policy_id="policy")
        self.receipt["claims"][name] = value
        return value

    def conditions(self):
        return experiment.assess_condition_receipt(self.receipt, self.manifest, self.directory)

    def test_alternating_schedule_balances_order_and_freezes_inputs(self):
        self.assertEqual([row["variant"] for row in self.plan["schedule"][:4]], ["A", "B", "B", "A"])
        self.config["scene"]["id"] = "mutated"
        self.assertEqual(self.plan["schedule"][0]["config"]["scene"]["id"], "scene")
        self.assertFalse(self.plan["execution"]["launch_supported"])
        self.assertTrue(self.plan["execution"]["requires_explicit_user_approval"])

    def test_randomized_schedule_is_seeded_balanced_and_reproducible(self):
        spec = self.spec | {"ordering": "randomized", "seed": "seed"}
        one = experiment.plan_experiment(spec, {"A": self.config, "B": self.config})
        two = experiment.plan_experiment(spec, {"A": self.config, "B": self.config})
        self.assertEqual(one, two)
        for index in range(0, len(one["schedule"]), 2):
            self.assertEqual({row["variant"] for row in one["schedule"][index:index + 2]}, {"A", "B"})

    def test_aa_rejects_hidden_diagnostic_change(self):
        changed = copy.deepcopy(self.config)
        changed["environment"]["overrides"]["APS5_PROFILE_DRAW"] = "1"
        with self.assertRaisesRegex(SchemaError, "identical"):
            experiment.plan_experiment(self.spec, {"A": self.config, "B": changed})

    def test_overhead_preserves_build_and_accepts_declared_diagnostic_treatment(self):
        changed = copy.deepcopy(self.config)
        changed["settings"]["diagnostics"] = {"mode": "attribution", "options": {"trace_detail": "draw"}}
        spec = self.spec | {"mode": "overhead", "treatment_paths": ["/settings/diagnostics"]}
        plan = experiment.plan_experiment(spec, {"A": self.config, "B": changed})
        self.assertEqual(plan["configuration_differences"], ["/settings/diagnostics/mode", "/settings/diagnostics/options/trace_detail"])
        changed["build"]["options"]["parallel"] = False
        with self.assertRaisesRegex(SchemaError, "same build"):
            experiment.plan_experiment(spec, {"A": self.config, "B": changed})

    def test_ab_rejects_undeclared_difference_and_scene_treatment(self):
        changed = copy.deepcopy(self.config)
        changed["build"]["options"]["feature"] = True
        with self.assertRaisesRegex(SchemaError, "explicit treatment"):
            experiment.plan_experiment(self.spec | {"mode": "ab"}, {"A": self.config, "B": changed})
        with self.assertRaisesRegex(SchemaError, "specific JSON"):
            experiment.plan_experiment(self.spec | {"mode": "ab", "treatment_paths": ["/scene/id"]}, {"A": self.config, "B": changed})

    def test_observer_overhead_is_explicit_and_collection_policy_is_bound(self):
        a, b = copy.deepcopy(self.config), copy.deepcopy(self.config)
        a["observation"] = {"enabled": False, "interval_ms": 1000}
        b["observation"] = {"enabled": True, "interval_ms": 500}
        spec = self.spec | {"mode": "overhead", "treatment_paths": ["/observation/enabled", "/observation/interval_ms"]}
        plan = experiment.plan_experiment(spec, {"A": a, "B": b})
        slot = plan["schedule"][0]
        manifest = self.make_manifest(slot, 20)
        manifest["observation"] = copy.deepcopy(slot["config"]["observation"])
        experiment._bound_config(manifest, slot["config"])
        manifest["observation"]["profiling_evidence"] = [{"attachment": "post-run"}]
        experiment._bound_config(manifest, slot["config"])
        manifest["observation"]["interval_ms"] = 50
        with self.assertRaisesRegex(SchemaError, "observer collection policy"):
            experiment._bound_config(manifest, slot["config"])

    def test_attachment_only_differences_cannot_masquerade_as_overhead(self):
        a, b = copy.deepcopy(self.config), copy.deepcopy(self.config)
        a["observation"] = {"enabled": False}
        b["observation"] = {"enabled": False, "build_receipts": [str(self.root / "build-receipt.json")]}
        spec = self.spec | {"mode": "overhead", "treatment_paths": ["/observation/build_receipts"]}
        with self.assertRaisesRegex(SchemaError, "diagnostic treatment"):
            experiment.plan_experiment(spec, {"A": a, "B": b})

    def test_collectors_require_explicit_overhead_treatment_and_actual_policy_binding(self):
        tool = self.root / "wpr.exe"
        tool.write_bytes(b"synthetic collector identity, never executed")
        a, b = copy.deepcopy(self.config), copy.deepcopy(self.config)
        a["collectors"] = {"enabled": False}
        b["collectors"] = {"enabled": True, "kind": "wpr_cpu", "wpr": str(tool)}
        with self.assertRaisesRegex(SchemaError, "declare every"):
            experiment.plan_experiment(self.spec | {"mode": "overhead"}, {"A": a, "B": b})
        plan = experiment.plan_experiment(self.spec | {"mode": "overhead", "treatment_paths": ["/collectors"]}, {"A": a, "B": b})
        slot = plan["schedule"][0]
        manifest = self.make_manifest(slot, 20)
        manifest["collectors"] = copy.deepcopy(slot["config"]["collectors"])
        experiment._bound_config(manifest, slot["config"])
        manifest["collectors"] = copy.deepcopy(plan["schedule"][1]["config"]["collectors"])
        with self.assertRaisesRegex(SchemaError, "collector collection policy"):
            experiment._bound_config(manifest, slot["config"])
        del manifest["collectors"]
        with self.assertRaisesRegex(SchemaError, "collector collection policy"):
            experiment._bound_config(manifest, slot["config"])

    def test_archived_collector_plan_validates_after_original_tools_profile_and_mapping_are_removed(self):
        tool = self.root / "wpr.exe"
        exporter = self.root / "WPAExporter.exe"
        profile = self.root / "synthetic.wpaProfile"
        mapping = self.root / "mapping.json"
        tool.write_bytes(b"synthetic WPR tool; never executed")
        exporter.write_bytes(b"synthetic export tool; never executed")
        profile.write_text("synthetic saved export profile", encoding="utf-8")
        atomic_write_json(mapping, {
            "schema_version": 1, "document_type": "anyps5.table-mapping",
            "tool": {"name": "Synthetic exported table", "version": "test-1"},
            "table": {"name": "Explicit synthetic rows", "record_kind": "samples"},
            "csv": {"delimiter": ",", "quotechar": '"', "encoding": "utf-8"},
            "clock": {"domain": "query_performance_counter", "input_unit": "ticks", "ticks_per_second": 10_000_000},
            "columns": {"pid": "P", "timestamp": "Time", "thread_id": "T", "weight_samples": "Count"}})
        config = copy.deepcopy(self.config)
        config["collectors"] = {"enabled": True, "kind": "wpr_cpu", "wpr": str(tool),
            "export": {"tool": str(exporter), "profile": str(profile), "table_file": "samples.csv", "mapping": str(mapping)}}
        from capture_collectors import validate_collectors_config
        validate_collectors_config(config["collectors"])
        plan = experiment.plan_experiment(self.spec, {"A": config, "B": config})
        archive = self.root / "archived-plan.json"
        atomic_write_json(archive, plan)
        for original in (tool, exporter, profile, mapping):
            original.unlink()
        archived = read_json(archive)
        with mock.patch.object(Path, "open", side_effect=AssertionError("archived plan validation must not read installed files")), \
                mock.patch("capture_collectors.tool_identity", side_effect=AssertionError("archived plan must not inspect installed tools")):
            self.assertIs(experiment.validate_plan(archived), archived)
        self.assertEqual(archived, plan)
        self.assertEqual(archived["plan_sha256"], plan["plan_sha256"])

    def lightweight_workload(self, health, *, status="valid"):
        return {"status": status, "run_id": health["run_id"],
                "metrics": {name: 100 for name in experiment.WORKLOAD_COUNTS},
                "metric_units": {name: "count" for name in experiment.WORKLOAD_COUNTS},
                "observed_counts": {"operation_status.complete": 300, "operation_status.skipped": 0,
                                    "operation_status.failed": 0, "operation_status.rejected": 0,
                                    "operation_status.cancelled": 0}}

    def test_lightweight_workload_does_not_certify_missing_scene_or_correctness(self):
        for _, health in self.runs.values():
            health["evidence"] = {}
            health["measurement_workload"] = self.lightweight_workload(health)
            health["conditions"] = {}
        report = self.assess()
        self.assertEqual(report["status"], "insufficient")
        self.assertFalse(any("workload" in reason for reason in report["reasons"]), report["reasons"])
        self.assertTrue(any("scene unproven" in reason for reason in report["reasons"]))
        self.assertTrue(any("correctness unproven" in reason for reason in report["reasons"]))
        self.assertTrue(all(row["workload"]["workload.operation_status.complete"] == 300 for row in report["runs"]))

    def test_lightweight_unknown_or_wrong_run_binding_never_becomes_workload_proof(self):
        health = self.runs[self.directory][1]
        health["evidence"] = {}
        health["measurement_workload"] = self.lightweight_workload(health, status="unknown")
        health["measurement_workload"]["coverage"] = {"pending_at_close": [1, 0, 0]}
        self.assertEqual(experiment.workload_observations(health), ("unknown", {}))
        health["measurement_workload"]["status"] = "valid"
        health["measurement_workload"]["run_id"] = "another run"
        self.assertEqual(experiment.workload_observations(health), ("invalid", {}))
        self.assertEqual(self.assess()["status"], "insufficient")

    def test_lightweight_detail_disagreement_withholds_counts_and_paired_changes_are_visible(self):
        health = self.runs[self.directory][1]
        health["measurement_workload"] = self.lightweight_workload(health)
        health["measurement_workload"]["metrics"]["host_draw_commands"] = 101
        self.assertEqual(experiment.workload_observations(health), ("invalid", {}))
        for _, observed in self.runs.values():
            observed["evidence"] = {}
            observed["measurement_workload"] = self.lightweight_workload(observed)
        health["measurement_workload"]["metrics"]["host_draw_commands"] = 101
        report = self.assess()
        self.assertEqual(report["status"], "insufficient")
        self.assertTrue(any("recorded workload counts differ" in reason for reason in report["reasons"]))

    def raw_lightweight_plan(self, *, crossing=False):
        config = copy.deepcopy(self.config)
        config["environment"]["overrides"]["APS5_TRACE_WORKLOAD"] = "1"
        plan = experiment.plan_experiment(self.spec, {"A": config, "B": config})
        for index, (directory, (manifest, health)) in enumerate(self.runs.items()):
            manifest["environment"]["effective"] = {"APS5_TRACE_WORKLOAD": "1", "APS5_CAPTURE_RUN_ID": manifest["run_id"]}
            manifest["execution"]["pid"] = 7
            name = f"aps5-trace-{'a' * 12}-7.json.gz"
            counts = dict.fromkeys(WORKLOAD_KEYS, 0)
            counts.update(requested_draw_operations=2, dispatch_operations=3, host_draw_commands=2,
                          host_dispatch_commands=3, host_commands_completed=5)
            counts["operation_status.complete"] = 5
            pending = [int(crossing), 0, 0]
            rows = [{"present": 600 + frame, "begin_ns": 1_000_000 + frame * 10_000_000,
                     "end_ns": 1_000_000 + (frame + 1) * 10_000_000, "counts": copy.deepcopy(counts),
                     "pending_at_begin": list(pending), "pending_at_end": list(pending)} for frame in range(100)]
            value = {"schema_version": 1, "document_type": "anyps5.measurement-workload", "run_id": manifest["run_id"],
                     "producer": {"pid": 7, "revision": "a" * 12, "dirty": False}, "trace": name,
                     "clock": {"domain": "steady_clock", "unit": "ns", "origin": "implementation_defined_steady_epoch"},
                     "capture": {"spec": "frames 600:100", "open_present": 600, "close_present": 700,
                                 "open_ns": 1_000_000, "close_ns": 1_001_000_000},
                     "counts": {key: count * 100 for key, count in counts.items()}, "frames": rows,
                     "coverage": {"closed": True, "maximum_intervals": 65536, "lost_intervals": 0, "overflow": False,
                                  "lifetime_error": False, "pending_at_open": pending, "pending_at_close": pending}}
            path = directory / f"aps5-workload-{manifest['run_id']}-7.json.gz"
            path.write_bytes(gzip.compress(canonical_json_bytes(value)))
            descriptor = {"kind": "measurement_workload", "path": path.name, "sha256": sha256_file(path),
                          "size": path.stat().st_size, "trace": name}
            manifest["artifacts"].append(descriptor)
            health["evidence"] = None
            health["artifacts_selected"] = []
            health["coverage"] = {"capture_completed": True}
            health["conditions"] = {}
            capture_health._assess_workload_sidecar(directory, manifest, {path.name: path},
                {"commit": "a" * 12, "dirty": False, "window": {"ms": 1000}},
                {"presents": [(600 + frame, frame * 10000, 1) for frame in range(100)]}, name, health,
                lambda *args, **kwargs: None)
        return plan

    def test_raw_lightweight_reader_feeds_experiment_without_certifying_conditions(self):
        plan = self.raw_lightweight_plan()
        report = self.assess(plan)
        self.assertEqual(report["status"], "insufficient")
        self.assertTrue(all(health["measurement_workload"]["status"] == "valid" for _, health in self.runs.values()))
        self.assertTrue(all(row["workload"]["workload.requested_draw_operations"] == 200 for row in report["runs"]))
        self.assertFalse(any("workload" in reason for reason in report["reasons"]), report["reasons"])
        self.assertTrue(any("readiness unproven" in reason for reason in report["reasons"]))

    def test_raw_crossing_lightweight_lifetime_withholds_experiment_workload(self):
        plan = self.raw_lightweight_plan(crossing=True)
        report = self.assess(plan)
        self.assertEqual(report["status"], "insufficient")
        self.assertTrue(all(health["measurement_workload"]["status"] == "unknown" for _, health in self.runs.values()))
        self.assertTrue(all(not row["workload"] for row in report["runs"]))
        self.assertTrue(any("workload equivalence unproven" in reason for reason in report["reasons"]))

    def collector_plan(self):
        tool = self.root / "wpr.exe"
        tool.write_bytes(b"synthetic tool; not executed")
        config = copy.deepcopy(self.config)
        config["collectors"] = {"enabled": True, "kind": "wpr_cpu", "wpr": str(tool)}
        plan = experiment.plan_experiment(self.spec, {"A": config, "B": config})
        for index, (_, (manifest, health)) in enumerate(self.runs.items()):
            manifest["collectors"] = copy.deepcopy(plan["schedule"][index]["config"]["collectors"])
            health["collector_observation"] = {"run_id": manifest["run_id"], "status": "unknown", "tool_identity_status": "valid",
                "tool_identities": [{"role": "start", "path": str(tool), "size": 32, "sha256": "a" * 64,
                                     "file_version": None, "version_source": "unavailable"}]}
        return plan

    def test_requested_collector_without_bound_receipt_identity_is_insufficient(self):
        plan = self.collector_plan()
        health = self.runs[self.directory][1]
        health.pop("collector_observation")
        report = self.assess(plan)
        self.assertEqual(report["status"], "insufficient")
        self.assertTrue(any("collector tool identity unproven" in reason for reason in report["reasons"]))
        health["collector_observation"] = {"run_id": "another run", "tool_identity_status": "valid", "tool_identities": []}
        self.assertEqual(experiment.collector_tool_observations(self.runs[self.directory][0], health), ("invalid", []))

    def test_same_path_collector_change_within_variant_is_rejected(self):
        plan = self.collector_plan()
        self.runs[self.directory][1]["collector_observation"]["tool_identities"][0]["sha256"] = "b" * 64
        with self.assertRaisesRegex(SchemaError, "collector tool identity drift"):
            self.assess(plan)

    def test_same_policy_collector_changes_across_variants_prevent_eligibility(self):
        plan = self.collector_plan()
        for slot, (_, health) in zip(plan["schedule"], self.runs.values()):
            if slot["variant"] == "B":
                health["collector_observation"]["tool_identities"][0]["sha256"] = "b" * 64
        report = self.assess(plan)
        self.assertEqual(report["status"], "insufficient")
        self.assertTrue(any("collector tool identities differ under the same collection policy" in reason for reason in report["reasons"]))

    def test_pair_and_tool_bounds_are_enforced_without_execution(self):
        for count in (0, 1001, True):
            with self.assertRaises(SchemaError):
                experiment.plan_experiment(self.spec | {"pairs": count}, {"A": self.config, "B": self.config})
        tool = {"id": "collector", "measurement": "live", "start": ["wpr", "-start", "CPU"], "stop": ["wpr", "-stop", "out.etl"], "export": ["export", "out.etl"]}
        with self.assertRaisesRegex(SchemaError, "bounded collection"):
            experiment.plan_experiment(self.spec | {"external_tools": [tool]}, {"A": self.config, "B": self.config})
        plan = experiment.plan_experiment(self.spec | {"external_tools": [tool | {"maximum_seconds": 60}]}, {"A": self.config, "B": self.config})
        self.assertEqual(plan["external_tools"][0]["measurement"], "live")

    def test_plan_hash_and_schedule_tampering_are_rejected(self):
        changed = copy.deepcopy(self.plan)
        changed["schedule"][0]["variant"] = "B"
        with self.assertRaisesRegex(SchemaError, "hash"):
            experiment.validate_plan(changed)
        changed["plan_sha256"] = experiment._sealed(changed, "plan_sha256")
        with self.assertRaisesRegex(SchemaError, "deterministic"):
            experiment.validate_plan(changed)

    def test_malformed_schedule_element_is_a_schema_error(self):
        changed = copy.deepcopy(self.plan)
        changed["schedule"][0] = "not an object"
        changed["plan_sha256"] = experiment._sealed(changed, "plan_sha256")
        with self.assertRaisesRegex(SchemaError, "slot must be an object"):
            experiment.validate_plan(changed)

    def test_five_matched_pairs_produce_descriptive_eligible_result(self):
        report = self.assess()
        self.assertEqual(report["status"], "eligible")
        self.assertEqual(report["summary"]["median_delta"], 1)
        self.assertEqual(report["summary"]["count"], 5)
        self.assertEqual(report["conclusion"], "descriptive_matched_observations")

    def test_fewer_pairs_remain_insufficient(self):
        plan = experiment.plan_experiment(self.spec | {"pairs": 2}, {"A": self.config, "B": self.config})
        report = self.assess(plan, self.attempts[:4])
        self.assertEqual(report["status"], "insufficient")
        self.assertTrue(any("five" in reason for reason in report["reasons"]))

    def test_missing_conditions_or_low_overhead_workload_remain_unproven(self):
        health = self.runs[self.directory][1]
        health["conditions"] = {}
        health["evidence"] = {}
        report = self.assess()
        self.assertEqual(report["status"], "insufficient")
        self.assertTrue(any("workload equivalence" in reason for reason in report["reasons"]))
        self.assertIsNotNone(report["runs"][0]["metric"])

    def consumer_workload_plan(self, *, missing_terminal=False):
        config = copy.deepcopy(self.config)
        config["capture"] = {"unit": "frames", "start": 10, "length": 2}
        config["settings"]["diagnostics"] = {"mode": "attribution", "options": {"trace_detail": "draw"}}
        config["environment"]["overrides"] = {"APS5_TRACE_DETAIL": "draw"}
        plan = experiment.plan_experiment(self.spec, {"A": config, "B": config})
        for index, (directory, (manifest, health)) in enumerate(self.runs.items()):
            slot = plan["schedule"][index]
            manifest["capture"] = copy.deepcopy(slot["config"]["capture"])
            manifest["settings"] = copy.deepcopy(slot["config"]["settings"])
            manifest["environment"]["effective"] = {"APS5_TRACE_DETAIL": "draw", "APS5_TRACE_FRAMES": "10:2",
                                                       "APS5_CAPTURE_RUN_ID": manifest["run_id"]}
            manifest["execution"]["pid"] = 7
            _, document, trace_name, summary, trace = make_detail(directory)
            document["run_id"] = manifest["run_id"]
            document["capture_id"] = f"{index + 101:08x}-2222-4222-8222-222222222222"
            operation = event(501, "operation", "701", name="dispatch", operation="701",
                              start=1_000_000, end=None if missing_terminal else 2_000_000,
                              status="unknown" if missing_terminal else "complete")
            path = event(502, "attribute", "701", name="dispatch_path", operation="701")
            path["text"] = "fill_hle"
            document["events"].extend([operation, path])
            recount(document)
            detail = capture_draws.assess_draw_trace(document, manifest, trace_name=trace_name, summary=summary, trace=trace)
            health["draw_attribution"] = detail
            health["evidence"] = capture_evidence.assess_evidence(document, manifest, detail)
            health["conditions"] = {"dimensions": {name: {"status": "unknown"} for name in experiment.CONDITIONS}}
            health["frame_metrics"]["metrics"]["mean_frame_ms"] = 20
        return plan

    def test_actual_consumer_known_noop_workload_does_not_certify_conditions(self):
        plan = self.consumer_workload_plan()
        report = self.assess(plan)
        self.assertEqual(report["status"], "insufficient")
        self.assertTrue(all(health["draw_attribution"]["identity_status"] == "unknown" for _, health in self.runs.values()))
        self.assertFalse(any("workload" in reason for reason in report["reasons"]), report["reasons"])
        self.assertTrue(any("correctness unproven" in reason for reason in report["reasons"]))
        self.assertTrue(all(row["workload"]["workload.dispatch_operations"] == 1 for row in report["runs"]))

    def test_actual_consumer_missing_terminal_keeps_experiment_workload_unproven(self):
        plan = self.consumer_workload_plan(missing_terminal=True)
        report = self.assess(plan)
        self.assertEqual(report["status"], "insufficient")
        self.assertTrue(any("workload equivalence unproven" in reason for reason in report["reasons"]))
        self.assertTrue(all(not row["workload"] for row in report["runs"]))

    def test_unknown_metric_units_do_not_become_measurements(self):
        spec = self.spec | {"metric": {"name": "external.cpu", "unit": "ms", "direction": "lower"}}
        plan = experiment.plan_experiment(spec, {"A": self.config, "B": self.config})
        for manifest, health in self.runs.values():
            health["evidence"]["metrics"]["external.cpu"] = 2
            health["evidence"]["metric_availability"] = {"external.cpu": "valid"}
        report = self.assess(plan)
        self.assertTrue(all(row["metric"] is None for row in report["runs"]))

    def test_duplicate_directory_and_run_id_are_rejected(self):
        attempts = copy.deepcopy(self.attempts)
        attempts[1]["run_dir"] = attempts[0]["run_dir"]
        with self.assertRaisesRegex(SchemaError, "directory reused"):
            self.assess(attempts=attempts)
        directories = list(self.runs)
        self.runs[directories[1]][0]["run_id"] = self.manifest["run_id"]
        self.runs[directories[1]][1]["run_id"] = self.manifest["run_id"]
        with self.assertRaisesRegex(SchemaError, "identity reused"):
            self.assess()

    def test_reused_trace_and_overlapping_process_are_rejected(self):
        directories = list(self.runs)
        second = self.runs[directories[1]][0]
        second["artifacts"][0]["sha256"] = self.manifest["artifacts"][0]["sha256"]
        with self.assertRaisesRegex(SchemaError, "content reused"):
            self.assess()
        second["artifacts"][0]["sha256"] = "f" * 64
        second["started_at"] = self.manifest["started_at"]
        with self.assertRaisesRegex(SchemaError, "overlap"):
            self.assess()

    def test_within_variant_source_drift_and_planned_config_mismatch_are_rejected(self):
        directory = list(self.runs)[3]
        self.runs[directory][0]["source"]["commit"] = "c" * 40
        with self.assertRaisesRegex(SchemaError, "drift within"):
            self.assess()
        self.runs[directory][0]["source"]["commit"] = "a" * 40
        self.runs[directory][0]["experiment"] = {"id": "unrelated", "pair_id": "pair-0002"}
        with self.assertRaisesRegex(SchemaError, "planned experiment"):
            self.assess()

    def test_workload_or_cache_policy_difference_blocks_eligibility(self):
        health = self.runs[list(self.runs)[1]][1]
        health["evidence"]["metrics"]["workload.requested_draw_operations"] = 99
        health["conditions"]["dimensions"]["cache.shader"]["policy_id"] = "different"
        report = self.assess()
        self.assertTrue(any("workload counts differ" in reason for reason in report["reasons"]))
        self.assertTrue(any("cache policies" in reason for reason in report["reasons"]))

    def test_rejection_history_remains_visible_and_conservative(self):
        path = self.directory / self.reference["path"]
        rejected = {"slot_id": self.attempts[0]["slot_id"], "selection": "rejected", "reason": "missing window",
                    "evidence": {"path": str(path), "sha256": self.reference["sha256"], "size": self.reference["size"]}}
        report = self.assess(attempts=[rejected] + self.attempts)
        self.assertEqual(report["status"], "insufficient")
        self.assertEqual(report["attempt_history"][0], rejected)

    def test_declared_unknown_conditions_remain_unknown(self):
        self.receipt["claims"]["scene"] = {"state": "unknown"}
        self.assertEqual(self.conditions()["dimensions"]["scene"]["status"], "unknown")

    def test_receipt_binds_run_window_recipe_and_capture_uuid(self):
        self.claim("scene")
        changed = copy.deepcopy(self.receipt)
        changed["run_id"] = "other"
        with self.assertRaisesRegex(SchemaError, "run binding"):
            experiment.assess_condition_receipt(changed, self.manifest, self.directory)
        wrong_capture = self.receipt | {"capture_id": "wrong"}
        with self.assertRaisesRegex(SchemaError, "capture binding"):
            experiment.assess_condition_receipt(wrong_capture, self.manifest, self.directory, capture_id="actual")
        self.receipt["claims"]["scene"]["recipe_sha256"] = "0" * 64
        with self.assertRaisesRegex(SchemaError, "recipe"):
            self.conditions()

    def test_reviewed_scene_is_distinct_from_automatic_observation(self):
        self.claim("scene")
        dimension = self.conditions()["dimensions"]["scene"]
        self.assertEqual(dimension["status"], "valid")
        self.assertEqual(dimension["basis"], "reviewed_claim")
        self.assertEqual(dimension["criteria"][0]["result"], "passed")

    def test_hashes_do_not_imply_passed_criteria(self):
        claim = self.claim("readiness")
        claim["criteria"][0]["result"] = "unavailable"
        self.assertEqual(self.conditions()["dimensions"]["readiness"]["status"], "unknown")
        claim["criteria"][0]["result"] = "failed"
        self.assertEqual(self.conditions()["dimensions"]["readiness"]["status"], "invalid")

    def test_one_image_cannot_cover_a_whole_window(self):
        claim = self.claim("correctness")
        claim["artifacts"][0]["media"] = "image"
        with self.assertRaisesRegex(SchemaError, "one sampled frame"):
            self.conditions()
        claim["artifacts"][0]["coverage"]["length"] = 1
        self.assertEqual(self.conditions()["dimensions"]["correctness"]["status"], "unknown")

    def test_correctness_requires_actual_and_reference_coverage(self):
        claim = self.claim("correctness")
        self.assertEqual(self.conditions()["dimensions"]["correctness"]["status"], "valid")
        claim["artifacts"] = claim["artifacts"][:1]
        self.assertEqual(self.conditions()["dimensions"]["correctness"]["status"], "unknown")

    def test_correctness_refuses_self_reference_but_allows_equal_pixels_on_distinct_paths(self):
        claim = self.claim("correctness")
        self.assertEqual(self.conditions()["dimensions"]["correctness"]["status"], "valid")
        self.assertEqual(claim["artifacts"][0]["sha256"], claim["artifacts"][1]["sha256"])
        claim["artifacts"][1]["path"] = claim["artifacts"][0]["path"]
        with self.assertRaisesRegex(SchemaError, "with itself"):
            self.conditions()

    def test_correctness_refuses_hardlink_aliases(self):
        claim = self.claim("correctness")
        path = self.directory / claim["artifacts"][1]["path"]
        path.unlink()
        try:
            os.link(self.directory / claim["artifacts"][0]["path"], path)
        except OSError as error:
            self.skipTest(f"synthetic hardlinks unavailable: {error}")
        with self.assertRaisesRegex(SchemaError, "hardlink aliases"):
            self.conditions()

    def test_artifact_tamper_and_unbound_path_are_rejected(self):
        claim = self.claim("scene")
        (self.directory / self.reference["path"]).write_bytes(b"changed")
        with self.assertRaisesRegex(SchemaError, "integrity"):
            self.conditions()
        claim["artifacts"][0]["path"] = "../escape"
        with self.assertRaisesRegex(SchemaError, "bound manifest"):
            self.conditions()

    def test_review_time_must_be_utc_and_after_capture(self):
        claim = self.claim("scene")
        for timestamp in ("2026-10-17T00:00:00", "2026-10-15T00:00:00Z"):
            claim["reviewed_at"] = timestamp
            with self.assertRaises(SchemaError):
                self.conditions()

    def test_driver_cache_file_snapshot_does_not_verify_global_state(self):
        claim = self.claim("cache.driver")
        dimension = self.conditions()["dimensions"]["cache.driver"]
        self.assertEqual(dimension["status"], "unknown")
        claim.update(observation_scope="vendor_measured", measurement_method="Explicit synthetic vendor measurement review")
        self.assertEqual(self.conditions()["dimensions"]["cache.driver"]["status"], "unknown")
        claim["artifacts"][0].update(media="measurement", evidence_type="vendor_cache_state", tool={"name": "Synthetic vendor measurement", "version": "1"})
        dimension = self.conditions()["dimensions"]["cache.driver"]
        self.assertEqual(dimension["status"], "valid")
        self.assertEqual(dimension["basis"], "reviewed_claim")

    def test_conditions_loader_is_independent_of_health_and_missing_is_unknown(self):
        with mock.patch.object(experiment, "validate_manifest", return_value=self.manifest), mock.patch.object(experiment, "read_json", return_value=self.manifest):
            report = experiment.assess_run_conditions(self.directory)
        self.assertEqual(report["status"], "unknown")

    def test_condition_uuid_requires_fresh_native_binding(self):
        self.claim("scene")
        self.receipt["capture_id"] = "native-capture"
        self.assertEqual(self.conditions()["dimensions"]["scene"]["status"], "unknown")
        report = experiment.assess_condition_receipt(self.receipt, self.manifest, self.directory, capture_id="native-capture")
        self.assertEqual(report["dimensions"]["scene"]["status"], "valid")

    def test_attach_conditions_preserves_manifest_bytes_and_excludes_existing_lock(self):
        self.claim("scene")
        manifest_path = self.directory / "manifest.json"
        atomic_write_json(manifest_path, self.manifest)
        original = manifest_path.read_bytes()
        receipt = self.root / "review.json"
        atomic_write_json(receipt, self.receipt)
        lock = self.directory / ".capture-attach.lock"
        lock.write_text("another attachment owns this", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            experiment.attach_conditions(self.directory, receipt)
        self.assertTrue(lock.exists())
        self.assertEqual(manifest_path.read_bytes(), original)
        lock.unlink()
        with mock.patch.object(experiment, "validate_manifest", side_effect=lambda value, **kwargs: value):
            result = experiment.attach_conditions(self.directory, receipt)
        self.assertEqual((self.directory / result["previous_manifest"]).read_bytes(), original)
        self.assertEqual(read_json(manifest_path)["artifacts"][-1]["kind"], "condition_receipt")
        self.assertFalse(lock.exists())
        with mock.patch.object(experiment, "validate_manifest", side_effect=lambda value, **kwargs: value):
            with self.assertRaisesRegex(SchemaError, "already exists"):
                experiment.attach_conditions(self.directory, receipt)

    def test_attachment_copies_explicit_evidence_without_changing_source(self):
        claim = self.claim("scene")
        claim["artifacts"][0]["path"] = "external/conditions/sequence.bin"
        manifest_path = self.directory / "manifest.json"
        atomic_write_json(manifest_path, self.manifest)
        receipt = self.root / "review.json"
        atomic_write_json(receipt, self.receipt)
        source = self.directory / self.reference["path"]
        before = source.read_bytes()
        with mock.patch.object(experiment, "validate_manifest", side_effect=lambda value, **kwargs: value):
            result = experiment.attach_conditions(self.directory, receipt, [{"path": "external/conditions/sequence.bin", "source": str(source)}])
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual((self.directory / "external/conditions/sequence.bin").read_bytes(), before)
        self.assertEqual(result["assessment"]["dimensions"]["scene"]["status"], "valid")

    def test_attachment_refuses_copying_one_original_into_both_correctness_roles(self):
        claim = self.claim("correctness")
        claim["artifacts"][0]["path"] = "external/conditions/actual.bin"
        claim["artifacts"][1]["path"] = "external/conditions/reference.bin"
        manifest_path = self.directory / "manifest.json"
        atomic_write_json(manifest_path, self.manifest)
        before = manifest_path.read_bytes()
        receipt = self.root / "review.json"
        atomic_write_json(receipt, self.receipt)
        source = str(self.directory / self.reference["path"])
        copies = [{"path": reference["path"], "source": source} for reference in claim["artifacts"]]
        with mock.patch.object(experiment, "validate_manifest", side_effect=lambda value, **kwargs: value):
            with self.assertRaisesRegex(SchemaError, "one original file"):
                experiment.attach_conditions(self.directory, receipt, copies)
        self.assertEqual(manifest_path.read_bytes(), before)
        self.assertFalse((self.directory / ".capture-attach.lock").exists())

    def test_fresh_default_assessment_never_reads_saved_health(self):
        manifest_path = self.directory / "manifest.json"
        atomic_write_json(manifest_path, self.manifest)
        (self.directory / "health.json").write_text('{"status":"valid","forged":true}', encoding="utf-8")
        expected = self.runs[self.directory][1]
        with mock.patch.object(experiment, "validate_manifest", return_value=self.manifest), \
                mock.patch.object(experiment, "_artifact_reference"), \
                mock.patch("capture_health.assess_capture", return_value=expected) as fresh:
            manifest, health = experiment._fresh_assessment(self.directory)
        self.assertIs(health, expected)
        self.assertIs(manifest, self.manifest)
        fresh.assert_called_once_with(self.directory, manifest=self.manifest)

    def test_hypothesis_history_requires_source_events_and_preserves_chain(self):
        ledger = {"schema_version": 1, "document_type": "anyps5.hypothesis-ledger", "events": []}
        path = self.directory / self.reference["path"]
        reference = {"path": str(path), "sha256": self.reference["sha256"], "size": self.reference["size"], "source_event": "/events/2"}
        observation = {"id": "o", "kind": "observation", "text": "Observed selected wait", "at": "2026-10-17T00:00:00Z", "artifacts": [reference]}
        ledger = experiment.append_hypothesis_event(ledger, observation)
        hypothesis = {"id": "h", "kind": "hypothesis", "text": "Wait may delay submission", "at": "2026-10-17T00:01:00Z", "links": ["o"], "artifacts": [reference]}
        ledger = experiment.append_hypothesis_event(ledger, hypothesis)
        self.assertEqual(ledger["events"][1]["previous_sha256"], ledger["events"][0]["event_sha256"])
        ledger["events"][0]["text"] = "edited history"
        with self.assertRaisesRegex(SchemaError, "hash chain"):
            experiment.append_hypothesis_event(ledger, hypothesis | {"id": "new"})

    def test_supported_claim_cannot_be_submitted_without_fresh_assessment(self):
        path = self.directory / self.reference["path"]
        reference = {"path": str(path), "sha256": self.reference["sha256"], "size": self.reference["size"], "line": 1}
        ledger = {"schema_version": 1, "document_type": "anyps5.hypothesis-ledger", "events": []}
        for kind, identifier, links in (("observation", "o", []), ("hypothesis", "h", ["o"]), ("prediction", "p", ["h"]), ("experiment", "e", ["p"])):
            event = {"id": identifier, "kind": kind, "links": links, "at": "2026-10-17T00:00:00Z", "text": kind, "artifacts": [reference]}
            if kind == "prediction":
                event["prediction"] = {"metric": "mean_frame_ms", "unit": "ms", "direction": "higher", "threshold": 0.5}
            if kind == "experiment":
                event["plan_sha256"] = self.plan["plan_sha256"]
            ledger = experiment.append_hypothesis_event(ledger, event)
        decision = {"id": "d", "kind": "decision", "links": ["e"], "at": "2026-10-17T00:00:00Z", "text": "Prediction evaluation", "artifacts": [reference], "outcome": "supported"}
        with self.assertRaisesRegex(SchemaError, "freshly assessed"):
            experiment.append_hypothesis_event(ledger, decision)
        report_path = self.root / "decision-report.json"
        updated, report = experiment._conclude_hypothesis(ledger, decision, self.plan, self.attempts, report_path=report_path,
                                                         assessor=lambda directory: self.runs[directory])
        self.assertEqual(report["status"], "eligible")
        self.assertEqual(updated["events"][-1]["outcome"], "supported")
        self.assertEqual(updated["events"][-1]["causal_status"], "unknown")
        self.assertEqual(updated["events"][-1]["statistical_significance"], "not_established")
        self.assertEqual(updated["events"][-1]["evidence_scope"], "descriptive_matched_observations")
        self.assertEqual(updated["events"][-1]["assessment_artifact"]["sha256"], sha256_file(report_path))
        self.assertEqual(len(ledger["events"]), 4)


if __name__ == "__main__":
    unittest.main()
