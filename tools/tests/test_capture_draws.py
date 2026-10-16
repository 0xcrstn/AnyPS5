import copy
import io
import json
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_compare
import capture_draws
import capture_manifest
import capture_run
import capture_schema
from test_capture_schema import make_manifest


def event(identifier, kind, subject=None, *, name=None, related=None, start=1_000_000, end=None,
          thread="1", parent=None, status="complete", operation=None):
    context = {key: None for key in ("driver", "submission", "segment", "packet", "operation", "device",
                                    "batch", "job", "flip", "present")}
    context.update(operation=operation, queue=None, packet_offset=0, opcode=None)
    return {"id": str(identifier), "kind": kind, "thread_id": thread, "ts_ns": start, "end_ns": end,
            "subject": subject, "related": related, "name": name, "text": None, "value": 0,
            "status": status, "context": context, "parent_span": parent,
            "data": {"begin_gpu_ns": None, "end_gpu_ns": None, "slot": None, "shared": None}}


def recount(document):
    counts = Counter(entry["thread_id"] for entry in document["events"])
    document["threads"] = [{"thread_id": identifier, "name": f"thread-{identifier}", "attempted": count,
                            "emitted": count, "lost": 0, "registered_ns": min(0, min(row["ts_ns"] for row in document["events"]
                                                                                  if row["thread_id"] == identifier)),
                            "last_event_ns": max(max(row["ts_ns"], row["end_ns"] or row["ts_ns"])
                                                 for row in document["events"] if row["thread_id"] == identifier),
                            "acknowledged": True} for identifier, count in sorted(counts.items())]
    document["completion"]["event_count"] = len(document["events"])


def make_detail(root):
    manifest = make_manifest(root)
    manifest["execution"]["pid"] = 7
    manifest["capture"].update(start=10, length=2)
    manifest["settings"]["diagnostics"]["options"]["trace_detail"] = "draw"
    manifest["environment"]["effective"] = {"APS5_TRACE_DETAIL": "draw", "APS5_CAPTURE_RUN_ID": manifest["run_id"],
                                               "APS5_TRACE_FRAMES": "10:2"}
    trace_name = "aps5-trace-aaaaaaaaaaaa-7.json.gz"
    dictionary = []
    encodings = [("100", "guest_shader", "anyps5.guest-shader.v1", []),
                 ("101", "compiler_key", "anyps5.recompile-key.v1", []),
                 ("102", "spirv", "anyps5.final-spirv.v1", []),
                 ("103", "shader_variant", "anyps5.shader-variant.v1", ["100", "101", "102"]),
                 ("104", "pipeline", "anyps5.graphics-pipeline.v1", ["103"])]
    for identifier, kind, encoding, references in encodings:
        dictionary.append({"id": identifier, "kind": kind, "key": f"{int(identifier):064x}",
                           "fields": {"digest_algorithm": "sha256", "encoding": encoding, "stage": "vertex"},
                           "references": references})
    entries = [event(1, "packet", "10", name="draw_packet"),
               event(2, "operation", "20", name="draw", related="10", end=10_000_000, operation="20"),
               event(3, "span", "30", name="Graphics.Draw", start=2_000_000, end=10_000_000, operation="20"),
               event(4, "span", "31", name="DrawPipeline.LockWait", start=3_000_000, end=5_000_000,
                     parent="30", operation="20"),
               event(5, "span", "32", name="Async.Resolve", start=3_000_000, end=9_000_000,
                     thread="2", parent="30", operation="20"),
               event(6, "host_command", "40", name="vkCmdDraw", operation="20"),
               event(7, "batch", "45", name="submit"),
               event(8, "gpu_query", "50", related="45", status="pending", operation="20"),
               event(9, "gpu_query", "50", related="45", status="available", operation="20"),
               event(10, "link", "20", related="104", name="pipeline"),
               event(11, "link", "50", related="40", name="measures")]
    entries[5]["data"].update(requested_count=3, count_dynamic=False)
    entries[5]["context"]["batch"] = "45"
    entries[7]["data"].update(slot=0, shared=False)
    entries[8]["data"].update(begin_gpu_ns=100, end_gpu_ns=1100, slot=0, shared=False)
    begin_command = copy.deepcopy(entries[5])
    begin_command.update(id="99", status="unknown")
    entries.append(begin_command)
    document = {"schema_version": 1, "document_type": "anyps5.draw-trace", "run_id": manifest["run_id"],
                "capture_id": "11111111-1111-4111-8111-111111111111",
                "producer": {"pid": 7, "revision": "a" * 12, "dirty": False,
                             "coverage": "selected instrumented paths only"},
                "capture": {"unit": "frames", "start": 10, "length": 2, "state": "complete", "open_present": "10",
                            "close_present": "12", "open_ns": 0, "close_ns": 40_000_000,
                            "trace": trace_name, "drain_deadline_ms": 2000, "close_reason": "window_closed",
                            "pending_spans": 0, "pending_jobs": 0, "pending_gpu_queries": 0},
                "boundaries": [{"present": "10", "ts_ns": 0}, {"present": "11", "ts_ns": 20_000_000},
                               {"present": "12", "ts_ns": 40_000_000}],
                "threads": [], "dictionaries": dictionary, "events": entries,
                "losses": {"events": 0, "metadata": 0, "gpu_queries": 0, "unassigned_events": 0},
                "limitations": [], "completion": {"state": "complete", "event_count": len(entries), "output_complete": True}}
    recount(document)
    summary = {"commit": "a" * 12, "dirty": False, "window": {"ms": 40}}
    trace = {"presents": [(10, 0, 1), (11, 20_000, 1)]}
    return manifest, document, trace_name, summary, trace


class CaptureDrawTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.manifest, self.document, self.trace_name, self.summary, self.trace = make_detail(self.root)

    def assess(self, **kwargs):
        return capture_draws.assess_draw_trace(self.document, self.manifest, trace_name=self.trace_name,
                                              summary=self.summary, trace=self.trace, **kwargs)

    def declare_event_loss(self, count=1):
        self.document["capture"]["state"] = "partial"
        self.document["completion"]["state"] = "partial"
        self.document["losses"].update(events=count, unassigned_events=count)

    def precise_query(self, first, last, *, period=0.5, wraps=False, duration=None):
        delta = (last - first) & (2 ** 64 - 1)
        duration = delta * period if duration is None else duration
        beginning = float(first) * period
        self.document["events"][8]["data"].update(
            begin_gpu_ns=beginning, end_gpu_ns=beginning + duration,
            begin_gpu_ticks=str(first), end_gpu_ticks=str(last), period_ns=period,
            duration_ns=duration, duration_origin="integer_tick_delta", absolute_time_precision="approximate_binary64",
            timestamp_valid_bits=64, timestamp_wraps=wraps)

    def test_capacity_boundaries_do_not_require_allocating_large_fixtures(self):
        capture_draws.validate_draw_capacity(8_388_608, 262_144)
        capture_draws.validate_draw_capacity(8_388_608, 262_144, view_count=8_388_608 + 1024)
        with self.assertRaises(capture_schema.SchemaError):
            capture_draws.validate_draw_capacity(0, 0, view_count=8_388_608 + 1025)
        for events, dictionaries in ((8_388_609, 0), (0, 262_145), (-1, 0), (True, 0)):
            with self.subTest(events=events, dictionaries=dictionaries):
                with self.assertRaises(capture_schema.SchemaError):
                    capture_draws.validate_draw_capacity(events, dictionaries)

    def test_incremental_reader_discards_only_duplicate_view_and_preserves_canonical_sources(self):
        document = copy.deepcopy(self.document)
        document["traceEvents"] = [{"name": "duplicate view", "args": {"quoted": "brackets ]} and \\"}}]
        path = self.root / "detail.json"
        path.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
        loaded = capture_draws.read_draw_trace(path)
        self.assertNotIn("traceEvents", loaded)
        self.assertEqual(loaded, self.document)
        report = capture_draws.assess_draw_trace(loaded, self.manifest, trace_name=self.trace_name,
                                               summary=self.summary, trace=self.trace)
        self.assertNotIn("events", report["index"])
        self.assertEqual(report["index"]["event_source"]["canonical_event_count"], len(document["events"]))
        self.assertEqual(report["index"]["draws"][0]["source_events"][0]["json_pointer"], "/events/1")

    def test_incremental_reader_validates_discarded_view_keys_numbers_and_trailing_content(self):
        for payload in ('{"events":[],"dictionaries":[],"traceEvents":[{"x":1,"x":2}]}',
                        '{"events":[],"dictionaries":[],"traceEvents":[NaN]}',
                        '{"events":[],"dictionaries":[],"traceEvents":[1e9999]}',
                        '{"events":[],"events":[],"traceEvents":[]}',
                        '{"events":[],"traceEvents":[1,]}',
                        '{"events":[],"traceEvents":[]} false'):
            with self.subTest(payload=payload):
                with self.assertRaises(capture_schema.SchemaError):
                    capture_draws._DrawJsonReader(io.StringIO(payload)).document()

    def test_incremental_reader_handles_split_numeric_tokens_and_bounds_file_and_items(self):
        class SmallChunks(io.StringIO):
            def read(self, size=-1):
                return super().read(min(size, 1))

        loaded = capture_draws._DrawJsonReader(SmallChunks('{"value":1.25e+2,"events":[],"traceEvents":[1e2]}')).document()
        self.assertEqual(loaded["value"], 125)
        path = self.root / "detail.json"
        path.write_text('{"events":[]}', encoding="utf-8")
        with patch.object(capture_draws, "MAX_DRAW_FILE_BYTES", 8):
            with self.assertRaises(capture_schema.SchemaError):
                capture_draws.read_draw_trace(path)
        with patch.object(capture_draws, "MAX_DRAW_VALUE_CHARS", 8):
            with self.assertRaises(capture_schema.SchemaError):
                capture_draws._DrawJsonReader(io.StringIO('{"value":"123456789"}')).document()

    def test_streamed_atomic_json_preserves_canonical_utf8_and_exclusive_publication(self):
        value = {"z": [1, None, True], "label": "draw \u03bb"}
        expected = capture_schema.canonical_json_bytes(value) + b"\n"
        path = self.root / "report.json"
        with patch.object(capture_schema, "canonical_json_bytes", side_effect=AssertionError("whole document copy")):
            capture_schema.atomic_write_json(path, value, replace=False)
        self.assertEqual(path.read_bytes(), expected)
        with self.assertRaises(FileExistsError):
            capture_schema.atomic_write_json(path, {}, replace=False)
        self.assertEqual(path.read_bytes(), expected)

    def test_event_loss_keeps_safe_index_and_observed_counts_without_complete_totals(self):
        self.declare_event_loss(2)
        result = self.assess()
        self.assertEqual(result["status"], "invalid")
        self.assertEqual(result["coverage"]["lost_event_count"], 2)
        self.assertEqual(result["coverage"]["attempted_event_count"], len(self.document["events"]) + 2)
        self.assertEqual(result["observed_counts"]["values"]["host_draw_commands"], 1)
        self.assertEqual(result["metrics"]["workload"], {})
        self.assertEqual(result["metrics"]["functions"], {})
        self.assertEqual(result["metrics"]["gpu"], {})
        self.assertTrue(all(row["self_ns"] is None for row in result["index"]["spans"]))
        self.assertEqual(result["index"]["spans"][0]["wall_ns"], 8_000_000)
        result["artifact"] = {"path": "detail.json"}
        report = capture_draws.report_from_health(self.root, {"run_id": self.manifest["run_id"],
                    "status": "invalid", "draw_attribution": result, "issues": result["issues"]})
        self.assertIn("unavailable", capture_draws.text_report(report))
        self.assertEqual(capture_draws.text_report(report), "".join(capture_draws.iter_text_report(report)))

    def test_missing_host_command_pairs_require_declared_loss_and_never_invent_elapsed_time(self):
        original = copy.deepcopy(self.document)
        for missing in ("unknown", "complete"):
            with self.subTest(missing=missing):
                self.document = copy.deepcopy(original)
                self.document["events"] = [e for e in self.document["events"]
                                           if not (e["kind"] == "host_command" and e["status"] == missing)]
                recount(self.document)
                with self.assertRaises(capture_schema.SchemaError):
                    self.assess()
                self.declare_event_loss()
                result = self.assess()
                command = result["index"]["host_commands"][0]
                self.assertEqual(result["status"], "invalid")
                self.assertEqual(result["workload_status"], "unknown")
                self.assertEqual(result["metrics"]["workload"], {})
                self.assertEqual(command["lifecycle_status"], "unknown")
                for key in ("recording_begin_ns", "recording_end_ns", "elapsed_ns"):
                    self.assertIsNone(command[key])
                self.assertEqual(len(command["source_events"]), 1)

    def test_declared_loss_does_not_waive_duplicate_or_conflicting_command_records(self):
        original = copy.deepcopy(self.document)
        for mutation in ("begin", "terminal", "descriptor"):
            with self.subTest(mutation=mutation):
                self.document = copy.deepcopy(original)
                if mutation == "descriptor":
                    self.document["events"][5]["data"]["requested_count"] += 1
                else:
                    entry = copy.deepcopy(self.document["events"][-1 if mutation == "begin" else 5])
                    entry["id"] = "999"
                    self.document["events"].append(entry)
                recount(self.document)
                self.declare_event_loss()
                with self.assertRaises(capture_schema.SchemaError):
                    self.assess()

    def test_partial_query_lifecycles_keep_measurements_and_unknown_missing_records(self):
        original = copy.deepcopy(self.document)
        for missing in ("pending", "available"):
            with self.subTest(missing=missing):
                self.document = copy.deepcopy(original)
                self.document["events"] = [e for e in self.document["events"]
                                           if not (e["kind"] == "gpu_query" and e["status"] == missing)]
                recount(self.document)
                self.declare_event_loss()
                result = self.assess()
                query = result["index"]["queries"][0]
                self.assertEqual(query["lifecycle_status"], "unknown")
                self.assertEqual(query["association_status"], "unknown")
                self.assertEqual(query["duration_ns"], 1000 if missing == "pending" else None)
                self.assertIsNone(result["index"]["draws"][0]["gpu_interval_sum_ns"])

    def test_raw_tick_delta_preserves_small_intervals_at_large_absolute_timestamps(self):
        for ticks in (1, 63):
            with self.subTest(ticks=ticks):
                self.precise_query(1_792_000_000_000_000_000, 1_792_000_000_000_000_000 + ticks)
                data = self.document["events"][8]["data"]
                self.assertEqual(data["begin_gpu_ns"], data["end_gpu_ns"])
                query = self.assess()["index"]["queries"][0]
                self.assertEqual(query["duration_ns"], ticks * 0.5)
                self.assertEqual(query["duration_origin"], "integer_tick_delta")
                self.assertEqual(query["absolute_time_precision"], "approximate_binary64")

    def test_raw_tick_wrap_zero_and_contradictions_are_distinguished(self):
        self.precise_query(2 ** 64 - 2, 1, wraps=True)
        self.assertEqual(self.assess()["index"]["queries"][0]["duration_ns"], 1.5)
        self.precise_query(10, 10)
        self.assertEqual(self.assess()["index"]["queries"][0]["duration_ns"], 0)
        for first, last, wraps, duration in ((140, 100, True, 0), (0, 2 ** 63, False, 0),
                                            (10, 11, True, 0.5), (10, 11, False, 2)):
            with self.subTest(first=first, last=last, wraps=wraps, duration=duration):
                self.precise_query(first, last, wraps=wraps, duration=duration)
                with self.assertRaises(capture_schema.SchemaError):
                    self.assess()

    def test_draw_shader_function_query_index_and_synchronous_self_time(self):
        report = self.assess()
        self.assertEqual(report["status"], "valid", report["issues"])
        draw = report["index"]["draws"][0]
        self.assertEqual(draw["operation_id"], "20")
        self.assertEqual(draw["pipeline_ids"], ["104"])
        self.assertIn("103", draw["shader_ids"])
        parent = next(row for row in report["index"]["spans"] if row["span_id"] == "30")
        self.assertEqual(parent["wall_ns"], 8_000_000)
        self.assertEqual(parent["self_ns"], 6_000_000)
        self.assertEqual(report["metrics"]["functions"]["DrawPipeline.LockWait.wait_ms"], 2)
        self.assertEqual(draw["gpu_interval_sum_ns"], 1000)
        self.assertEqual(parent["time_window_membership"]["overlap_interval_range"], [0, 0])
        self.assertEqual(len(report["index"]["present_intervals"]), 2)
        self.assertIsNone(report["index"]["gpu_alignment"]["time_window_membership"])
        self.assertEqual(draw["source_events"][0], {"event_id": "2", "json_pointer": "/events/1"})
        self.document["events"][3]["name"] = "Driver.DrawGpuMutexWait"
        classified = self.assess()
        wait = next(row for row in classified["index"]["spans"] if row["span_id"] == "31")
        self.assertEqual(wait["classification"], "wait")
        self.assertEqual(classified["metrics"]["functions"]["Driver.DrawGpuMutexWait.wait_ms"], 2)

    def test_run_process_trace_window_and_revision_are_bound(self):
        cases = [("run_id", "22222222-2222-4222-8222-222222222222"), ("producer", {**self.document["producer"], "pid": 8}),
                 ("capture", {**self.document["capture"], "trace": "different.json.gz"}),
                 ("capture", {**self.document["capture"], "start": 11}),
                 ("producer", {**self.document["producer"], "revision": "b" * 12})]
        for key, value in cases:
            with self.subTest(key=key):
                original = self.document[key]
                self.document[key] = value
                with self.assertRaises(capture_schema.SchemaError):
                    self.assess()
                self.document[key] = original

    def test_boundaries_include_final_interval_and_match_legacy_instants(self):
        for change in ("missing", "duration", "present"):
            with self.subTest(change=change):
                original = copy.deepcopy(self.document["boundaries"])
                if change == "missing":
                    self.document["boundaries"].pop()
                elif change == "duration":
                    self.document["boundaries"][-1]["ts_ns"] += 10_000
                else:
                    self.document["boundaries"][1]["present"] = "99"
                with self.assertRaises(capture_schema.SchemaError):
                    self.assess()
                self.document["boundaries"] = original

    def test_thread_accounting_and_duplicate_records_are_not_trusted(self):
        self.document["threads"][0]["emitted"] += 1
        with self.assertRaises(capture_schema.SchemaError):
            self.assess()
        recount(self.document)
        self.document["events"].append(copy.deepcopy(self.document["events"][0]))
        recount(self.document)
        with self.assertRaises(capture_schema.SchemaError):
            self.assess()

    def test_explicit_unassigned_loss_is_partial_evidence_not_invented_thread_loss(self):
        self.document["capture"]["state"] = "partial"
        self.document["completion"]["state"] = "partial"
        self.document["losses"].update(events=2, unassigned_events=2)
        report = self.assess()
        self.assertEqual(report["status"], "invalid")
        self.assertTrue(any(item["code"] == "draw_detail_loss" for item in report["issues"]))
        self.assertTrue(all(thread["lost"] == 0 for thread in self.document["threads"]))

    def test_span_parent_cycles_and_wrong_types_are_invalid(self):
        parent, child = self.document["events"][2:4]
        parent["parent_span"] = "31"
        child.update(ts_ns=parent["ts_ns"], end_ns=parent["end_ns"])
        with self.assertRaisesRegex(capture_schema.SchemaError, "cycle"):
            self.assess()
        parent["parent_span"] = "20"
        child["parent_span"] = None
        with self.assertRaisesRegex(capture_schema.SchemaError, "non-span"):
            self.assess()

    def test_external_origin_does_not_erase_shader_or_elapsed_measurements(self):
        self.document["events"][2]["parent_span"] = "900"
        report = self.assess()
        self.assertEqual(report["origin_graph_status"], "unknown")
        self.assertEqual(report["identity_status"], "valid")
        self.assertEqual(report["cpu_status"], "valid")
        self.assertEqual(report["status"], "valid")
        self.assertEqual(report["index"]["external_references"][0]["id"], "900")

    def test_references_to_known_entities_and_declared_context_are_resolved(self):
        known = event(12, "reference", "30", name="span", status="unknown")
        known["text"] = "outside_window_or_uninstrumented"
        declared = event(13, "reference", "901", name="driver")
        declared["text"] = "identity_only_no_lifecycle"
        self.document["events"].extend([known, declared])
        self.document["events"][2]["context"]["driver"] = "901"
        recount(self.document)
        report = self.assess()
        self.assertEqual(report["origin_graph_status"], "valid")
        self.assertEqual(report["index"]["external_references"], [])

    def test_shader_dictionary_graph_hash_shape_and_key_conflicts(self):
        for change in ("dangling", "hash", "conflict"):
            with self.subTest(change=change):
                original = copy.deepcopy(self.document["dictionaries"])
                if change == "dangling":
                    self.document["dictionaries"][-1]["references"] = ["999"]
                elif change == "hash":
                    self.document["dictionaries"][-1]["key"] = "not-a-sha256"
                else:
                    duplicate = copy.deepcopy(self.document["dictionaries"][-1])
                    duplicate.update(id="105", references=["100"])
                    self.document["dictionaries"].append(duplicate)
                with self.assertRaises(capture_schema.SchemaError):
                    self.assess()
                self.document["dictionaries"] = original

    def test_zero_gpu_duration_is_measured_but_unavailable_is_not_zero(self):
        query = self.document["events"][8]
        query["data"]["end_gpu_ns"] = query["data"]["begin_gpu_ns"]
        report = self.assess()
        self.assertEqual(report["index"]["queries"][0]["duration_ns"], 0)
        self.assertEqual(report["gpu_status"], "valid")
        query["status"] = "unsupported"
        query["data"].update(begin_gpu_ns=None, end_gpu_ns=None)
        report = self.assess()
        self.assertEqual(report["gpu_status"], "unknown")
        self.assertIsNone(report["index"]["queries"][0]["duration_ns"])
        self.assertEqual(report["metrics"]["gpu"], {})

    def test_gpu_failures_and_fabricated_unavailable_timestamps(self):
        query = self.document["events"][8]
        query["status"] = "read_failed"
        query["data"].update(begin_gpu_ns=None, end_gpu_ns=None)
        self.assertEqual(self.assess()["gpu_status"], "invalid")
        query["data"]["begin_gpu_ns"] = 0
        with self.assertRaises(capture_schema.SchemaError):
            self.assess()

    def test_gpu_query_lifecycle_cannot_change_batch_or_operation(self):
        self.document["events"][8]["related"] = "10"
        with self.assertRaisesRegex(capture_schema.SchemaError, "immutable"):
            self.assess()

    def test_status_enums_are_specific_to_record_kind(self):
        self.document["events"][2]["status"] = "available"
        with self.assertRaises(capture_schema.SchemaError):
            self.assess()
        self.document["events"][2]["status"] = "complete"
        self.document["events"][8]["status"] = "complete"
        with self.assertRaises(capture_schema.SchemaError):
            self.assess()

    def test_shared_presentation_path_is_not_a_host_draw_range(self):
        present = event(200, "present", "200", name="present")
        self.document["events"].append(present)
        for query in self.document["events"][7:9]:
            query["name"] = "present_blit"
            query["data"]["shared"] = True
            query["context"]["present"] = "200"
        self.document["events"][10].update(related="200", name="presentation_path")
        recount(self.document)
        result = self.assess()
        self.assertEqual(result["gpu_status"], "valid")
        self.assertEqual(result["gpu_association_status"], "unknown")
        self.assertIsNone(result["index"]["draws"][0]["gpu_interval_sum_ns"])

    def test_idle_clip_does_not_erase_other_complete_function_totals(self):
        idle = event(201, "span", "201", name="Idle.Wait", start=0, end=40_000_000, status="unknown")
        idle["text"] = "clipped_at_window_close"
        self.document["events"].append(idle)
        recount(self.document)
        result = self.assess()
        self.assertEqual(result["cpu_status"], "unknown")
        self.assertEqual(result["function_availability"]["Graphics.Draw"], "valid")
        self.assertEqual(result["function_availability"]["Idle.Wait"], "unknown")
        self.assertNotIn("Idle.Wait.wall_ms", result["metrics"]["functions"])
        self.assertIn("draw.functions.Graphics.Draw.wall_ms", capture_compare._draw_metrics({"draw_attribution": result}))

    def test_gpu_only_partial_keeps_closed_cpu_function_measurements(self):
        self.document["capture"]["state"] = "partial"
        self.document["completion"]["state"] = "partial"
        for query in self.document["events"][7:9]:
            if query["status"] == "available":
                query["status"] = "unsupported"
                query["data"].update(begin_gpu_ns=None, end_gpu_ns=None)
        result = self.assess()
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(result["cpu_status"], "valid")
        self.assertEqual(result["function_availability"]["Graphics.Draw"], "valid")
        self.assertIn("Graphics.Draw.wall_ms", result["metrics"]["functions"])

    def test_clipped_span_is_not_complete_function_timing(self):
        span = self.document["events"][2]
        span["status"] = "unknown"
        span["text"] = "clipped_at_window_close"
        self.assertEqual(self.assess()["cpu_status"], "unknown")
        span["status"] = "complete"
        span["text"] = None
        span["ts_ns"] = -1_000_000
        recount(self.document)
        report = self.assess()
        self.assertNotIn("Graphics.Draw.wall_ms", report["metrics"]["functions"])
        self.assertTrue(next(row for row in report["index"]["spans"] if row["span_id"] == "30")["boundary_crossing"])

    def test_terminal_operation_state_survives_later_unknown_progress_record(self):
        self.document["events"].append(event(12, "operation", "20", name="draw", related="10",
                                              start=11_000_000, status="unknown", operation="20"))
        recount(self.document)
        self.assertEqual(self.assess()["index"]["draws"][0]["status"], "complete")

    def test_time_membership_is_overlap_not_frame_ownership(self):
        value = capture_draws._time_membership([0, 20, 40, 60], 15, 45)
        self.assertEqual(value["start_interval"], 0)
        self.assertEqual(value["overlap_interval_range"], [0, 2])
        self.assertIn("not presented-frame ownership", value["meaning"])
        self.assertIsNone(capture_draws._time_membership([0, 20, 40], 40, 40)["start_interval"])

    def test_comparison_uses_only_independently_valid_dimensions(self):
        detail = self.assess()
        detail["gpu_status"] = "unknown"
        metrics = capture_compare._draw_metrics({"draw_attribution": detail})
        self.assertTrue(any(name.startswith("draw.functions.") for name in metrics))
        self.assertFalse(any(name.startswith("draw.gpu.") for name in metrics))
        detail["origin_graph_status"] = "unknown"
        self.assertEqual(capture_compare._draw_metrics({"draw_attribution": detail}), metrics)
        detail["correlation_valid"] = False
        self.assertEqual(capture_compare._draw_metrics({"draw_attribution": detail}), {})

    def test_runner_uuid_binding_and_explicit_detail_configuration(self):
        config = {"settings": self.manifest["settings"], "capture": self.manifest["capture"],
                  "environment": {"overrides": {"APS5_PROFILE_DRAW": "1"}}}
        child, metadata = capture_run.child_environment(config, self.root / "traces", {}, run_id=self.manifest["run_id"])
        self.assertEqual(child["APS5_CAPTURE_RUN_ID"], self.manifest["run_id"])
        self.assertEqual(child["APS5_TRACE_DETAIL"], "draw")
        self.assertNotIn("APS5_PROFILE_GPU", child)
        self.assertEqual(metadata["effective"]["APS5_CAPTURE_RUN_ID"], self.manifest["run_id"])
        config["environment"]["overrides"]["aps5_capture_run_id"] = "forged"
        with self.assertRaises(ValueError):
            capture_run.child_environment(config, self.root / "traces", {}, run_id=self.manifest["run_id"])

    def test_schema_and_report_keep_exact_source_ids_and_explicit_limits(self):
        detail = self.assess()
        detail["artifact"] = {"kind": "draw_trace", "path": "traces/detail.json", "sha256": "a" * 64, "size": 1}
        report = {"schema_version": 1, "document_type": "anyps5.draw-report", "run_id": self.manifest["run_id"],
                  "status": "valid", "capture_health_status": "unknown", "detail": detail,
                  "issues": [], "limitations": detail["limitations"], "source_directory": str(self.root)}
        capture_schema.validate_draw_report(report)
        text = capture_draws.text_report(report)
        self.assertIn("#/events/1", text)
        self.assertIn("Graphics.Draw", text)
        self.assertIn("GPU interval membership is unavailable", text)
        self.assertIn("not CPU usage", text)
        json.dumps(report, allow_nan=False)

    def test_thread_cpu_counter_delta_preserves_measured_zero_and_native_thread(self):
        span = self.document["events"][2]
        span["data"].update(thread_cpu_status="measured", thread_cpu_begin_ticks=str(2 ** 60),
                            thread_cpu_end_ticks=str(2 ** 60), thread_cpu_frequency="10000000",
                            thread_cpu_native_id="77", thread_cpu_ns=0)
        self.document["threads"][0].update(native_thread_id="77", native_thread_id_source="windows_thread_id")
        result = self.assess()
        scope = next(row for row in result["index"]["spans"] if row["span_id"] == "30")
        self.assertEqual(scope["thread_cpu"]["cpu_ns"], 0)
        self.assertEqual(scope["thread_cpu"]["native_thread_binding"], "valid")

    def test_thread_cpu_unavailable_never_becomes_zero(self):
        self.document["events"][2]["data"]["thread_cpu_status"] = "unavailable"
        result = self.assess()
        scope = next(row for row in result["index"]["spans"] if row["span_id"] == "30")
        self.assertIsNone(scope["thread_cpu"]["cpu_ns"])

    def test_thread_cpu_rejects_changed_thread_or_fabricated_duration(self):
        span = self.document["events"][2]
        span["data"].update(thread_cpu_status="measured", thread_cpu_begin_ticks="10", thread_cpu_end_ticks="13",
                            thread_cpu_frequency="10000000", thread_cpu_native_id="77", thread_cpu_ns=300)
        self.document["threads"][0].update(native_thread_id="78", native_thread_id_source="windows_thread_id")
        with self.assertRaises(ValueError):
            self.assess()
        self.document["threads"][0]["native_thread_id"] = "77"
        span["data"]["thread_cpu_ns"] = 0
        with self.assertRaises(ValueError):
            self.assess()

    def test_query_cannot_measure_other_batch_same_operation(self):
        for entry in self.document["events"]:
            if entry["kind"] == "host_command":
                entry["context"]["batch"] = "999"
        with self.assertRaises(ValueError):
            self.assess()

    def test_precise_query_rejects_contradictory_absolute_coordinates(self):
        self.precise_query(100, 110)
        self.document["events"][8]["data"].update(begin_gpu_ns=10_000, end_gpu_ns=10_005)
        with self.assertRaises(ValueError):
            self.assess()

    def test_complete_noop_attempts_do_not_require_materialized_shader_identity_for_counts(self):
        for offset, name, status in ((0, "draw", "skipped"), (10, "dispatch", "complete"), (20, "draw", "complete")):
            identifier = str(700 + offset)
            self.document["events"].extend([
                event(700 + offset, "operation", identifier, name=name, operation=identifier, status="unknown"),
                event(701 + offset, "operation", identifier, name=name, operation=identifier, status=status)])
        recount(self.document)
        result = self.assess()
        self.assertEqual(result["identity_status"], "unknown")
        self.assertEqual(result["metrics"]["shaders"], {})
        self.assertEqual(result["workload_status"], "valid")
        self.assertEqual(result["metrics"]["workload"]["requested_draw_operations"], 3)
        self.assertEqual(result["metrics"]["workload"]["dispatch_operations"], 1)
        self.assertEqual(result["metrics"]["workload"]["host_draw_commands"], 1)

    def test_missing_operation_begin_or_terminal_keeps_workload_unavailable(self):
        original = copy.deepcopy(self.document)
        for status in ("unknown", "complete"):
            with self.subTest(status=status):
                self.document = copy.deepcopy(original)
                self.document["events"].append(event(700, "operation", "700", name="draw", operation="700", status=status))
                recount(self.document)
                result = self.assess()
                self.assertEqual(result["workload_status"], "unknown")
                self.assertEqual(result["metrics"]["workload"], {})
                self.assertEqual(result["coverage"]["lifecycle"]["operations"]["incomplete"], 1)

    def test_unknown_progress_after_terminal_keeps_observed_outcome_and_valid_counts(self):
        self.document["events"].extend([
            event(700, "operation", "700", name="dispatch", operation="700", status="complete"),
            event(701, "operation", "700", name="dispatch", operation="700", status="unknown"),
            event(702, "operation", "700", name="dispatch", operation="700", status="unknown")])
        recount(self.document)
        result = self.assess()
        operation = next(row for row in result["index"]["draws"] if row["operation_id"] == "700")
        self.assertEqual(operation["status"], "complete")
        self.assertEqual(result["workload_status"], "valid")

    def test_complete_command_with_external_operation_cannot_qualify_workload(self):
        for entry in self.document["events"]:
            if entry["kind"] in ("host_command", "gpu_query"):
                entry["context"]["operation"] = "999"
        result = self.assess()
        self.assertEqual(result["workload_status"], "unknown")
        self.assertEqual(result["metrics"]["workload"], {})

    def test_failed_host_command_keeps_known_records_without_complete_workload_metrics(self):
        for entry in self.document["events"]:
            if entry["kind"] == "host_command" and entry["status"] == "complete":
                entry["status"] = "failed"
        result = self.assess()
        self.assertEqual(result["observed_counts"]["values"]["host_draw_commands"], 1)
        self.assertEqual(result["workload_status"], "unknown")
        self.assertEqual(result["metrics"]["workload"], {})

    def test_window_loss_pending_or_missing_ack_prevents_workload_totals(self):
        original = copy.deepcopy(self.document)
        for mutation in ("loss", "metadata_loss", "pending", "unacknowledged", "window"):
            with self.subTest(mutation=mutation):
                self.document = copy.deepcopy(original)
                if mutation == "loss":
                    self.declare_event_loss()
                elif mutation == "metadata_loss":
                    self.document["capture"]["state"] = "partial"
                    self.document["completion"]["state"] = "partial"
                    self.document["losses"]["metadata"] = 1
                elif mutation == "pending":
                    self.document["capture"].update(state="partial", pending_jobs=1)
                    self.document["completion"]["state"] = "partial"
                elif mutation == "unacknowledged":
                    self.document["threads"][0]["acknowledged"] = False
                else:
                    self.document["capture"].update(state="partial", close_reason="shutdown")
                    self.document["completion"]["state"] = "partial"
                result = self.assess()
                self.assertEqual(result["workload_status"], "unknown")
                self.assertEqual(result["metrics"]["workload"], {})


    def test_compact_assessment_preserves_metrics_without_span_index_rows(self):
        full = self.assess()
        transient = {}
        compact = self.assess(include_index=False, analysis_index=transient)
        self.assertNotIn("index", compact)
        self.assertEqual(transient["spans"], [])
        self.assertEqual(compact["metrics"], full["metrics"])
        self.assertEqual(compact["function_availability"], full["function_availability"])
        self.assertEqual(compact["summary"], full["summary"])
        self.assertEqual(compact["event_source"]["json_pointer_template"], "/events/{index}")
        self.assertTrue(compact["summary"]["source_examples"][0]["source"]["json_pointer"].startswith("/events/"))
        self.assertTrue(transient["draws"][0]["source_events"][0]["json_pointer"].startswith("/events/"))

    def test_compact_activity_keeps_explicit_wait_reason_classification(self):
        note = event(500, "attribute", "30", name="wait_reason")
        note["text"] = "selected_wait"
        self.document["events"].append(note)
        recount(self.document)
        compact = self.assess(include_index=False)
        full = self.assess()
        self.assertEqual(compact["summary"], full["summary"])
        self.assertEqual(compact["summary"]["frames"][0]["named_wait_union_ns_by_thread"]["1"], 8_000_000)

    def test_compact_frame_summary_bounds_omissions_explicitly(self):
        boundaries = [{"present": str(index), "ts_ns": index * 10} for index in range(1027)]
        summary = capture_draws._activity_summary({"1": [(0, 20)]}, {}, boundaries)
        self.assertEqual(len(summary["frames"]), 1024)
        self.assertEqual(summary["frames_omitted"], 2)
        self.assertEqual(summary["frames"][0]["host_scope_union_ns"], 10)
        self.assertEqual(summary["frames"][-1]["host_scope_uncovered_ns"], 10)

    def test_thread_cycles_preserve_small_delta_and_have_no_time_conversion(self):
        span = self.document["events"][2]
        self.document["threads"][0].update(native_thread_id="77", native_thread_id_source="windows_thread_id")
        span["data"].update(thread_cycles_status="measured", thread_cycles_begin=str(2 ** 60),
            thread_cycles_end=str(2 ** 60 + 63), thread_cycles_delta="63", thread_cycles_native_id="77",
            thread_cycles_unit="cycles", thread_cycles_source="query_thread_cycle_time")
        result = self.assess()
        scope = next(row for row in result["index"]["spans"] if row["span_id"] == "30")
        self.assertEqual(scope["thread_cycles"]["cycles"], 63)
        self.assertEqual(scope["thread_cycles"]["time_conversion"], "unavailable")
        self.assertEqual(self.assess(include_index=False)["summary"]["thread_cpu_functions"]["Graphics.Draw"]["cycles"], 63)

    def test_unavailable_cycles_do_not_become_zero_or_poison_wall_metrics(self):
        self.document["events"][2]["data"]["thread_cycles_status"] = "unavailable"
        result = self.assess()
        self.assertEqual(result["cpu_status"], "valid")
        self.assertIsNone(next(row for row in result["index"]["spans"] if row["span_id"] == "30")["thread_cycles"]["cycles"])

    def test_cycle_counter_contradiction_rejects_in_compact_mode_too(self):
        span = self.document["events"][2]
        span["data"].update(thread_cycles_status="measured", thread_cycles_begin="10", thread_cycles_end="12",
            thread_cycles_delta="3", thread_cycles_native_id="77", thread_cycles_unit="cycles", thread_cycles_source="query_thread_cycle_time")
        with self.assertRaises(ValueError):
            self.assess(include_index=False)

    def test_perfetto_presence_is_optional_but_declared_presence_must_match(self):
        target = self.root / "draw.json"
        self.document["coverage"] = {"perfetto_view_included": False}
        target.write_text(json.dumps(self.document), encoding="utf-8")
        self.assertNotIn("traceEvents", capture_draws.read_draw_trace(target))
        self.document["coverage"]["perfetto_view_included"] = True
        target.write_text(json.dumps(self.document), encoding="utf-8")
        with self.assertRaises(ValueError):
            capture_draws.read_draw_trace(target)

    def test_compact_text_has_summaries_without_detailed_span_rows(self):
        result = self.assess(include_index=False)
        report = capture_draws.report_from_health(self.root, {"run_id": self.manifest["run_id"], "status": "unknown", "draw_attribution": result, "issues": []})
        value = capture_draws.text_report(report)
        self.assertIn("Compact report", value)
        self.assertIn("Graphics.Draw", value)
        self.assertEqual(report["report_detail"], "compact")

    def test_causal_links_do_not_import_unrelated_operation_shader_identities(self):
        other = event(600, "operation", "700", name="draw", operation="700", end=2_000_000)
        causal = event(601, "link", "700", related="20", name="contains")
        self.document["events"].extend([other, causal])
        recount(self.document)
        result = self.assess()
        operation = next(row for row in result["index"]["draws"] if row["operation_id"] == "700")
        self.assertEqual(operation["shader_ids"], [])
        self.assertEqual(operation["pipeline_ids"], [])


if __name__ == "__main__":
    unittest.main()
