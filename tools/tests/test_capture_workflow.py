import copy
import gzip
import json
import os
import shutil
import sys
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capture_compare
import capture_health
import capture_manifest
import capture_run
import capture_schema


class SyntheticProcess:
    def __init__(self, pid, on_wait=None, interrupted=False):
        self.pid = pid
        self.returncode = None
        self.on_wait = on_wait
        self.interrupted = interrupted
        self.signals = []

    def wait(self, timeout=None):
        if self.interrupted:
            self.interrupted = False
            raise KeyboardInterrupt
        if self.on_wait is not None:
            operation, self.on_wait = self.on_wait, None
            operation()
        if self.returncode is None:
            self.returncode = 0
        return self.returncode

    def poll(self):
        return self.returncode

    def send_signal(self, value):
        self.signals.append(value)
        self.returncode = 130

    def terminate(self):
        self.returncode = -15

    def kill(self):
        self.returncode = -9


class CaptureWorkflowTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source_root = self.root / "source"
        self.source_root.mkdir()
        self.executable = self.root / "synthetic.exe"
        self.library = self.root / "synthetic.prx"
        self.executable.write_bytes(b"synthetic executable, never executed")
        self.library.write_bytes(b"synthetic declared library, never loaded")
        self.sequence = 0
        self.source = {
            "root": str(self.source_root), "commit": "a" * 40, "dirty": False,
            "diff_sha256": "0" * 64, "untracked": [], "submodules": [],
        }
        self.config = {
            "source_root": str(self.source_root),
            "runtime": {"executable": str(self.executable), "libraries": [str(self.library)]},
            "build": {"compiler": "synthetic compiler", "build_type": "Release", "options": {}},
            "machine": {"id": "synthetic-pc", "cpu": "CPU", "gpu": "GPU", "driver": "Driver", "os": "OS"},
            "title": {"id": "synthetic-title", "version": "1"},
            "scene": {"id": "idle", "recipe": {"steps": ["wait"]}},
            "settings": {
                "resolution": "1920x1080", "presentation": "windowed",
                "cache": {"shader": "warm", "pipeline": "warm", "driver": "warm"},
                "diagnostics": {"mode": "measurement", "options": {}},
                "relinker": {"flags": ["--windows", "--to-intel"]},
            },
            "capture": {"unit": "frames", "start": 100, "length": 4},
            "execution": {"command": [str(self.executable)], "cwd": str(self.root)},
            "environment": {"overrides": {"APS5_STORE_AT_FLIP": "1"}},
        }

    def emit_capture(self, trace_dir, manifest, pid, frame_ms):
        revision = manifest["source"]["commit"][:12]
        filename = f"aps5-trace-{revision}-{pid}.json.gz"
        start = manifest["capture"]["start"]
        count = manifest["capture"]["length"]
        events = [{"name": "process_name", "ph": "M", "pid": pid, "tid": 0,
                   "ts": 0, "args": {"name": "AnyPS5"}},
                  {"name": "thread_name", "ph": "M", "pid": pid, "tid": 1,
                   "ts": 0, "args": {"name": "presenter"}}]
        events.extend({"name": "present", "ph": "i", "pid": pid, "tid": 1,
                       "ts": index * frame_ms * 1000, "s": "g", "args": {"present": start + index}}
                      for index in range(count))
        trace = {"traceEvents": events}
        (trace_dir / filename).write_bytes(gzip.compress(capture_schema.canonical_json_bytes(trace), mtime=0))
        environment = {key: value for key, value in manifest["environment"]["effective"].items()
                       if key.startswith("APS5_")}
        environment["APS5_TRACE_DIR"] = "<path>"
        summary = {
            "commit": revision, "dirty": manifest["source"]["dirty"],
            "env": environment, "gpu": "GPU", "driver": "Driver", "os": "OS",
            "resolution": manifest["settings"]["resolution"],
            "window": {"spec": f"frames {start}:{count}", "first_present": start, "ms": count * frame_ms},
            "presents": count, "frame_ms": {key: frame_ms for key in ("p50", "p90", "p99", "max")},
            "gpu_busy_ms_per_present": 0, "worker_busy_pct": {},
            "top_phases_ms_per_frame": {}, "trace_dropped": False, "trace": filename,
        }
        (trace_dir / "aps5-summary.jsonl").write_text(json.dumps(summary) + "\n", encoding="utf-8")

    def capture(self, name, *, config=None, source=None, frame_ms=20, emit=True,
                inherited=None, on_wait=None, interrupted=False):
        self.sequence += 1
        index = self.sequence
        run_dir = self.root / name
        config = copy.deepcopy(self.config if config is None else config)
        source = copy.deepcopy(self.source if source is None else source)
        real_create = capture_manifest.create_manifest
        created = f"2026-01-01T00:{index:02d}:00Z"
        timestamps = iter([f"2026-01-01T00:{index:02d}:01Z", f"2026-01-01T00:{index:02d}:02Z"])
        launches = []
        processes = []

        def create(value, **kwargs):
            return real_create(value, source=source, now=created, **kwargs)

        def launch(command, **options):
            launches.append((command, options))
            planned = capture_schema.read_json(run_dir / "manifest.json")
            self.assertEqual(planned["state"], "planned")
            self.assertIsNone(planned["started_at"])
            self.assertFalse(options["shell"])
            if emit:
                self.emit_capture(Path(options["env"]["APS5_TRACE_DIR"]), planned, 1000 + index, frame_ms)
            options["stdout"].write(b"synthetic child output\n")
            process = SyntheticProcess(1000 + index, on_wait, interrupted)
            processes.append(process)
            return process

        with mock.patch.object(capture_run.capture_manifest, "create_manifest", side_effect=create), \
                mock.patch.object(capture_run, "utc_now", side_effect=lambda: next(timestamps)), \
                mock.patch.object(capture_run.os, "killpg", create=True,
                                  side_effect=lambda pid, value: processes[-1].send_signal(value)):
            result = capture_run.run_capture(config, run_dir, inherited_environment=inherited, popen=launch)
        return run_dir, result, launches

    def rewrite_manifest(self, run_dir, operation):
        manifest = capture_schema.read_json(run_dir / "manifest.json")
        operation(manifest)
        capture_schema.atomic_write_json(run_dir / "manifest.json", manifest)
        return manifest

    def test_runner_creates_exclusive_evidence_and_isolates_child_environment(self):
        inherited = {"Path": "synthetic path", "TOKEN": "private", "aps5_old": "1", "Vk_Layer_Path": "old"}
        original = copy.deepcopy(inherited)
        parent = dict(os.environ)
        run_dir, result, launches = self.capture("exclusive", inherited=inherited)
        self.assertEqual(inherited, original)
        self.assertEqual(dict(os.environ), parent)
        child = launches[0][1]["env"]
        self.assertEqual(child["TOKEN"], "private")
        self.assertNotIn("aps5_old", child)
        self.assertNotIn("Vk_Layer_Path", child)
        self.assertEqual(child["APS5_TRACE_FRAMES"], "100:4")
        self.assertEqual(child["APS5_TRACE_DIR"], str(run_dir / "traces"))
        self.assertNotIn("APS5_PROFILE_GPU", child)
        self.assertNotIn("TOKEN", result["manifest"]["environment"]["effective"])
        before = {path.relative_to(run_dir): path.read_bytes() for path in run_dir.rglob("*") if path.is_file()}
        with mock.patch.object(capture_manifest, "source_fingerprint", return_value=self.source), \
                mock.patch.object(capture_run.subprocess, "Popen") as spawn:
            with self.assertRaises(FileExistsError):
                capture_run.run_capture(self.config, run_dir, inherited_environment={})
            spawn.assert_not_called()
        after = {path.relative_to(run_dir): path.read_bytes() for path in run_dir.rglob("*") if path.is_file()}
        self.assertEqual(before, after)

    def test_hashes_and_checkout_never_become_build_or_loaded_module_proof(self):
        run_dir, result, _ = self.capture("provenance")
        manifest = result["manifest"]
        self.assertEqual(manifest["build"]["source_binary_binding"], "unverified")
        self.assertEqual(manifest["runtime"]["inventory_kind"], "declared_files")
        self.assertFalse(manifest["runtime"]["loaded_modules_verified"])
        self.assertTrue(manifest["execution"]["inventory_verification"]["before"]["verified"])
        self.assertTrue(manifest["execution"]["inventory_verification"]["after"]["verified"])
        health = capture_health.assess_capture(run_dir)
        self.assertEqual(health["frame_metrics"]["status"], "valid")
        self.assertEqual(health["provenance"]["source_binary_binding"], "unverified")
        self.assertEqual(health["provenance"]["status"], "unknown")
        self.assertEqual(health["detailed_attribution"]["status"], "unknown")
        limits = json.dumps(health["detailed_attribution"])
        self.assertIn("20", limits)
        self.assertIn("GPU", limits)
        self.assertFalse(any(path.suffix in (".exe", ".prx") for path in run_dir.rglob("*")))

    def test_partial_capture_and_interruption_preserve_log_but_cannot_compare(self):
        baseline, _, _ = self.capture("baseline")
        for name, options in (("partial", {"emit": False}), ("interrupted", {"interrupted": True})):
            with self.subTest(name=name):
                run_dir, result, _ = self.capture(name, **options)
                self.assertTrue((run_dir / "manifest.json").is_file())
                self.assertEqual((run_dir / "run.log").read_bytes(), b"synthetic child output\n")
                self.assertEqual(capture_compare.compare_runs([baseline], [run_dir])["status"], "invalid")
                if name == "interrupted":
                    self.assertEqual(result["manifest"]["state"], "interrupted")
                    self.assertEqual(result["manifest"]["execution"]["termination"]["requested_by"], "runner_interrupt")

    def test_runtime_mutation_during_child_wait_invalidates_recorded_provenance(self):
        run_dir, result, _ = self.capture("changed-runtime", on_wait=lambda: self.library.write_bytes(b"changed"))
        self.assertEqual(result["manifest"]["state"], "failed")
        after = result["manifest"]["execution"]["inventory_verification"]["after"]
        self.assertFalse(after["verified"])
        self.assertEqual(after["findings"][0]["reason"], "changed")
        self.assertEqual(capture_health.assess_capture(run_dir)["status"], "invalid")

    def test_recorded_boundary_checks_keep_old_runs_usable_after_runtime_replacement(self):
        before = [self.capture(f"before-{index}")[0] for index in range(2)]
        after = [self.capture(f"after-{index}", frame_ms=18)[0] for index in range(2)]
        self.executable.write_bytes(b"a later build at the same path")
        self.library.unlink()
        for run_dir in before + after:
            health = capture_health.assess_capture(run_dir)
            self.assertEqual(health["frame_metrics"]["status"], "valid")
            self.assertTrue(health["provenance"]["runtime_boundaries_verified"])
            self.assertEqual(health["provenance"]["source_binary_binding"], "unverified")
        self.assertEqual(capture_compare.compare_runs(before, after)["status"], "comparable")

    def test_malformed_duplicate_and_tampered_evidence_is_reassessed(self):
        baseline, _, _ = self.capture("baseline")
        for index, payload in enumerate((b"{bad JSON\n", b'{"trace": "first", "trace": "second"}\n')):
            with self.subTest(payload=payload):
                run_dir, _, _ = self.capture(f"bad-summary-{index}")
                summary = run_dir / "traces" / "aps5-summary.jsonl"
                summary.write_bytes(payload)

                def refresh(manifest):
                    item = next(item for item in manifest["artifacts"] if item["kind"] == "summary")
                    item.update(size=summary.stat().st_size, sha256=capture_schema.sha256_file(summary))

                self.rewrite_manifest(run_dir, refresh)
                self.assertEqual(capture_health.assess_capture(run_dir)["status"], "invalid")
        run_dir, _, _ = self.capture("tampered")
        trace = next((run_dir / "traces").glob("*.json.gz"))
        trace.write_bytes(b"changed after inventory")
        self.assertEqual(capture_compare.compare_runs([baseline], [run_dir])["status"], "invalid")

    def test_scene_cache_and_effective_input_environment_must_match(self):
        baseline, _, _ = self.capture("baseline")
        configurations = []
        scene = copy.deepcopy(self.config)
        scene["scene"]["recipe"]["steps"] = ["different action"]
        configurations.append(scene)
        cache = copy.deepcopy(self.config)
        cache["settings"]["cache"]["shader"] = "cold"
        configurations.append(cache)
        diagnostics = copy.deepcopy(self.config)
        diagnostics["settings"]["diagnostics"]["mode"] = "forensic"
        configurations.append(diagnostics)
        environment = copy.deepcopy(self.config)
        environment["environment"]["overrides"]["VK_DRIVER_FILES"] = str(self.root / "other-driver.json")
        configurations.append(environment)
        for index, config in enumerate(configurations):
            with self.subTest(index=index):
                candidate, _, _ = self.capture(f"different-{index}", config=config)
                self.assertEqual(capture_compare.compare_runs([baseline], [candidate])["status"], "incomparable")

    def test_observed_machine_changes_cannot_hide_behind_matching_declared_configuration(self):
        baseline, _, _ = self.capture("baseline")
        candidate, _, _ = self.capture("candidate")
        summary_path = candidate / "traces" / "aps5-summary.jsonl"
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary["gpu"] = "Different observed GPU"
        summary_path.write_text(json.dumps(summary) + "\n", encoding="utf-8")

        def refresh(manifest):
            item = next(item for item in manifest["artifacts"] if item["kind"] == "summary")
            item.update(size=summary_path.stat().st_size, sha256=capture_schema.sha256_file(summary_path))

        self.rewrite_manifest(candidate, refresh)
        health = capture_health.assess_capture(candidate)
        self.assertEqual(health["observed_context"]["gpu"], "Different observed GPU")
        self.assertEqual(capture_compare.compare_runs([baseline], [candidate])["status"], "incomparable")

    def test_repeated_comparison_reports_run_distributions_without_win_verdict(self):
        before = [self.capture(f"before-{index}", frame_ms=value)[0] for index, value in enumerate((20, 22))]
        after_source = copy.deepcopy(self.source)
        after_source["commit"] = "b" * 40
        after = [self.capture(f"after-{index}", frame_ms=value, source=after_source)[0]
                 for index, value in enumerate((18, 19))]
        single = capture_compare.compare_runs(before[:1], after[:1])
        self.assertEqual(single["status"], "insufficient")
        report = capture_compare.compare_runs(before, after)
        self.assertEqual(report["status"], "comparable")
        metric = report["metrics"]["frame_ms.p50"]
        self.assertEqual(metric["baseline"]["count"], 2)
        self.assertEqual(metric["baseline"]["values"], [20, 22])
        self.assertEqual(metric["candidate"]["median"], 18.5)
        self.assertEqual(metric["baseline"]["range"], 2)
        self.assertLess(metric["change"], 0)
        self.assertNotIn("winner", report)
        self.assertTrue(report["limitations"])

    def test_explicit_pairs_use_identity_and_reject_unmatched_repetitions(self):
        before, after = [], []
        for pair in ("first", "second"):
            config = copy.deepcopy(self.config)
            config["experiment"] = {"id": "synthetic-experiment", "pair_id": pair}
            before.append(self.capture(f"before-{pair}", config=config)[0])
        for pair in ("second", "first"):
            config = copy.deepcopy(self.config)
            config["experiment"] = {"id": "synthetic-experiment", "pair_id": pair}
            after.append(self.capture(f"after-{pair}", config=config, frame_ms=18)[0])
        report = capture_compare.compare_runs(before, after)
        self.assertEqual(report["status"], "comparable")
        self.assertEqual(report["pairing"], "explicit_pairs")
        self.assertEqual({pair["pair_id"] for pair in report["pairs"]}, {"first", "second"})
        self.rewrite_manifest(after[0], lambda manifest: manifest["experiment"].update(pair_id="unmatched"))
        self.assertEqual(capture_compare.compare_runs(before, after)["status"], "incomparable")

    def test_cohort_source_identity_and_reused_capture_are_rejected(self):
        baseline, _, _ = self.capture("baseline")
        candidate, _, _ = self.capture("candidate")
        changed_source = copy.deepcopy(self.source)
        changed_source["dirty"] = True
        changed_source["diff_sha256"] = "f" * 64
        mixed, _, _ = self.capture("mixed", source=changed_source)
        self.assertEqual(capture_compare.compare_runs([baseline, mixed], [candidate])["status"], "incomparable")
        self.assertEqual(capture_compare.compare_runs([baseline], [baseline])["status"], "incomparable")
        cloned = self.root / "reused"
        shutil.copytree(baseline, cloned)

        def new_identity(manifest):
            manifest.update(run_id=str(uuid.uuid4()), created_at="2026-01-01T02:00:00Z",
                            started_at="2026-01-01T02:00:01Z", ended_at="2026-01-01T02:00:02Z")

        self.rewrite_manifest(cloned, new_identity)
        self.assertEqual(capture_compare.compare_runs([baseline, cloned], [candidate])["status"], "incomparable")


if __name__ == "__main__":
    unittest.main()
