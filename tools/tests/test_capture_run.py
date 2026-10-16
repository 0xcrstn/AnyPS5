import copy
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capture_run
import capture_schema


class CaptureRunTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source_root = self.root / "source"
        self.source_root.mkdir()
        self.run_dir = self.root / "capture"
        self.executable = self.root / "synthetic-runtime.bin"
        self.library = self.root / "synthetic-library.bin"
        self.executable.write_bytes(b"synthetic executable identity only")
        self.library.write_bytes(b"synthetic library identity only")
        self.config = {
            "source_root": str(self.source_root),
            "runtime": {"executable": str(self.executable), "libraries": [str(self.library)]},
            "build": {"compiler": "synthetic compiler", "build_type": "Release", "options": {}},
            "machine": {"id": "fixture", "cpu": "CPU", "gpu": "GPU", "driver": "1", "os": "OS"},
            "title": {"id": "synthetic", "version": "1"},
            "scene": {"id": "fixture", "recipe": {"step": "synthetic"}},
            "settings": {"resolution": "640x480", "presentation": "windowed",
                         "cache": {"shader": "warm", "pipeline": "warm", "driver": "warm"},
                         "diagnostics": {"mode": "measurement", "options": {}},
                         "relinker": {"flags": []}},
            "capture": {"unit": "frames", "start": 3, "length": 2},
            "execution": {"command": [str(self.executable), "argument with spaces", "$(literal)"],
                          "cwd": str(self.root)},
            "environment": {"overrides": {"APS5_STORE_AT_FLIP": "1"}},
        }
        source = {"root": str(self.source_root), "commit": "1" * 40, "dirty": False,
                  "diff_sha256": "2" * 64, "untracked": [], "submodules": []}
        self.patch(capture_run.capture_manifest, "source_fingerprint", return_value=source)
        self.process = mock.Mock()
        self.process.pid = 12345
        self.process.wait.return_value = 0
        self.process.poll.return_value = 0
        self.launch = self.patch(capture_run.subprocess, "Popen", return_value=self.process)
        self.health = self.patch(capture_run.capture_health, "assess_capture", side_effect=self.health_report)
        self.interrupt = self.patch(capture_run, "interrupt_child")

    def patch(self, target, name, **kwargs):
        patcher = mock.patch.object(target, name, **kwargs)
        self.addCleanup(patcher.stop)
        return patcher.start()

    def health_report(self, run_dir, manifest=None, **kwargs):
        return {"schema_version": 1, "document_type": "anyps5.capture-health",
                "run_id": manifest["run_id"], "status": "unknown",
                "frame_metrics": {"status": "unknown", "metrics": {}},
                "detailed_attribution": {"status": "unknown", "limitations": []},
                "provenance": {"status": "unknown", "loaded_modules_verified": False,
                               "source_binary_binding": "unverified"}, "issues": []}

    def run_capture(self, **kwargs):
        return capture_run.run_capture(self.config, self.run_dir, inherited_environment={"PATH": "fixture"}, **kwargs)

    def write_capture(self, *, pid=12345, extra_summary=False):
        trace = f"aps5-trace-{'1' * 40}-{pid}.json"
        (self.run_dir / "traces" / trace).write_text('{"traceEvents":[]}', encoding="utf-8")
        record = {"trace": trace, "window": {"spec": "frames 3:2"}}
        records = [record, record] if extra_summary else [record]
        (self.run_dir / "traces" / "aps5-summary.jsonl").write_text(
            "".join(json.dumps(item) + "\n" for item in records), encoding="utf-8")

    def configure_collectors(self, *, errors=None, events=None):
        tool = self.root / "wpr.exe"
        tool.write_bytes(b"synthetic collector tool identity only")
        self.config["collectors"] = {"enabled": True, "kind": "wpr_cpu", "wpr": str(tool),
            "profile": "CPU", "max_duration_seconds": 10, "max_trace_bytes": 1024 * 1024,
            "command_timeout_seconds": 2, "export": None}
        session = mock.Mock()
        session.artifacts = []

        def finish():
            if events is not None:
                events.append("collector_finish")
            receipt = {"schema_version": 1, "document_type": "anyps5.collector-receipt",
                "run_id": capture_schema.read_json(self.run_dir / "manifest.json")["run_id"],
                "state": "failed" if errors else "stopped", "status": "failed" if errors else "stopped",
                "errors": [] if errors is None else errors, "test_only": True}
            directory = self.run_dir / "collectors"
            directory.mkdir(exist_ok=True)
            path = directory / "receipt.json"
            capture_schema.atomic_write_json(path, receipt)
            session.artifacts[:] = [{"kind": "collector_receipt", "path": "collectors/receipt.json",
                **capture_run.capture_manifest.file_identity(path)}]
            return receipt

        session.finish.side_effect = finish
        constructor = self.patch(capture_run.capture_collectors, "CollectorSession", return_value=session)
        return session, constructor

    def test_manifest_precedes_spawn_and_child_environment_is_isolated(self):
        inherited = {"PATH": "fixture", "SECRET": "private", "aPs5_PROFILE_GPU": "1",
                     "ANYPS5_OLD": "on", "vk_OLD": "on", "APS5_TRACE_SECONDS": "7:9"}
        original = dict(inherited)
        seen = []

        def launch(command, **kwargs):
            planned = capture_schema.read_json(self.run_dir / "manifest.json")
            self.assertEqual(planned["state"], "planned")
            self.assertTrue(planned["execution"]["inventory_verification"]["before"]["verified"])
            self.assertEqual(command, self.config["execution"]["command"])
            self.assertIs(kwargs["shell"], False)
            self.assertEqual(kwargs["executable"], str(self.executable))
            self.assertEqual(kwargs["cwd"], str(self.root))
            self.assertEqual(kwargs["stderr"], subprocess.STDOUT)
            self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
            self.assertEqual(kwargs["env"]["APS5_TRACE_FRAMES"], "3:2")
            self.assertEqual(kwargs["env"]["APS5_TRACE_DIR"], str(self.run_dir / "traces"))
            self.assertNotIn("APS5_PROFILE_GPU", kwargs["env"])
            self.assertNotIn("aPs5_PROFILE_GPU", kwargs["env"])
            self.assertNotIn("APS5_TRACE_SECONDS", kwargs["env"])
            self.assertEqual(kwargs["env"]["SECRET"], "private")
            kwargs["stdout"].write(b"synthetic process output\n")
            seen.append(planned)
            return self.process

        result = capture_run.run_capture(self.config, self.run_dir, inherited_environment=inherited, popen=launch)
        self.assertEqual(inherited, original)
        self.assertEqual(len(seen), 1)
        manifest = result["manifest"]
        self.assertEqual(manifest["state"], "completed")
        self.assertEqual(manifest["execution"]["pid"], 12345)
        self.assertIsNotNone(manifest["started_at"])
        self.assertIsNotNone(manifest["ended_at"])
        self.assertNotIn("SECRET", manifest["environment"]["effective"])
        self.assertNotIn("PATH", manifest["environment"]["effective"])
        self.assertEqual(set(manifest["environment"]["removed_inherited"]),
                         {"aPs5_PROFILE_GPU", "ANYPS5_OLD", "vk_OLD", "APS5_TRACE_SECONDS"})
        self.assertEqual((self.run_dir / "run.log").read_bytes(), b"synthetic process output\n")
        self.assertTrue(manifest["execution"]["inventory_verification"]["after"]["verified"])
        self.assertFalse(manifest["runtime"]["loaded_modules_verified"])

    def test_seconds_window_does_not_inherit_conflicting_frames(self):
        config = copy.deepcopy(self.config)
        config["capture"] = {"unit": "seconds", "start": 0, "length": 5}
        child, metadata = capture_run.child_environment(config, self.run_dir / "traces",
                                                       {"APS5_TRACE_FRAMES": "9:9"})
        self.assertEqual(child["APS5_TRACE_SECONDS"], "0:5")
        self.assertNotIn("APS5_TRACE_FRAMES", child)
        self.assertEqual(metadata["removed_inherited"], ["APS5_TRACE_FRAMES"])

    def test_reserved_trace_environment_fails_before_directory_creation(self):
        for name in ("APS5_TRACE_DIR", "aps5_trace_frames", "APS5_TRACE_SECONDS"):
            with self.subTest(name=name):
                self.config["environment"]["overrides"] = {name: "conflict"}
                with self.assertRaisesRegex(ValueError, "capture config owns"):
                    self.run_capture()
                self.assertFalse(self.run_dir.exists())
        self.launch.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "Windows batch script launch policy")
    def test_windows_batch_script_is_rejected_before_mutation(self):
        self.config["runtime"]["executable"] = str(self.root / "synthetic.cmd")
        self.config["execution"]["command"][0] = self.config["runtime"]["executable"]
        with self.assertRaisesRegex(ValueError, "batch script"):
            self.run_capture()
        self.assertFalse(self.run_dir.exists())
        self.launch.assert_not_called()

    def test_existing_run_directory_preserves_unrelated_evidence(self):
        self.run_dir.mkdir()
        old = self.run_dir / "manifest.json"
        old.write_bytes(b"existing evidence")
        with self.assertRaises(FileExistsError):
            self.run_capture()
        self.assertEqual(old.read_bytes(), b"existing evidence")
        self.launch.assert_not_called()

    def test_evidence_inside_source_tree_is_rejected_before_mutation(self):
        destination = self.source_root / "capture"
        with self.assertRaisesRegex(ValueError, "outside source_root"):
            capture_run.run_capture(self.config, destination)
        self.assertFalse(destination.exists())
        self.launch.assert_not_called()

    def test_initial_exclusive_manifest_failure_does_not_overwrite_existing_file(self):
        real_save = capture_run.save_manifest

        def save(directory, manifest, *, replace=True):
            if not replace:
                (directory / "manifest.json").write_bytes(b"unexpected evidence")
            return real_save(directory, manifest, replace=replace)

        self.patch(capture_run, "save_manifest", side_effect=save)
        result = self.run_capture()
        self.assertEqual(result["manifest"]["state"], "failed")
        self.assertEqual((self.run_dir / "manifest.json").read_bytes(), b"unexpected evidence")
        self.assertEqual({path.name for path in self.run_dir.iterdir()}, {"manifest.json"})
        self.health.assert_not_called()
        self.launch.assert_not_called()

    def test_mutation_before_spawn_fails_without_launch(self):
        real_create = capture_run.capture_manifest.create_manifest

        def create(*args, **kwargs):
            manifest = real_create(*args, **kwargs)
            self.executable.write_bytes(b"changed after inventory")
            return manifest

        self.patch(capture_run.capture_manifest, "create_manifest", side_effect=create)
        result = self.run_capture()
        manifest = result["manifest"]
        self.assertEqual(manifest["state"], "failed")
        self.assertEqual(manifest["execution"]["outcome"], "runtime_verification_failed_before_spawn")
        self.assertEqual(manifest["execution"]["inventory_verification"]["before"]["findings"][0]["reason"], "changed")
        self.launch.assert_not_called()

    def test_library_mutation_after_spawn_is_recorded(self):
        def wait():
            self.library.write_bytes(b"changed library")
            return 0

        self.process.wait.side_effect = wait
        result = self.run_capture()
        manifest = result["manifest"]
        self.assertEqual(manifest["exit_code"], 0)
        self.assertEqual(manifest["state"], "failed")
        self.assertEqual(manifest["execution"]["outcome"], "runtime_verification_failed_after_exit")
        after = manifest["execution"]["inventory_verification"]["after"]
        self.assertFalse(after["verified"])
        self.assertEqual(after["findings"][0]["path"], str(self.library))

    def test_spawn_failure_preserves_manifest_log_and_health(self):
        self.launch.side_effect = OSError("synthetic launch failure")
        result = self.run_capture()
        saved = capture_schema.read_json(self.run_dir / "manifest.json")
        self.assertEqual(saved["state"], "failed")
        self.assertIsNone(saved["started_at"])
        self.assertEqual(saved["execution"]["outcome"], "spawn_failed")
        self.assertEqual(saved["execution"]["error"]["message"], "synthetic launch failure")
        self.assertEqual(saved["artifacts"][0]["kind"], "log")
        self.assertTrue((self.run_dir / "health.json").exists())
        self.assertEqual(result["manifest"], saved)

    def test_interrupt_stops_child_and_preserves_partial_capture(self):
        def launch(*args, **kwargs):
            self.write_capture()
            kwargs["stdout"].write(b"partial evidence\n")
            return self.process

        self.launch.side_effect = launch
        self.process.wait.side_effect = [KeyboardInterrupt(), -2]
        self.process.poll.return_value = None
        result = self.run_capture()
        manifest = result["manifest"]
        self.assertEqual(manifest["state"], "interrupted")
        self.assertEqual(manifest["exit_code"], -2)
        self.assertEqual(manifest["execution"]["termination"]["requested_by"], "runner_interrupt")
        self.interrupt.assert_called_once_with(self.process)
        self.process.terminate.assert_not_called()
        self.process.kill.assert_not_called()
        self.assertEqual({item["kind"] for item in manifest["artifacts"]}, {"log", "summary", "trace", "issue_report"})
        self.assertEqual(manifest["execution"]["artifact_findings"], [])
        self.assertEqual((self.run_dir / "run.log").read_bytes(), b"partial evidence\n")

    def test_interrupt_escalation_is_bounded_and_recorded(self):
        timeout = subprocess.TimeoutExpired("synthetic", capture_run.INTERRUPT_WAIT_SECONDS)
        self.process.wait.side_effect = [KeyboardInterrupt(), timeout, timeout, -9]
        self.process.poll.return_value = None
        manifest = self.run_capture()["manifest"]
        self.assertEqual(manifest["state"], "interrupted")
        self.assertEqual(manifest["exit_code"], -9)
        termination = manifest["execution"]["termination"]
        self.assertTrue(termination["escalated"])
        self.assertEqual([item["action"] for item in termination["attempts"]],
                         ["interrupt", "wait", "terminate", "wait", "kill"])
        self.process.terminate.assert_called_once()
        self.process.kill.assert_called_once()
        for call in self.process.wait.call_args_list[1:]:
            self.assertEqual(call.kwargs, {"timeout": capture_run.INTERRUPT_WAIT_SECONDS})

    def test_capture_window_does_not_terminate_child(self):
        collectors = self.patch(capture_run.capture_collectors, "CollectorSession")
        self.launch.side_effect = lambda *args, **kwargs: (self.write_capture() or self.process)
        manifest = self.run_capture()["manifest"]
        self.assertEqual(manifest["state"], "completed")
        self.process.wait.assert_called_once_with()
        self.interrupt.assert_not_called()
        self.process.terminate.assert_not_called()
        self.process.kill.assert_not_called()
        collectors.assert_not_called()
        summaries = [item for item in manifest["artifacts"] if item["kind"] == "summary"]
        self.assertEqual(summaries[0]["trace"], f"aps5-trace-{'1' * 40}-12345.json")

    def test_collectors_bind_actual_child_and_poll_with_bounded_waits(self):
        events = []
        session, constructor = self.configure_collectors(events=events)

        def start():
            saved = capture_schema.read_json(self.run_dir / "manifest.json")
            self.assertEqual(saved["state"], "running")
            self.assertEqual(saved["execution"]["pid"], self.process.pid)
            events.append("collector_start")

        session.start.side_effect = start
        session.poll.side_effect = lambda: events.append("collector_poll")
        self.process.wait.side_effect = [subprocess.TimeoutExpired("synthetic", .25), 0]
        result = self.run_capture()
        self.assertIs(constructor.call_args.kwargs["process"], self.process)
        session.start.assert_called_once_with()
        self.assertEqual(session.poll.call_count, 2)
        session.finish.assert_called_once_with()
        self.assertEqual(events[0], "collector_start")
        self.assertEqual(events[-1], "collector_finish")
        for call in self.process.wait.call_args_list:
            self.assertEqual(call.kwargs, {"timeout": capture_run.COLLECTOR_POLL_SECONDS})
        self.assertEqual(result["manifest"]["execution"]["collector_receipt"]["state"], "stopped")
        self.assertFalse(result["manifest"]["execution"].get("collector_errors"))
        descriptor = next(item for item in result["manifest"]["artifacts"] if item["kind"] == "collector_receipt")
        self.assertEqual({key: descriptor[key] for key in ("size", "sha256")},
            capture_run.capture_manifest.file_identity(self.run_dir / descriptor["path"]))
        self.process.terminate.assert_not_called()
        self.process.kill.assert_not_called()

    def test_collector_start_failure_preserves_receipt_and_child_exit(self):
        errors = [{"phase": "start", "type": "OSError", "message": "access denied"}]
        session, unused = self.configure_collectors(errors=errors)
        session.start.side_effect = OSError("access denied")
        result = self.run_capture()
        self.assertEqual(result["manifest"]["exit_code"], 0)
        self.assertEqual(result["manifest"]["state"], "completed")
        session.poll.assert_not_called()
        session.finish.assert_called_once_with()
        self.process.wait.assert_called_once_with()
        self.assertEqual(result["manifest"]["execution"]["collector_errors"][0]["phase"], "start")
        self.assertEqual(result["manifest"]["execution"]["collector_receipt"]["state"], "failed")
        self.assertTrue((self.run_dir / "collectors/receipt.json").is_file())
        self.process.terminate.assert_not_called()

    def test_invalid_collector_configuration_is_rejected_before_output_creation(self):
        self.config["collectors"] = {"enabled": True, "kind": "wpr_cpu", "wpr": str(self.root / "missing-wpr.exe")}
        with self.assertRaises(ValueError):
            self.run_capture()
        self.assertFalse(self.run_dir.exists())
        self.launch.assert_not_called()

    def test_collector_poll_failure_finishes_before_process_observer(self):
        events = []
        session, unused = self.configure_collectors(errors=[{"phase": "finish", "type": "OSError", "message": "failed"}], events=events)
        session.poll.side_effect = OSError("session ownership lost")
        self.config["observation"] = {"enabled": True, "interval_ms": 50, "hash_roots": []}
        observer = mock.Mock()

        def wait(process, *, poll_callback):
            poll_callback()
            return 0

        observer.wait.side_effect = wait
        observer.finish.side_effect = lambda: events.append("observer_finish")
        self.patch(capture_run.capture_process, "ProcessObserver", return_value=observer)
        self.patch(capture_run.capture_process, "snapshot_artifacts")
        self.patch(capture_run.capture_process, "attach_observation_evidence")
        result = self.run_capture()
        session.finish.assert_called_once_with()
        self.assertEqual(events, ["collector_finish", "observer_finish"])
        self.assertEqual(result["manifest"]["execution"]["collector_errors"][0]["phase"], "poll")
        self.assertEqual(result["manifest"]["exit_code"], 0)

    def test_collector_declared_hash_is_rechecked_during_inventory(self):
        session, unused = self.configure_collectors()
        self.config["observation"] = {"enabled": True, "interval_ms": 50, "hash_roots": []}
        observer = mock.Mock()
        observer.wait.return_value = 0
        observer.finish.side_effect = lambda: (self.run_dir / "collectors/receipt.json").write_bytes(b"changed receipt")
        self.patch(capture_run.capture_process, "ProcessObserver", return_value=observer)
        self.patch(capture_run.capture_process, "snapshot_artifacts")
        self.patch(capture_run.capture_process, "attach_observation_evidence")
        result = self.run_capture()
        session.finish.assert_called_once_with()
        self.assertTrue(any(row["reason"] == "collector_artifact_changed"
            for row in result["manifest"]["execution"]["artifact_findings"]))

    def test_collector_cannot_relabel_unrelated_capture_artifacts(self):
        self.run_dir.mkdir()
        (self.run_dir / "traces").mkdir()
        (self.run_dir / "run.log").write_bytes(b"native process log")
        declaration = {"kind": "cpu_profile", "path": "run.log",
            **capture_run.capture_manifest.file_identity(self.run_dir / "run.log")}
        with self.assertRaises(ValueError):
            capture_run.inventory_artifacts(self.run_dir, declared_artifacts=[declaration])

    def test_measurement_workload_derives_run_uuid_without_draw_detail(self):
        self.config["environment"]["overrides"]["APS5_TRACE_WORKLOAD"] = "1"
        result = self.run_capture()
        environment = self.launch.call_args.kwargs["env"]
        self.assertEqual(environment["APS5_CAPTURE_RUN_ID"], result["manifest"]["run_id"])
        self.assertNotIn("APS5_TRACE_DETAIL", environment)

    def test_measurement_workload_artifact_is_selected_by_exact_run_and_pid(self):
        self.config["environment"]["overrides"]["APS5_TRACE_WORKLOAD"] = "1"

        def launch(*args, **kwargs):
            self.write_capture()
            manifest = capture_schema.read_json(self.run_dir / "manifest.json")
            path = self.run_dir / "traces" / f"aps5-workload-{manifest['run_id']}-{self.process.pid}.json.gz"
            path.write_bytes(b"synthetic compressed workload artifact")
            return self.process

        self.launch.side_effect = launch
        result = self.run_capture()
        workload = next(item for item in result["manifest"]["artifacts"] if item["kind"] == "measurement_workload")
        self.assertTrue(workload["trace"].startswith("aps5-trace-"))
        self.assertFalse(result["manifest"]["execution"]["artifact_findings"])

    def test_draw_report_reuses_one_fresh_health_assessment(self):
        import capture_draws

        self.config["environment"]["overrides"]["APS5_TRACE_DETAIL"] = "draw"
        self.config["settings"]["diagnostics"]["options"]["report_detail"] = "full"
        assessed = []

        def assess(run_dir, manifest=None, **kwargs):
            self.assertTrue(kwargs["include_draw_index"])
            report = self.health_report(run_dir, manifest)
            assessed.append(report)
            return report

        self.health.side_effect = assess
        report_from_health = self.patch(capture_draws, "report_from_health", wraps=capture_draws.report_from_health)
        independent_report = self.patch(capture_draws, "report_capture", side_effect=AssertionError("duplicate assessment"))
        result = self.run_capture()
        self.health.assert_called_once()
        report_from_health.assert_called_once()
        self.assertIs(report_from_health.call_args.args[1], assessed[0])
        independent_report.assert_not_called()
        self.assertTrue((self.run_dir / "draws.json").is_file())
        self.assertTrue((self.run_dir / "draws.txt").is_file())
        self.assertFalse(result["manifest"]["execution"].get("finalization_errors"))

    def test_draw_reports_default_to_compact_and_preserve_canonical_artifact(self):
        self.config["environment"]["overrides"]["APS5_TRACE_DETAIL"] = "draw"
        raw = b'{"raw":"synthetic canonical evidence"}'
        paths = []

        def launch(*args, **kwargs):
            self.write_capture()
            manifest = capture_schema.read_json(self.run_dir / "manifest.json")
            path = self.run_dir / "traces" / f"aps5-draw-{manifest['run_id']}-{self.process.pid}.json"
            path.write_bytes(raw)
            paths.append(path)
            return self.process

        self.launch.side_effect = launch
        result = self.run_capture()
        self.health.assert_called_once()
        self.assertIs(self.health.call_args.kwargs["include_draw_index"], False)
        report = capture_schema.read_json(self.run_dir / "draws.json")
        self.assertEqual(report["report_detail"], "compact")
        self.assertNotIn("index", report.get("detail") or {})
        self.assertEqual(paths[0].read_bytes(), raw)
        descriptor = next(row for row in result["manifest"]["artifacts"] if row["kind"] == "draw_trace")
        self.assertEqual(descriptor["sha256"], capture_run.capture_manifest.file_identity(paths[0])["sha256"])
        self.assertFalse(result["manifest"]["execution"].get("finalization_errors"))

    def test_invalid_report_detail_is_rejected_before_output_creation(self):
        for value in (None, [], {}, 1, "everything"):
            with self.subTest(value=value):
                self.config["settings"]["diagnostics"]["options"]["report_detail"] = value
                with self.assertRaises(ValueError):
                    self.run_capture()
                self.assertFalse(self.run_dir.exists())
        self.launch.assert_not_called()

    def test_mixed_summaries_are_not_silently_selected(self):
        self.launch.side_effect = lambda *args, **kwargs: (self.write_capture(extra_summary=True) or self.process)
        manifest = self.run_capture()["manifest"]
        self.assertEqual(manifest["execution"]["artifact_findings"][0]["reason"], "mixed_or_mismatched_summary")
        summaries = [item for item in manifest["artifacts"] if item["kind"] == "summary"]
        self.assertNotIn("trace", summaries[0])

    def test_evidence_reports_reuse_assessment_and_health_stays_compact(self):
        self.config["environment"]["overrides"]["APS5_TRACE_DETAIL"] = "draw"
        self.config["settings"]["diagnostics"]["options"]["report_detail"] = "full"

        def assess(run_dir, manifest=None, **kwargs):
            health = self.health_report(run_dir, manifest)
            health["evidence"] = {"schema_version": 1, "document_type": "anyps5.evidence-report",
                "run_id": manifest["run_id"], "capture_id": None, "status": "unknown", "issues": [],
                "dimensions": {"clock_mapping": {"status": "unknown", "reasons": ["no raw clock anchors"]}},
                "metrics": {"cpu.elapsed_ms": 1.0}, "metric_availability": {"cpu.elapsed_ms": "valid"},
                "metric_units": {"cpu.elapsed_ms": "ms"}, "frames": [], "operations": [],
                "dependency_graph": {"nodes": [], "edges": []}, "clock_mapping": {"queries": {}},
                "hardware_counters": [], "limitations": ["Selected scopes only."]}
            return health

        self.health.side_effect = assess
        result = self.run_capture()
        self.health.assert_called_once()
        self.assertFalse(result["manifest"]["execution"].get("finalization_errors"))
        saved = capture_schema.read_json(self.run_dir / "evidence.json")
        compact = capture_schema.read_json(self.run_dir / "health.json")["evidence"]
        self.assertIn("frames", saved)
        self.assertNotIn("frames", compact)
        self.assertEqual(compact["metrics"], saved["metrics"])
        self.assertIn("no raw clock anchors", (self.run_dir / "evidence.html").read_text(encoding="utf-8"))
        expected = {"evidence_report": "evidence.json", "evidence_html": "evidence.html", "issue_report": "issues.json"}
        for kind, name in expected.items():
            descriptor = next(item for item in result["manifest"]["artifacts"] if item["kind"] == kind)
            self.assertEqual(descriptor["path"], name)
            self.assertEqual({key: descriptor[key] for key in ("size", "sha256")},
                             capture_run.capture_manifest.file_identity(self.run_dir / name))

    def test_recovery_journal_is_not_misclassified_as_frame_summary(self):
        self.run_dir.mkdir()
        (self.run_dir / "traces").mkdir()
        (self.run_dir / "run.log").write_bytes(b"")
        journal = self.run_dir / "traces" / "aps5-draw-fixture-12345.journal.jsonl"
        journal.write_text('{}\n', encoding="utf-8")
        artifacts, findings = capture_run.inventory_artifacts(self.run_dir)
        self.assertEqual(findings, [])
        self.assertEqual(next(item["kind"] for item in artifacts if item["path"].endswith(".jsonl")), "draw_journal")
        self.assertFalse(any(item["kind"] == "summary" for item in artifacts))

    def test_trace_from_other_pid_is_not_accepted(self):
        self.launch.side_effect = lambda *args, **kwargs: (self.write_capture(pid=777) or self.process)
        manifest = self.run_capture()["manifest"]
        self.assertEqual(manifest["execution"]["artifact_findings"][0]["reason"], "trace_child_pid_mismatch")

    def test_finalization_failure_does_not_replace_original_spawn_error(self):
        self.launch.side_effect = OSError("original failure")
        self.health.side_effect = ValueError("synthetic health failure")
        result = self.run_capture()
        saved = capture_schema.read_json(self.run_dir / "manifest.json")
        self.assertEqual(saved["execution"]["outcome"], "spawn_failed")
        self.assertEqual(saved["execution"]["error"]["message"], "original failure")
        self.assertEqual(saved["execution"]["finalization_errors"][0]["phase"], "health_report")
        self.assertIsNone(result["health"])

    def test_report_manifest_failure_cannot_leave_successful_saved_health(self):
        save = capture_run.save_manifest

        def save_with_failure(run_dir, manifest, **kwargs):
            if manifest["state"] == "completed" and any(item["kind"] == "issue_report" for item in manifest["artifacts"]):
                raise OSError("synthetic final report manifest failure")
            return save(run_dir, manifest, **kwargs)

        self.patch(capture_run, "save_manifest", side_effect=save_with_failure)
        result = self.run_capture()
        saved = capture_schema.read_json(self.run_dir / "manifest.json")
        self.assertEqual(saved["state"], "failed")
        self.assertEqual(saved["execution"]["outcome"], "finalization_failed")
        self.assertIsNone(result["health"])
        self.assertFalse((self.run_dir / "health.json").exists())

    def test_symlink_artifact_escape_is_not_hashed(self):
        self.run_dir.mkdir()
        traces = self.run_dir / "traces"
        traces.mkdir()
        (self.run_dir / "run.log").write_bytes(b"")
        try:
            (traces / "escape.json").symlink_to(self.library)
        except (OSError, NotImplementedError):
            self.skipTest("symbolic links are unavailable")
        artifacts, findings = capture_run.inventory_artifacts(self.run_dir)
        self.assertEqual([item["path"] for item in artifacts], ["run.log"])
        self.assertEqual(findings[0]["path"], "traces/escape.json")

    def test_cli_returns_failure_when_child_exits_without_usable_capture(self):
        result = self.run_capture()
        self.patch(capture_run, "run_capture", return_value=result)
        self.patch(capture_run.capture_schema, "read_json", return_value=self.config)
        output = io.StringIO()
        with mock.patch.object(sys, "stdout", output):
            code = capture_run.main(["--config", str(self.root / "config.json"), "--run-dir", str(self.run_dir)])
        self.assertEqual(code, 1)
        self.assertIn("frame metrics: unknown", output.getvalue())

    def test_cli_surfaces_collector_errors_even_with_usable_frame_metrics(self):
        result = {"run_dir": str(self.run_dir), "manifest": {"state": "completed", "execution": {
            "collector_errors": [{"phase": "finish", "type": "OSError", "message": "owned stop failed"}]}},
            "health": {"status": "unknown", "frame_metrics": {"status": "valid"}}}
        self.patch(capture_run, "run_capture", return_value=result)
        self.patch(capture_run.capture_schema, "read_json", return_value=self.config)
        with mock.patch.object(sys, "stdout", io.StringIO()), mock.patch.object(sys, "stderr", io.StringIO()) as errors:
            code = capture_run.main(["--config", str(self.root / "config.json"), "--run-dir", str(self.run_dir)])
        self.assertEqual(code, 1)
        self.assertIn("collector finish: OSError: owned stop failed", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
