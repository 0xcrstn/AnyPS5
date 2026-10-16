import copy
import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import capture_schema as schema


def make_manifest(root="/synthetic"):
    root = Path(root).absolute()
    recipe = {"steps": []}
    return {
        "schema_version": 1,
        "document_type": "anyps5.run-manifest",
        "run_id": "12345678-1234-4234-8234-123456789abc",
        "created_at": "2026-01-01T00:00:00Z",
        "started_at": None,
        "ended_at": None,
        "state": "planned",
        "exit_code": None,
        "source": {"root": str(root / "source"), "commit": "a" * 40, "dirty": False,
                   "diff_sha256": schema.canonical_sha256({}), "untracked": [], "submodules": []},
        "build": {"compiler": "synthetic compiler", "build_type": "Release", "options": {},
                  "provenance": "declared_configuration", "source_binary_binding": "unverified"},
        "runtime": {"inventory_kind": "declared_files", "loaded_modules_verified": False,
                    "files": [{"role": "executable", "path": str(root / "program.exe"),
                               "size": 3, "sha256": hashlib.sha256(b"exe").hexdigest()}]},
        "machine": {"id": "test-machine", "cpu": "synthetic CPU", "gpu": "synthetic GPU",
                    "driver": "synthetic driver", "os": "synthetic OS", "provenance": "declared_configuration"},
        "title": {"id": "synthetic-title", "version": "1"},
        "scene": {"id": "synthetic-scene", "recipe": recipe,
                  "recipe_sha256": schema.canonical_sha256(recipe)},
        "settings": {"resolution": "640x480", "presentation": "windowed",
                     "cache": {"shader": "warm", "pipeline": "warm", "driver": "warm"},
                     "diagnostics": {"mode": "measurement", "options": {}},
                     "relinker": {"flags": []}},
        "environment": {"effective": {"APS5_TRACE_FRAMES": "10:4"}, "removed_inherited": []},
        "capture": {"unit": "frames", "start": 10, "length": 4},
        "artifacts": [],
        "execution": {"command": [str(root / "program.exe")], "cwd": str(root), "pid": None},
    }


def make_health():
    return {
        "schema_version": 1, "document_type": "anyps5.capture-health",
        "run_id": "12345678-1234-4234-8234-123456789abc", "status": "valid",
        "frame_metrics": {"status": "valid", "metrics": {"frame_ms.p50": 16.0}},
        "detailed_attribution": {"status": "unknown", "limitations": ["producer omits short slices"]},
        "provenance": {"status": "unknown", "loaded_modules_verified": False,
                       "source_binary_binding": "unverified"},
        "issues": [],
    }


class CaptureSchemaTests(unittest.TestCase):
    def test_collector_policy_is_structural_for_archived_tool_paths(self):
        for policy in (None, {"enabled": False}, {"enabled": True, "kind": "wpr_cpu", "wpr": "C:/archived-no-longer-installed/wpr.exe",
                "export": {"tool": "C:/archived-no-longer-installed/WPAExporter.exe", "profile": "C:/archived/profile.wpaProfile", "table_file": "cpu.csv"}}):
            manifest = make_manifest()
            manifest["collectors"] = policy
            schema.validate_manifest(manifest)
        for policy in ([], "enabled", {"enabled": 1}, {"enabled": False, "wpr": "C:/wpr.exe"},
                {"enabled": True, "kind": "wpr_cpu", "wpr": "C:/wpr.exe", "max_trace_bytes": 0},
                {"enabled": True, "kind": "wpr_cpu", "wpr": "C:/wpr.exe", "export": []}):
            manifest = make_manifest()
            manifest["collectors"] = policy
            with self.subTest(policy=policy), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)

    def test_report_detail_option_is_optional_and_exactly_typed(self):
        for detail in ("compact", "full"):
            manifest = make_manifest()
            manifest["settings"]["diagnostics"]["options"]["report_detail"] = detail
            schema.validate_manifest(manifest)
        for detail in (None, True, "FULL", 1):
            manifest = make_manifest()
            manifest["settings"]["diagnostics"]["options"]["report_detail"] = detail
            with self.subTest(detail=detail), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)

    def test_light_workload_requires_frame_window_and_owned_run_uuid(self):
        manifest = make_manifest()
        manifest["environment"]["effective"].update(APS5_TRACE_WORKLOAD="1", APS5_CAPTURE_RUN_ID=manifest["run_id"])
        schema.validate_manifest(manifest)
        manifest["capture"]["unit"] = "seconds"
        with self.assertRaises(schema.SchemaError):
            schema.validate_manifest(manifest)
        manifest["capture"]["unit"] = "frames"
        del manifest["environment"]["effective"]["APS5_CAPTURE_RUN_ID"]
        with self.assertRaises(schema.SchemaError):
            schema.validate_manifest(manifest)

    def test_minimal_manifest_preserves_source_binary_distinction(self):
        manifest = make_manifest()
        self.assertIs(schema.validate_manifest(manifest), manifest)
        self.assertEqual(manifest["build"]["source_binary_binding"], "unverified")
        self.assertFalse(manifest["runtime"]["loaded_modules_verified"])
        self.assertEqual(schema.capture_spec(manifest["capture"]), "10:4")

    def test_json_rejects_nested_duplicate_keys_and_nonfinite_numbers(self):
        for text in ('{"a":1,"a":2}', '{"outer":{"a":1,"a":2}}',
                     '{"x":NaN}', '{"x":Infinity}', '{"x":-Infinity}', '{"x":1e999}'):
            with self.subTest(text=text), self.assertRaises(schema.SchemaError):
                schema.loads_json(text)

    def test_json_read_failure_identifies_context(self):
        with self.assertRaisesRegex(schema.SchemaError, "capture.json"):
            schema.loads_json('{"schema_version":', context="capture.json")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.json"
            path.write_bytes(b"\xff")
            with self.assertRaises(schema.SchemaError):
                schema.read_json(path)

    def test_canonical_hash_is_order_independent_and_utf8(self):
        first = {"z": [2, 3], "a": "caf\u00e9"}
        second = {"a": "caf\u00e9", "z": [2, 3]}
        self.assertEqual(schema.canonical_sha256(first), schema.canonical_sha256(second))
        self.assertEqual(schema.canonical_json_bytes(first), b'{"a":"caf\xc3\xa9","z":[2,3]}')
        for bad in ({1: "value"}, {"x": float("nan")}, {"x": object()}):
            with self.subTest(bad=bad), self.assertRaises(schema.SchemaError):
                schema.canonical_sha256(bad)

    def test_boolean_is_not_integer_or_metric(self):
        for key in ("schema_version", "exit_code"):
            manifest = make_manifest()
            manifest[key] = True
            with self.subTest(key=key), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)
        for key in ("start", "length"):
            capture = {"unit": "frames", "start": 0, "length": 1, key: True}
            with self.subTest(key=key), self.assertRaises(schema.SchemaError):
                schema.validate_capture(capture)
        for metric in (True, float("inf"), float("nan"), 10 ** 1000, -1):
            health = make_health()
            health["frame_metrics"]["metrics"]["frame_ms.p50"] = metric
            with self.subTest(metric=repr(metric)), self.assertRaises(schema.SchemaError):
                schema.validate_health(health)

    def test_core_metadata_cannot_be_accidentally_empty(self):
        for section, key, value in (("machine", "cpu", ""), ("title", "version", " "),
                                    ("scene", "id", ""), ("build", "compiler", {}),
                                    ("settings", "resolution", []), ("settings", "cache", {})):
            manifest = make_manifest()
            manifest[section][key] = value
            with self.subTest(section=section, key=key), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)

    def test_schema_uuid_and_recipe_mismatch_rejected(self):
        for key, value in (("schema_version", 2), ("document_type", "legacy-summary"),
                           ("run_id", "not-a-uuid"),
                           ("run_id", "12345678-1234-4234-8234-123456789ABC")):
            manifest = make_manifest()
            manifest[key] = value
            with self.subTest(key=key), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)
        manifest = make_manifest()
        manifest["scene"]["recipe"]["steps"].append("changed")
        with self.assertRaisesRegex(schema.SchemaError, "recipe_sha256"):
            schema.validate_manifest(manifest)

    def test_lifecycle_consistency_and_spawn_failure(self):
        cases = (("running", None, None, None), ("planned", "2026-01-01T00:00:01Z", None, None),
                 ("completed", "2026-01-01T00:00:01Z", "2026-01-01T00:00:02Z", 1),
                 ("completed", None, "2026-01-01T00:00:02Z", 0),
                 ("failed", None, None, None),
                 ("interrupted", "2026-01-01T00:00:03Z", "2026-01-01T00:00:02Z", None))
        for state, started, ended, code in cases:
            manifest = make_manifest()
            manifest.update(state=state, started_at=started, ended_at=ended, exit_code=code)
            with self.subTest(state=state, started=started), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)
        manifest = make_manifest()
        manifest.update(state="failed", ended_at="2026-01-01T00:00:02Z")
        self.assertIs(schema.validate_manifest(manifest), manifest)

    def test_timestamps_require_utc_and_chronological_order(self):
        for timestamp in ("2026-01-01", "2026-01-01T00:00:00", "2026-01-01T01:00:00+01:00",
                          "not-time"):
            manifest = make_manifest()
            manifest["created_at"] = timestamp
            with self.subTest(timestamp=timestamp), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)

    def test_declared_runtime_is_one_executable_with_unique_paths(self):
        for change in ("none", "duplicate", "observed", "relative"):
            manifest = make_manifest()
            files = manifest["runtime"]["files"]
            if change == "none":
                files.clear()
            elif change == "duplicate":
                files.append(dict(files[0], role="library"))
            elif change == "observed":
                manifest["runtime"]["loaded_modules_verified"] = True
            else:
                files[0]["path"] = "program.exe"
            with self.subTest(change=change), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)

    def test_required_cache_dimensions_and_unknown_policy(self):
        for cache in ({"foo": 1}, {"shader": "warm", "pipeline": "warm"},
                      {"shader": "maybe", "pipeline": "warm", "driver": "warm"},
                      {"shader": {"identity": "id"}, "pipeline": "warm", "driver": "warm"}):
            manifest = make_manifest()
            manifest["settings"]["cache"] = cache
            with self.subTest(cache=cache), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)
        manifest = make_manifest()
        manifest["settings"]["cache"] = {"shader": {"state": "cold", "identity": "cache-1"},
                                            "pipeline": "disabled", "driver": "unknown"}
        self.assertIs(schema.validate_manifest(manifest), manifest)

    def test_submodule_state_requires_its_source_identity_when_initialized(self):
        for submodule in ({}, {"path": "thirdparty", "index_commit": "c" * 40, "initialized": "false"},
                          {"path": "thirdparty", "index_commit": "c" * 40, "initialized": True},
                          {"path": "../thirdparty", "index_commit": "c" * 40, "initialized": False}):
            manifest = make_manifest()
            manifest["source"]["submodules"] = [submodule]
            with self.subTest(submodule=submodule), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)
        manifest = make_manifest()
        child = copy.deepcopy(manifest["source"])
        child["commit"] = "c" * 40
        manifest["source"]["submodules"] = [{"path": "thirdparty", "index_commit": "c" * 40,
                                               "initialized": True, "source": child}]
        self.assertIs(schema.validate_manifest(manifest), manifest)

    def test_command_identifies_inventory_executable(self):
        manifest = make_manifest()
        manifest["execution"]["command"][0] = str(Path(manifest["execution"]["cwd"]) / "other.exe")
        with self.assertRaises(schema.SchemaError):
            schema.validate_manifest(manifest)
        manifest = make_manifest()
        manifest["execution"]["command"].append("")
        self.assertIs(schema.validate_manifest(manifest), manifest)

    def test_declared_metadata_cannot_claim_a_source_binary_binding(self):
        for section, key, value in (("build", "source_binary_binding", "verified"),
                                    ("build", "provenance", "observed"),
                                    ("machine", "provenance", "observed")):
            manifest = make_manifest()
            manifest[section][key] = value
            with self.subTest(section=section, key=key), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)

    def test_capture_matches_producer_unsigned_integer_range(self):
        for key in ("start", "length"):
            capture = {"unit": "frames", "start": 0, "length": 1, key: 2 ** 64}
            with self.subTest(key=key), self.assertRaises(schema.SchemaError):
                schema.validate_capture(capture)
        self.assertEqual(schema.capture_spec({"unit": "frames", "start": 2 ** 64 - 1,
                                              "length": 2 ** 64 - 1}), f"{2 ** 64 - 1}:{2 ** 64 - 1}")

    def test_environment_excludes_unrelated_names_and_case_aliases(self):
        for effective in ({"SECRET": "never record"}, {"APS5_X": "1", "aps5_x": "2"},
                          {"APS5_X": None}, {"APS5_X=Y": "1"}):
            manifest = make_manifest()
            manifest["environment"]["effective"] = effective
            with self.subTest(effective=effective), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)

    def test_artifact_paths_reject_both_platform_traversal_and_streams(self):
        with tempfile.TemporaryDirectory() as directory:
            for path in ("../outside", "sub/../../outside", "sub\\..\\outside", "C:\\outside",
                         "/outside", "\\\\host\\share", "trace:stream", "./trace", "sub//trace",
                         "trace.", "sub /trace", "NUL", "sub/CON.json", "COM1.txt"):
                with self.subTest(path=path), self.assertRaises(schema.SchemaError):
                    schema.contained_path(directory, path)
            expected = Path(directory).resolve() / "traces" / "capture.json.gz"
            self.assertEqual(schema.contained_path(directory, "traces/capture.json.gz"), expected)
            with self.assertRaises(schema.SchemaError):
                schema.contained_path(directory, "missing", must_exist=True)

    def test_symlink_escape_and_missing_descendant_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            run = base / "run"
            outside = base / "outside"
            run.mkdir()
            outside.mkdir()
            link = run / "escape"
            try:
                link.symlink_to(outside, target_is_directory=True)
            except (OSError, NotImplementedError) as error:
                self.skipTest(f"symlinks unavailable: {error}")
            with self.assertRaises(schema.SchemaError):
                schema.contained_path(run, "escape/missing.json")

    def test_artifacts_require_typed_hashes_and_unique_paths(self):
        artifact = {"kind": "trace", "path": "traces/trace.json.gz", "sha256": "b" * 64, "size": 4}
        for change in ("hash", "size", "duplicate"):
            manifest = make_manifest()
            manifest["artifacts"] = [copy.deepcopy(artifact)]
            if change == "hash":
                manifest["artifacts"][0]["sha256"] = "bad"
            elif change == "size":
                manifest["artifacts"][0]["size"] = False
            else:
                manifest["artifacts"].append(copy.deepcopy(artifact))
            with self.subTest(change=change), self.assertRaises(schema.SchemaError):
                schema.validate_manifest(manifest)

    def test_atomic_create_preserves_existing_evidence_and_cleans_temporary(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            schema.atomic_write_json(path, {"value": 1}, replace=False)
            with self.assertRaises(FileExistsError):
                schema.atomic_write_json(path, {"value": 2}, replace=False)
            self.assertEqual(schema.read_json(path), {"value": 1})
            self.assertEqual(list(Path(directory).iterdir()), [path])
            schema.atomic_write_json(path, {"value": 3})
            self.assertEqual(schema.read_json(path), {"value": 3})

    def test_failed_atomic_replace_preserves_original(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text('{"value":1}', encoding="utf-8")
            with mock.patch.object(schema.os, "replace", side_effect=OSError("synthetic failure")):
                with self.assertRaises(OSError):
                    schema.atomic_write_json(path, {"value": 2})
            self.assertEqual(schema.read_json(path), {"value": 1})
            self.assertEqual(list(Path(directory).iterdir()), [path])

    def test_file_digest_matches_content(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.bin"
            path.write_bytes(b"synthetic content")
            self.assertEqual(schema.sha256_file(path), hashlib.sha256(b"synthetic content").hexdigest())

    def test_health_valid_does_not_claim_complete_attribution(self):
        health = make_health()
        self.assertIs(schema.validate_health(health), health)
        self.assertEqual(health["detailed_attribution"]["status"], "unknown")
        self.assertEqual(health["provenance"]["status"], "unknown")
        health["issues"].append({"code": "loss", "severity": "error", "message": "lost events"})
        with self.assertRaises(schema.SchemaError):
            schema.validate_health(health)

    def test_health_status_cannot_hide_invalid_dimensions_or_error_issues(self):
        for dimension in ("frame_metrics", "detailed_attribution", "provenance"):
            health = make_health()
            health["status"] = "unknown"
            health[dimension]["status"] = "invalid"
            with self.subTest(dimension=dimension), self.assertRaises(schema.SchemaError):
                schema.validate_health(health)
        health = make_health()
        health["status"] = "unknown"
        health["issues"] = [{"code": "error", "severity": "error", "message": "invalid evidence"}]
        with self.assertRaises(schema.SchemaError):
            schema.validate_health(health)

    def test_comparison_dispatch_and_nonfinite_change(self):
        comparison = {"schema_version": 1, "document_type": "anyps5.capture-comparison",
                      "status": "insufficient", "reasons": ["one pair"], "limitations": ["descriptive only"],
                      "baseline": {}, "candidate": {}, "metrics": {"frame_ms.p50": {"change": -1}}}
        self.assertIs(schema.validate_document(comparison), comparison)
        comparison["metrics"]["frame_ms.p50"]["change"] = float("nan")
        with self.assertRaises(schema.SchemaError):
            schema.validate_document(comparison)
        with self.assertRaises(schema.SchemaError):
            schema.validate_document({"document_type": []})

    def test_draw_workload_status_is_optional_and_typed(self):
        attribution = {"status": "unknown", "identity_status": "unknown", "cpu_status": "valid",
                       "gpu_status": "unknown", "run_id": make_health()["run_id"],
                       "capture_id": "22345678-1234-4234-8234-123456789abc",
                       "correlation_valid": False, "limitations": [], "metrics": {}}
        self.assertIs(schema.validate_draw_attribution(attribution), attribution)
        for status in ("valid", "invalid", "unknown"):
            with self.subTest(status=status):
                attribution["workload_status"] = status
                self.assertIs(schema.validate_draw_attribution(attribution), attribution)
        for status in (None, True, 1, "complete", "", [], {}):
            attribution["workload_status"] = status
            with self.subTest(status=status), self.assertRaisesRegex(schema.SchemaError, "workload_status"):
                schema.validate_draw_attribution(attribution)


class EvidenceSchemaTests(unittest.TestCase):
    def report(self):
        return {"schema_version": 1, "document_type": "anyps5.evidence-report",
                "run_id": make_health()["run_id"], "capture_id": None, "status": "unknown",
                "issues": [], "dimensions": {"clock_mapping": {"status": "unknown", "reasons": ["no raw anchors"]}},
                "metrics": {"cpu.elapsed_ms": 1.5, "cpu.scheduled_ms": None},
                "metric_availability": {"cpu.elapsed_ms": "valid", "cpu.scheduled_ms": "unknown"}}

    def test_partial_evidence_keeps_independent_measurement(self):
        report = self.report()
        self.assertIs(schema.validate_document(report), report)
        self.assertIsNone(report["metrics"]["cpu.scheduled_ms"])
        health = make_health()
        health["evidence"] = report
        self.assertIs(schema.validate_health(health), health)

    def test_missing_measurement_cannot_be_available(self):
        report = self.report()
        report["metric_availability"]["cpu.scheduled_ms"] = "valid"
        with self.assertRaises(schema.SchemaError):
            schema.validate_evidence_report(report)
        report = self.report()
        report["metric_availability"]["fabricated"] = "valid"
        with self.assertRaises(schema.SchemaError):
            schema.validate_evidence_report(report)

    def test_structural_error_and_wrong_run_cannot_hide_in_health(self):
        report = self.report()
        report["dimensions"]["clock_mapping"]["status"] = "invalid"
        with self.assertRaises(schema.SchemaError):
            schema.validate_evidence_report(report)
        report["status"] = "invalid"
        schema.validate_evidence_report(report)
        health = make_health()
        health["evidence"] = report
        with self.assertRaises(schema.SchemaError):
            schema.validate_health(health)
        health["evidence"] = self.report()
        health["evidence"]["run_id"] = "87654321-1234-4234-8234-123456789abc"
        with self.assertRaises(schema.SchemaError):
            schema.validate_health(health)


class NativeObservationSchemaTests(unittest.TestCase):
    def test_raw_cycle_delta_is_precise_and_not_converted_to_time(self):
        record = {"kind": "span", "end_ns": 100, "data": {"thread_cycles_status": "measured",
            "thread_cycles_begin": str(2 ** 60), "thread_cycles_end": str(2 ** 60 + 1), "thread_cycles_delta": "1",
            "thread_cycles_native_id": "12", "thread_cycles_unit": "cycles", "thread_cycles_source": "query_thread_cycle_time"}}
        schema._validate_draw_extended_data(record)
        record["data"]["thread_cycles_delta"] = "0"
        with self.assertRaises(schema.SchemaError):
            schema._validate_draw_extended_data(record)

    def test_unavailable_cycle_measurement_cannot_fabricate_zero(self):
        record = {"kind": "span", "end_ns": 100, "data": {"thread_cycles_status": "unavailable"}}
        schema._validate_draw_extended_data(record)
        record["data"]["thread_cycles_delta"] = "0"
        with self.assertRaises(schema.SchemaError):
            schema._validate_draw_extended_data(record)

    def measured_span(self):
        return {"kind": "span", "end_ns": 1000, "status": "complete", "data": {
            "thread_cpu_status": "measured", "thread_cpu_begin_ticks": "10000000000000000000",
            "thread_cpu_end_ticks": "10000000000000000005", "thread_cpu_frequency": "1000000000",
            "thread_cpu_native_id": "12", "thread_cpu_ns": 5}}

    def test_large_cpu_ticks_keep_small_measured_delta(self):
        record = self.measured_span()
        schema._validate_draw_extended_data(record)
        record["status"] = "failed"
        schema._validate_draw_extended_data(record)
        record["data"]["thread_cpu_ns"] = 0
        with self.assertRaises(schema.SchemaError):
            schema._validate_draw_extended_data(record)

    def test_clipped_or_unavailable_cpu_cannot_become_zero(self):
        record = self.measured_span()
        record["data"]["thread_cpu_status"] = "clipped"
        with self.assertRaises(schema.SchemaError):
            schema._validate_draw_extended_data(record)
        record["data"] = {key: None for key in record["data"]}
        record["data"]["thread_cpu_status"] = "clipped"
        schema._validate_draw_extended_data(record)

    def test_unavailable_hardware_counter_differs_from_measured_zero(self):
        record = {"kind": "counter", "name": "fragment_invocations", "text": "invocations", "value": 0,
                  "status": "unknown", "data": {"availability": "unsupported", "counter_value": None}}
        schema._validate_draw_extended_data(record)
        record["data"]["counter_value"] = "0"
        with self.assertRaises(schema.SchemaError):
            schema._validate_draw_extended_data(record)
        record["status"] = "complete"
        record["data"]["availability"] = "available"
        schema._validate_draw_extended_data(record)

    def test_reversed_clock_bracket_is_rejected(self):
        clock = {"domain": "steady_clock", "time_unit": "ns", "origin_steady_ns": "9000000000000",
                 "native_anchor": {"domain": "query_performance_counter", "ticks": "123",
                                   "frequency": "10000000", "steady_before_ns": -10,
                                   "steady_after_ns": 10, "status": "available"}}
        schema._validate_draw_clock(clock)
        clock["native_anchor"]["steady_after_ns"] = -11
        with self.assertRaises(schema.SchemaError):
            schema._validate_draw_clock(clock)

    def test_unavailable_calibration_preserves_null_period(self):
        record = {"kind": "calibration", "status": "failed", "data": {
            "period_ns": None, "valid_bits": 0, "begin_gpu_ns": None, "end_gpu_ns": None,
            "deviation_ns": None}}
        schema._validate_draw_extended_data(record)
        record["status"] = "complete"
        with self.assertRaises(schema.SchemaError):
            schema._validate_draw_extended_data(record)

    def test_present_ordinal_and_fence_completion_domains_are_explicit(self):
        present = {"kind": "present", "value": 0, "data": {
            "present_ordinal_status": "unavailable", "present_ordinal": None}}
        schema._validate_draw_extended_data(present)
        present["data"].update(present_ordinal_status="observed", present_ordinal="0")
        schema._validate_draw_extended_data(present)
        present["data"]["present_ordinal"] = "1"
        with self.assertRaises(schema.SchemaError):
            schema._validate_draw_extended_data(present)
        batch = {"kind": "batch", "name": "batch_submission", "status": "unknown",
                 "data": {"completion_domain": "native_queue_submit_returned"}}
        schema._validate_draw_extended_data(batch)
        batch["status"] = "complete"
        with self.assertRaises(schema.SchemaError):
            schema._validate_draw_extended_data(batch)
        batch["data"]["completion_domain"] = "gpu_fence_observed"
        schema._validate_draw_extended_data(batch)


if __name__ == "__main__":
    unittest.main()
