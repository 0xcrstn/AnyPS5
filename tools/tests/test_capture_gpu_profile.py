import contextlib
import copy
import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import capture_external
import capture_gpu_profile
from capture_schema import SchemaError, canonical_sha256
from test_capture_schema import make_manifest


RUN_ID = "00000000-0000-4000-8000-000000000001"
CAPTURE_ID = "00000000-0000-4000-8000-000000000002"


def request():
    return {"schema_version": 1, "document_type": "anyps5.gpu-profile-request", "run_id": RUN_ID, "capture_id": CAPTURE_ID,
        "process": {"pid": 7, "creation_filetime": "12345", "executable_sha256": "a" * 64},
        "native_artifact": {"path": "traces/native.json", "size": 2, "sha256": hashlib.sha256(b"{}").hexdigest()},
        "targets": [{"operation": "20", "batch": "30", "command": "40", "pipeline": "50", "shader_variant": "60",
                     "spirv": "70", "spirv_sha256": "b" * 64, "stage": "fragment"}],
        "tool": {"name": "NVIDIA Nsight Graphics", "version": "declared-test-version"}, "scope": "live",
        "collection": {"start_after_frames": 600, "limit_to_frames": 1, "max_duration_ms": 1000}}


def mapping():
    return {"schema_version": 1, "document_type": "anyps5.nsight-shader-mapping", "tool_version": "declared-test-version",
        "csv": {"delimiter": ",", "quotechar": '"', "encoding": "utf-8"},
        "columns": {"row_type": "Type", "shader_label": "Name", "vendor_shader_hash": "Hash"},
        "row_types": {"Fragment": "shader", "Compute": "shader", "Pipeline": "pipeline", "Total": "aggregate", "Unattributed": "unattributed"},
        "stage_values": {"Fragment": "fragment", "Compute": "compute"},
        "metrics": [{"column": "Samples", "name": "Samples", "unit": "samples", "measurement_kind": "warp_samples", "unavailable_values": ["N/A"]},
                    {"column": "Long Scoreboard", "name": "long_scoreboard", "unit": "samples", "measurement_kind": "stall_samples", "unavailable_values": ["N/A"]},
                    {"column": "Instructions Executed", "name": "Instructions Executed", "unit": "instructions", "measurement_kind": "counter", "unavailable_values": ["N/A"]}]}


TABLE = "Type,Name,Hash,Samples,Long Scoreboard,Instructions Executed\r\nFragment,Observed shader,vendor-opaque,12,3,40\r\nPipeline,Observed pipeline,other,50,20,100\r\nUnattributed,Unattributed,,5,2,N/A\r\n"


def normalize(text=TABLE, selected_mapping=None, selected_request=None):
    raw = text.encode("utf-8")
    return capture_gpu_profile.normalize_nsight_shader_csv(text, selected_mapping or mapping(), request=selected_request or request(),
        source_artifact="external/shaders.csv", source_sha256=hashlib.sha256(raw).hexdigest(), source_size=len(raw))


class CaptureGpuProfileTests(unittest.TestCase):
    def test_documented_table_semantics_keep_stage_samples_and_counters_distinct(self):
        document = normalize()
        rows = document["records"]["shader_metrics"]
        self.assertEqual([row["value"] for row in rows], [12, 3, 40])
        self.assertEqual([row["measurement_kind"] for row in rows], ["warp_samples", "stall_samples", "counter"])
        self.assertTrue(all(row["stage"] == "fragment" for row in rows))
        self.assertTrue(all("timestamp_ticks" not in row and row["debug_label"] is None and row["shader_sha256"] is None for row in rows))
        self.assertEqual(rows[0]["vendor_shader_hash"], "vendor-opaque")
        self.assertEqual(document["adapter_metadata"]["excluded_row_types"], {"pipeline": 1, "aggregate": 0, "unattributed": 1})
        self.assertEqual(document["coverage"]["state"], "unknown")
        self.assertIsNone(document["coverage"]["lost_records"])
        self.assertEqual(document["clock"]["anchors"], [])

    def test_requests_never_become_observed_command_or_shader_ids(self):
        document = normalize()
        row = document["records"]["shader_metrics"][0]
        self.assertFalse(set(capture_gpu_profile.GRAPH_IDS) & set(row))
        self.assertEqual(document["adapter_metadata"]["native_target_binding"], "missing_proof")
        self.assertEqual(document["adapter_metadata"]["tool_identity"], "missing_proof")
        self.assertEqual(document["adapter_metadata"]["request_sha256"], canonical_sha256(request()))

    def test_only_actual_exported_identity_columns_are_copied(self):
        selected = mapping()
        selected["columns"].update(debug_label="Observed Label", spirv_sha256="Observed SPIRV SHA256")
        selected["spirv_digest_scope"] = "anyps5.final-spirv.v1"
        label = f"aps5 run={RUN_ID} capture={CAPTURE_ID} op=20 batch=30 pipeline=50 command=40"
        text = f'Type,Name,Hash,Samples,Long Scoreboard,Instructions Executed,Observed Label,Observed SPIRV SHA256\nFragment,Shader,h,1,0,5,{label},{"b" * 64}\n'
        row = normalize(text, selected)["records"]["shader_metrics"][0]
        self.assertEqual(row["debug_label"], label)
        self.assertEqual(row["shader_sha256"], "b" * 64)
        self.assertNotIn("binding_status", row)

    def test_vendor_or_raw_file_hash_is_not_native_domain_specific_spirv_identity(self):
        selected = mapping()
        selected["columns"]["spirv_sha256"] = "Raw Bytecode File SHA256"
        for scope in (None, "raw_file_sha256", "vendor_hash"):
            if scope is not None:
                selected["spirv_digest_scope"] = scope
            with self.subTest(scope=scope), self.assertRaises(SchemaError):
                capture_gpu_profile.validate_shader_mapping(selected)

    def test_unavailable_is_not_zero_and_absent_column_is_not_imputed(self):
        document = normalize(TABLE.replace(",12,3,40", ",0,N/A,40"))
        rows = document["records"]["shader_metrics"]
        self.assertEqual((rows[0]["status"], rows[0]["value"]), ("measured", 0))
        self.assertEqual((rows[1]["status"], rows[1]["value"]), ("unavailable", None))
        with self.assertRaises(SchemaError):
            normalize(TABLE.replace(",Long Scoreboard", "").replace(",12,3,40", ",12,40"))

    def test_raw_uint64_counts_remain_exact(self):
        document = normalize(TABLE.replace(",12,3,40", f",{2 ** 64 - 1},3,40"))
        self.assertEqual(document["records"]["shader_metrics"][0]["value"], 2 ** 64 - 1)
        self.assertIs(type(document["records"]["shader_metrics"][0]["value"]), int)

    def test_raw_uint64_cycle_counts_are_not_rounded_to_binary64(self):
        selected = mapping()
        selected["metrics"][0].update(unit="cycles", measurement_kind="counter")
        document = normalize(TABLE.replace(",12,3,40", f",{2 ** 64 - 1},3,40"), selected)
        self.assertEqual(document["records"]["shader_metrics"][0]["value"], 2 ** 64 - 1)
        self.assertIs(type(document["records"]["shader_metrics"][0]["value"]), int)
        with self.assertRaises(SchemaError):
            normalize(TABLE.replace(",12,3,40", ",1.5,3,40"), selected)

    def test_no_stage_time_units_or_unknown_metric_kinds(self):
        for changes in ({"unit": "ns"}, {"measurement_kind": "stage_elapsed"}, {"unit": "seconds"}):
            selected = mapping()
            selected["metrics"][0].update(changes)
            with self.subTest(changes=changes), self.assertRaises(SchemaError):
                normalize(selected_mapping=selected)

    def test_nonfinite_negative_fractional_or_missing_counts_reject(self):
        for cell in ("NaN", "Infinity", "-1", "1.5", "", "18446744073709551616"):
            with self.subTest(cell=cell), self.assertRaises(SchemaError):
                normalize(TABLE.replace(",12,3,40", f",{cell},3,40"))

    def test_percentage_is_explicit_and_bounded(self):
        selected = mapping()
        selected["metrics"][0].update(unit="percent", measurement_kind="sample_percent")
        self.assertEqual(normalize(TABLE.replace(",12,3,40", ",12.5,3,40"), selected)["records"]["shader_metrics"][0]["value"], 12.5)
        with self.assertRaises(SchemaError):
            normalize(TABLE.replace(",12,3,40", ",101,3,40"), selected)

    def test_rows_have_exact_logical_and_physical_source_pointers(self):
        text = TABLE.replace("Observed shader", '"Observed\r\nshader"')
        row = normalize(text)["records"]["shader_metrics"][0]
        self.assertEqual(row["source_record"], 1)
        self.assertEqual(row["source_lines"], {"first": 2, "last": 3})

    def test_missing_duplicate_headers_and_malformed_rows_reject(self):
        tables = [TABLE.replace("Samples", "Hash", 1), TABLE.replace("Fragment,Observed shader,vendor-opaque,12,3,40", "Fragment,Shader,h,12,3"),
                  TABLE.replace("Fragment,Observed shader", 'Fragment,"unclosed'), TABLE.replace("Fragment,Observed shader", "Unknown,Observed shader")]
        for text in tables:
            with self.subTest(text=text[:80]), self.assertRaises(SchemaError):
                normalize(text)

    def test_pipeline_rows_cannot_be_relabelled_as_stage_or_metric_headers_reused(self):
        for mutate in (lambda value: value["stage_values"].update(Pipeline="fragment"),
                       lambda value: value["metrics"][0].update(column="Name"),
                       lambda value: value["columns"].update(spirv_sha256="Hash")):
            selected = mapping()
            mutate(selected)
            with self.assertRaises(SchemaError):
                normalize(selected_mapping=selected)

    def test_bom_and_crlf_are_part_of_source_hash(self):
        selected = mapping()
        selected["csv"]["encoding"] = "utf-8-sig"
        document = normalize("\ufeff" + TABLE, selected)
        self.assertEqual(document["source"]["size"], len(("\ufeff" + TABLE).encode("utf-8")))
        with self.assertRaises(SchemaError):
            normalize("\ufeff" + TABLE)
        with self.assertRaises(SchemaError):
            capture_gpu_profile.normalize_nsight_shader_csv(TABLE.replace("\r\n", "\n"), mapping(), request=request(),
                source_artifact="external/shaders.csv", source_sha256=hashlib.sha256(TABLE.encode()).hexdigest(), source_size=len(TABLE.encode()))

    def test_request_requires_nonzero_ids_unique_targets_and_safe_artifact(self):
        changes = [lambda value: value["targets"][0].update(command="0"), lambda value: value["targets"].append(copy.deepcopy(value["targets"][0])),
                   lambda value: value["native_artifact"].update(path="../native.json"), lambda value: value["native_artifact"].update(path="dump/native.json"),
                   lambda value: value["targets"][0].update(spirv_sha256="vendor-hash"), lambda value: value["collection"].update(limit_to_frames=100),
                   lambda value: value["tool"].update(name="Nsight Systems"), lambda value: value.update(capture_id=RUN_ID)]
        for mutate in changes:
            selected = request()
            mutate(selected)
            with self.subTest(request=selected), self.assertRaises(SchemaError):
                capture_gpu_profile.validate_profile_request(selected)

    def test_packet_is_documented_argv_only_with_explicit_missing_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            packet = capture_gpu_profile.build_profile_packet(request(), ngfx=root / "ngfx.exe", target_executable=root / "test.exe",
                working_directory=root, output_directory=root / "output")
            args = packet["commands"][1]["argv"]
            self.assertEqual(args[1:5], ["--activity", "GPU Trace Profiler", "--platform", "Windows"])
            self.assertIn("--real-time-shader-profiler", args)
            self.assertEqual(args[args.index("--set-gpu-clocks") + 1], "unaltered")
            self.assertNotIn("--multi-pass-metrics", args)
            self.assertNotIn("--time-every-action", args)
            self.assertFalse(packet["executed"])
            self.assertEqual(packet["execution_gate"]["installed_version_supported"], "unknown")
            self.assertEqual(packet["capabilities"]["exact_stage_elapsed"], "unsupported")

    def test_replay_packet_does_not_offer_live_launch_or_claim_original_scope(self):
        selected = request()
        selected["scope"] = "replay"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            packet = capture_gpu_profile.build_profile_packet(selected, ngfx=root / "ngfx.exe", target_executable=root / "test.exe",
                working_directory=root, output_directory=root / "output")
        self.assertEqual(packet["commands"], [])
        document = normalize(selected_request=selected)
        self.assertEqual(document["adapter_metadata"]["profile_scope"], "replay")
        self.assertEqual(document["adapter_metadata"]["scope_provenance"], "unknown")

    def test_table_bounds_and_declared_tool_version_reject(self):
        with patch.object(capture_gpu_profile, "MAX_TABLE_ROWS", 1), self.assertRaises(SchemaError):
            normalize()
        with patch.object(capture_gpu_profile, "MAX_TABLE_BYTES", 10), self.assertRaises(SchemaError):
            normalize()
        selected = mapping()
        selected["tool_version"] = "another-version"
        with self.assertRaises(SchemaError):
            normalize(selected_mapping=selected)
        with patch.object(capture_gpu_profile, "MAX_SHADER_RECORDS", 2), self.assertRaises(SchemaError):
            normalize()
        with patch.object(capture_gpu_profile, "MAX_SHADER_RECORD_BYTES", 8), self.assertRaises(SchemaError):
            normalize()

    def test_external_validator_rejects_fabricated_clock_and_coverage(self):
        changes = [lambda value: value["records"]["shader_metrics"][0].update(timestamp_ticks="0"),
                   lambda value: value["coverage"].update(state="complete"), lambda value: value["coverage"].update(lost_records=0),
                   lambda value: value["clock"].update(domain="windows_qpc"), lambda value: value["records"]["shader_metrics"][0].update(unit="ns"),
                   lambda value: value.update(kind=[]), lambda value: value.update(kind={})]
        for mutate in changes:
            document = normalize()
            mutate(document)
            with self.assertRaises(SchemaError):
                capture_external.validate_external_document(document)

    def test_cli_guards_reject_prohibited_paths_before_read(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "external").mkdir()
            (root / "external/request.json").write_text(json.dumps(request()), encoding="utf-8")
            for run_dir, source in ((root / "dump", "external/shaders.csv"), (root, "external/program.prx")):
                with patch.object(capture_gpu_profile, "_read_bounded") as reader, contextlib.redirect_stderr(io.StringIO()):
                    result = capture_gpu_profile.main(["--run-dir", str(run_dir), "normalize", "--source", source,
                        "--mapping", "external/mapping.json", "--request", "external/request.json", "--output", "external/profile.json"])
                    self.assertEqual(result, 2)
                    reader.assert_not_called()

    def test_declared_collector_receipt_never_authenticates_vendor_or_live_scope(self):
        receipt = {"document_type": "anyps5.collector-receipt", "schema_version": 1, "run_id": RUN_ID,
            "process": request()["process"], "commands": [{"argv": ["ngfx.exe", "--help-all"],
                "tool": {"path": "private/ngfx.exe", "size": 100, "sha256": "c" * 64, "file_version": "declared-test-version"},
                "returncode": 0, "stdout": {"path": "external/help.log", "size": 1, "sha256": "d" * 64}}],
            "artifacts": [], "coverage": {"scope": "declared_live", "lost_events": None, "start_qpc": None, "end_qpc": None}}
        inspected = capture_gpu_profile.inspect_collector_receipt(receipt, request())
        self.assertEqual(inspected["status"], "recorded")
        self.assertEqual(inspected["commands"][0]["tool_sha256"], "c" * 64)
        self.assertEqual(inspected["tool_authenticity"], "unknown")
        self.assertEqual(inspected["scope_provenance"], "unknown")
        self.assertIsNone(inspected["reported_lost_events"])
        receipt["process"] = dict(receipt["process"], creation_filetime="54321")
        with self.assertRaises(SchemaError):
            capture_gpu_profile.inspect_collector_receipt(receipt, request())

    def test_unmapped_stage_does_not_inherit_requested_fragment_stage(self):
        selected = mapping()
        selected["stage_values"] = {}
        self.assertIsNone(normalize(selected_mapping=selected)["records"]["shader_metrics"][0]["stage"])

    def test_failed_collector_receipt_preserves_failure_and_does_not_imply_zero_loss(self):
        receipt = {"document_type": "anyps5.collector-receipt", "schema_version": 1, "run_id": RUN_ID,
            "process": request()["process"], "commands": [], "status": "failed", "state": "failed",
            "artifacts": [], "coverage": {"scope": "partial_command_attempt", "lost_events": None,
                                            "start_qpc": None, "end_qpc": None}}
        inspected = capture_gpu_profile.inspect_collector_receipt(receipt, request())
        self.assertEqual(inspected["recorded_collector_status"], "failed")
        self.assertEqual(inspected["commands"], [])
        self.assertIsNone(inspected["reported_lost_events"])

    def test_collector_related_logs_bind_manifest_hashes_without_tool_binary_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "collectors").mkdir()
            log = b"recorded command output\n"
            (root / "collectors/command.stdout.txt").write_bytes(log)
            reference = {"path": "collectors/command.stdout.txt", "size": len(log), "sha256": hashlib.sha256(log).hexdigest()}
            receipt = {"document_type": "anyps5.collector-receipt", "schema_version": 1, "run_id": RUN_ID,
                "process": request()["process"], "commands": [{"argv": ["ngfx.exe", "--help-all"],
                    "tool": {"path": "not-present/ngfx.exe", "size": 100, "sha256": "c" * 64, "file_version": None},
                    "returncode": 0, "stdout": reference}], "artifacts": [],
                "coverage": {"scope": "tool_help_only", "lost_events": None, "start_qpc": None, "end_qpc": None}}
            raw = json.dumps(receipt).encode()
            (root / "collectors/receipt.json").write_bytes(raw)
            descriptor = {"path": "collectors/receipt.json", "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
            document = normalize()
            document["collector_receipt_artifact"] = descriptor
            descriptors = {item["path"]: item for item in (reference, descriptor)}
            verified = {key: root / key for key in descriptors}
            inspected = capture_external._shader_collector_receipt(root, document, request(), descriptors, verified)
            self.assertEqual(inspected["artifact_integrity"], "valid")
            self.assertFalse(inspected["source_csv_in_receipt"])
            self.assertEqual(inspected["tool_authenticity"], "unknown")
            descriptors[reference["path"]] = dict(reference, sha256="e" * 64)
            with self.assertRaises(SchemaError):
                capture_external._shader_collector_receipt(root, document, request(), descriptors, verified)

    def test_malformed_mapping_inputs_are_schema_errors(self):
        changes = [lambda value: value.update(row_types=[]), lambda value: value.update(stage_values=[]),
                   lambda value: value["metrics"][0].update(measurement_kind=[]), lambda value: value["metrics"][0].update(unit=[]),
                   lambda value: value["metrics"][0].update(unavailable_values=[None]), lambda value: value["csv"].update(delimiter="\n"),
                   lambda value: value["columns"].update(timestamp="Synthetic Time")]
        for mutate in changes:
            selected = mapping()
            mutate(selected)
            with self.assertRaises(SchemaError):
                normalize(selected_mapping=selected)

    def test_cli_saves_exact_request_and_mapping_descriptors_without_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "external").mkdir()
            for path, raw in (("shaders.csv", TABLE.encode()), ("mapping.json", json.dumps(mapping()).encode()), ("request.json", json.dumps(request()).encode())):
                (root / "external" / path).write_bytes(raw)
            args = ["--run-dir", str(root), "normalize", "--source", "external/shaders.csv", "--mapping", "external/mapping.json",
                    "--request", "external/request.json", "--output", "external/profile.json"]
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(capture_gpu_profile.main(args), 0)
            saved = (root / "external/profile.json").read_bytes()
            document = json.loads(saved)
            self.assertEqual(document["request_artifact"]["sha256"], hashlib.sha256((root / "external/request.json").read_bytes()).hexdigest())
            self.assertEqual(document["mapping_artifact"]["sha256"], hashlib.sha256((root / "external/mapping.json").read_bytes()).hexdigest())
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(capture_gpu_profile.main(args), 2)
            self.assertEqual((root / "external/profile.json").read_bytes(), saved)

    def test_external_loader_reparses_csv_request_and_mapping(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / "external").mkdir()
            (root / "traces").mkdir()
            manifest = make_manifest(root)
            manifest["run_id"] = RUN_ID
            manifest["execution"]["pid"] = 7
            document = normalize()
            artifacts = []
            for kind, relative, raw in (("external_source", "external/shaders.csv", TABLE.encode()),
                    ("external_mapping", "external/mapping.json", json.dumps(mapping()).encode()),
                    ("gpu_profile_request", "external/request.json", json.dumps(request()).encode()), ("draw_trace", "traces/native.json", b"{}")):
                (root / relative).write_bytes(raw)
                descriptor = {"kind": kind, "path": relative, "sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw)}
                artifacts.append(descriptor)
                if kind in ("external_mapping", "gpu_profile_request"):
                    document["mapping_artifact" if kind == "external_mapping" else "request_artifact"] = {key: descriptor[key] for key in ("path", "sha256", "size")}
            def assess(value):
                raw = json.dumps(value).encode()
                (root / "external/profile.json").write_bytes(raw)
                manifest["artifacts"] = artifacts + [{"kind": "gpu_profile", "path": "external/profile.json", "sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw)}]
                return capture_external.load_external_evidence(root, manifest, process_evidence={"process": None})
            result = assess(document)
            self.assertEqual(result["status"], "unknown")
            self.assertEqual(result["profiles"][0]["extraction_provenance"]["transformation_status"], "valid")
            changed = copy.deepcopy(document)
            changed["records"]["shader_metrics"][0]["value"] = 0
            self.assertEqual(assess(changed)["status"], "invalid")
            changed = copy.deepcopy(document)
            changed["profile_request"]["targets"][0]["command"] = "41"
            self.assertEqual(assess(changed)["status"], "invalid")


if __name__ == "__main__":
    unittest.main()
