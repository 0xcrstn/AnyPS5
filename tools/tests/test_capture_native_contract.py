import argparse
import copy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_draws
import capture_evidence
import capture_recover
from capture_schema import read_json, validate_draw_trace, validate_evidence_report
from test_capture_schema import make_manifest


PRODUCER = None


class NativeSerializationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if PRODUCER is None:
            raise RuntimeError("--producer must identify the built synthetic native fixture")
        if PRODUCER.stem != "agc_draw_trace_contract_fixture":
            raise RuntimeError("only the synthetic agc_draw_trace_contract_fixture producer is allowed")
        cls.temporary = tempfile.TemporaryDirectory(prefix="capture-native-contract-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        result = subprocess.run([str(PRODUCER), str(cls.root)], capture_output=True, text=True, timeout=10, check=False)
        if result.returncode:
            raise RuntimeError(f"native fixture failed: {result.returncode}: {result.stderr[:1000]}")
        cls.document = read_json(next(cls.root.glob("aps5-draw-*.json")))
        cls.manifest = make_manifest(cls.root)
        cls.manifest["execution"]["pid"] = 7
        cls.manifest["capture"].update(start=10, length=2)
        cls.manifest["settings"]["diagnostics"]["options"]["trace_detail"] = "draw"
        cls.manifest["environment"]["effective"] = {
            "APS5_TRACE_DETAIL": "draw", "APS5_CAPTURE_RUN_ID": cls.manifest["run_id"], "APS5_TRACE_FRAMES": "10:2"}

    def assess(self):
        validate_draw_trace(self.document)
        return capture_draws.assess_draw_trace(copy.deepcopy(self.document), self.manifest,
            trace_name="aps5-trace-aaaaaaaaaaaa-7.json.gz",
            summary={"commit": "a" * 12, "dirty": False, "window": {"ms": 20}},
            trace={"presents": [(10, 0, 1), (11, 10_000, 1)]})

    def test_native_serialization_has_valid_complete_lifecycle_and_typed_intervals(self):
        detail = self.assess()
        self.assertEqual(detail["status"], "valid", detail.get("issues"))
        self.assertEqual(detail["coverage"]["boundaries"], "valid")
        self.assertEqual(len(detail["index"]["present_intervals"]), 2)
        self.assertEqual(self.document["clock"]["time_unit"], "ns")
        self.assertEqual(self.document["completion"]["event_count"], len(self.document["events"]))
        self.assertNotIn("traceEvents", self.document)
        self.assertFalse(self.document["coverage"]["perfetto_view_included"])
        native_span = next(event for event in self.document["events"] if event["kind"] == "span" and event["name"] == "Fixture.Draw")
        cycles = native_span["data"]
        if sys.platform == "win32":
            self.assertEqual(cycles["thread_cycles_status"], "measured")
            self.assertEqual(cycles["thread_cycles_begin"], str(1 << 60))
            self.assertEqual(cycles["thread_cycles_end"], str((1 << 60) + 63))
            self.assertEqual(cycles["thread_cycles_delta"], "63")
            self.assertEqual(cycles["thread_cycles_unit"], "cycles")
            self.assertEqual(cycles["thread_cycles_source"], "query_thread_cycle_time")
            self.assertGreater(int(cycles["thread_cycles_native_id"]), 0)
        else:
            self.assertEqual(cycles["thread_cycles_status"], "unavailable")
            self.assertIsNone(cycles["thread_cycles_begin"])
            self.assertIsNone(cycles["thread_cycles_end"])
            self.assertIsNone(cycles["thread_cycles_delta"])
        span = next(row for row in detail["index"]["spans"] if row["function"] == "Fixture.Draw")
        self.assertEqual(span["wall_ns"], 2_000_000)
        self.assertEqual(span["thread_cpu"]["cpu_ns"], 300)
        self.assertEqual(span["thread_cpu"]["native_thread_binding"], "valid")
        query = detail["index"]["queries"][0]
        self.assertEqual(query["duration_ns"], 50)
        self.assertEqual(query["duration_origin"], "integer_tick_delta")
        self.assertEqual(int(query["end_gpu_ticks"]) - int(query["begin_gpu_ticks"]), 20)
        self.assertEqual(query["association_status"], "valid")
        self.assertTrue(all(row["source"]["json_pointer"].startswith("/events/") for row in detail["index"]["host_commands"]))

    def test_native_counter_calibration_present_and_cpu_fields_feed_evidence(self):
        detail = self.assess()
        report = capture_evidence.assess_evidence(self.document, self.manifest, detail)
        validate_evidence_report(report)
        self.assertEqual(report["metrics"]["thread_cpu.Fixture.Draw.inclusive_ns"], 300)
        self.assertEqual(report["metric_units"]["thread_cpu.Fixture.Draw.inclusive_ns"], "ns")
        self.assertEqual(report["metrics"]["copies.copy_cpu_bytes"], 64)
        self.assertEqual(report["metric_units"]["copies.copy_cpu_bytes"], "bytes")
        self.assertEqual(report["hardware_counters"][0]["value"], 3)
        self.assertEqual(report["hardware_counters"][0]["unit"], "vertices")
        calibrations = [event for event in self.document["events"] if event["kind"] == "calibration"]
        self.assertEqual(len(calibrations), 2)
        self.assertEqual(calibrations[0]["data"]["calibration_gpu_ticks"], str(1 << 60))
        self.assertEqual(calibrations[0]["data"]["period_ns"], 2.5)
        self.assertTrue(report["frames"][0]["explicit_present_entity_ids"])
        self.assertTrue(report["frames"][1]["explicit_present_entity_ids"])
        self.assertEqual(report["dimensions"]["correctness"]["status"], "unknown")

    def test_native_live_prefix_and_final_journal_never_become_complete_capture(self):
        name = f"aps5-draw-{self.manifest['run_id']}-7.journal.jsonl"
        prefix = capture_recover.recover_journal(self.root / "prefix" / name, self.manifest)
        final = capture_recover.recover_journal(self.root / name, self.manifest)
        self.assertEqual(prefix["status"], "unknown", prefix["issues"])
        self.assertEqual(final["status"], "unknown", final["issues"])
        self.assertFalse(prefix["complete"])
        self.assertFalse(final["complete"])
        self.assertEqual(prefix["capture_eligibility"], "invalid")
        self.assertEqual(final["capture_eligibility"], "invalid")
        self.assertIsNone(prefix["footer"])
        self.assertIsNotNone(final["footer"])
        self.assertEqual(final["event_count"], len(self.document["events"]))
        self.assertEqual({row["event"]["id"] for row in final["retained_events"]}, {event["id"] for event in self.document["events"]})


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    PRODUCER = args.producer.resolve(strict=True)
    unittest.main(argv=[sys.argv[0], *remaining])
