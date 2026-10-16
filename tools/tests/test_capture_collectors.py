import copy
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import capture_collectors as collectors
from capture_process import identity_file
from capture_schema import SchemaError, atomic_write_json, read_json


class CollectorTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.run = self.root / "run"
        self.run.mkdir()
        self.tool = self.root / "wpr.exe"
        self.tool.write_bytes(b"synthetic tool, never executed")
        self.executable = self.root / "synthetic.exe"
        self.executable.write_bytes(b"synthetic child image")
        self.config = collectors.validate_collectors_config({"enabled": True, "kind": "wpr_cpu", "wpr": str(self.tool), "max_duration_seconds": 2})
        self.manifest = {"run_id": "91caa5be-6203-4bca-b0a1-691b5612259e", "execution": {"pid": 321},
                         "runtime": {"files": [{"role": "executable", "path": str(self.executable), **identity_file(self.executable)}]}}
        self.process = mock.Mock(pid=321, _handle=123)
        self.process.poll.return_value = None
        self.backend = mock.Mock(frequency=10000000)
        self.backend.identity.return_value = {"pid": 321, "creation_filetime": "134366159652720814", "path": str(self.executable)}
        self.backend.ticks.side_effect = range(100, 200)
        self.backend_patch = mock.patch.object(collectors, "WindowsProcess", return_value=self.backend)
        self.backend_patch.start()
        self.addCleanup(self.backend_patch.stop)
        self.rows = []
        self.calls = []
        self.fail_after_start = False

    def make_session(self):
        sessions = mock.Mock()
        sessions.query.side_effect = lambda: copy.deepcopy(self.rows)
        def command(argv, **kwargs):
            self.calls.append(list(argv))
            prefix = Path(kwargs["output_prefix"])
            outputs = {}
            for name in ("stdout", "stderr"):
                path = Path(str(prefix) + f".{name}.txt")
                path.write_bytes(b"")
                outputs[name] = {"path": str(path), **identity_file(path)}
            if "-start" in argv:
                self.rows.append({"name": "WPR_" + session.instance + "_NT Kernel Logger", "session_id": "91", "guid": "8ddcb346-09ae-487a-ab80-83f38765064d", "enable_flags": 1, "events_lost": 2})
                if self.fail_after_start:
                    raise OSError("synthetic command failure after session starts")
            if "-stop" in argv:
                self.rows.clear()
                Path(argv[argv.index("-stop") + 1]).write_bytes(b"synthetic ETL evidence")
            if "-outputfolder" in argv:
                destination = Path(argv[argv.index("-outputfolder") + 1])
                (destination / "samples.csv").write_bytes(b"PID,QPC,TID,Weight,IP\n321,100,7,1,4096\n")
            return {"argv": list(argv), "tool": {"path": argv[0], **identity_file(argv[0]), "file_version": None},
                    "status": "completed", "returncode": 0, **outputs}
        session = collectors.CollectorSession(self.run, self.manifest, self.config, process=self.process, command_runner=command, session_backend=sessions)
        return session

    def test_unique_instance_stop_and_idempotent_finish_preserve_explicit_identity(self):
        session = self.make_session()
        session.start()
        receipt = session.finish()
        calls = len(self.calls)
        self.assertIs(session.finish(), receipt)
        self.assertEqual(len(self.calls), calls)
        self.assertEqual(receipt["status"], "stopped")
        self.assertEqual(receipt["ownership"]["state"], "verified_stopped")
        self.assertEqual(receipt["process"]["creation_filetime"], "134366159652720814")
        self.assertEqual(receipt["process"]["identity_binding"], "retained_child_handle")
        self.assertIsNone(receipt["coverage"]["lost_events"])
        self.assertEqual(receipt["coverage"]["observed_pre_stop_events_lost"], 2)
        self.assertFalse(receipt["coverage"]["full_lifetime"])
        self.assertTrue(all(argv[-2:] == ["-instancename", session.instance] for argv in self.calls))
        self.assertFalse(any("-cancel" in argv for argv in self.calls))
        self.assertEqual({row["kind"] for row in session.artifacts}, {"raw_etw", "collector_receipt", "collector_log"})
        self.backend.close.assert_called_once()

    def test_existing_kernel_session_refuses_start_without_control_commands(self):
        self.rows = [{"name": "Existing", "session_id": "1", "guid": "existing", "enable_flags": 1, "events_lost": 0}]
        session = self.make_session()
        with self.assertRaises(SchemaError):
            session.start()
        self.assertEqual(self.calls, [])
        session.finish()
        self.assertEqual(self.rows[0]["name"], "Existing")
        self.assertEqual(session.receipt["status"], "failed")

    def test_changed_session_identity_never_stops_replacement(self):
        session = self.make_session()
        session.start()
        self.rows[0]["session_id"] = "92"
        receipt = session.finish()
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.rows[0]["session_id"], "92")
        self.assertIn("refusing any stop", receipt["errors"][-1]["message"])

    def test_start_exception_rediscovers_owned_session_for_safe_cleanup(self):
        self.fail_after_start = True
        session = self.make_session()
        with self.assertRaises(OSError):
            session.start()
        self.assertTrue(session.owned)
        receipt = session.finish()
        self.assertEqual(self.rows, [])
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(len(self.calls), 2)
        self.assertTrue(receipt["errors"])

    def test_duration_poll_stops_only_own_instance(self):
        session = self.make_session()
        session.start()
        session.started_monotonic = time.monotonic() - 3
        session.poll()
        self.assertTrue(session.finished)
        self.assertEqual(session.receipt["stop_reason"], "duration_limit")
        self.assertEqual(self.rows, [])

    def test_retained_child_image_mismatch_refuses_collection(self):
        self.backend.identity.return_value["pid"] = 999
        session = self.make_session()
        with self.assertRaises(SchemaError):
            session.start()
        session.finish()
        self.assertEqual(self.calls, [])

    def test_unresolved_session_query_failure_is_explicit_and_never_global_stop(self):
        session = self.make_session()
        session.session_backend.query.side_effect = [[], OSError("query failed"), OSError("query failed"), OSError("query failed")]
        with self.assertRaises(OSError):
            session.start()
        receipt = session.finish()
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(len(self.calls), 1)
        self.assertTrue(any("query failed" in row["message"] for row in receipt["errors"]))

    def test_configuration_rejects_unbounded_or_unsupported_tools_and_exports(self):
        for changed in ({"max_duration_seconds": 0}, {"max_trace_bytes": 2**64}, {"profile": "GeneralProfile"}, {"unexpected": True}, {"enabled": 1}):
            with self.subTest(changed=changed), self.assertRaises(SchemaError):
                collectors.validate_collectors_config({**self.config, **changed})
        with self.assertRaises(SchemaError):
            collectors.validate_collectors_config({"enabled": False, "wpr": str(self.tool)})
        self.assertIsNone(collectors.validate_collectors_config(None))
        self.assertEqual(collectors.validate_collectors_config({"enabled": False}), {"enabled": False})

    def process_evidence(self):
        return {"run_id": self.manifest["run_id"], "status": "valid", "process": {"pid": 321, "creation_filetime": "134366159652720814", "identity_verified": True,
                "identity_binding": "retained_child_handle", "executable": {"path": str(self.executable), **identity_file(self.executable)}},
                "clock": {"domain": "windows_qpc", "ticks_per_second": 10000000, "anchors": [{"ticks": "100"}, {"ticks": "200"}]},
                "threads": [{"native_thread_id": "7", "first_seen_ticks": "100", "last_seen_ticks": "200"}], "modules": []}

    def test_offline_assessor_binds_preserved_receipt_without_current_installed_tool(self):
        session = self.make_session()
        session.start()
        session.finish()
        self.manifest["collectors"] = self.config
        self.manifest["artifacts"] = session.artifacts
        self.tool.unlink()
        archived = self.root / "archive"
        shutil.move(str(self.run), archived)
        usable = {row["path"]: archived / row["path"] for row in session.artifacts}
        assessed = collectors.assess_collector_receipt(archived, self.manifest, verified_artifacts=usable, process_evidence=self.process_evidence())
        self.assertEqual(assessed["status"], "valid", assessed["issues"])
        self.assertEqual(assessed["tool_identity_status"], "valid")
        self.assertEqual(assessed["run_id"], self.manifest["run_id"])
        self.assertEqual(assessed["tool_identity_verification"], "preserved_receipt_only_not_current_binary_rehash")
        self.assertEqual([row["role"] for row in assessed["tool_identities"]], ["wpr_start", "wpr_stop"])
        log = archived / session.receipt["commands"][0]["stdout"]["path"]
        log.write_bytes(b"changed")
        self.assertEqual(collectors.assess_collector_receipt(archived, self.manifest, verified_artifacts=usable, process_evidence=self.process_evidence())["status"], "invalid")

    def test_absent_or_disabled_optional_collector_is_unknown(self):
        for value in (None, {"enabled": False}):
            with self.subTest(value=value):
                self.manifest["collectors"] = value
                result = collectors.assess_collector_receipt(self.run, self.manifest)
                self.assertEqual(result["status"], "unknown")
                self.assertEqual(result["issues"], [])

    def test_structural_policy_does_not_read_missing_historical_tools(self):
        self.tool.unlink()
        self.assertEqual(collectors.validate_collectors_config(self.config, check_files=False), self.config)
        with self.assertRaises(SchemaError):
            collectors.validate_collectors_config(self.config)

    def test_success_receipt_cannot_omit_required_raw_output(self):
        session = self.make_session()
        session.start()
        session.finish()
        path = self.run / "collectors/receipt.json"
        receipt = read_json(path)
        receipt["artifacts"] = []
        atomic_write_json(path, receipt)
        self.manifest["collectors"] = self.config
        self.manifest["artifacts"] = [dict(row) for row in session.artifacts]
        selected = next(row for row in self.manifest["artifacts"] if row["kind"] == "collector_receipt")
        selected.update(identity_file(path))
        result = collectors.assess_collector_receipt(self.run, self.manifest, process_evidence=self.process_evidence())
        self.assertEqual(result["status"], "invalid")
        self.assertTrue(any("output missing" in issue["message"] for issue in result["issues"]))

    def test_handle_cleanup_failure_does_not_skip_failed_receipt(self):
        session = self.make_session()
        session.start()
        self.backend.close.side_effect = OSError("synthetic close failure")
        receipt = session.finish()
        self.assertEqual(receipt["status"], "failed")
        self.assertTrue((self.run / "collectors/receipt.json").is_file())
        self.assertTrue(any("cleanup failed" in row["message"] for row in receipt["errors"]))

    def test_wpa_export_normalizes_and_loads_preserved_csv_and_mapping(self):
        from capture_external import load_external_evidence
        tool = self.root / "wpaexporter.exe"
        tool.write_bytes(b"synthetic exporter never executed")
        profile = self.root / "selected.wpaProfile"
        profile.write_text("<Profile />", encoding="utf-8")
        mapping_path = self.root / "mapping.json"
        mapping = {"schema_version": 1, "document_type": "anyps5.table-mapping", "tool": {"name": "WPAExporter", "version": "synthetic"},
                   "table": {"name": "explicit selected sample rows", "record_kind": "samples"}, "csv": {"delimiter": ",", "quotechar": '"', "encoding": "utf-8"},
                   "clock": {"domain": "windows_qpc", "ticks_per_second": 10000000, "input_unit": "ticks"},
                   "columns": {"pid": "PID", "timestamp": "QPC", "thread_id": "TID", "weight_samples": "Weight", "address": "IP"}}
        atomic_write_json(mapping_path, mapping, replace=False)
        self.config["export"] = {"tool": str(tool), "profile": str(profile), "table_file": "samples.csv", "mapping": str(mapping_path)}
        session = self.make_session()
        session.start()
        receipt = session.finish()
        self.assertEqual(receipt["status"], "stopped", receipt["errors"])
        self.manifest["artifacts"] = session.artifacts
        usable = {row["path"]: self.run / row["path"] for row in session.artifacts}
        assessed = load_external_evidence(self.run, self.manifest, verified_artifacts=usable, process_evidence=self.process_evidence())
        self.assertEqual(assessed["issues"], [])
        self.assertEqual(len(assessed["profiles"]), 1)
        selected = assessed["profiles"][0]
        self.assertEqual(selected["extraction_provenance"]["transformation_status"], "valid")
        self.assertEqual(selected["document"]["records"]["samples"][0]["stack"][0]["address"], "4096")
        self.assertEqual(selected["document"]["clock"]["anchors"], [])
        self.assertIsNone(selected["document"]["coverage"]["lost_records"])
        self.assertEqual(assessed["status"], "unknown")


if __name__ == "__main__":
    unittest.main()
