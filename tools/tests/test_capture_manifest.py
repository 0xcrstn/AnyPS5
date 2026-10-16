import copy
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import capture_manifest as manifest
from capture_schema import SchemaError, canonical_sha256


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.executable = self.root / "synthetic.exe"
        self.library = self.root / "synthetic.prx"
        self.executable.write_bytes(b"synthetic executable identity")
        self.library.write_bytes(b"synthetic library identity")
        self.config = {
            "source_root": str(self.root),
            "runtime": {"executable": str(self.executable), "libraries": [str(self.library)]},
            "build": {"compiler": "declared compiler", "build_type": "Release", "options": {}},
            "machine": {"id": "machine-a", "cpu": "cpu", "gpu": "gpu", "driver": "driver", "os": "os"},
            "title": {"id": "synthetic-title", "version": "1"},
            "scene": {"id": "scene-a", "recipe": {"steps": ["start", "wait"]}},
            "settings": {"resolution": [1280, 720], "presentation": "windowed",
                         "cache": {"shader": "warm", "pipeline": "warm", "driver": "unknown"},
                         "diagnostics": {"mode": "measurement", "options": {}},
                         "relinker": {"flags": []}},
            "capture": {"unit": "frames", "start": 10, "length": 20},
            "execution": {"command": [str(self.executable), "argument with spaces"], "cwd": str(self.root)},
            "environment": {"overrides": {"APS5_STORE_AT_FLIP": "1"}},
            "experiment": {"id": "experiment-a", "pair_id": "pair-a"},
        }
        self.source = {"root": str(self.root), "commit": "a" * 40, "dirty": False,
                       "diff_sha256": hashlib.sha256(b"").hexdigest(), "untracked": [], "submodules": []}

    def create(self, config=None):
        return manifest.create_manifest(self.config if config is None else config, source=self.source,
                                        inherited_environment={}, now="2026-10-15T00:00:00Z",
                                        run_id="12345678-1234-4234-8234-123456789abc")

    def test_manifest_keeps_source_and_binary_identity_distinct(self):
        result = self.create()
        self.assertEqual(result["state"], "planned")
        self.assertEqual(result["build"]["source_binary_binding"], "unverified")
        self.assertEqual(result["build"]["provenance"], "declared_configuration")
        self.assertFalse(result["runtime"]["loaded_modules_verified"])
        self.assertEqual(result["runtime"]["inventory_kind"], "declared_files")
        self.assertEqual(result["runtime"]["files"][0]["sha256"],
                         hashlib.sha256(self.executable.read_bytes()).hexdigest())
        self.assertEqual(result["scene"]["recipe_sha256"], canonical_sha256(self.config["scene"]["recipe"]))
        self.assertEqual(result["experiment"], self.config["experiment"])
        self.assertNotIn("source_binary_binding", self.config["build"])

    def test_receipt_hash_does_not_assert_a_proven_build(self):
        receipt = self.root / "receipt.json"
        receipt.write_bytes(b'{"claimed_source":"other-checkout"}')
        self.config["build"]["receipt"] = str(receipt)
        result = self.create()
        self.assertEqual(result["build"]["receipt_sha256"], hashlib.sha256(receipt.read_bytes()).hexdigest())
        self.assertEqual(result["build"]["source_binary_binding"], "unverified")
        self.assertNotIn("receipt", result["build"])

    def test_observation_policy_is_explicit_separate_and_does_not_verify_loaded_modules(self):
        self.config["observation"] = {"enabled": True, "interval_ms": 500, "hash_roots": [str(self.root)]}
        result = self.create()
        self.assertTrue(result["observation"]["enabled"])
        self.assertEqual(result["observation"]["interval_ms"], 500)
        self.assertFalse(result["runtime"]["loaded_modules_verified"])
        self.assertNotIn("process", result["observation"])
        self.assertNotIn("max_samples", self.config["observation"])

    def test_observation_rejects_dump_paths_before_source_or_runtime_inspection(self):
        self.config["observation"] = {"enabled": True, "hash_roots": [str(self.root / "dump")]}
        with patch.object(manifest, "source_fingerprint") as source, patch.object(manifest, "file_identity") as files, self.assertRaises(SchemaError):
            self.create()
        source.assert_not_called()
        files.assert_not_called()

    def test_report_detail_is_explicit_without_rewriting_legacy_configuration(self):
        self.assertNotIn("report_detail", self.create()["settings"]["diagnostics"]["options"])
        for detail in ("compact", "full"):
            config = copy.deepcopy(self.config)
            config["settings"]["diagnostics"]["options"]["report_detail"] = detail
            with self.subTest(detail=detail):
                result = self.create(config)
                self.assertEqual(result["settings"]["diagnostics"]["options"]["report_detail"], detail)
                self.assertEqual(result["state"], "planned")
        self.assertNotIn("report_detail", self.config["settings"]["diagnostics"]["options"])

    def test_invalid_report_detail_is_rejected_before_file_inspection(self):
        for detail in (None, True, 1, "", "all", [], {}):
            config = copy.deepcopy(self.config)
            config["settings"]["diagnostics"]["options"]["report_detail"] = detail
            with self.subTest(detail=detail), patch.object(manifest, "file_identity") as files:
                with self.assertRaises(SchemaError):
                    self.create(config)
                files.assert_not_called()

    def test_measurement_workload_has_run_binding_without_draw_instrumentation(self):
        self.config["environment"]["overrides"]["aps5_trace_workload"] = "1"
        result = self.create()
        effective = result["environment"]["effective"]
        self.assertEqual(effective["APS5_TRACE_WORKLOAD"], "1")
        self.assertEqual(effective["APS5_CAPTURE_RUN_ID"], result["run_id"])
        self.assertNotIn("APS5_TRACE_DETAIL", effective)
        self.assertEqual(result["settings"]["diagnostics"]["mode"], "measurement")
        self.assertNotIn("APS5_CAPTURE_RUN_ID", self.config["environment"]["overrides"])

    def test_optional_collector_policy_is_preserved_without_starting_a_session(self):
        self.assertNotIn("collectors", self.create())
        self.config["collectors"] = {"enabled": False}
        with patch("capture_collectors.CollectorSession") as session:
            self.assertEqual(self.create()["collectors"], {"enabled": False})
            tool = self.root / "wpr.exe"
            tool.write_bytes(b"synthetic collector identity")
            self.config["collectors"] = {"enabled": True, "kind": "wpr_cpu", "wpr": str(tool)}
            result = self.create()
            session.assert_not_called()
        self.assertEqual(result["collectors"]["wpr"], str(tool))
        self.assertEqual(result["collectors"]["profile"], "CPU")
        self.assertEqual(result["collectors"]["max_duration_seconds"], 120)
        self.assertEqual(result["state"], "planned")
        self.assertNotIn("collector_errors", result["execution"])
        self.assertNotIn("max_duration_seconds", self.config["collectors"])

    def test_invalid_collector_policy_fails_before_runtime_hashing(self):
        for policy in ({}, {"enabled": "yes"}, {"enabled": False, "kind": "wpr_cpu"},
                       {"enabled": True, "kind": "unsupported"}):
            config = copy.deepcopy(self.config)
            config["collectors"] = policy
            with self.subTest(policy=policy), patch.object(manifest, "file_identity") as files:
                with self.assertRaises(SchemaError):
                    self.create(config)
                files.assert_not_called()

    def test_measurement_workload_requires_explicit_flag_and_frame_window(self):
        for flag in (None, "0"):
            config = copy.deepcopy(self.config)
            config["environment"]["overrides"]["APS5_TRACE_WORKLOAD"] = flag
            self.assertNotIn("APS5_CAPTURE_RUN_ID", self.create(config)["environment"]["effective"])
        for flag, unit in (("yes", "frames"), ("2", "frames"), ("1", "seconds")):
            config = copy.deepcopy(self.config)
            config["environment"]["overrides"]["APS5_TRACE_WORKLOAD"] = flag
            config["capture"]["unit"] = unit
            with self.subTest(flag=flag, unit=unit), patch.object(manifest, "file_identity") as files:
                with self.assertRaises(SchemaError):
                    self.create(config)
                files.assert_not_called()

    def test_environment_is_case_insensitively_isolated_and_metadata_is_restricted(self):
        inherited = {"Path": "system-path", "aps5_old": "stale", "Vk_LAYER_PATH": "stale", "ANYPS5_LOG": "stale",
                     "PRIVATE_TOKEN": "never-record-this"}
        before = dict(inherited)
        child, metadata = manifest.build_environment({"aps5_new": "explicit", "VK_LAYER_PATH": None}, inherited)
        self.assertEqual(inherited, before)
        self.assertEqual(child, {"Path": "system-path", "PRIVATE_TOKEN": "never-record-this", "APS5_NEW": "explicit"})
        self.assertEqual(metadata["effective"], {"APS5_NEW": "explicit"})
        self.assertEqual(set(metadata["removed_inherited"]), {"aps5_old", "Vk_LAYER_PATH", "ANYPS5_LOG"})
        self.assertNotIn("never-record-this", str(metadata))

    def test_invalid_environment_overrides_are_rejected(self):
        for overrides in ({"PATH": "bad"}, {"APS5_A": 1}, {"APS5_A": "a\0b"},
                          {"APS5_A": "1", "aps5_a": "2"}, {"APS5_BAD=NAME": "1"}, {"APS5_OK": "1", 7: "x"}):
            with self.subTest(overrides=overrides), self.assertRaises(SchemaError):
                manifest.build_environment(overrides, {})

    def test_runtime_reverification_finds_changed_and_missing_files(self):
        result = self.create()
        self.assertTrue(manifest.verify_runtime(result)["verified"])
        self.executable.write_bytes(b"different binary")
        self.library.unlink()
        verification = manifest.verify_runtime(result)
        self.assertFalse(verification["verified"])
        self.assertEqual([item["reason"] for item in verification["findings"]], ["changed", "missing"])
        self.assertEqual(verification["findings"][0]["expected"],
                         {key: result["runtime"]["files"][0][key] for key in ("size", "sha256")})
        self.assertIsNone(verification["findings"][1]["actual"])

    def test_config_rejects_ambiguous_or_incomplete_identity_before_hashing(self):
        mutations = [lambda value: value["runtime"]["libraries"].append(str(self.executable)),
                     lambda value: value["execution"]["command"].__setitem__(0, "relative.exe"),
                     lambda value: value["settings"].__setitem__("cache", {}),
                     lambda value: value["machine"].__setitem__("gpu", ""),
                     lambda value: value["build"].__setitem__("compiler", None),
                     lambda value: value["capture"].__setitem__("length", True)]
        for mutate in mutations:
            config = copy.deepcopy(self.config)
            mutate(config)
            with self.subTest(config=config), patch.object(manifest, "file_identity") as identity:
                with self.assertRaises((SchemaError, ValueError)):
                    self.create(config)
                identity.assert_not_called()

    def test_fakelib_paths_are_rejected_without_opening_files(self):
        config = copy.deepcopy(self.config)
        config["runtime"]["libraries"] = [str(self.root / "FaKeLiB" / "forbidden.prx")]
        with patch.object(manifest, "file_identity") as identity, self.assertRaises(SchemaError):
            self.create(config)
        identity.assert_not_called()

    def fake_git(self, index=b"", working=b"", entries=b"", untracked=b""):
        def read(root, *arguments):
            if arguments == ("rev-parse", "--show-toplevel"):
                return os.fsencode(str(root)) + b"\n"
            if arguments == ("rev-parse", "--verify", "HEAD"):
                return b"a" * 40 + b"\n"
            if arguments == ("ls-files", "--stage", "-z"):
                return entries
            if arguments == ("ls-files", "--others", "--exclude-standard", "-z"):
                return untracked
            if arguments[0] == "diff":
                return index if "--cached" in arguments else working
            raise AssertionError(arguments)
        return read

    def test_source_tracks_untracked_content_and_both_diff_layers(self):
        source_file = self.root / "new.py"
        source_file.write_bytes(b"first source")
        with patch.object(manifest, "git_bytes", side_effect=self.fake_git(index=b"staged", working=b"unstaged",
                                                                         untracked=b"new.py\0")):
            first = manifest.source_fingerprint(self.root)
            source_file.write_bytes(b"changed source")
            second = manifest.source_fingerprint(self.root)
        self.assertTrue(first["dirty"])
        self.assertNotEqual(first["fingerprint_sha256"], second["fingerprint_sha256"])
        self.assertNotEqual(first["untracked"][0]["sha256"], second["untracked"][0]["sha256"])
        with patch.object(manifest, "git_bytes", side_effect=self.fake_git(index=b"unstaged", working=b"staged")):
            swapped = manifest.source_fingerprint(self.root)
        self.assertNotEqual(first["diff_sha256"], swapped["diff_sha256"])

    def test_uninitialized_submodule_identity_is_recorded_without_claiming_checkout(self):
        entries = b"160000 " + b"b" * 40 + b" 0\tdependency\0"
        with patch.object(manifest, "git_bytes", side_effect=self.fake_git(entries=entries)):
            result = manifest.source_fingerprint(self.root)
        self.assertEqual(result["submodules"], [{"path": "dependency", "index_commit": "b" * 40,
                                                "initialized": False}])

    def test_git_inspection_uses_no_shell_and_discards_inherited_git_redirection(self):
        completed = subprocess.CompletedProcess([], 0, stdout=b"result", stderr=b"")
        no_filters = subprocess.CompletedProcess([], 1, stdout=b"", stderr=b"")
        with patch.dict(os.environ, {"GIT_DIR": "wrong", "GIT_INDEX_FILE": "wrong"}), \
                patch.object(manifest.subprocess, "run", side_effect=[no_filters, completed]) as run:
            self.assertEqual(manifest.git_bytes(self.root, "rev-parse", "HEAD"), b"result")
        kwargs = run.call_args.kwargs
        self.assertFalse(kwargs["shell"])
        self.assertNotIn("GIT_DIR", kwargs["env"])
        self.assertNotIn("GIT_INDEX_FILE", kwargs["env"])
        self.assertEqual(kwargs["env"]["GIT_OPTIONAL_LOCKS"], "0")

    def test_git_content_filters_are_disabled_before_source_inspection(self):
        configured = subprocess.CompletedProcess([], 0, stdout=b"filter.custom.clean\0filter.custom.process\0", stderr=b"")
        inspected = subprocess.CompletedProcess([], 0, stdout=b"source diff", stderr=b"")
        with patch.object(manifest.subprocess, "run", side_effect=[configured, inspected]) as run:
            self.assertEqual(manifest.git_bytes(self.root, "diff", "--no-ext-diff", "--no-textconv"), b"source diff")
        command = run.call_args.args[0]
        self.assertIn("filter.custom.clean=", command)
        self.assertIn("filter.custom.process=", command)
        self.assertIn("filter.custom.required=false", command)
        self.assertTrue(all(not call.kwargs["shell"] for call in run.call_args_list))

    def test_hashing_rejects_file_mutation_during_read(self):
        actual = self.executable.stat()
        fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_mode")
        changed = SimpleNamespace(**{key: getattr(actual, key) for key in fields})
        changed.st_mtime_ns += 1
        with patch.object(manifest.os, "fstat", side_effect=[actual, changed]), self.assertRaises(SchemaError):
            manifest.file_identity(self.executable)

    def test_hashing_compares_timestamp_stability_within_each_stat_api(self):
        actual = self.executable.stat()
        fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_mode")
        handle_stat = SimpleNamespace(**{key: getattr(actual, key) for key in fields})
        handle_stat.st_ctime_ns += 1000
        with patch.object(manifest.os, "fstat", return_value=handle_stat):
            identity = manifest.file_identity(self.executable)
        self.assertEqual(identity["sha256"], hashlib.sha256(self.executable.read_bytes()).hexdigest())
        changed_handle_stat = SimpleNamespace(**vars(handle_stat))
        changed_handle_stat.st_ctime_ns += 1
        with patch.object(manifest.os, "fstat", side_effect=[handle_stat, changed_handle_stat]), \
                self.assertRaises(SchemaError):
            manifest.file_identity(self.executable)

    def test_hashing_rejects_path_replacement_with_equal_size_and_timestamps(self):
        actual = self.executable.stat()
        fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_mode")
        replaced = SimpleNamespace(**{key: getattr(actual, key) for key in fields})
        replaced.st_ino += 1
        with patch.object(manifest.Path, "stat", side_effect=[actual, replaced]), \
                patch.object(manifest.os, "fstat", return_value=actual), self.assertRaises(SchemaError):
            manifest.file_identity(self.executable)

    def test_hashing_rejects_nonregular_inputs(self):
        directory = self.root / "directory"
        directory.mkdir()
        with self.assertRaises((SchemaError, OSError)):
            manifest.file_identity(directory)

    def test_source_rejects_untracked_mutation_during_inspection(self):
        source_file = self.root / "new.py"
        source_file.write_bytes(b"source")
        first = {"size": 6, "sha256": "a" * 64}
        changed = {"size": 7, "sha256": "b" * 64}
        with patch.object(manifest, "git_bytes", side_effect=self.fake_git(untracked=b"new.py\0")), \
                patch.object(manifest, "file_identity", side_effect=[first, changed]), self.assertRaises(SchemaError):
            manifest.source_fingerprint(self.root)

    def test_source_identity_omits_nested_checkout_paths(self):
        first = copy.deepcopy(self.source)
        first["submodules"] = [{"path": "dependency", "index_commit": "b" * 40,
                                 "initialized": True, "source": copy.deepcopy(self.source)}]
        second = copy.deepcopy(first)
        second["root"] = str(self.root / "other")
        second["submodules"][0]["source"]["root"] = str(self.root / "other" / "dependency")
        self.assertEqual(canonical_sha256(manifest.source_identity(first)),
                         canonical_sha256(manifest.source_identity(second)))
        second["submodules"][0]["source"]["commit"] = "c" * 40
        self.assertNotEqual(canonical_sha256(manifest.source_identity(first)),
                            canonical_sha256(manifest.source_identity(second)))

    def test_untracked_symlink_hashes_link_identity_without_reading_target(self):
        target = self.root / "target"
        target.write_bytes(b"synthetic target content")
        link = self.root / "link"
        try:
            link.symlink_to(target)
        except (OSError, NotImplementedError):
            self.skipTest("symbolic links unavailable")
        with patch.object(manifest, "git_bytes", side_effect=self.fake_git(untracked=b"link\0")), \
                patch.object(manifest, "file_identity") as identity:
            result = manifest.source_fingerprint(self.root)
        identity.assert_not_called()
        self.assertEqual(result["untracked"][0]["kind"], "symlink")
        self.assertEqual(result["untracked"][0]["sha256"], hashlib.sha256(os.fsencode(os.readlink(link))).hexdigest())

    def test_config_rejects_nonfinite_metadata_and_accepts_explicit_disabled_cache(self):
        config = copy.deepcopy(self.config)
        config["settings"]["cache"]["shader"] = "disabled"
        self.assertEqual(manifest.validate_config(config)["settings"]["cache"]["shader"], "disabled")
        config["scene"]["recipe"]["invalid"] = float("nan")
        with patch.object(manifest, "file_identity") as identity, self.assertRaises(SchemaError):
            self.create(config)
        identity.assert_not_called()

    def test_manifest_cli_does_not_create_evidence_on_invalid_config(self):
        run_directory = self.root / "new-evidence"
        with patch.object(manifest, "read_json", return_value={}), \
                patch.object(sys, "stderr"), self.assertRaises(SystemExit):
            manifest.main(["--config", "synthetic-config.json", "--run-dir", str(run_directory)])
        self.assertFalse(run_directory.exists())

    def test_manifest_cli_exclusively_creates_manifest_outside_source(self):
        source_directory = self.root / "source"
        source_directory.mkdir()
        run_directory = self.root / "new-evidence"
        document = self.create()
        document["source"]["root"] = str(source_directory)
        with patch.object(manifest, "read_json", return_value=self.config), \
                patch.object(manifest, "create_manifest", return_value=document), \
                patch.object(sys, "stdout"):
            self.assertEqual(manifest.main(["--config", "synthetic-config.json", "--run-dir", str(run_directory)]), 0)
        self.assertTrue((run_directory / "manifest.json").is_file())

    def test_manifest_cli_rejects_evidence_inside_source(self):
        run_directory = self.root / "new-evidence"
        with patch.object(manifest, "read_json", return_value=self.config), \
                patch.object(manifest, "create_manifest", return_value=self.create()), \
                patch.object(sys, "stderr"), self.assertRaises(SystemExit):
            manifest.main(["--config", "synthetic-config.json", "--run-dir", str(run_directory)])
        self.assertFalse(run_directory.exists())

    def test_manifest_cli_preserves_existing_directory(self):
        run_directory = self.root / "existing-evidence"
        run_directory.mkdir()
        evidence = run_directory / "manifest.json"
        evidence.write_text("preserved", encoding="utf-8")
        document = self.create()
        document["source"]["root"] = str(self.root / "source")
        with patch.object(manifest, "read_json", return_value=self.config), \
                patch.object(manifest, "create_manifest", return_value=document), \
                patch.object(manifest, "atomic_write_json") as writer, \
                patch.object(sys, "stderr"), self.assertRaises(SystemExit) as raised:
            manifest.main(["--config", "synthetic-config.json", "--run-dir", str(run_directory)])
        self.assertEqual(raised.exception.code, 2)
        writer.assert_not_called()
        self.assertEqual(evidence.read_text(encoding="utf-8"), "preserved")


if __name__ == "__main__":
    unittest.main()
