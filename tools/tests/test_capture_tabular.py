import copy
import contextlib
import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import capture_tabular
from capture_schema import SchemaError, canonical_sha256
from capture_tabular import normalize_exported_table, validate_mapping


RUN_ID = "11111111-1111-4111-8111-111111111111"
PROCESS = {"pid": 42, "creation_filetime": "134365813786197107", "executable_sha256": "a" * 64}


def mapping(kind="samples", unit="ticks", frequency=10_000_000):
    columns = {"pid": "Observed Process", "timestamp": "Observed Time", "thread_id": "Observed Thread", "weight_samples": "Observed Count"}
    result = {"schema_version": 1, "document_type": "anyps5.table-mapping",
              "tool": {"name": "Synthetic exported table", "version": "test-1"},
              "table": {"name": "Explicit synthetic rows", "record_kind": kind},
              "csv": {"delimiter": ",", "quotechar": '"', "encoding": "utf-8"},
              "clock": {"domain": "query_performance_counter", "input_unit": unit, "ticks_per_second": frequency},
              "columns": columns}
    if kind == "context_switches":
        result["columns"] = {"pid": "P", "thread_id": "T", "start": "Begin", "end": "End", "cpu": "CPU", "state": "State"}
        result["state_values"] = {"On CPU": "running", "Off CPU": "excluded"}
    if kind == "gpu_counters":
        result["columns"] = {"pid": "P", "timestamp": "Time", "name": "Metric", "unit": "Unit", "scope": "Scope",
                             "measurement_kind": "Kind", "status": "Availability", "value": "Value"}
        result["status_values"] = {"Collected": "measured", "Missing": "unavailable", "Disabled": "not_requested", "Error": "failed"}
    return result


def normalize(text, selected=None, process=None):
    raw = text.encode("utf-8")
    return normalize_exported_table(text, selected or mapping(), run_id=RUN_ID, process=process or PROCESS,
        source_artifact="external/observed.csv", source_sha256=hashlib.sha256(raw).hexdigest(), source_size=len(raw))


class ExportedTableTests(unittest.TestCase):
    def test_real_columns_filter_pid_and_preserve_sample_counts(self):
        result = normalize("Observed Process,Observed Time,Observed Thread,Observed Count\r\n42,1792106218872829761,16,3\r\n7,2,8,9\r\n")
        row = result["records"]["samples"][0]
        self.assertEqual(row["timestamp_ticks"], "1792106218872829761")
        self.assertEqual(row["thread_id"], "16")
        self.assertEqual(row["weight_samples"], 3)
        self.assertEqual(row["stack"], [])
        self.assertIsNone(row["weight_ns"])
        self.assertEqual(result["adapter_metadata"]["excluded_process_rows"], 1)
        self.assertEqual(row["source_row"], 2)
        self.assertEqual(row["source_record"], "external/observed.csv#csv-record=2")
        self.assertEqual((row["source_line_start"], row["source_line_end"]), (2, 2))
        self.assertEqual(result["coverage"]["state"], "unknown")
        self.assertIsNone(result["coverage"]["lost_records"])
        self.assertEqual(result["clock"]["anchors"], [])

    def test_exact_clock_conversion_and_fraction_refusal(self):
        text = "Observed Process,Observed Time,Observed Thread,Observed Count\n42,1.0000001,16,1\n"
        result = normalize(text, mapping(unit="s"))
        self.assertEqual(result["records"]["samples"][0]["timestamp_ticks"], "10000001")
        with self.assertRaises(SchemaError):
            normalize(text.replace("1.0000001", "0.00000001"), mapping(unit="s"))
        with self.assertRaises(SchemaError):
            normalize(text.replace("1.0000001", "18446744073709551616"))

    def test_observed_context_switch_intervals_and_explicit_exclusion(self):
        result = normalize("P,T,Begin,End,CPU,State\n42,19,1,2,3,On CPU\n42,19,2,3,3,Off CPU\n", mapping("context_switches"))
        self.assertEqual(result["records"]["context_switches"][0]["state"], "running")
        self.assertEqual(result["adapter_metadata"]["excluded_state_rows"], 1)
        with self.assertRaises(SchemaError):
            normalize("P,T,Begin,End,CPU,State\n42,19,2,1,3,On CPU\n", mapping("context_switches"))
        with self.assertRaises(SchemaError):
            normalize("P,T,Begin,End,CPU,State\n42,19,1,2,3,Unknown\n", mapping("context_switches"))

    def test_gpu_available_zero_differs_from_unavailable(self):
        text = "P,Time,Metric,Unit,Scope,Kind,Availability,Value\n42,1,Observed Metric,count,exported_row,sample,Collected,0\n42,2,Observed Metric,count,exported_row,sample,Missing,\n"
        rows = normalize(text, mapping("gpu_counters"))["records"]["gpu_counters"]
        self.assertEqual(rows[0]["value"], 0)
        self.assertEqual(rows[0]["status"], "measured")
        self.assertIsNone(rows[1]["value"])
        self.assertIsNone(rows[0]["debug_label"])
        self.assertNotIn("shader_sha256", rows[0])
        with self.assertRaises(SchemaError):
            normalize(text.replace("sample,Missing,", "sample,Missing,0"), mapping("gpu_counters"))
        with self.assertRaises(SchemaError):
            normalize(text.replace("sample,Collected,0", "sample,Collected,NaN"), mapping("gpu_counters"))

    def test_missing_timestamp_rejects_aggregated_gpu_table(self):
        with self.assertRaises(SchemaError):
            normalize("P,Metric,Unit,Scope,Kind,Availability,Value\n42,Observed Metric,count,exported_row,sample,Collected,4\n", mapping("gpu_counters"))

    def test_optional_label_and_shader_are_copied_only_from_columns(self):
        selected = mapping("gpu_counters")
        selected["columns"].update(debug_label="Actual Label", shader_sha256="Actual Hash", stage="Actual Stage")
        label = "aps5 run=11111111-1111-4111-8111-111111111111 capture=22222222-2222-4222-8222-222222222222 op=1 batch=2 pipeline=3 command=4"
        text = f"P,Time,Metric,Unit,Scope,Kind,Availability,Value,Actual Label,Actual Hash,Actual Stage\n42,1,Metric,count,exported_row,sample,Collected,4,{label},{'b' * 64},fragment\n"
        row = normalize(text, selected)["records"]["gpu_counters"][0]
        self.assertEqual(row["debug_label"], label)
        self.assertEqual(row["shader_sha256"], "b" * 64)
        self.assertEqual(row["stage"], "fragment")
        with self.assertRaises(SchemaError):
            normalize(text.replace("b" * 64, "not-a-hash"), selected)

    def test_stack_address_format_and_multiline_source_pointer(self):
        selected = mapping()
        selected["columns"].update(address="Instruction", symbol="Declared Symbol")
        selected["integer_formats"] = {"address": "hex"}
        text = 'Observed Process,Observed Time,Observed Thread,Observed Count,Instruction,Declared Symbol\r\n42,1,16,1,0x1000,"declared\r\nsymbol"\r\n'
        row = normalize(text, selected)["records"]["samples"][0]
        self.assertEqual(row["stack"][0]["address"], "4096")
        self.assertEqual(row["stack"][0]["symbol"], "declared\r\nsymbol")
        self.assertEqual((row["source_line_start"], row["source_line_end"]), (2, 3))

    def test_bom_and_semicolon_are_explicit(self):
        selected = mapping()
        selected["csv"].update(delimiter=";", encoding="utf-8-sig")
        text = "\ufeffObserved Process;Observed Time;Observed Thread;Observed Count\r\n42;1;16;1\r\n"
        self.assertEqual(len(normalize(text, selected)["records"]["samples"]), 1)
        selected["csv"]["encoding"] = "utf-8"
        with self.assertRaises(SchemaError):
            normalize(text, selected)

    def test_source_bytes_and_mapping_digest_are_reproducible(self):
        text = "Observed Process,Observed Time,Observed Thread,Observed Count\r\n42,1,16,1\r\n"
        selected = mapping()
        first = normalize(text, selected)
        self.assertEqual(first, normalize(text, copy.deepcopy(selected)))
        self.assertEqual(first["producer"]["mapping_sha256"], canonical_sha256(selected))
        changed = copy.deepcopy(selected)
        changed["tool"]["version"] = "test-2"
        self.assertNotEqual(first["producer"]["mapping_sha256"], normalize(text, changed)["producer"]["mapping_sha256"])
        raw = text.encode("utf-8")
        with self.assertRaises(SchemaError):
            normalize_exported_table(text.replace("\r\n", "\n"), selected, run_id=RUN_ID, process=PROCESS,
                source_artifact="external/observed.csv", source_sha256=hashlib.sha256(raw).hexdigest(), source_size=len(raw))

    def test_malformed_or_oversized_rows_reject(self):
        for text in ("Observed Process,Observed Time,Observed Thread,Observed Count\n42,1,16\n",
                     "Observed Process,Observed Time,Observed Thread,Observed Count\n42,1,16,1,extra\n",
                     "Observed Process,Observed Time,Observed Thread,Observed Thread\n42,1,16,1\n",
                     'Observed Process,Observed Time,Observed Thread,Observed Count\n42,1,16,"unclosed\n'):
            with self.subTest(text=text), self.assertRaises(SchemaError):
                normalize(text)
        text = "Observed Process,Observed Time,Observed Thread,Observed Count\n42,1,16,1\n42,2,16,1\n"
        with patch.object(capture_tabular, "MAX_TABLE_ROWS", 1), self.assertRaises(SchemaError):
            normalize(text)

    def test_unsupported_mapping_fields_and_types_reject(self):
        mutations = [lambda value: value.update(guess_headers=True),
                     lambda value: value["tool"].pop("version"),
                     lambda value: value["clock"].update(input_unit=[]),
                     lambda value: value["table"].update(record_kind={}),
                     lambda value: value["clock"].update(ticks_per_second=True),
                     lambda value: value["columns"].pop("timestamp"),
                     lambda value: value["columns"].update(symbol="Symbol")]
        for mutation in mutations:
            selected = mapping()
            mutation(selected)
            with self.subTest(mapping=selected), self.assertRaises(SchemaError):
                validate_mapping(selected)

    def test_unknown_process_identity_stays_unknown(self):
        result = normalize("Observed Process,Observed Time,Observed Thread,Observed Count\n42,1,16,1\n", process={"pid": 42})
        self.assertIsNone(result["process"]["creation_filetime"])
        self.assertIsNone(result["process"]["executable_sha256"])
        self.assertEqual(result["adapter_metadata"]["aggregation"], "unknown")

    def test_cli_rejects_prohibited_paths_and_binary_suffixes_before_read(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            for run_dir, source in ((root / "dump", "external/observed.csv"), (root, "external/program.prx")):
                with patch.object(capture_tabular, "_read_bounded") as read, contextlib.redirect_stderr(io.StringIO()):
                    status = capture_tabular.main(["--run-dir", str(run_dir), "--source", source,
                        "--mapping", "external/mapping.json", "--output", "external/normalized.json", "--run-id", RUN_ID, "--pid", "42"])
                    self.assertEqual(status, 2)
                    read.assert_not_called()

    def test_cli_preserves_source_and_saves_mapping_descriptor_without_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            external = root / "external"
            external.mkdir()
            source = b"Observed Process,Observed Time,Observed Thread,Observed Count\r\n42,1,16,1\r\n"
            selected_mapping = json.dumps(mapping(), indent=2).encode("utf-8")
            (external / "observed.csv").write_bytes(source)
            (external / "mapping.json").write_bytes(selected_mapping)
            args = ["--run-dir", str(root), "--source", "external/observed.csv", "--mapping", "external/mapping.json",
                    "--output", "external/normalized.json", "--run-id", RUN_ID, "--pid", "42"]
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(capture_tabular.main(args), 0)
            output = (external / "normalized.json").read_bytes()
            document = json.loads(output)
            self.assertEqual(document["mapping_artifact"], {"path": "external/mapping.json",
                "sha256": hashlib.sha256(selected_mapping).hexdigest(), "size": len(selected_mapping)})
            self.assertEqual((external / "observed.csv").read_bytes(), source)
            self.assertEqual((external / "mapping.json").read_bytes(), selected_mapping)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(capture_tabular.main(args), 2)
            self.assertEqual((external / "normalized.json").read_bytes(), output)


if __name__ == "__main__":
    unittest.main()
