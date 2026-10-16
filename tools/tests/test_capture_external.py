import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_external
from capture_schema import SchemaError, sha256_file
from test_capture_schema import make_manifest


class CaptureExternalTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        (self.root / "external").mkdir()
        self.manifest = make_manifest(self.root)
        self.manifest["execution"]["pid"] = 7
        self.text = "ProcessID,SwapChainAddress,PresentMode,CPUStartQPC,MsGPUTime,DisplayedTime\r\n7,0x10,Composed,123,2.5,NA\r\n8,0x20,Composed,124,3,1\r\n"
        self.process = {"pid": 7, "creation_filetime": "12345", "executable_sha256": "a" * 64}
        self.observation = {"status": "unknown", "run_id": self.manifest["run_id"],
            "process": {"pid": 7, "creation_filetime": "12345", "identity_binding": "retained_child_handle",
            "identity_verified": True, "executable": {"sha256": "a" * 64}},
            "clock": {"domain": "windows_qpc", "ticks_per_second": 10_000_000,
                      "anchors": [{"ticks": "100"}, {"ticks": "200"}]}}

    def normalize(self, **kwargs):
        return capture_external.normalize_presentmon_csv(self.text, run_id=self.manifest["run_id"], pid=7,
            tool_version="2.4", source_artifact="external/presentmon.csv", source_sha256=hashlib.sha256(self.text.encode()).hexdigest(),
            source_size=len(self.text.encode()), frequency=10_000_000, process=self.process, **kwargs)

    def attach(self, document):
        source, normalized = self.root / "external/presentmon.csv", self.root / "external/profile.json"
        source.write_bytes(self.text.encode())
        normalized.write_text(json.dumps(document), encoding="utf-8")
        self.manifest["artifacts"] = [{"kind": kind, "path": str(path.relative_to(self.root)).replace("\\", "/"),
            "size": path.stat().st_size, "sha256": sha256_file(path)} for kind, path in
            (("external_source", source), ("presentation_profile", normalized))]

    def test_documented_csv_preserves_missing_and_selects_pid(self):
        document = self.normalize()
        self.assertEqual(len(document["records"]["presentations"]), 1)
        self.assertIsNone(document["records"]["presentations"][0]["metrics"]["DisplayedTime"])
        self.assertEqual(document["records"]["presentations"][0]["metrics"]["MsGPUTime"], 2.5)
        self.assertEqual(document["adapter_metadata"]["excluded_other_pid_rows"], 1)

    def test_raw_qpc_needs_frequency(self):
        with self.assertRaises(SchemaError):
            capture_external.normalize_presentmon_csv(self.text, run_id=self.manifest["run_id"], pid=7, tool_version="2",
                source_artifact="external/p.csv", source_sha256="a" * 64, source_size=1)

    def test_duplicate_csv_header_rejects(self):
        self.text = self.text.replace("MsGPUTime", "CPUStartQPC")
        with self.assertRaises(SchemaError):
            self.normalize()

    def test_nonfinite_csv_metric_rejects(self):
        self.text = self.text.replace("2.5", "NaN")
        with self.assertRaises(SchemaError):
            self.normalize()

    def test_final_debug_label_binds_capture_uuid(self):
        capture = "11111111-1111-4111-8111-111111111111"
        label = f"aps5 run={self.manifest['run_id']} capture={capture} op=20 batch=45 pipeline=104 command=40"
        self.assertEqual(capture_external.parse_debug_label(label)["capture_id"], capture)
        self.assertIsNone(capture_external.parse_debug_label(label.replace(capture, "draw")))

    def test_source_path_cannot_escape_or_name_executable(self):
        for source in ("../raw.etl", "external/../raw.etl", "external/raw.exe", "external/fakelib/raw.csv"):
            document = self.normalize()
            document["source"]["artifact"] = source
            with self.subTest(source=source), self.assertRaises(SchemaError):
                capture_external.validate_external_document(document)

    def test_loader_reparses_preserved_csv(self):
        self.attach(self.normalize())
        result = capture_external.load_external_evidence(self.root, self.manifest, process_evidence=self.observation)
        self.assertEqual(result["status"], "valid")
        self.assertEqual(result["profiles"][0]["extraction_provenance"]["method"], "documented_presentmon_csv")

    def test_normalized_fabrication_is_rejected_even_with_new_hash(self):
        document = self.normalize()
        document["records"]["presentations"][0]["metrics"]["MsGPUTime"] = 0
        self.attach(document)
        self.assertEqual(capture_external.load_external_evidence(self.root, self.manifest, process_evidence=self.observation)["status"], "invalid")

    def test_missing_observed_process_is_unknown_not_exception(self):
        self.attach(self.normalize())
        result = capture_external.load_external_evidence(self.root, self.manifest, process_evidence={"process": None})
        self.assertEqual(result["status"], "unknown")

    def test_creation_time_mismatch_rejects_pid_reuse(self):
        self.attach(self.normalize())
        self.observation["process"]["creation_filetime"] = "54321"
        self.assertEqual(capture_external.load_external_evidence(self.root, self.manifest, process_evidence=self.observation)["status"], "invalid")

    def test_duplicate_record_ids_reject(self):
        document = self.normalize()
        document["records"]["presentations"].append(copy.deepcopy(document["records"]["presentations"][0]))
        with self.assertRaises(SchemaError):
            capture_external.validate_external_document(document)

    def test_source_hash_tamper_rejects(self):
        self.attach(self.normalize())
        (self.root / "external/presentmon.csv").write_bytes(b"corrupt")
        self.assertEqual(capture_external.load_external_evidence(self.root, self.manifest, process_evidence=self.observation)["status"], "invalid")

    def test_same_pid_profile_outside_observed_lifetime_stays_unknown(self):
        self.attach(self.normalize())
        self.observation["clock"]["anchors"] = [{"ticks": "200"}, {"ticks": "300"}]
        result = capture_external.load_external_evidence(self.root, self.manifest, process_evidence=self.observation)
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(result["profiles"][0]["process_binding"]["status"], "unknown")

    def test_oversized_or_malformed_csv_field_is_schema_error(self):
        self.text = self.text.replace("Composed", "x" * 140_000)
        with self.assertRaises(SchemaError):
            self.normalize()

    def test_explicit_table_reparse_preserves_unknown_original_extraction(self):
        from capture_tabular import normalize_exported_table
        from test_capture_tabular import mapping
        self.text = "P,Time,Metric,Unit,Scope,Kind,Availability,Value\r\n7,123,Invocations,count,command,counter,Collected,0\r\n"
        mapped = mapping("gpu_counters")
        raw = self.text.encode()
        document = normalize_exported_table(self.text, mapped, run_id=self.manifest["run_id"], process=self.process,
            source_artifact="external/presentmon.csv", source_sha256=hashlib.sha256(raw).hexdigest(), source_size=len(raw))
        mapping_path = self.root / "external/mapping.json"
        mapping_path.write_text(json.dumps(mapped), encoding="utf-8")
        reference = {"kind": "external_mapping", "path": "external/mapping.json", "size": mapping_path.stat().st_size,
                     "sha256": sha256_file(mapping_path)}
        document["mapping_artifact"] = {key: reference[key] for key in ("path", "size", "sha256")}
        self.attach(document)
        self.manifest["artifacts"][-1]["kind"] = "gpu_profile"
        self.manifest["artifacts"].append(reference)
        result = capture_external.load_external_evidence(self.root, self.manifest, process_evidence=self.observation)
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(result["profiles"][0]["extraction_provenance"]["transformation_status"], "valid")
        self.assertEqual(result["profiles"][0]["document"]["records"]["gpu_counters"][0]["value"], 0)

    def test_collector_table_source_and_mapping_are_independently_reparsed(self):
        from capture_tabular import normalize_exported_table
        from test_capture_tabular import mapping

        (self.root / "collectors").mkdir()
        text = "P,Time,Metric,Unit,Scope,Kind,Availability,Value\r\n7,123,Invocations,count,command,counter,Collected,0\r\n"
        selected = mapping("gpu_counters")
        raw = text.encode()
        document = normalize_exported_table(text, selected, run_id=self.manifest["run_id"], process=self.process,
            source_artifact="collectors/table.csv", source_sha256=hashlib.sha256(raw).hexdigest(), source_size=len(raw))
        artifacts = []
        for kind, relative, content in (("collector_source", "collectors/table.csv", raw),
                ("external_mapping", "collectors/mapping.json", json.dumps(selected).encode())):
            path = self.root / relative
            path.write_bytes(content)
            item = {"kind": kind, "path": relative, "size": len(content), "sha256": hashlib.sha256(content).hexdigest()}
            artifacts.append(item)
            if kind == "external_mapping":
                document["mapping_artifact"] = {key: item[key] for key in ("path", "size", "sha256")}
        def assess(value):
            content = json.dumps(value).encode()
            (self.root / "collectors/profile.json").write_bytes(content)
            self.manifest["artifacts"] = artifacts + [{"kind": "gpu_profile", "path": "collectors/profile.json",
                "size": len(content), "sha256": hashlib.sha256(content).hexdigest()}]
            return capture_external.load_external_evidence(self.root, self.manifest, process_evidence=self.observation)
        result = assess(document)
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(result["profiles"][0]["extraction_provenance"]["transformation_status"], "valid")
        document["records"]["gpu_counters"][0]["value"] = 5
        self.assertEqual(assess(document)["status"], "invalid")

    def test_identity_binding_survives_missing_clock_without_claiming_time_alignment(self):
        document = self.normalize()
        document["clock"] = {"domain": "unavailable_aggregated_shader_table", "ticks_per_second": 1, "anchors": []}
        binding = capture_external._binding(document, self.manifest, self.observation)
        self.assertEqual(binding["identity_status"], "valid")
        self.assertEqual(binding["status"], "unknown")
        self.observation["process"]["identity_verified"] = False
        self.assertEqual(capture_external._binding(document, self.manifest, self.observation)["identity_status"], "unknown")

    def test_symbol_names_require_observed_module_match_before_fresh_debug_assessment(self):
        receipt = {"schema_version": 1, "document_type": "anyps5.symbol-receipt",
                   "image": {"size": 100, "sha256": "b" * 64}, "function_ranges": []}
        path = self.root / "external/symbols.json"
        path.write_text(json.dumps(receipt), encoding="utf-8")
        descriptor = {"kind": "symbol_receipt", "path": "external/symbols.json", "size": path.stat().st_size, "sha256": sha256_file(path)}
        self.manifest["artifacts"] = [descriptor]
        verified = {descriptor["path"]: path}
        assessment = {"status": "valid", "binding": "embedded_dwarf_image_sha256", "image": receipt["image"],
            "function_ranges": [{"start_rva": "16", "end_rva": "32", "name": "ObservedHostFunction"}],
            "address_resolutions": [], "issues": [], "reason": "synthetic verified debug metadata"}
        with patch("capture_symbols.assess_symbol_receipt", return_value=assessment) as assess:
            unknown = capture_external._symbol_maps(self.root, self.manifest, verified,
                {"modules": [{"module_id": "other", "file_identity": {"size": 100, "sha256": "c" * 64}}]})
            assess.assert_not_called()
            self.assertEqual(unknown[0]["status"], "unknown")
            self.assertEqual(unknown[0]["ranges"], [])
            observed = capture_external._symbol_maps(self.root, self.manifest, verified,
                {"modules": [{"module_id": "actual", "file_identity": receipt["image"]}]})
            assess.assert_called_once_with(receipt, observed_image=receipt["image"])
            self.assertEqual(observed[0]["status"], "valid")
            self.assertEqual(observed[0]["ranges"], [{"start": 16, "end": 32, "name": "ObservedHostFunction"}])
            self.assertEqual(observed[0]["matched_observed_module_ids"], ["actual"])


if __name__ == "__main__":
    unittest.main()
