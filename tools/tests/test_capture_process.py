import copy
import datetime
import hashlib
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import capture_manifest
import capture_process
from capture_schema import SchemaError, atomic_write_json, read_json


class ProcessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.executable = self.root / "synthetic.exe"
        self.library = self.root / "synthetic.prx"
        self.executable.write_bytes(b"synthetic executable only")
        self.library.write_bytes(b"synthetic library only")
        self.run_dir = self.root / "run"
        self.run_dir.mkdir()
        self.policy = capture_process.validate_observation_config({"enabled": True, "interval_ms": 50, "hash_roots": [str(self.root)]})
        config = {"source_root": str(self.root), "runtime": {"executable": str(self.executable), "libraries": [str(self.library)]},
                  "build": {"compiler": "synthetic", "build_type": "Release", "options": {}},
                  "machine": {"id": "fixture", "cpu": "cpu", "gpu": "gpu", "driver": "driver", "os": "os"},
                  "title": {"id": "synthetic", "version": "1"}, "scene": {"id": "synthetic", "recipe": {}},
                  "settings": {"resolution": "640x480", "presentation": "windowed", "cache": {"shader": "unknown", "pipeline": "unknown", "driver": "unknown"},
                               "diagnostics": {"mode": "forensic", "options": {}}, "relinker": {"flags": []}},
                  "capture": {"unit": "frames", "start": 1, "length": 2}, "environment": {"overrides": {}},
                  "execution": {"command": [str(self.executable)], "cwd": str(self.root)}, "observation": self.policy}
        source = {"root": str(self.root), "commit": "1" * 40, "dirty": False, "diff_sha256": "2" * 64, "untracked": [], "submodules": []}
        self.manifest = capture_manifest.create_manifest(config, source=source, inherited_environment={}, now="2026-10-16T00:00:00Z")
        self.manifest.update(state="running", started_at="2026-10-16T00:00:00.2Z")
        self.manifest["execution"]["pid"] = 123
        epoch = datetime.datetime(1601, 1, 1, tzinfo=datetime.timezone.utc)
        creation = datetime.datetime(2026, 10, 16, tzinfo=datetime.timezone.utc) - epoch
        self.creation = str((creation.days * 86400 + creation.seconds) * 10000000 + 1000000)

    def sample(self, ticks, *, library=True, creation=None, working=10):
        modules = [{"path": str(self.executable), "base_address": "4096", "image_size": 256}]
        if library:
            modules.append({"path": str(self.library), "base_address": "8192", "image_size": 256})
        return {"timestamp_ticks": str(ticks), "end_ticks": str(ticks + 1),
                "identity": {"pid": 123, "creation_filetime": self.creation if creation is None else creation, "path": str(self.executable), "kernel_cpu_100ns": ticks, "user_cpu_100ns": ticks},
                "modules": modules, "native_thread_ids": ["10", "11"],
                "anchor": {"ticks": str(ticks), "observer_before_ns": ticks * 1000, "observer_after_ns": ticks * 1000 + 100},
                "resources": {"working_set_bytes": working, "system_available_physical_bytes": 1000 - working, "kernel_cpu_100ns": ticks}}

    def observe(self, samples):
        backend = mock.Mock()
        backend.frequency = 1000
        backend.bound_to_child_handle = True
        backend.sample.side_effect = copy.deepcopy(samples)
        observer = capture_process.ProcessObserver(self.run_dir, self.manifest, self.policy, backend_factory=lambda pid: backend, expected_creation_filetime=self.creation)
        for unused in samples:
            observer.sample()
        result = observer.finish()
        self.manifest.update(state="completed", ended_at="2026-10-16T00:00:02Z", exit_code=0)
        self.manifest["artifacts"] = [capture_process.descriptor(kind, relative, self.run_dir) for kind, relative in
                                      (("process_observation", "observations/process.json"), ("process_samples", "observations/samples.jsonl"))]
        return result, backend

    def assess(self):
        usable = {item["path"]: self.run_dir / item["path"] for item in self.manifest["artifacts"]}
        return capture_process.assess_process_observation(self.run_dir, self.manifest, usable)

    def refresh(self, kind):
        item = next(row for row in self.manifest["artifacts"] if row["kind"] == kind)
        item.update(capture_process.identity_file(self.run_dir / item["path"]))

    @unittest.skipUnless(os.name == "nt", "Windows native process observation only")
    def test_real_retained_child_handle_observes_and_reassesses_synthetic_python(self):
        executable = str(Path(sys.executable).resolve())
        child_source = "import sys; print('READY', flush=True); token=sys.stdin.buffer.read(1); print('STOPPED' if token == b'S' else 'BAD_STOP', flush=True)"
        command = [executable, "-I", "-u", "-c", child_source]
        self.manifest["created_at"] = capture_process.utc_now()
        process = subprocess.Popen(command, cwd=self.root, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def cleanup():
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream is not None:
                    stream.close()
        self.addCleanup(cleanup)
        ready = queue.Queue(maxsize=1)
        reader = threading.Thread(target=lambda: ready.put(process.stdout.readline()), daemon=True)
        reader.start()
        self.assertEqual(ready.get(timeout=5), b"READY\r\n" if os.name == "nt" else b"READY\n")
        reader.join(timeout=1)
        self.assertFalse(reader.is_alive())
        self.manifest["started_at"] = capture_process.utc_now()
        self.manifest["execution"].update(pid=process.pid, command=command)
        self.manifest["runtime"]["files"][0] = {"role": "executable", "path": executable, **capture_process.identity_file(executable)}
        self.policy["hash_roots"] = [str(Path(executable).parent)]
        observer = capture_process.ProcessObserver(self.run_dir, self.manifest, self.policy, process=process)
        try:
            self.assertIsNotNone(observer.backend, observer.report["errors"])
            self.assertTrue(observer.backend.bound_to_child_handle)
            creation = observer.backend.identity()["creation_filetime"]
            observer.sample()
            observer.sample()
        finally:
            report = observer.finish()
        output, errors = process.communicate(input=b"S", timeout=5)
        self.assertEqual(process.returncode, 0, errors)
        self.assertEqual(output.strip(), b"STOPPED")
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["coverage"]["sample_count"], 2)
        sample_bytes = (self.run_dir / "observations/samples.jsonl").read_bytes()
        self.assertEqual(len(sample_bytes), report["coverage"]["sample_bytes"])
        self.assertEqual(len(sample_bytes), report["coverage"]["complete_sample_bytes"])
        self.assertFalse(report["coverage"]["sample_output_failed"])
        self.assertEqual(sample_bytes.count(b"\n"), 2)
        self.assertNotIn(b"\r\n", sample_bytes)
        self.manifest.update(state="completed", ended_at=capture_process.utc_now(), exit_code=0)
        self.manifest["artifacts"] = [capture_process.descriptor(kind, relative, self.run_dir) for kind, relative in
                                      (("process_observation", "observations/process.json"), ("process_samples", "observations/samples.jsonl"))]
        assessed = self.assess()
        self.assertEqual(assessed["status"], "valid", assessed["issues"])
        self.assertEqual(assessed["process"]["pid"], process.pid)
        self.assertEqual(assessed["process"]["creation_filetime"], creation)
        self.assertEqual(assessed["process"]["identity_binding"], "retained_child_handle")
        self.assertEqual(assessed["process"]["executable"]["sha256"], self.manifest["runtime"]["files"][0]["sha256"])
        self.assertTrue(assessed["threads"])
        self.assertTrue(all(row["native_thread_id_source"] == "windows_thread_id" for row in assessed["threads"]))
        image = next(row for row in assessed["modules"] if os.path.normcase(row["path"]) == os.path.normcase(executable))
        self.assertGreater(int(image["base_address"]), 0)
        self.assertGreater(image["image_size"], 0)
        self.assertEqual(image["file_identity"], capture_process.identity_file(executable))
        for name in ("working_set_bytes", "private_bytes", "system_total_physical_bytes", "system_available_physical_bytes"):
            self.assertGreater(assessed["resources"][name]["minimum"], 0)
        self.assertTrue(assessed["coverage"]["raw_samples_verified"])
        self.assertFalse(assessed["coverage"]["full_lifetime"])
        self.assertFalse(self.manifest["runtime"]["loaded_modules_verified"])

    def test_real_sample_file_respects_exact_fit_and_one_byte_short_caps(self):
        def observer_at(name, limit=None):
            run_dir = self.run_dir / name
            run_dir.mkdir()
            policy = copy.deepcopy(self.policy)
            policy["max_samples"] = 8
            if limit is not None:
                policy["max_output_bytes"] = limit
            backend = mock.Mock(frequency=1000, bound_to_child_handle=True)
            backend.sample.side_effect = copy.deepcopy([self.sample(10), self.sample(20)])
            observer = capture_process.ProcessObserver(run_dir, self.manifest, policy,
                backend_factory=lambda pid: backend, expected_creation_filetime=self.creation)
            self.addCleanup(observer.stream.close)
            return observer, backend, run_dir / "observations/samples.jsonl"

        reference, unused, reference_path = observer_at("reference")
        reference.sample()
        reference_report = reference.finish()
        first = reference_path.read_bytes()
        self.assertEqual(reference_report["coverage"]["sample_count"], 1)
        self.assertEqual(reference_report["errors"], [])
        self.assertGreater(len(first), 0)
        exact, exact_backend, exact_path = observer_at("exact", len(first))
        exact.sample()
        self.assertEqual(exact_path.read_bytes(), first)
        exact.sample()
        child = mock.Mock()
        child.wait.return_value = 19
        self.assertEqual(exact.wait(child), 19)
        child.wait.assert_called_once_with()
        exact_report = exact.finish()
        self.assertEqual(exact_path.read_bytes(), first)
        self.assertEqual(exact_report["coverage"]["sample_count"], 1)
        self.assertEqual(exact_report["coverage"]["sample_bytes"], len(first))
        self.assertFalse(exact_report["coverage"]["sample_output_failed"])
        self.assertIn("capacity exceeded", exact_report["errors"][0]["message"])
        exact_backend.close.assert_called_once_with()
        short, short_backend, short_path = observer_at("short", len(first) - 1)
        short.sample()
        short.sample()
        short_report = short.finish()
        self.assertEqual(short_path.read_bytes(), b"")
        self.assertEqual(short_report["coverage"]["sample_count"], 0)
        self.assertEqual(short_report["coverage"]["sample_bytes"], 0)
        self.assertFalse(short_report["coverage"]["sample_output_failed"])
        self.assertIn("capacity exceeded", short_report["errors"][0]["message"])
        short_backend.sample.assert_called_once_with()
        short_backend.close.assert_called_once_with()

    def test_output_failure_preserves_partial_bytes_and_stops_future_writes(self):
        class FailingOutput:
            def __init__(self, stream, mode):
                self.stream = stream
                self.mode = mode
                self.write_calls = 0
                self.close_calls = 0

            def write(self, data):
                self.write_calls += 1
                if self.mode == "short_write":
                    return self.stream.write(data[:len(data) // 2])
                if self.mode == "raise_after_partial_write":
                    self.stream.write(data[:len(data) // 2])
                    raise OSError("injected failure after a real partial write")
                return self.stream.write(data)

            def flush(self):
                if self.mode == "flush_failure":
                    raise OSError("injected flush failure after a real complete write")
                self.stream.flush()

            def close(self):
                self.close_calls += 1
                self.stream.close()

        for mode in ("short_write", "raise_after_partial_write", "flush_failure"):
            with self.subTest(mode=mode):
                run_dir = self.run_dir / mode
                run_dir.mkdir()
                policy = copy.deepcopy(self.policy)
                policy["max_samples"] = 8
                backend = mock.Mock(frequency=1000, bound_to_child_handle=True)
                backend.sample.return_value = self.sample(10)
                observer = capture_process.ProcessObserver(run_dir, self.manifest, policy,
                    backend_factory=lambda pid: backend, expected_creation_filetime=self.creation)
                actual = observer.stream
                self.addCleanup(actual.close)
                failing = FailingOutput(actual, mode)
                observer.stream = failing
                observer.sample()
                sample_path = run_dir / "observations/samples.jsonl"
                preserved = sample_path.read_bytes()
                self.assertGreater(len(preserved), 0)
                observer.sample()
                observer.sample()
                child = mock.Mock()
                child.wait.return_value = 23
                self.assertEqual(observer.wait(child), 23)
                child.wait.assert_called_once_with()
                report = observer.finish()
                backend.sample.assert_called_once_with()
                backend.close.assert_called_once_with()
                self.assertEqual(failing.write_calls, 1)
                self.assertEqual(failing.close_calls, 1)
                self.assertTrue(actual.closed)
                self.assertEqual(observer.attempt_count, 1)
                self.assertEqual(sample_path.read_bytes(), preserved)
                self.assertEqual(report["coverage"]["sample_count"], 0)
                self.assertEqual(report["coverage"]["complete_sample_bytes"], 0)
                self.assertEqual(report["coverage"]["sample_bytes"], len(preserved))
                self.assertTrue(report["coverage"]["sample_output_failed"])
                self.assertEqual(report["coverage"]["sampling_status"], "output_failed")
                self.assertFalse(report["coverage"]["full_lifetime"])
                self.assertEqual(len(report["errors"]), 1)
                self.assertEqual(report["errors"][0]["phase"], "sample")

    def test_primary_samples_reconstruct_native_identity_modules_and_resources(self):
        result, backend = self.observe([self.sample(10, working=10), self.sample(20, working=30)])
        assessed = self.assess()
        self.assertEqual(assessed["status"], "valid")
        self.assertEqual(assessed["process"]["creation_filetime"], self.creation)
        resource = assessed["resources"]["working_set_bytes"]
        self.assertEqual({key: resource[key] for key in ("first", "last", "minimum", "maximum", "delta")}, {"first": 10, "last": 30, "minimum": 10, "maximum": 30, "delta": 20})
        self.assertEqual(resource["source_records"]["maximum"], {"artifact": "observations/samples.jsonl", "line": 2})
        self.assertEqual({item["native_thread_id"] for item in assessed["threads"]}, {"10", "11"})
        self.assertFalse(result["coverage"]["full_lifetime"])
        self.assertFalse(self.manifest["runtime"]["loaded_modules_verified"])
        backend.close.assert_called_once()

    def test_forged_derived_resource_report_is_rejected_despite_matching_saved_hash(self):
        result, unused = self.observe([self.sample(10)])
        result["resources"]["working_set_bytes"]["maximum"] = 999
        atomic_write_json(self.run_dir / "observations/process.json", result)
        self.refresh("process_observation")
        self.assertEqual(self.assess()["status"], "invalid")

    def test_forged_primary_process_creation_time_is_rejected(self):
        self.observe([self.sample(10)])
        path = self.run_dir / "observations/samples.jsonl"
        row = json.loads(path.read_text(encoding="utf-8"))
        row["identity"]["creation_filetime"] = "1"
        path.write_text(json.dumps(row) + "\n", encoding="utf-8")
        self.refresh("process_samples")
        self.assertEqual(self.assess()["status"], "invalid")

    def test_sampled_unload_reload_does_not_assign_gap_or_heap_ownership(self):
        result, unused = self.observe([self.sample(10), self.sample(20), self.sample(30, library=False), self.sample(40)])
        library = next(row for row in result["modules"] if row["path"] == str(self.library))
        self.assertEqual(len(library["presence_intervals"]), 2)
        self.assertEqual(capture_process.resolve_address(8200, result, at_ticks=15)["offset"], "8")
        self.assertEqual(capture_process.resolve_address(8200, result, at_ticks=35)["status"], "unknown")
        self.assertEqual(capture_process.resolve_address(9000, result, at_ticks=15)["status"], "unknown")
        self.assertEqual(capture_process.resolve_address(8448, result, at_ticks=15)["status"], "unknown")
        self.assertIsNone(capture_process.resolve_address(8200, result, at_ticks=15)["symbol"])

    def test_pid_reuse_and_unselected_module_hashes_stay_unknown(self):
        self.policy["hash_roots"] = []
        result, unused = self.observe([self.sample(10), self.sample(20, creation=str(int(self.creation) + 100))])
        self.assertEqual(result["coverage"]["sample_count"], 1)
        self.assertTrue(result["errors"])
        self.assertTrue(all(row["file_identity"] is None for row in result["modules"]))
        self.assertEqual(self.assess()["status"], "unknown")

    def test_failed_sampling_is_bounded_and_wait_path_preserves_child_exit(self):
        self.policy["max_samples"] = 2
        backend = mock.Mock(frequency=1000)
        backend.bound_to_child_handle = True
        backend.sample.side_effect = OSError("access denied")
        child = mock.Mock()
        child.wait.side_effect = [subprocess.TimeoutExpired("synthetic", .05), 0]
        observer = capture_process.ProcessObserver(self.run_dir, self.manifest, self.policy, backend_factory=lambda pid: backend, expected_creation_filetime=self.creation)
        self.assertEqual(observer.wait(child), 0)
        result = observer.finish()
        self.assertEqual(result["coverage"]["attempt_count"], 2)
        self.assertEqual(result["coverage"]["sample_count"], 0)

    def test_optional_collector_polling_continues_after_sample_cap(self):
        self.policy["max_samples"] = 1
        backend = mock.Mock(frequency=1000, bound_to_child_handle=True)
        backend.sample.return_value = self.sample(10)
        child = mock.Mock()
        child.wait.side_effect = [subprocess.TimeoutExpired("synthetic", .05), 0]
        callback = mock.Mock()
        observer = capture_process.ProcessObserver(self.run_dir, self.manifest, self.policy,
            backend_factory=lambda pid: backend, expected_creation_filetime=self.creation)
        self.addCleanup(observer.stream.close)
        self.assertEqual(observer.wait(child, poll_callback=callback), 0)
        report = observer.finish()
        self.assertEqual(callback.call_count, 2)
        backend.sample.assert_called_once_with()
        self.assertEqual(report["coverage"]["sample_count"], 1)
        self.assertEqual(report["coverage"]["sampling_status"], "sample_cap")

    def test_collector_polling_does_not_increase_observation_sampling_frequency(self):
        self.policy["interval_ms"] = 60000
        backend = mock.Mock(frequency=1000, bound_to_child_handle=True)
        backend.sample.return_value = self.sample(10)
        observer = capture_process.ProcessObserver(self.run_dir, self.manifest, self.policy,
            backend_factory=lambda pid: backend, expected_creation_filetime=self.creation)
        self.addCleanup(observer.stream.close)
        clock = [0.0]
        waits = []

        def wait(*, timeout):
            waits.append(timeout)
            clock[0] += timeout
            if len(waits) < 4:
                raise subprocess.TimeoutExpired("synthetic", timeout)
            return 0

        child = mock.Mock()
        child.wait.side_effect = wait
        callback = mock.Mock()
        with mock.patch.object(capture_process.time, "monotonic", side_effect=lambda: clock[0]):
            self.assertEqual(observer.wait(child, poll_callback=callback), 0)
        report = observer.finish()
        self.assertEqual(callback.call_count, 4)
        self.assertTrue(all(0 < timeout <= 0.25 for timeout in waits))
        backend.sample.assert_called_once_with()
        self.assertEqual(report["coverage"]["sample_count"], 1)

    def test_cache_snapshot_hashes_only_declared_files_and_never_infers_warmth(self):
        cache = self.root / "cache"
        cache.mkdir()
        content = cache / "entry"
        content.write_bytes(b"cache identity only")
        self.policy["cache_locations"] = [{"kind": "driver", "path": str(cache), "max_files": 2, "max_bytes": 1024}]
        before = capture_process.snapshot_locations(self.policy, "before")
        self.assertEqual(before["locations"][0]["files"][0]["sha256"], hashlib.sha256(b"cache identity only").hexdigest())
        self.assertEqual(before["locations"][0]["state"], "unknown")
        self.assertEqual(content.read_bytes(), b"cache identity only")
        self.assertFalse(before["policy_inferred_from_contents"])

    def test_observation_rejects_forbidden_paths_and_unbounded_sampling(self):
        for policy in ({"enabled": True, "max_samples": 0}, {"enabled": True, "interval_ms": True},
                       {"enabled": True, "hash_roots": [str(self.root / "dump")]},
                       {"enabled": True, "effective_settings_files": [str(self.root / "fakelib" / "x")]}):
            with self.subTest(policy=policy), self.assertRaises(SchemaError):
                capture_process.validate_observation_config(policy)

    def test_backend_without_exact_child_binding_cannot_verify_process_identity(self):
        backend = mock.Mock(frequency=1000, bound_to_child_handle=False)
        observer = capture_process.ProcessObserver(self.run_dir, self.manifest, self.policy, backend_factory=lambda pid: backend, expected_creation_filetime=self.creation)
        observer.sample()
        result = observer.finish()
        self.assertFalse(result["process"]["identity_verified"])
        self.assertEqual(result["coverage"]["sample_count"], 0)
        self.assertTrue(result["errors"])
        backend.sample.assert_not_called()

    def profile(self, kind):
        raw = self.root / "sample.csv"
        raw.write_bytes(b"synthetic profiling export\n")
        identity = capture_process.identity_file(raw)
        profile = self.root / "profile.json"
        value = {"schema_version": 1, "document_type": "anyps5.external-evidence", "kind": kind,
                 "run_id": self.manifest["run_id"], "process": {"pid": 123, "creation_filetime": self.creation,
                 "executable_sha256": self.manifest["runtime"]["files"][0]["sha256"]},
                 "source": {"artifact": "external/cpu-source-0000.csv", **identity}}
        profile.write_text(json.dumps(value, indent=2), encoding="utf-8")
        return profile, raw

    def shader_profile(self, *, collector=False):
        import capture_gpu_profile
        from test_capture_gpu_profile import TABLE, mapping, request

        external = self.run_dir / "external"
        external.mkdir(exist_ok=True)
        traces = self.run_dir / "traces"
        traces.mkdir(exist_ok=True)
        (traces / "native.json").write_bytes(b"{}")
        native = capture_process.descriptor("draw_trace", "traces/native.json", self.run_dir)
        self.manifest["artifacts"].append(native)
        selected_request = request()
        selected_request["run_id"] = self.manifest["run_id"]
        selected_request["process"] = {"pid": 123, "creation_filetime": self.creation,
            "executable_sha256": self.manifest["runtime"]["files"][0]["sha256"]}
        selected_request["native_artifact"] = {key: native[key] for key in ("path", "size", "sha256")}
        selected_mapping = mapping()
        atomic_write_json(external / "shader-mapping.json", selected_mapping)
        atomic_write_json(external / "shader-request.json", selected_request)
        raw = self.root / "shaders.csv"
        raw.write_bytes(TABLE.encode("utf-8"))
        identity = capture_process.identity_file(raw)
        value = capture_gpu_profile.normalize_nsight_shader_csv(TABLE, selected_mapping, request=selected_request,
            source_artifact="external/shaders.csv", source_sha256=identity["sha256"], source_size=identity["size"])
        value["mapping_artifact"] = {"path": "external/shader-mapping.json", **capture_process.identity_file(external / "shader-mapping.json")}
        value["request_artifact"] = {"path": "external/shader-request.json", **capture_process.identity_file(external / "shader-request.json")}
        if collector:
            directory = self.run_dir / "collectors"
            directory.mkdir()
            (directory / "command.stdout.txt").write_bytes(b"recorded tool output\n")
            reference = {"path": "collectors/command.stdout.txt", **capture_process.identity_file(directory / "command.stdout.txt")}
            receipt = {"schema_version": 1, "document_type": "anyps5.collector-receipt", "run_id": self.manifest["run_id"],
                "process": selected_request["process"], "commands": [{"argv": ["not-present/ngfx.exe", "--help-all"],
                    "tool": {"path": "not-present/ngfx.exe", "size": 100, "sha256": "c" * 64},
                    "status": "completed", "returncode": 0, "stdout": reference}],
                "artifacts": [dict(reference, kind="collector_log")],
                "coverage": {"scope": "tool_help_only", "lost_events": None, "start_qpc": None, "end_qpc": None}}
            atomic_write_json(directory / "receipt.json", receipt)
            value["collector_receipt_artifact"] = {"path": "collectors/receipt.json", **capture_process.identity_file(directory / "receipt.json")}
        profile = self.root / "gpu-profile.json"
        atomic_write_json(profile, value)
        return profile, raw

    def test_shader_attachment_preserves_auxiliaries_logs_and_manifest_revision(self):
        import capture_external

        self.observe([self.sample(10)])
        profile, raw = self.shader_profile(collector=True)
        atomic_write_json(self.run_dir / "manifest.json", self.manifest)
        before = (self.run_dir / "manifest.json").read_bytes()
        original = profile.read_bytes()
        raw_bytes = raw.read_bytes()
        artifacts = capture_process.attach_profile(self.run_dir, profile, raw)
        saved = read_json(self.run_dir / "manifest.json")
        self.assertEqual((self.run_dir / "external/manifest-before-profile-0000.json").read_bytes(), before)
        self.assertEqual((self.run_dir / "external/gpu_profile-0000.json").read_bytes(), original)
        self.assertEqual(raw.read_bytes(), raw_bytes)
        paths = [item["path"] for item in saved["artifacts"]]
        self.assertEqual(len(paths), len(set(paths)))
        for relative in ("external/shader-mapping.json", "external/shader-request.json", "collectors/receipt.json", "collectors/command.stdout.txt"):
            item = next(row for row in artifacts if row["path"] == relative)
            self.assertEqual({key: item[key] for key in ("size", "sha256")}, capture_process.identity_file(self.run_dir / relative))
        assessment = capture_external.load_external_evidence(self.run_dir, saved)
        self.assertNotEqual(assessment["status"], "invalid", assessment["issues"])
        self.assertEqual(assessment["profiles"][0]["extraction_provenance"]["transformation_status"], "valid")
        self.assertFalse((self.run_dir / ".capture-attach.lock").exists())

    def test_mapped_table_attachment_inventories_existing_mapping(self):
        import capture_tabular
        from test_capture_tabular import mapping

        self.observe([self.sample(10)])
        selected = mapping(frequency=1000)
        raw = self.root / "table.csv"
        text = "Observed Process,Observed Time,Observed Thread,Observed Count\n123,10,10,1\n"
        raw.write_bytes(text.encode("utf-8"))
        identity = capture_process.identity_file(raw)
        value = capture_tabular.normalize_exported_table(text, selected, run_id=self.manifest["run_id"],
            process={"pid": 123, "creation_filetime": self.creation, "executable_sha256": self.manifest["runtime"]["files"][0]["sha256"]},
            source_artifact="collectors/table.csv", source_sha256=identity["sha256"], source_size=identity["size"])
        directory = self.run_dir / "collectors"
        directory.mkdir()
        atomic_write_json(directory / "mapping.json", selected)
        value["mapping_artifact"] = {"path": "collectors/mapping.json", **capture_process.identity_file(directory / "mapping.json")}
        profile = self.root / "mapped-profile.json"
        atomic_write_json(profile, value)
        artifacts = capture_process.attach_profile_files(self.run_dir, self.manifest, profile, raw, index=0)
        self.assertEqual(next(row for row in artifacts if row["path"] == "collectors/mapping.json")["kind"], "external_mapping")
        self.assertEqual((directory / "table.csv").read_bytes(), raw.read_bytes())

    def test_shader_dependency_tamper_preserves_old_manifest_and_outputs(self):
        self.observe([self.sample(10)])
        profile, raw = self.shader_profile()
        atomic_write_json(self.run_dir / "manifest.json", self.manifest)
        before = (self.run_dir / "manifest.json").read_bytes()
        (self.run_dir / "external/shader-mapping.json").write_bytes(b"changed mapping")
        with self.assertRaises(SchemaError):
            capture_process.attach_profile(self.run_dir, profile, raw)
        self.assertEqual((self.run_dir / "manifest.json").read_bytes(), before)
        self.assertFalse((self.run_dir / "external/shaders.csv").exists())
        self.assertFalse((self.run_dir / "external/gpu_profile-0000.json").exists())
        self.assertFalse((self.run_dir / ".capture-attach.lock").exists())

    def test_shader_request_cannot_introduce_uninventoried_native_artifact(self):
        self.observe([self.sample(10)])
        profile, raw = self.shader_profile()
        self.manifest["artifacts"] = [item for item in self.manifest["artifacts"] if item["kind"] != "draw_trace"]
        with self.assertRaises(SchemaError):
            capture_process.attach_profile_files(self.run_dir, self.manifest, profile, raw, index=0)
        self.assertFalse((self.run_dir / "external/shaders.csv").exists())

    def test_profile_auxiliary_suffix_and_actual_size_reject_before_hashing(self):
        directory = self.run_dir / "external"
        directory.mkdir()
        for name, content in (("mapping.prx", b"synthetic binary"), ("mapping.json", b"x" * (1024**2 + 1))):
            with self.subTest(name=name):
                (directory / name).write_bytes(content)
                value = {"producer": {"adapter": "explicit_table_v1"}, "mapping_artifact": {
                    "path": "external/" + name, "size": 2, "sha256": hashlib.sha256(b"{}").hexdigest()}}
                with mock.patch.object(capture_process, "identity_file") as hashed, self.assertRaises(SchemaError):
                    capture_process.profile_dependency_artifacts(self.run_dir, self.manifest, value)
                hashed.assert_not_called()

    def test_postrun_attachment_preserves_normalized_bytes_and_previous_manifest(self):
        self.observe([self.sample(10)])
        atomic_write_json(self.run_dir / "manifest.json", self.manifest)
        previous = (self.run_dir / "manifest.json").read_bytes()
        profile, raw = self.profile("presentation_profile")
        original = profile.read_bytes()
        artifacts = capture_process.attach_profile(self.run_dir, profile, raw)
        self.assertIn("presentation_profile", {row["kind"] for row in artifacts})
        self.assertEqual((self.run_dir / "external/presentation_profile-0000.json").read_bytes(), original)
        self.assertEqual((self.run_dir / "external/manifest-before-profile-0000.json").read_bytes(), previous)
        self.assertEqual(read_json(self.run_dir / "manifest.json")["run_id"], self.manifest["run_id"])

    def test_profile_process_binding_and_raw_source_hash_mismatch_are_rejected(self):
        self.observe([self.sample(10)])
        profile, raw = self.profile("gpu_profile")
        row = read_json(profile)
        row["process"]["creation_filetime"] = "1"
        atomic_write_json(profile, row)
        with self.assertRaises(SchemaError):
            capture_process.attach_profile_files(self.run_dir, self.manifest, profile, raw, index=0)
        profile, raw = self.profile("cpu_profile")
        raw.write_bytes(b"changed export")
        with self.assertRaises(SchemaError):
            capture_process.attach_profile_files(self.run_dir, self.manifest, profile, raw, index=0)

    def test_array_profile_is_rejected_without_copying_evidence(self):
        profile = self.root / "invalid-profile.json"
        raw = self.root / "raw.csv"
        profile.write_text("[]", encoding="utf-8")
        raw.write_text("synthetic", encoding="utf-8")
        with self.assertRaises(SchemaError):
            capture_process.attach_profile_files(self.run_dir, self.manifest, profile, raw, index=0)
        self.assertFalse((self.run_dir / "external").exists())

    def test_unhashable_profile_kinds_are_rejected_without_copying_evidence(self):
        profile, raw = self.profile("cpu_profile")
        value = read_json(profile)
        for kind in ([], {}):
            with self.subTest(kind=kind):
                value["kind"] = kind
                atomic_write_json(profile, value)
                with self.assertRaises(SchemaError):
                    capture_process.attach_profile_files(self.run_dir, self.manifest, profile, raw, index=0)
                self.assertFalse((self.run_dir / "external").exists())

    def test_receipts_do_not_promote_declared_source_or_symbol_binding(self):
        symbols = self.root / "synthetic.debug"
        symbols.write_bytes(b"synthetic symbols")
        source = self.manifest["source"]
        with mock.patch.object(capture_manifest, "source_fingerprint", return_value=source):
            build = capture_process.create_build_receipt(self.root, {"compiler": "declared"}, [self.executable], [(symbols, self.executable)])
        self.assertEqual(build["source_binary_binding"], "unverified")
        self.assertFalse(build["compiler_invocation_verified"])
        symbol = capture_process.create_symbol_receipt(self.executable, symbols)
        self.assertEqual(symbol["binding"], "declared_pair_not_verified_debug_id")
        self.assertEqual(symbol["function_ranges"], [])
        self.assertEqual(symbol["symbol_resolution"], "unknown")

    def test_matching_receipt_only_matches_observed_image_hash(self):
        observed, unused = self.observe([self.sample(10)])
        symbols = self.root / "synthetic.debug"
        symbols.write_bytes(b"synthetic symbols")
        receipt = capture_process.create_symbol_receipt(self.executable, symbols)
        destination = self.run_dir / "external" / "symbol_receipt-0000.json"
        destination.parent.mkdir()
        atomic_write_json(destination, receipt)
        artifact = capture_process.descriptor("symbol_receipt", "external/symbol_receipt-0000.json", self.run_dir)
        self.manifest["artifacts"].append(artifact)
        assessed = self.assess()
        match = assessed["receipt_matches"][0]
        self.assertEqual(match["status"], "observed_image_hash_matches")
        self.assertEqual(match["source_binary_binding"], "unverified")
        self.assertEqual(match["symbol_binding"], "unverified")

    def test_fresh_symbol_metadata_is_reassessed_against_observed_image(self):
        import capture_symbols
        from test_capture_symbols import pe_bytes

        self.executable.write_bytes(pe_bytes())
        self.manifest["runtime"]["files"][0].update(capture_process.identity_file(self.executable))
        sample = self.sample(10)
        sample["modules"][0]["image_size"] = 0x5000
        self.observe([sample])
        receipt = capture_symbols.create_symbol_receipt(self.executable)
        destination = self.run_dir / "external/symbol_receipt-0000.json"
        destination.parent.mkdir()
        atomic_write_json(destination, receipt)
        self.manifest["artifacts"].append(capture_process.descriptor("symbol_receipt", "external/symbol_receipt-0000.json", self.run_dir))
        match = self.assess()["receipt_matches"][0]
        self.assertEqual(match["status"], "observed_image_hash_matches")
        self.assertEqual(match["symbol_binding"], "embedded_dwarf_image_sha256")
        self.assertTrue(match["symbol_assessment"]["matched_observed_image"])
        self.assertEqual(match["symbol_assessment"]["function_range_count"], 1)
        self.assertEqual(match["source_binary_binding"], "unverified")
        receipt["function_ranges"][0]["name"] = "forged symbol"
        atomic_write_json(destination, receipt)
        self.refresh("symbol_receipt")
        invalid = self.assess()["receipt_matches"][0]
        self.assertEqual(invalid["status"], "invalid")

    def test_cpu_samples_resolve_only_observed_ranges_and_keep_exported_names_unverified(self):
        self.observe([self.sample(10), self.sample(20)])
        assessed = self.assess()
        document = {"kind": "cpu_profile", "run_id": self.manifest["run_id"],
                    "process": {"pid": 123, "creation_filetime": self.creation, "executable_sha256": self.manifest["runtime"]["files"][0]["sha256"]},
                    "clock": {"domain": "windows_qpc", "ticks_per_second": 1000}, "source": {"artifact": "external/cpu-source-0000.csv"},
                    "records": {"samples": [{"id": "sample-1", "timestamp_ticks": "15", "thread_id": "10", "weight_samples": 3,
                                                "stack": [{"address": "4104", "symbol": "declared_name"}, {"address": "900000", "symbol": None}]}]}}
        result = capture_process.assess_cpu_sample_ownership(document, assessed, artifact={"path": "external/cpu_profile-0000.json"})
        self.assertEqual(result["status"], "valid")
        entry = result["module_offset_aggregates"][0]
        self.assertEqual(entry["offset"], "8")
        self.assertEqual(entry["weighted_sample_hits"], 3)
        self.assertIsNone(entry["symbol"])
        self.assertEqual(result["exporter_symbol_labels"][0]["status"], "exporter_declared_unverified")
        self.assertEqual(sum(result["unknown_counts"].values()), 1)
        self.assertEqual(entry["source_examples"][0]["record"], "records.samples[0]")
        document["clock"]["domain"] = "query_performance_counter"
        self.assertEqual(capture_process.assess_cpu_sample_ownership(document, assessed)["status"], "valid")
        document["clock"]["ticks_per_second"] = 1001
        self.assertEqual(capture_process.assess_cpu_sample_ownership(document, assessed)["status"], "unknown")
        document["clock"]["ticks_per_second"] = 1000
        document["clock"]["domain"] = "unaligned_vendor_clock"
        self.assertEqual(capture_process.assess_cpu_sample_ownership(document, assessed)["status"], "unknown")


if __name__ == "__main__":
    unittest.main()
