import copy
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_draws
import capture_evidence
from capture_schema import SchemaError, validate_evidence_report
from test_capture_draws import event, make_detail, recount


class CaptureEvidenceTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.manifest, self.document, name, summary, trace = make_detail(self.root)
        self.draws = capture_draws.assess_draw_trace(self.document, self.manifest, trace_name=name, summary=summary, trace=trace)

    def assess(self, **kwargs):
        return capture_evidence.assess_evidence(self.document, self.manifest, self.draws, **kwargs)

    def add_noop_dispatch(self, *, terminal=True):
        operation = event(501, "operation", "701", name="dispatch", operation="701",
                          start=1_000_000, end=2_000_000 if terminal else None,
                          status="complete" if terminal else "unknown")
        path = event(502, "attribute", "701", name="dispatch_path", operation="701")
        path["text"] = "fill_hle"
        self.document["events"].extend([operation, path])
        recount(self.document)
        self.draws = capture_draws.assess_draw_trace(self.document, self.manifest,
            trace_name="aps5-trace-aaaaaaaaaaaa-7.json.gz",
            summary={"commit": "a" * 12, "dirty": False, "window": {"ms": 40}},
            trace={"presents": [(10, 0, 1), (11, 20_000, 1)]})

    def test_noop_shader_unknown_retains_independently_known_workload_counts(self):
        self.add_noop_dispatch()
        report = self.assess()
        self.assertEqual(self.draws["identity_status"], "unknown")
        self.assertEqual(self.draws["workload_status"], "valid")
        self.assertEqual(report["dimensions"]["workload"]["status"], "valid")
        self.assertEqual(report["metrics"]["workload.dispatch_operations"], 1)
        self.assertEqual(report["metrics"]["workload.host_draw_commands"], 1)
        noop = next(row for row in self.draws["index"]["draws"] if row["operation_id"] == "701")
        self.assertEqual(noop["lifecycle_status"], "valid")
        self.assertIsNone(noop["gpu_interval_sum_ns"])
        self.assertEqual(report["dimensions"]["correctness"]["status"], "unknown")

    def test_missing_noop_terminal_withholds_whole_window_workload(self):
        self.add_noop_dispatch(terminal=False)
        report = self.assess()
        self.assertEqual(self.draws["workload_status"], "unknown")
        self.assertEqual(self.draws["metrics"]["workload"], {})
        self.assertEqual(report["dimensions"]["workload"]["status"], "unknown")
        self.assertFalse(any(name.startswith("workload.") for name in report["metrics"]))

    def test_legacy_sidecar_remains_unknown_without_optional_evidence(self):
        report = self.assess()
        validate_evidence_report(report)
        self.assertEqual(report["status"], "unknown")
        self.assertEqual(report["dimensions"]["thread_cpu"]["status"], "unknown")
        self.assertNotIn("thread_cpu.Graphics.Draw.inclusive_ns", report["metrics"])

    def test_nested_overlapping_intervals_are_unioned_not_summed(self):
        self.assertEqual(capture_evidence.interval_union([(1, 8), (2, 4), (6, 10), (12, 15)]), [[1, 10], [12, 15]])
        self.assertEqual(capture_evidence.interval_uncovered((0, 16), [(1, 10), (12, 15)]), [[0, 1], [10, 12], [15, 16]])

    def test_union_window_clip_preserves_half_open_endpoints(self):
        self.assertEqual(capture_evidence.clip_union([[0, 5], [8, 10]], (5, 8)), [])
        self.assertEqual(capture_evidence.clip_union([[0, 5], [8, 10]], (4, 9)), [[4, 5], [8, 9]])

    def test_uncovered_time_never_becomes_cpu_idle_or_frame_ownership(self):
        report = self.assess()
        self.assertGreater(report["frames"][1]["host_scope_uncovered_ns"], 0)
        self.assertEqual(report["frames"][0]["causal_frame_membership"], "unknown")
        self.assertIsNone(report["frames"][0]["gpu_uncovered_time"])

    def test_operations_include_elapsed_gpu_digest_and_source_evidence(self):
        operation = self.assess()["operations"][0]
        self.assertTrue(operation["selected_cpu_scopes"])
        self.assertEqual(operation["whole_command_gpu_intervals"][0]["duration_ns"], 1000)
        self.assertTrue(all(len(row["key"]) == 64 for row in operation["shader_identities"]))
        self.assertTrue(operation["source_events"][0]["json_pointer"].startswith("/events/"))

    def test_workload_count_signature_uses_independent_fresh_metrics(self):
        report = self.assess()
        self.assertEqual(report["dimensions"]["workload"]["status"], "valid")
        self.assertEqual(report["metric_units"]["workload.host_draw_commands"], "count")
        self.draws["metrics"]["workload"] = {}
        self.assertEqual(self.assess()["dimensions"]["workload"]["status"], "unknown")

    def test_loss_does_not_publish_complete_counter_totals(self):
        counter = event(501, "counter", "20", name="copy_cpu_bytes", operation="20")
        counter.update(value=64, text="bytes")
        counter["data"]["availability"] = "observed"
        self.document["events"].append(counter)
        self.draws["coverage"]["lost_event_count"] = 1
        report = self.assess()
        self.assertEqual(report["metrics"]["copies.copy_cpu_bytes"], 64)
        self.assertEqual(report["metric_availability"]["copies.copy_cpu_bytes"], "unknown")

    def test_pending_host_work_prevents_whole_window_counter_validity(self):
        self.document["capture"]["pending_jobs"] = 1
        counter = event(501, "counter", "20", name="shader_compilations", operation="20")
        counter.update(value=1, text="attempts")
        counter["data"]["availability"] = "observed"
        self.document["events"].append(counter)
        report = self.assess()
        self.assertEqual(report["metric_availability"]["compilation.shader_compilations"], "unknown")

    def test_dependency_cycle_is_invalid(self):
        self.document["events"].extend([event(501, "operation", "21", name="draw"),
            event(502, "link", "20", related="21", name="indirect_record"),
            event(503, "link", "21", related="20", name="indirect_record")])
        self.assertEqual(self.assess()["dimensions"]["dependency_graph"]["status"], "invalid")

    def test_unknown_external_dependency_stays_explicit(self):
        self.document["events"].append(event(501, "link", "900", related="20", name="schedules_job"))
        report = self.assess()
        self.assertEqual(report["dimensions"]["dependency_graph"]["status"], "unknown")
        node = next(row for row in report["dependency_graph"]["nodes"] if row["id"] == "900")
        self.assertEqual(node["status"], "unknown")

    def test_eventual_job_completion_cannot_prove_wait_release(self):
        wait = event(501, "span", "700", name="Wait", start=1, end=5)
        job = event(502, "job", "701", name="Job", start=1, end=10)
        note = event(503, "attribute", "700", name="wait_outcome")
        note["text"] = "satisfied"
        self.document["events"].extend([wait, job, note, event(504, "link", "700", related="701", name="waits_for")])
        graph = self.assess()["dependency_graph"]
        self.assertFalse(any(row["category"] == "observed_wait_completion" for row in graph["edges"]))

    def test_native_qpc_anchor_uses_exact_integer_differences(self):
        self.document["clock"] = {"native_anchor": {"status": "available", "domain": "query_performance_counter",
            "ticks": str(2 ** 60), "frequency": "1000000000", "steady_before_ns": 0, "steady_after_ns": 2}}
        anchor = capture_evidence.native_clock_anchor(self.document)
        self.assertEqual(capture_evidence.map_host_ticks(2 ** 60 + 1, "windows_qpc", 1_000_000_000, anchor),
                         {"lower_ns": 0, "upper_ns": 4})

    def anchors(self):
        from fractions import Fraction
        return [{"event_id": "1", "gpu_ticks": 100, "position": 0, "host_lower": Fraction(0), "host_upper": Fraction(2), "period_ns": 1},
                {"event_id": "2", "gpu_ticks": 200, "position": 100, "host_lower": Fraction(100), "host_upper": Fraction(102), "period_ns": 1}]

    def query(self):
        return {"status": "available", "duration_origin": "integer_tick_delta", "begin_gpu_ticks": "125",
                "end_gpu_ticks": "150", "period_ns": 1, "timestamp_valid_bits": 64}

    def test_gpu_mapping_retains_wide_drift_bounds_and_model_estimate(self):
        mapping = capture_evidence.map_gpu_interval(self.query(), self.anchors())
        self.assertEqual(mapping["status"], "valid")
        self.assertEqual(mapping["start"]["lower_ns"], 0)
        self.assertEqual(mapping["start"]["upper_ns"], 102)
        self.assertFalse(mapping["exact"])

    def test_gpu_mapping_never_extrapolates(self):
        query = self.query()
        query["end_gpu_ticks"] = "201"
        self.assertEqual(capture_evidence.map_gpu_interval(query, self.anchors())["status"], "unknown")

    def test_gpu_query_period_must_match_calibration(self):
        query = self.query()
        query["period_ns"] = 2
        with self.assertRaises(SchemaError):
            capture_evidence.map_gpu_interval(query, self.anchors())

    def test_rejected_optional_hardware_request_is_unavailable_not_invalid(self):
        counter = event(501, "counter", "40", name="occlusion_samples")
        counter["data"].update(availability="unsupported", counter_value=None)
        counter["text"] = "samples"
        self.document["events"].append(counter)
        report = self.assess()
        self.assertEqual(report["dimensions"]["gpu_counters"]["status"], "unknown")
        self.assertTrue(report["hardware_counters"][0]["request_rejected"])

    def test_compact_health_excludes_large_indices(self):
        report = capture_evidence.compact_evidence(self.assess())
        validate_evidence_report(report)
        self.assertNotIn("operations", report)
        self.assertNotIn("dependency_graph", report)
        self.assertEqual(report["index_counts"]["frames"], 2)

    def test_html_escapes_native_names_and_links_source_pointers(self):
        report = self.assess()
        report["operations"][0]["kind"] = "<script>"
        report["sources"]["native_artifact"] = {"path": "traces/draw.json"}
        value = capture_evidence.html_report(report)
        self.assertIn("&lt;script&gt;", value)
        self.assertIn("traces/draw.json#/events/", value)
        self.assertNotIn("<script>", value)

    def test_reviewed_conditions_remain_distinct_from_native_proof(self):
        conditions = {"run_id": self.manifest["run_id"], "dimensions": {name: {"status": "valid", "basis": "reviewed_claim",
            "reasons": [], "artifacts": [{"path": "review.json", "sha256": "a" * 64, "size": 1}]}
            for name in ("scene", "readiness", "correctness", "cache.shader", "cache.pipeline", "cache.driver")}}
        report = self.assess(condition_evidence=conditions)
        self.assertEqual(report["dimensions"]["scene"]["status"], "valid")
        self.assertEqual(report["dimensions"]["scene"]["basis"], "reviewed_claim")
        self.assertEqual(report["conditions"], conditions)

    def test_process_resources_are_run_wide_with_source_units(self):
        process = {"status": "valid", "coverage": {"raw_samples_verified": True}, "resources": {
            "private_bytes": {"minimum": 100, "maximum": 300, "delta": -1},
            "user_cpu_100ns": {"minimum": 10, "maximum": 20, "delta": 10}}}
        report = self.assess(process_evidence=process)
        self.assertEqual(report["metric_units"]["process_run.user_cpu_100ns.maximum"], "100ns")
        self.assertEqual(report["process_resources"]["resources"]["private_bytes"]["delta"], -1)

    def test_host_batch_link_requires_matching_known_context(self):
        self.document["events"].append(event(501, "link", "40", related="45", name="records_in"))
        for row in self.document["events"]:
            if row["kind"] == "host_command":
                row["context"]["batch"] = "999"
        self.assertEqual(self.assess()["dimensions"]["dependency_graph"]["status"], "invalid")

    def test_hardware_counter_cannot_change_unit_or_context(self):
        first = event(501, "counter", "40", name="vertex_shader_invocations", operation="20", status="unknown")
        first.update(text="count")
        first["data"].update(availability="pending", counter_value=None)
        last = copy.deepcopy(first)
        last.update(id="502", text="bytes")
        last["data"].update(availability="available", counter_value="3")
        self.document["events"].extend([first, last])
        self.assertEqual(self.assess()["dimensions"]["gpu_counters"]["status"], "invalid")

    def test_explicit_batch_origin_capacity_loss_keeps_graph_incomplete(self):
        counter = event(501, "counter", "45", name="batch_origin_capacity_dropped")
        counter.update(value=1, text="batch")
        counter["data"]["availability"] = "observed"
        self.document["events"].append(counter)
        self.assertEqual(self.assess()["dimensions"]["dependency_graph"]["status"], "unknown")

    def test_exported_html_uses_actual_run_directory_for_source_links(self):
        report = self.assess()
        report["sources"]["native_artifact"] = {"path": "traces/draw.json"}
        output = self.root / "exports"
        output.mkdir()
        value = capture_evidence.html_report(report, source_directory=self.root, output_directory=output)
        self.assertIn("../traces/draw.json#/events/", value)


    def test_compact_and_full_metrics_match_without_building_span_rows(self):
        full = self.assess()
        index = {}
        self.draws = capture_draws.assess_draw_trace(self.document, self.manifest,
            trace_name="aps5-trace-aaaaaaaaaaaa-7.json.gz", summary={"commit": "a" * 12, "dirty": False, "window": {"ms": 40}},
            trace={"presents": [(10, 0, 1), (11, 20_000, 1)]}, include_index=False, analysis_index=index)
        compact = self.assess(draw_index=index)
        self.assertEqual(compact["metrics"], full["metrics"])
        self.assertEqual(compact["metric_availability"], full["metric_availability"])
        self.assertEqual(compact["operations"], [])
        self.assertNotIn("nodes", compact["dependency_graph"])
        self.assertNotIn("queries", compact["clock_mapping"])
        self.assertEqual(compact["frames"][0]["host_scope_union_ns"], full["frames"][0]["host_scope_union_ns"])

    def test_compaction_is_idempotent_and_preserves_counts_and_frames(self):
        compact = capture_evidence.compact_evidence(self.assess())
        twice = capture_evidence.compact_evidence(compact)
        self.assertEqual(twice["index_counts"], compact["index_counts"])
        self.assertEqual(twice["summary"], compact["summary"])
        self.assertIn("Compact report", capture_evidence.html_report(twice))

    def test_measured_cycles_do_not_imply_elapsed_time_or_unsampled_functions(self):
        for thread in self.document["threads"]:
            thread.update(native_thread_id=str(int(thread["thread_id"]) + 70), native_thread_id_source="windows_thread_id")
        for row in self.document["events"]:
            if row["kind"] == "span":
                row["data"].update(thread_cycles_status="measured", thread_cycles_begin=str(2 ** 60),
                    thread_cycles_end=str(2 ** 60 + 63), thread_cycles_delta="63", thread_cycles_native_id=str(int(row["thread_id"]) + 70),
                    thread_cycles_unit="cycles", thread_cycles_source="query_thread_cycle_time")
        self.draws = capture_draws.assess_draw_trace(self.document, self.manifest,
            trace_name="aps5-trace-aaaaaaaaaaaa-7.json.gz", summary={"commit": "a" * 12, "dirty": False, "window": {"ms": 40}},
            trace={"presents": [(10, 0, 1), (11, 20_000, 1)]})
        report = self.assess()
        self.assertEqual(report["metrics"]["thread_cycles.Graphics.Draw.inclusive_cycles"], 63)
        self.assertEqual(report["metric_units"]["thread_cycles.Graphics.Draw.inclusive_cycles"], "cycles")
        self.assertNotIn("thread_cpu.Graphics.Draw.inclusive_ns", report["metrics"])

    def test_light_and_detailed_workload_disagreement_blocks_availability(self):
        report = self.assess()
        workload = {"status": "valid", "run_id": self.manifest["run_id"], "metrics": {"host_draw_commands": 2}, "issues": []}
        merged = capture_evidence.merge_measurement_workload(report, workload, self.manifest)
        self.assertEqual(merged["dimensions"]["workload"]["status"], "invalid")
        self.assertEqual(merged["metric_availability"]["workload.host_draw_commands"], "invalid")
        self.assertEqual(merged["status"], "invalid")

    def test_verified_exact_rva_labels_samples_without_function_time_claim(self):
        image = {"sha256": "a" * 64, "size": 100, "image_size": 100}
        ownership = {"module_offset_aggregates": [{"module": {"module_id": "1", "file_identity": image},
            "offset": "12", "sample_hits": 3, "weighted_sample_hits": 4}]}
        receipt = {"status": "valid", "image": image, "binding": "embedded_dwarf_image_sha256", "function_ranges": [], "address_resolutions": [
            {"rva": "12", "status": "resolved", "frames": [{"name": "ObservedFunction", "file": "source.cpp", "line": 7}]}]}
        result = capture_evidence.sampled_functions(ownership, [receipt])
        self.assertEqual(result["resolved_module_offset_count"], 1)
        self.assertEqual(result["examples"][0]["resolutions"][0]["frames"][0]["name"], "ObservedFunction")
        self.assertNotIn("cpu_ns", result["examples"][0])
        receipt["image"] = {"sha256": "b" * 64, "size": 100}
        self.assertEqual(capture_evidence.sampled_functions(ownership, [receipt])["unknown_module_offset_count"], 1)

    def test_symbol_resolution_disagreement_and_bounds_remain_unknown(self):
        image = {"sha256": "a" * 64, "size": 100, "image_size": 100}
        ownership = {"module_offset_aggregates": [{"module": {"module_id": "1", "file_identity": image}, "offset": "12"} for _ in range(130)]}
        receipt = {"status": "valid", "image": image, "binding": "embedded_dwarf_image_sha256", "function_ranges": [],
            "address_resolutions": [{"rva": "12", "status": "resolved", "frames": [{"name": "First"}]}]}
        result = capture_evidence.sampled_functions(ownership, [receipt])
        self.assertEqual(result["lookup_attempts"], 128)
        self.assertEqual(result["lookup_omitted_count"], 2)
        self.assertEqual(result["resolved_module_offset_count"] + result["unknown_module_offset_count"], 130)
        other = copy.deepcopy(receipt)
        other["address_resolutions"][0]["frames"][0]["name"] = "Second"
        result = capture_evidence.sampled_functions({"module_offset_aggregates": ownership["module_offset_aggregates"][:1]}, [receipt, other])
        self.assertEqual(result["resolved_module_offset_count"], 0)
        receipt["address_resolutions"] = []
        receipt["function_ranges"] = [{"start_rva": "0", "end_rva": "20", "name": "First"},
            {"start_rva": "5", "end_rva": "30", "name": "Second"}]
        result = capture_evidence.sampled_functions({"module_offset_aggregates": ownership["module_offset_aggregates"][:1]}, [receipt])
        self.assertEqual(result["unknown_module_offset_count"], 1)

    def shader_profile(self):
        artifact = {"kind": "draw_trace", "path": "traces/draw.json", "sha256": "b" * 64, "size": 100}
        self.draws["artifact"] = artifact
        self.document["dictionaries"][2]["fields"].pop("stage")
        target = {"operation": "20", "batch": "45", "command": "40", "pipeline": "104",
            "shader_variant": "103", "spirv": "102", "spirv_sha256": f"{102:064x}", "stage": "vertex"}
        process = {"pid": 7, "creation_filetime": "123", "executable_sha256": "c" * 64}
        row = {"id": "1", "name": "sample_count", "unit": "samples", "measurement_kind": "warp_samples",
            "status": "available", "value": 7, "shader_sha256": target["spirv_sha256"], "stage": "vertex",
            "shader_digest_scope": "anyps5.final-spirv.v1", "scope": "exported_shader_row_aggregate",
            "debug_label": f"aps5 run={self.manifest['run_id']} capture={self.document['capture_id']} op=20 batch=45 pipeline=104 command=40"}
        profile = {"document": {"kind": "gpu_profile", "capture_id": self.document["capture_id"], "process": process,
            "clock": {"domain": "unavailable_aggregated_shader_table", "ticks_per_second": 1},
            "records": {"shader_metrics": [row]}, "profile_request": {"native_artifact": artifact, "targets": [target]}},
            "artifact": {"path": "external/profile.json"}, "source_artifact": {"path": "external/source.csv"},
            "process_binding": {"status": "unknown", "identity_status": "valid"}, "source_integrity": {"status": "valid"},
            "extraction_provenance": {"status": "unknown", "transformation_status": "valid"}}
        observed = {"run_id": self.manifest["run_id"], "status": "valid", "process": {"pid": 7,
            "creation_filetime": "123", "identity_verified": True, "identity_binding": "retained_child_handle", "executable": {"sha256": "c" * 64}}}
        return profile, observed

    def test_aggregate_shader_rows_join_identity_without_inventing_clock_or_stage_elapsed(self):
        profile, observed = self.shader_profile()
        report = self.assess(external_evidence={"profiles": [profile]}, process_evidence=observed)
        self.assertEqual(report["external"]["profiles"][0]["matched"], 1)
        self.assertIn("unknown", report["external"]["profiles"][0]["stage_duration_status"])
        self.assertFalse(any("stage_elapsed" in name for name in report["metrics"]))

    def test_requested_shader_target_does_not_invent_exported_digest(self):
        profile, observed = self.shader_profile()
        profile["document"]["records"]["shader_metrics"][0]["shader_sha256"] = None
        report = self.assess(external_evidence={"profiles": [profile]}, process_evidence=observed)
        self.assertEqual(report["external"]["profiles"][0]["matched"], 0)

    def test_shader_target_graph_mismatch_is_invalid(self):
        profile, observed = self.shader_profile()
        profile["document"]["profile_request"]["targets"][0]["batch"] = "999"
        report = self.assess(external_evidence={"profiles": [profile]}, process_evidence=observed)
        self.assertEqual(report["dimensions"]["gpu_counters"]["status"], "invalid")


if __name__ == "__main__":
    unittest.main()
