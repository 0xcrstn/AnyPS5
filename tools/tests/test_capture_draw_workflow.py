import copy
import json
import sys
import unittest
import uuid
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_draws
import capture_health
import capture_run
import capture_schema
import test_capture_workflow as legacy_workflow


def event(identifier, kind, subject, *, name=None, begin=1_000_000, end=None,
          related=None, parent=None, status="complete", operation=None):
    context = {key: None for key in ("driver", "submission", "segment", "packet", "operation",
                                    "device", "batch", "job", "flip", "present")}
    context.update(operation=operation, queue=None, packet_offset=0, opcode=None)
    return {"id": str(identifier), "kind": kind, "thread_id": "1", "ts_ns": begin,
            "end_ns": end, "subject": str(subject) if subject is not None else None,
            "related": str(related) if related is not None else None,
            "parent_span": str(parent) if parent is not None else None, "name": name,
            "text": None, "value": 0, "status": status, "context": context,
            "data": {"begin_gpu_ns": None, "end_gpu_ns": None, "slot": None, "shared": None}}


def document(manifest, trace_name):
    events = [event(101, "span", 10, name="synthetic host work", end=2_000_000)]
    return {
        "schema_version": 1, "document_type": "anyps5.draw-trace", "run_id": manifest["run_id"],
        "capture_id": str(uuid.uuid4()),
        "producer": {"pid": manifest["execution"]["pid"], "revision": "a" * 12,
                     "dirty": False, "coverage": "selected instrumented paths only"},
        "capture": {"unit": "frames", "start": 100, "length": 4, "state": "complete",
                    "open_present": "100", "close_present": "104", "open_ns": 0,
                    "close_ns": 80_000_000, "trace": trace_name, "drain_deadline_ms": 2000,
                    "close_reason": "window_closed", "pending_spans": 0,
                    "pending_jobs": 0, "pending_gpu_queries": 0},
        "boundaries": [{"present": str(100 + index), "ts_ns": index * 20_000_000}
                       for index in range(5)],
        "threads": [{"thread_id": "1", "name": "synthetic worker", "attempted": len(events),
                     "emitted": len(events), "lost": 0, "registered_ns": 0,
                     "last_event_ns": 2_000_000, "acknowledged": True}],
        "dictionaries": [], "events": events,
        "losses": {"events": 0, "metadata": 0, "gpu_queries": 0, "unassigned_events": 0}, "limitations": [],
        "completion": {"state": "complete", "event_count": len(events), "output_complete": True},
    }


def replace_events(value, events):
    value["events"] = events
    value["completion"]["event_count"] = len(events)
    value["threads"][0].update(attempted=len(events), emitted=len(events),
                                last_event_ns=max(item["end_ns"] or item["ts_ns"] for item in events))


def available_query(value, begin_ticks, end_ticks, period_ns=1):
    duration_ns = ((end_ticks - begin_ticks) & (2**64 - 1)) * period_ns
    begin_ns = float(begin_ticks) * period_ns
    value["data"].update(begin_gpu_ns=begin_ns, end_gpu_ns=begin_ns + duration_ns,
                         begin_gpu_ticks=str(begin_ticks), end_gpu_ticks=str(end_ticks),
                         period_ns=period_ns, duration_ns=duration_ns,
                         timestamp_valid_bits=64, timestamp_wraps=end_ticks < begin_ticks,
                         duration_origin="integer_tick_delta",
                         absolute_time_precision="approximate_binary64")


def registered_query(value, identifier):
    registered = copy.deepcopy(value)
    registered.update(id=str(identifier), status="pending", ts_ns=1_000_000)
    for key in ("begin_gpu_ns", "end_gpu_ns", "begin_gpu_ticks", "end_gpu_ticks",
                "period_ns", "duration_ns", "timestamp_valid_bits", "timestamp_wraps",
                "duration_origin", "absolute_time_precision"):
        registered["data"][key] = None
    return registered


class CaptureDrawWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.fixture = legacy_workflow.CaptureWorkflowTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def capture(self, name, *, report_detail=None):
        config = copy.deepcopy(self.fixture.config)
        config["settings"]["diagnostics"]["mode"] = "attribution"
        if report_detail is not None:
            config["settings"]["diagnostics"]["options"]["report_detail"] = report_detail
        config["environment"]["overrides"]["APS5_TRACE_DETAIL"] = "draw"
        original_emit = self.fixture.emit_capture

        def emit(trace_dir, manifest, pid, frame_ms):
            original_emit(trace_dir, manifest, pid, frame_ms)
            planned = copy.deepcopy(manifest)
            planned["execution"]["pid"] = pid
            trace_name = f"aps5-trace-{manifest['source']['commit'][:12]}-{pid}.json.gz"
            value = document(planned, trace_name)
            path = trace_dir / f"aps5-draw-{manifest['run_id']}-{pid}.json"
            path.write_bytes(capture_schema.canonical_json_bytes(value))

        with mock.patch.object(self.fixture, "emit_capture", side_effect=emit):
            run_dir, result, launches = self.fixture.capture(name, config=config)
        self.assertEqual(launches[0][1]["env"]["APS5_TRACE_DETAIL"], "draw")
        manifest = capture_schema.read_json(run_dir / "manifest.json")
        trace_name = next(Path(item["path"]).name for item in manifest["artifacts"] if item["kind"] == "trace")
        path = run_dir / "traces" / f"aps5-draw-{manifest['run_id']}-{manifest['execution']['pid']}.json"
        value = capture_schema.read_json(path)
        self.assertEqual(result["manifest"]["state"], "completed")
        self.assertEqual(result["manifest"]["execution"]["artifact_findings"], [])
        return run_dir, path, value

    def save(self, run_dir, path, value=None, *, payload=None):
        if payload is None:
            payload = capture_schema.canonical_json_bytes(value)
        path.write_bytes(payload)
        manifest = capture_schema.read_json(run_dir / "manifest.json")
        manifest["artifacts"], _ = capture_run.inventory_artifacts(run_dir)
        manifest["execution"]["artifact_findings"] = capture_run.select_capture(run_dir, manifest)
        capture_schema.atomic_write_json(run_dir / "manifest.json", manifest)

    def detail(self, run_dir):
        return capture_health.assess_capture(run_dir, include_draw_index=True)

    def test_legacy_opt_out_preserves_unknown_attribution(self):
        run_dir, _, _ = self.fixture.capture("legacy")
        report = self.detail(run_dir)
        self.assertEqual(report["frame_metrics"]["status"], "valid")
        self.assertEqual(report["detailed_attribution"]["status"], "unknown")
        self.assertNotIn("draw_attribution", report)

    def test_sidecar_is_selected_separately_and_recomputed(self):
        run_dir, path, value = self.capture("selected")
        capture_schema.validate_draw_trace(value)
        health = self.detail(run_dir)
        self.assertEqual(health["frame_metrics"]["status"], "valid")
        self.assertNotEqual(health["detailed_attribution"]["status"], "invalid")
        self.assertIn("draw_attribution", health)
        self.assertEqual(health["draw_attribution"]["cpu_status"], "valid")
        self.assertEqual(health["draw_attribution"]["gpu_status"], "unknown")
        self.assertEqual(health["draw_attribution"]["metrics"]["functions"]["synthetic host work.wall_ms"], 1)
        selected = health["artifacts_selected"]
        self.assertEqual(sum(item["kind"] == "trace" for item in selected), 1)
        self.assertEqual(sum(item["kind"] == "draw_trace" for item in selected), 1)
        manifest = capture_schema.read_json(run_dir / "manifest.json")
        self.assertEqual({item["kind"] for item in manifest["artifacts"] if item["kind"].startswith("draw_")},
                         {"draw_trace", "draw_index", "draw_report"})
        self.assertEqual(capture_schema.read_json(run_dir / "draws.json")["detail"]["capture_id"], value["capture_id"])
        capture_schema.atomic_write_json(run_dir / "health.json", {"status": "fabricated pass"})
        report = capture_draws.report_capture(run_dir)
        self.assertNotIn("fabricated pass", json.dumps(report))
        self.assertIn(value["capture_id"], json.dumps(report))
        self.assertIn(path.name, json.dumps(report))

    def test_runner_decodes_detail_once_and_keeps_index_out_of_saved_health(self):
        with mock.patch.object(capture_draws, "read_draw_trace", wraps=capture_draws.read_draw_trace) as reader:
            run_dir, _, value = self.capture("one-decode", report_detail="full")
        self.assertEqual(reader.call_count, 1)
        saved_health = capture_schema.read_json(run_dir / "health.json")
        saved_report = capture_schema.read_json(run_dir / "draws.json")
        self.assertNotIn("index", saved_health["draw_attribution"])
        index = saved_report["detail"]["index"]
        self.assertEqual(index["event_source"]["canonical_event_count"], len(value["events"]))
        self.assertEqual(index["event_source"]["json_pointer_template"], "/events/{index}")
        self.assertNotIn("events", index)
        self.assertEqual(index["spans"][0]["source"]["json_pointer"], "/events/0")

    def test_default_runner_report_remains_compact_with_valid_scope_metrics(self):
        with mock.patch.object(capture_draws, "read_draw_trace", wraps=capture_draws.read_draw_trace) as reader:
            run_dir, _, value = self.capture("compact-default")
        self.assertEqual(reader.call_count, 1)
        saved_health = capture_schema.read_json(run_dir / "health.json")
        saved_report = capture_schema.read_json(run_dir / "draws.json")
        self.assertNotIn("index", saved_health["draw_attribution"])
        self.assertNotIn("index", saved_report["detail"])
        self.assertEqual(saved_report["detail"]["capture_id"], value["capture_id"])
        self.assertEqual(saved_report["detail"]["metrics"]["functions"]["synthetic host work.wall_ms"], 1)

    def test_missing_requested_sidecar_never_passes(self):
        config = copy.deepcopy(self.fixture.config)
        config["environment"]["overrides"]["APS5_TRACE_DETAIL"] = "draw"
        run_dir, _, _ = self.fixture.capture("missing", config=config)
        report = self.detail(run_dir)
        self.assertEqual(report["detailed_attribution"]["status"], "invalid")

    def test_malformed_and_duplicate_json_remain_invalid_after_rehash(self):
        for index, payload in enumerate((b"{bad", b'{"schema_version":1,"schema_version":1}')):
            with self.subTest(index=index):
                run_dir, path, _ = self.capture(f"bad-json-{index}")
                self.save(run_dir, path, payload=payload)
                self.assertEqual(self.detail(run_dir)["detailed_attribution"]["status"], "invalid")

    def test_wrong_run_process_window_trace_and_record_count_are_invalid(self):
        mutations = (
            lambda value: value.update(run_id=str(uuid.uuid4())),
            lambda value: value["producer"].update(pid=9999),
            lambda value: value["capture"].update(start=101),
            lambda value: value["capture"].update(trace="different.json.gz"),
            lambda value: value["completion"].update(event_count=999),
            lambda value: value["boundaries"][-1].update(present="105"),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                run_dir, path, value = self.capture(f"mismatch-{index}")
                mutation(value)
                self.save(run_dir, path, value)
                self.assertEqual(self.detail(run_dir)["detailed_attribution"]["status"], "invalid")

    def test_tampering_fails_artifact_integrity_before_detail_metrics(self):
        run_dir, path, value = self.capture("tampered")
        value["events"][0]["end_ns"] += 1
        path.write_bytes(capture_schema.canonical_json_bytes(value))
        report = self.detail(run_dir)
        self.assertEqual(report["status"], "invalid")
        self.assertEqual(report["detailed_attribution"]["status"], "invalid")

    def test_dictionary_corruption_is_invalid(self):
        mutations = (
            lambda value: value["dictionaries"].extend([
                {"id": "50", "kind": "shader", "key": "same", "fields": {}, "references": []},
                {"id": "50", "kind": "shader", "key": "different", "fields": {}, "references": []}]),
            lambda value: value["dictionaries"].append(
                {"id": "50", "kind": "pipeline", "key": "pipeline", "fields": {}, "references": ["999"]}),
            lambda value: value["dictionaries"].extend([
                {"id": "50", "kind": "synthetic", "key": "first", "fields": {}, "references": []},
                {"id": "51", "kind": "synthetic", "key": "same", "fields": {}, "references": ["50"]},
                {"id": "52", "kind": "synthetic", "key": "same", "fields": {}, "references": []}]),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                run_dir, path, value = self.capture(f"dictionary-{index}")
                mutation(value)
                self.save(run_dir, path, value)
                self.assertEqual(self.detail(run_dir)["detailed_attribution"]["status"], "invalid")

    def test_unresolved_parent_preserves_intervals_with_unknown_origin(self):
        run_dir, path, value = self.capture("external-parent")
        value["events"][0]["parent_span"] = "999"
        self.save(run_dir, path, value)
        detail = self.detail(run_dir)["draw_attribution"]
        self.assertEqual(detail["origin_graph_status"], "unknown")
        self.assertEqual(detail["cpu_status"], "valid")
        self.assertEqual(detail["status"], "unknown")
        self.assertIn("999", json.dumps(detail["index"]["external_references"]))

    def test_boundary_reference_resolved_in_window_is_not_unknown(self):
        run_dir, path, value = self.capture("resolved-reference")
        reference = event(100, "reference", 10, name="span", begin=0, status="unknown")
        reference["text"] = "outside_window_or_uninstrumented"
        replace_events(value, [reference, value["events"][0]])
        self.save(run_dir, path, value)
        detail = self.detail(run_dir)["draw_attribution"]
        self.assertEqual(detail["identity_status"], "valid")
        self.assertEqual(detail["origin_graph_status"], "valid")
        self.assertEqual(detail["index"]["external_references"], [])

    def test_parent_cycles_cannot_fabricate_zero_self_time(self):
        run_dir, path, value = self.capture("span-cycle")
        replace_events(value, [event(101, "span", 10, name="first", end=2_000_000, parent=11),
                               event(102, "span", 11, name="second", end=2_000_000, parent=10)])
        self.save(run_dir, path, value)
        self.assertEqual(self.detail(run_dir)["detailed_attribution"]["status"], "invalid")

    def test_nested_elapsed_self_time_and_boundary_crossing_are_separate(self):
        run_dir, path, value = self.capture("self-time")
        replace_events(value, [event(101, "span", 10, name="parent", begin=1_000_000, end=9_000_000),
                               event(102, "span", 11, name="child", begin=2_000_000, end=4_000_000, parent=10),
                               event(103, "span", 12, name="tail", begin=79_000_000, end=81_000_000)])
        self.save(run_dir, path, value)
        detail = self.detail(run_dir)["draw_attribution"]
        metrics = detail["metrics"]["functions"]
        self.assertEqual(metrics["parent.wall_ms"], 8)
        self.assertEqual(metrics["parent.self_ms"], 6)
        self.assertEqual(metrics["child.wall_ms"], 2)
        self.assertNotIn("tail.wall_ms", metrics)
        self.assertEqual(detail["function_availability"]["parent"], "valid")
        self.assertEqual(detail["function_availability"]["tail"], "unknown")
        tail = next(item for item in detail["index"]["spans"] if item["function"] == "tail")
        self.assertTrue(tail["boundary_crossing"])
        self.assertEqual(tail["wall_ns"], 2_000_000)

    def test_clipped_idle_observation_preserves_elapsed_but_not_completeness(self):
        run_dir, path, value = self.capture("clipped-idle")
        value["events"][0].update(name="Driver.IdleWait", status="unknown", end_ns=80_000_000,
                                  text="clipped_at_window_close")
        replace_events(value, [value["events"][0],
                               event(102, "span", 11, name="healthy scope", begin=2_000_000, end=3_000_000)])
        self.save(run_dir, path, value)
        detail = self.detail(run_dir)["draw_attribution"]
        self.assertEqual(detail["cpu_status"], "unknown")
        self.assertNotIn("Driver.IdleWait.wall_ms", detail["metrics"]["functions"])
        self.assertEqual(detail["metrics"]["functions"]["healthy scope.wall_ms"], 1)
        self.assertEqual(detail["function_availability"]["Driver.IdleWait"], "unknown")
        self.assertEqual(detail["function_availability"]["healthy scope"], "valid")
        self.assertEqual(detail["index"]["spans"][0]["wall_ns"], 79_000_000)

    def test_query_failure_has_no_zero_duration_or_shader_stage_claim(self):
        for index, status in enumerate(("unsupported", "allocation_failed", "read_failed", "invalid_timestamp", "uncollected")):
            with self.subTest(status=status):
                run_dir, path, value = self.capture(f"query-{index}")
                query = event(102, "gpu_query", 20, name="draw", status=status, related=30)
                query["context"]["batch"] = "30"
                query["data"].update(slot=0, shared=False)
                replace_events(value, [value["events"][0], event(103, "batch", 30, name="batch"),
                                       registered_query(query, 104), query])
                self.save(run_dir, path, value)
                detail = self.detail(run_dir)["draw_attribution"]
                self.assertNotEqual(detail["gpu_status"], "valid")
                self.assertIsNone(detail["index"]["queries"][0]["duration_ns"])
                self.assertEqual(detail["metrics"]["gpu"], {})
                self.assertTrue(any("shader-stage" in item for item in detail["limitations"]))

    def test_late_available_query_keeps_batch_identity(self):
        run_dir, path, value = self.capture("late-query")
        queries = []
        for index, batch in enumerate((30, 31)):
            query = event(104 + index, "gpu_query", 20 + index, name="draw", begin=81_000_000,
                          status="available", related=batch)
            query["context"]["batch"] = str(batch)
            query["data"].update(begin_gpu_ns=100 + index * 100, end_gpu_ns=130 + index * 100,
                                 slot=0, shared=False)
            available_query(query, 100 + index * 100, 130 + index * 100)
            queries.extend([registered_query(query, 106 + index), query])
        replace_events(value, [value["events"][0], event(102, "batch", 30, name="first batch"),
                               event(103, "batch", 31, name="second batch"), *queries])
        self.save(run_dir, path, value)
        detail = self.detail(run_dir)["draw_attribution"]
        self.assertEqual(detail["gpu_status"], "valid")
        self.assertEqual({item["batch_id"] for item in detail["index"]["queries"]}, {"30", "31"})
        self.assertEqual([item["duration_ns"] for item in detail["index"]["queries"]], [30, 30])

    def test_equal_available_raw_ticks_are_measured_zero(self):
        run_dir, path, value = self.capture("zero-query")
        query = event(103, "gpu_query", 20, name="draw", status="available", related=30)
        query["context"]["batch"] = "30"
        query["data"].update(begin_gpu_ns=100, end_gpu_ns=100, slot=0, shared=False)
        available_query(query, 100, 100)
        replace_events(value, [value["events"][0], event(102, "batch", 30, name="batch"),
                               registered_query(query, 104), query])
        self.save(run_dir, path, value)
        detail = self.detail(run_dir)["draw_attribution"]
        self.assertEqual(detail["gpu_status"], "valid")
        self.assertEqual(detail["index"]["queries"][0]["duration_ns"], 0)

    def test_query_lifecycle_cannot_change_batch_slot_kind_or_sharing(self):
        mutations = (
            lambda value: value.update(related="31"),
            lambda value: value["data"].update(slot=1),
            lambda value: value["data"].update(shared=True),
            lambda value: value.update(name="dispatch"),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                run_dir, path, value = self.capture(f"query-identity-{index}")
                pending = event(104, "gpu_query", 20, name="draw", status="pending", related=30)
                pending["context"]["batch"] = "30"
                pending["data"].update(slot=0, shared=False)
                terminal = copy.deepcopy(pending)
                terminal.update(id="105", status="available", ts_ns=81_000_000)
                terminal["data"].update(begin_gpu_ns=100, end_gpu_ns=130)
                available_query(terminal, 100, 130)
                mutation(terminal)
                replace_events(value, [value["events"][0], event(102, "batch", 30, name="first batch"),
                                       event(103, "batch", 31, name="second batch"), pending, terminal])
                self.save(run_dir, path, value)
                self.assertEqual(self.detail(run_dir)["detailed_attribution"]["status"], "invalid")

    def test_producer_host_command_start_and_terminal_form_one_recorded_command(self):
        run_dir, path, value = self.capture("command-lifecycle")
        command = event(103, "host_command", 40, name="draw", status="unknown", operation="50")
        command["context"]["batch"] = "30"
        command["data"].update(requested_count=1, count_dynamic=False)
        terminal = copy.deepcopy(command)
        terminal.update(id="104", ts_ns=2_000_000, status="complete")
        pending = event(105, "gpu_query", 20, name="draw", status="pending", related=30, operation="50")
        pending["context"]["batch"] = "30"
        pending["data"].update(slot=0, shared=False)
        available = copy.deepcopy(pending)
        available.update(id="106", ts_ns=81_000_000, status="available")
        available["data"].update(begin_gpu_ns=100, end_gpu_ns=130)
        available_query(available, 100, 130)
        replace_events(value, [value["events"][0], event(102, "batch", 30, name="batch"),
                               command, terminal, pending, available,
                               event(107, "link", 20, related=40, name="measures"),
                               event(108, "operation", 50, name="draw", status="unknown", operation="50", begin=500_000),
                               event(109, "operation", 50, name="draw", status="complete", operation="50", begin=3_000_000)])
        self.save(run_dir, path, value)
        detail = self.detail(run_dir)["draw_attribution"]
        self.assertEqual(detail["gpu_status"], "valid")
        self.assertEqual(detail["gpu_association_status"], "valid")
        self.assertEqual(len(detail["index"]["host_commands"]), 1)
        self.assertEqual(detail["workload_status"], "valid")
        self.assertEqual(detail["metrics"]["workload"]["host_draw_commands"], 1)
        self.assertEqual(detail["index"]["host_commands"][0]["status"], "complete")

    def test_query_and_host_lifecycle_statuses_are_not_interchangeable(self):
        run_dir, path, value = self.capture("wrong-span-status")
        value["events"][0]["status"] = "available"
        self.save(run_dir, path, value)
        self.assertEqual(self.detail(run_dir)["detailed_attribution"]["status"], "invalid")
        run_dir, path, value = self.capture("wrong-query-status")
        query = event(103, "gpu_query", 20, name="draw", status="complete", related=30)
        query["data"].update(slot=0, shared=False)
        replace_events(value, [value["events"][0], event(102, "batch", 30, name="batch"),
                               registered_query(query, 104), query])
        self.save(run_dir, path, value)
        self.assertEqual(self.detail(run_dir)["detailed_attribution"]["status"], "invalid")

    def test_loss_and_incomplete_tail_cannot_become_complete(self):
        mutations = (
            lambda value: value["losses"].update(events=1),
            lambda value: value["losses"].update(metadata=1),
            lambda value: value["losses"].update(gpu_queries=1),
            lambda value: value["capture"].update(pending_jobs=1),
            lambda value: value["capture"].update(pending_spans=1),
            lambda value: value["completion"].update(output_complete=False),
            lambda value: value["threads"][0].update(acknowledged=False),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                run_dir, path, value = self.capture(f"partial-{index}")
                mutation(value)
                self.save(run_dir, path, value)
                detail = self.detail(run_dir)
                self.assertNotEqual(detail["detailed_attribution"]["status"], "valid")

    def test_unassigned_registration_and_admission_losses_are_accounted(self):
        run_dir, path, value = self.capture("unassigned-loss")
        value["losses"].update(events=2, unassigned_events=2)
        value["capture"]["state"] = "partial"
        value["completion"]["state"] = "partial"
        self.save(run_dir, path, value)
        health = self.detail(run_dir)
        self.assertIn("draw_attribution", health)
        self.assertEqual(health["draw_attribution"]["cpu_status"], "invalid")
        self.assertEqual(health["draw_attribution"]["status"], "invalid")
        self.assertFalse(any(item["code"] == "draw_evidence_invalid" for item in health["issues"]))

    def test_declared_event_loss_retains_spans_without_complete_self_time(self):
        run_dir, path, value = self.capture("lost-span")
        value["capture"]["state"] = "partial"
        value["completion"]["state"] = "partial"
        value["threads"][0].update(attempted=2, lost=1)
        value["losses"]["events"] = 1
        self.save(run_dir, path, value)
        health = self.detail(run_dir)
        detail = health["draw_attribution"]
        self.assertEqual(health["frame_metrics"]["status"], "valid")
        self.assertEqual(detail["status"], "invalid")
        self.assertEqual(detail["cpu_status"], "invalid")
        self.assertTrue(detail["correlation_valid"])
        self.assertEqual(detail["coverage"]["recorded_event_count"], 1)
        self.assertEqual(detail["coverage"]["attempted_event_count"], 2)
        self.assertEqual(detail["coverage"]["lost_event_count"], 1)
        self.assertEqual(detail["index"]["spans"][0]["wall_ns"], 1_000_000)
        self.assertIsNone(detail["index"]["spans"][0]["self_ns"])
        self.assertEqual(detail["metrics"]["functions"], {})
        self.assertFalse(any(item["code"] == "draw_evidence_invalid" for item in health["issues"]))
        text = capture_draws.text_report(capture_draws.report_from_health(run_dir, health))
        span_row = next(line for line in text.splitlines() if "synthetic host work" in line and "|" in line)
        self.assertNotIn("0.000000", span_row)

    def test_loss_explains_missing_command_lifecycle_without_inventing_elapsed(self):
        for index, retained_status in enumerate(("unknown", "complete")):
            with self.subTest(retained_status=retained_status):
                run_dir, path, value = self.capture(f"lost-command-{index}")
                command = event(103, "host_command", 40, name="draw", status=retained_status)
                command["context"]["batch"] = "30"
                command["data"].update(requested_count=1, count_dynamic=False)
                replace_events(value, [value["events"][0], event(102, "batch", 30, name="batch"), command])
                value["capture"]["state"] = "partial"
                value["completion"]["state"] = "partial"
                value["threads"][0].update(attempted=4, lost=1)
                value["losses"]["events"] = 1
                self.save(run_dir, path, value)
                health = self.detail(run_dir)
                detail = health["draw_attribution"]
                self.assertEqual(detail["status"], "invalid")
                self.assertEqual(len(detail["index"]["host_commands"]), 1)
                retained = detail["index"]["host_commands"][0]
                self.assertEqual(retained["lifecycle_status"], "unknown")
                self.assertIsNone(retained["recording_begin_ns"])
                self.assertIsNone(retained["recording_end_ns"])
                self.assertIsNone(retained["elapsed_ns"])
                self.assertTrue(retained["source_events"])
                self.assertEqual(detail["observed_counts"]["values"]["host_draw_commands"], 1)
                self.assertEqual(detail["metrics"]["workload"], {})
                self.assertEqual(detail["metrics"]["gpu"], {})
                self.assertFalse(any(item["code"] == "draw_evidence_invalid" for item in health["issues"]))

    def test_loss_never_hides_duplicate_or_contradictory_command_records(self):
        for index, contradiction in enumerate(("duplicate_begin", "duplicate_terminal", "changed_request")):
            with self.subTest(contradiction=contradiction):
                run_dir, path, value = self.capture(f"corrupt-lossy-command-{index}")
                begin = event(103, "host_command", 40, name="draw", status="unknown")
                begin["context"]["batch"] = "30"
                begin["data"].update(requested_count=1, count_dynamic=False)
                terminal = copy.deepcopy(begin)
                terminal.update(id="104", ts_ns=2_000_000, status="complete")
                events = [value["events"][0], event(102, "batch", 30, name="batch"), begin, terminal]
                if contradiction == "changed_request":
                    terminal["data"]["requested_count"] = 2
                else:
                    duplicate = copy.deepcopy(begin if contradiction == "duplicate_begin" else terminal)
                    duplicate["id"] = "105"
                    events.append(duplicate)
                replace_events(value, events)
                value["capture"]["state"] = "partial"
                value["completion"]["state"] = "partial"
                value["threads"][0].update(attempted=len(events) + 1, lost=1)
                value["losses"]["events"] = 1
                self.save(run_dir, path, value)
                health = self.detail(run_dir)
                self.assertEqual(health["detailed_attribution"]["status"], "invalid")
                self.assertTrue(any(item["code"] == "draw_evidence_invalid" for item in health["issues"]))

    def test_loss_retains_query_observation_without_complete_lifecycle_or_association(self):
        for index, retained_status in enumerate(("pending", "available")):
            with self.subTest(retained_status=retained_status):
                run_dir, path, value = self.capture(f"lost-query-{index}")
                query = event(103, "gpu_query", 20, name="draw", status=retained_status, related=30)
                query["context"]["batch"] = "30"
                query["data"].update(slot=0, shared=False)
                if retained_status == "available":
                    available_query(query, 100, 130)
                replace_events(value, [value["events"][0], event(102, "batch", 30, name="batch"), query])
                value["capture"]["state"] = "partial"
                value["completion"]["state"] = "partial"
                value["threads"][0].update(attempted=4, lost=1)
                value["losses"]["events"] = 1
                self.save(run_dir, path, value)
                health = self.detail(run_dir)
                detail = health["draw_attribution"]
                self.assertEqual(detail["status"], "invalid")
                self.assertEqual(detail["metrics"]["gpu"], {})
                retained = detail["index"]["queries"][0]
                self.assertEqual(retained["lifecycle_status"], "unknown")
                self.assertEqual(retained["duration_ns"], 30 if retained_status == "available" else None)
                self.assertEqual(detail["gpu_association_status"], "unknown")
                self.assertFalse(any(item["code"] == "draw_evidence_invalid" for item in health["issues"]))

    def test_integer_gpu_tick_delta_survives_rounded_equal_absolute_endpoints(self):
        run_dir, path, value = self.capture("precise-query")
        query = event(103, "gpu_query", 20, name="draw", status="available", related=30)
        query["context"]["batch"] = "30"
        query["data"].update(slot=0, shared=False)
        available_query(query, 2**60, 2**60 + 1, 0.5)
        self.assertEqual(query["data"]["begin_gpu_ns"], query["data"]["end_gpu_ns"])
        replace_events(value, [value["events"][0], event(102, "batch", 30, name="batch"),
                               registered_query(query, 104), query])
        self.save(run_dir, path, value)
        detail = self.detail(run_dir)["draw_attribution"]
        self.assertEqual(detail["gpu_status"], "valid")
        self.assertEqual(detail["index"]["queries"][0]["duration_ns"], 0.5)

    def test_precise_query_rejects_ambiguous_tick_delta_and_fabricated_duration(self):
        for index, corruption in enumerate(("ambiguous_delta", "wrong_duration")):
            with self.subTest(corruption=corruption):
                run_dir, path, value = self.capture(f"corrupt-precise-query-{index}")
                query = event(103, "gpu_query", 20, name="draw", status="available", related=30)
                query["context"]["batch"] = "30"
                query["data"].update(slot=0, shared=False)
                available_query(query, 2**60, 2**60 + 1, 0.5)
                if corruption == "ambiguous_delta":
                    query["data"]["end_gpu_ticks"] = str(2**60 - 1)
                    query["data"]["timestamp_wraps"] = True
                else:
                    query["data"]["duration_ns"] = 0
                replace_events(value, [value["events"][0], event(102, "batch", 30, name="batch"), query])
                self.save(run_dir, path, value)
                health = self.detail(run_dir)
                self.assertEqual(health["detailed_attribution"]["status"], "invalid")
                self.assertTrue(any(item["code"] == "draw_evidence_invalid" for item in health["issues"]))

    def test_precise_query_preserves_small_interval_across_uint64_wrap(self):
        run_dir, path, value = self.capture("wrapped-query")
        query = event(103, "gpu_query", 20, name="draw", status="available", related=30)
        query["context"]["batch"] = "30"
        query["data"].update(slot=0, shared=False)
        available_query(query, 2**64 - 2, 1, 0.5)
        self.assertTrue(query["data"]["timestamp_wraps"])
        replace_events(value, [value["events"][0], event(102, "batch", 30, name="batch"),
                               registered_query(query, 104), query])
        self.save(run_dir, path, value)
        detail = self.detail(run_dir)["draw_attribution"]
        self.assertEqual(detail["gpu_status"], "valid")
        self.assertEqual(detail["index"]["queries"][0]["duration_ns"], 1.5)


if __name__ == "__main__":
    unittest.main()
