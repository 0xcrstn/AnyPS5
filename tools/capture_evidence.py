import argparse
import html
import json
import math
import os
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict, deque
from fractions import Fraction
from pathlib import Path
from urllib.parse import quote

from capture_external import parse_debug_label
from capture_schema import SchemaError, atomic_write_json, validate_evidence_report


DIMENSIONS = ("dependency_graph", "clock_mapping", "cpu_elapsed", "thread_cpu", "thread_cycles", "cpu_sampling", "scheduler_running",
              "waits", "workload", "copies", "cache_activity", "compilation", "gpu_intervals", "gpu_counters",
              "external_presentations", "scene", "readiness", "cache_state", "correctness")
RELATIONS = {
    "contains": "containment", "executes": "execution_origin", "schedules": "scheduling", "schedules_job": "scheduling",
    "waits_for": "wait_observation", "waits_for_job": "wait_observation", "prepares_operation": "preparation_origin",
    "operation_batch": "recording_origin", "packet_batch": "recording_origin", "job_batch": "recording_origin",
    "submission_batch": "recording_origin", "records_in": "recording_membership", "queue_batch": "queue_membership",
    "queue_after": "submission_order", "flip_depends_on_batch": "outstanding_dependency_snapshot",
    "packet_flip": "flip_origin", "presents_flip": "presentation_origin", "indirect_record": "expansion_origin",
    "measures": "measurement", "presentation_path": "measurement", "rewinds_to": "control_flow",
}
COPY_COUNTERS = frozenset({"command_copy_bytes", "shader_capture_bytes", "copy_cpu_bytes", "copy_gpu_bytes",
                           "copy_alias_bytes", "fill_cpu_bytes", "fill_gpu_bytes", "fill_guest_write_elided_bytes"})
COMPILE_COUNTERS = frozenset({"resource_plan_builds", "shader_compilations"})
NOTE_NAMES = frozenset({"wait_reason", "wait_outcome", "cache_outcome", "draw_cache_outcome", "dispatch_cache_outcome",
                        "compile_outcome", "compiler_key", "compiler_key_domain", "dispatch_path", "prepare_only",
                        "skip_reason", "completion_domain", "label_id_loss", "draw_cache_verification_requested",
                        "dispatch_cache_validated", "wait_timeout_reported"})
NUMERIC_NOTES = frozenset({"prepare_only", "draw_cache_verification_requested", "dispatch_cache_validated", "wait_timeout_reported"})
MAX_GRAPH_NODES = 8_388_608
MAX_GRAPH_EDGES = 16_777_216
LIMITATIONS = [
    "Time-window membership is observed activity, never exclusive frame ownership.",
    "Dependency edges describe recorded relationships; they do not establish a complete causal graph or critical path.",
    "Uncovered host time means no retained selected scope covers that interval; it is not CPU idle time or missing work measured as zero.",
    "Interval unions cover observed selected scopes only; CPU and GPU coverage cannot be added into a frame-time breakdown.",
    "GPU clock bounds retain calibration uncertainty and unobserved drift; interpolation is a model estimate, not an exact clock conversion.",
    "Thread CPU counters are inclusive observations, distinct from elapsed wall time, scheduler-running intervals and sampled estimates.",
    "Hardware invocation counters and sampled shader-stage activity do not measure exclusive shader-stage execution time.",
    "Cache activity and compiler outcomes do not prove cache warmth, rendering correctness, scene equivalence or a performance gain.",
    "Copy-alias bytes describe an aliased range and guest-write-elided fill bytes describe guest-memory behavior; neither is measured transfer traffic or omitted GPU action.",
]


def interval_union(intervals, bounds=None):
    rows = []
    for first, last in intervals:
        if type(first) not in (int, float) or type(last) not in (int, float) or not math.isfinite(first) or not math.isfinite(last) or last < first:
            raise SchemaError("interval union requires ordered finite endpoints")
        if bounds is not None:
            first, last = max(first, bounds[0]), min(last, bounds[1])
        if last > first:
            rows.append((first, last))
    result = []
    for first, last in sorted(rows):
        if result and first <= result[-1][1]:
            result[-1][1] = max(last, result[-1][1])
        else:
            result.append([first, last])
    return result


def interval_uncovered(bounds, intervals):
    first, last = bounds
    if last < first:
        raise SchemaError("coverage bounds are reversed")
    cursor, result = first, []
    for opening, closing in interval_union(intervals, bounds):
        if opening > cursor:
            result.append([cursor, opening])
        cursor = max(cursor, closing)
    if cursor < last:
        result.append([cursor, last])
    return result


def _length(intervals):
    return sum(last - first for first, last in intervals)


def clip_union(intervals, bounds):
    start = bisect_right(intervals, [bounds[0], math.inf]) - 1
    result = []
    for index in range(max(0, start), len(intervals)):
        first, last = intervals[index]
        if first >= bounds[1]:
            break
        if last > bounds[0]:
            result.append([max(first, bounds[0]), min(last, bounds[1])])
    return result


def _clock_domain(domain):
    return {"windows_qpc": "query_performance_counter", "linux_monotonic": "clock_monotonic"}.get(domain, domain)


def _raw(value, name):
    if type(value) is not str or not value.isascii() or not value.isdigit() or str(int(value)) != value or not 0 <= int(value) < 2 ** 64:
        raise SchemaError(f"{name} must be a canonical uint64 decimal string")
    return int(value)


def _finite(value, name):
    if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
        raise SchemaError(f"{name} must be finite and nonnegative")
    return value


def _bounds(first, last, name):
    if type(first) is not int or type(last) is not int or last < first:
        raise SchemaError(f"{name} must be an ordered integer bracket")
    return first, last


def _round_bounds(first, last):
    return {"lower_ns": math.floor(first), "upper_ns": math.ceil(last)}


def native_clock_anchor(document):
    clock = document.get("clock")
    anchor = clock.get("native_anchor") if type(clock) is dict else None
    if type(anchor) is not dict or anchor.get("status") not in ("available", "complete", "measured"):
        return None
    domain = anchor.get("domain")
    if type(domain) is not str or not domain:
        raise SchemaError("native clock anchor domain is invalid")
    ticks = _raw(anchor.get("ticks"), "native clock anchor ticks")
    frequency = _raw(anchor.get("frequency"), "native clock anchor frequency")
    if frequency == 0:
        raise SchemaError("native clock anchor frequency must be positive")
    first, last = _bounds(anchor.get("steady_before_ns"), anchor.get("steady_after_ns"), "native clock anchor")
    return {"domain": _clock_domain(domain), "ticks": ticks, "frequency": frequency, "lower": Fraction(first), "upper": Fraction(last)}


def map_host_ticks(ticks, domain, frequency, anchor):
    if anchor is None:
        return None
    if _clock_domain(domain) == anchor["domain"] and frequency == anchor["frequency"]:
        offset = Fraction((ticks - anchor["ticks"]) * 1_000_000_000, frequency)
    elif domain == "windows_qpc_ns" and anchor["domain"] == "query_performance_counter" and frequency == 1_000_000_000:
        offset = Fraction(ticks) - Fraction(anchor["ticks"] * 1_000_000_000, anchor["frequency"])
    else:
        return None
    return _round_bounds(anchor["lower"] + offset - 1, anchor["upper"] + offset + 1)


def calibration_anchors(document):
    anchor = native_clock_anchor(document)
    if anchor is None:
        return {}, ["raw native host-clock anchor is unavailable"]
    result, unavailable = defaultdict(list), []
    fields = ("calibration_gpu_ticks", "calibration_host_ticks", "calibration_conversion_host_ticks",
              "calibration_host_frequency", "calibration_steady_before_ns", "calibration_steady_after_ns")
    for index, event in enumerate(document.get("events", [])):
        if event["kind"] != "calibration":
            continue
        data = event["data"]
        if event["status"] != "complete" or any(data.get(key) is None for key in fields):
            unavailable.append(f"calibration {event['id']} lacks a complete raw anchored sample")
            continue
        gpu = _raw(data["calibration_gpu_ticks"], "calibration GPU ticks")
        host = _raw(data["calibration_host_ticks"], "calibration host ticks")
        conversion = _raw(data["calibration_conversion_host_ticks"], "calibration conversion ticks")
        frequency = _raw(data["calibration_host_frequency"], "calibration host frequency")
        domain = data.get("calibration_host_domain")
        if frequency != anchor["frequency"] or _clock_domain(domain) != anchor["domain"] or data.get("valid_bits") != 64:
            unavailable.append(f"calibration {event['id']} clock domain, frequency or timestamp width is unsupported")
            continue
        first, last = _bounds(data["calibration_steady_before_ns"], data["calibration_steady_after_ns"], "calibration conversion")
        deviation = Fraction(str(_finite(data.get("deviation_ns"), "Vulkan calibration deviation")))
        period = _finite(data.get("period_ns"), "GPU timestamp period")
        if period == 0:
            raise SchemaError("GPU timestamp period must be positive")
        offset = Fraction((host - conversion) * 1_000_000_000, frequency)
        local_first, local_last = Fraction(first) + offset - deviation - 1, Fraction(last) + offset + deviation + 1
        global_bounds = map_host_ticks(host, domain, frequency, anchor)
        global_first, global_last = Fraction(global_bounds["lower_ns"]) - deviation, Fraction(global_bounds["upper_ns"]) + deviation
        if local_last < global_first or global_last < local_first:
            raise SchemaError("calibration raw host sample contradicts its trace-origin anchor")
        device = event["context"].get("device")
        if device is None:
            unavailable.append(f"calibration {event['id']} has no recorded device identity")
            continue
        result[device].append({"event_id": event["id"], "gpu_ticks": gpu, "host_lower": max(local_first, global_first),
                               "host_upper": min(local_last, global_last), "period_ns": period,
                               "source": {"event_id": event["id"], "json_pointer": f"/events/{index}"}})
    for device, samples in result.items():
        samples.sort(key=lambda sample: sample["host_lower"] + sample["host_upper"])
        base = samples[0]["gpu_ticks"]
        previous = None
        for sample in samples:
            position = (sample["gpu_ticks"] - base) & (2 ** 64 - 1)
            if position >= 2 ** 63 or previous is not None and position <= previous:
                raise SchemaError("calibration GPU clock sequence is ambiguous or nonmonotonic")
            if not math.isclose(sample["period_ns"], samples[0]["period_ns"], rel_tol=1e-12, abs_tol=0):
                raise SchemaError("calibration timestamp period changes for the same device")
            sample["position"] = position
            previous = position
    return dict(result), unavailable


def map_gpu_interval(query, anchors):
    result = {"status": "unknown", "reasons": [], "exact": False, "start": None, "end": None,
              "meaning": "bounded host-clock activity estimate; not frame ownership"}
    if query.get("status") != "available" or query.get("duration_origin") != "integer_tick_delta":
        result["reasons"].append("available raw tick-delta query evidence is unavailable")
        return result
    if len(anchors) < 2:
        result["reasons"].append("two same-device raw calibration samples are required")
        return result
    period = _finite(query.get("period_ns"), "query timestamp period")
    if (period == 0 or query.get("timestamp_valid_bits") != 64
            or any(not math.isclose(period, sample["period_ns"], rel_tol=1e-12, abs_tol=0) for sample in anchors)):
        raise SchemaError("query timestamp period or width contradicts its device calibration")
    base = anchors[0]["gpu_ticks"]
    first = (_raw(query.get("begin_gpu_ticks"), "query start GPU ticks") - base) & (2 ** 64 - 1)
    duration = (_raw(query.get("end_gpu_ticks"), "query end GPU ticks") - _raw(query["begin_gpu_ticks"], "query start")) & (2 ** 64 - 1)
    if first >= 2 ** 63:
        first -= 2 ** 64
    last = first + duration
    coordinates = [sample["position"] for sample in anchors]

    def point(position):
        if position < coordinates[0] or position > coordinates[-1]:
            return None
        exact = bisect_left(coordinates, position)
        if exact < len(coordinates) and coordinates[exact] == position:
            sample = anchors[exact]
            return {**_round_bounds(sample["host_lower"], sample["host_upper"]),
                    "estimate_ns": float((sample["host_lower"] + sample["host_upper"]) / 2),
                    "anchor_event_ids": [sample["event_id"]]}
        right = bisect_right(coordinates, position)
        left_sample, right_sample = anchors[right - 1], anchors[right]
        ratio = Fraction(position - left_sample["position"], right_sample["position"] - left_sample["position"])
        estimate = (1 - ratio) * (left_sample["host_lower"] + left_sample["host_upper"]) / 2
        estimate += ratio * (right_sample["host_lower"] + right_sample["host_upper"]) / 2
        return {**_round_bounds(left_sample["host_lower"], right_sample["host_upper"]), "estimate_ns": float(estimate),
                "anchor_event_ids": [left_sample["event_id"], right_sample["event_id"]]}

    start, end = point(first), point(last)
    if start is None or end is None:
        result["reasons"].append("query is outside the observed calibration bracket; extrapolation is unavailable")
        return result
    result.update(status="valid", start=start, end=end, method="monotonic_bracketing_bounds",
                  interpolation="model estimate only; unobserved inter-anchor drift is not tightly bounded")
    return result


def _dag(nodes, edges):
    outgoing, incoming = defaultdict(list), Counter()
    for edge in edges:
        first, last = (edge["from"], edge["to"]) if type(edge) is dict else edge
        outgoing[first].append(last)
        incoming[last] += 1
    ready = deque(identifier for identifier in nodes if incoming[identifier] == 0)
    visited = 0
    while ready:
        identifier = ready.popleft()
        visited += 1
        for target in outgoing[identifier]:
            incoming[target] -= 1
            if incoming[target] == 0:
                ready.append(target)
    return visited == len(nodes)


def sampled_functions(ownership, receipts):
    from capture_symbols import resolve_symbol_address
    result = {"status": "unknown", "resolved_module_offset_count": 0, "unknown_module_offset_count": 0,
        "lookup_limit": 128, "lookup_attempts": 0, "lookup_omitted_count": 0, "examples": [],
        "meaning": "bounded sampled image-RVA candidates; recorded addr2line outputs and mangled explicit range names retain separate provenance; not function elapsed or self CPU time"}
    by_image = defaultdict(list)
    for receipt in receipts:
        verified = receipt.get("assessment") or receipt
        image = verified.get("image") or {}
        if verified.get("status") == "valid":
            by_image[(image.get("sha256"), image.get("size"))].append((receipt, verified))
    for row in (ownership or {}).get("module_offset_aggregates", []):
        image = row["module"].get("file_identity") or {}
        matches, unknown = [], False
        for receipt, verified in by_image[(image.get("sha256"), image.get("size"))]:
            if result["lookup_attempts"] >= result["lookup_limit"]:
                result["lookup_omitted_count"] += 1
                unknown = True
                break
            result["lookup_attempts"] += 1
            offset = int(row["offset"])
            duplicate_exact = sum(resolution.get("status") == "resolved" and int(resolution["rva"]) == offset
                                  for resolution in verified.get("address_resolutions", [])) > 1
            resolution = resolve_symbol_address(verified, offset) if not duplicate_exact else {"status": "unknown"}
            if resolution["status"] == "resolved":
                matches.append({**resolution, "artifact": receipt.get("artifact")})
            else:
                unknown = True
        signatures = {json.dumps({key: value for key, value in match.items() if key != "artifact"}, sort_keys=True) for match in matches}
        resolved = bool(matches) and len(signatures) == 1 and not unknown
        result["resolved_module_offset_count" if resolved else "unknown_module_offset_count"] += 1
        if len(result["examples"]) < 32:
            result["examples"].append({"module_id": row["module"]["module_id"], "offset": row["offset"],
                "sample_hits": row.get("sample_hits"), "weighted_sample_hits": row.get("weighted_sample_hits"),
                "resolutions": matches, "status": "resolved_candidate" if resolved else "unknown",
                "source_examples": row.get("source_examples", [])})
    if result["resolved_module_offset_count"] and not result["unknown_module_offset_count"]:
        result["status"] = "valid"
    return result


def assess_evidence(document, manifest, draw_attribution, *, external_evidence=None, process_evidence=None,
                    condition_evidence=None, log_review=None, draw_index=None):
    include_detail = "index" in draw_attribution
    report = {"schema_version": 1, "document_type": "anyps5.evidence-report", "run_id": manifest["run_id"],
              "capture_id": document.get("capture_id"), "status": "unknown", "issues": [],
              "dimensions": {name: {"status": "unknown", "reasons": ["independent evidence is unavailable"]} for name in DIMENSIONS},
              "metrics": {}, "metric_availability": {}, "metric_units": {}, "frames": [], "operations": [],
              "dependency_graph": {}, "clock_mapping": {}, "external": {}, "limitations": list(LIMITATIONS)}
    report["report_detail"] = "full" if include_detail else "compact"

    def dimension(name, status, *reasons):
        if report["dimensions"][name]["status"] == "invalid" and status != "invalid":
            return
        report["dimensions"][name] = {"status": status, "reasons": list(reasons)}

    def issue(code, message, area=None, invalid=False):
        report["issues"].append({"code": code, "message": message, "severity": "error" if invalid else "warning"})
        if area:
            dimension(area, "invalid" if invalid else "unknown", message)

    def metric(name, value, unit, availability="valid"):
        report["metrics"][name] = _finite(value, name)
        report["metric_availability"][name] = availability
        report["metric_units"][name] = unit

    if condition_evidence is not None:
        if condition_evidence.get("run_id") != manifest["run_id"]:
            issue("evidence_conditions_binding", "Condition review belongs to a different run.", "scene", True)
        else:
            report["conditions"] = condition_evidence
            for name in ("scene", "readiness", "correctness"):
                value = condition_evidence.get("dimensions", {}).get(name, {})
                dimension(name, value.get("status", "unknown"), *(value.get("reasons") or
                    ["Reviewed claim with bound artifacts; semantic truth is not automatically proved."]))
                report["dimensions"][name]["basis"] = value.get("basis", "unavailable")
            cache_values = [condition_evidence.get("dimensions", {}).get(f"cache.{name}", {})
                            for name in ("shader", "pipeline", "driver")]
            cache_status = "invalid" if any(value.get("status") == "invalid" for value in cache_values) else (
                "valid" if all(value.get("status") == "valid" for value in cache_values) else "unknown")
            dimension("cache_state", cache_status, "Cache states retain explicit reviewed-claim scope; application files do not prove global driver cache state.")
            report["dimensions"]["cache_state"]["basis"] = "reviewed_claim"
    if log_review is not None:
        report["log_review"] = log_review
    process = process_evidence or {}
    report["process_resources"] = {"status": process.get("status", "unknown"), "scope": "run-wide sampled observations, not per-frame",
        "resources": process.get("resources", {}), "coverage": process.get("coverage", {}), "artifact": process.get("artifact")}
    for name, value in process.get("resources", {}).items():
        unit = "bytes" if name.endswith("_bytes") else "100ns" if name.endswith("_100ns") else "ticks" if name.endswith("_ticks") else "count"
        for statistic in ("minimum", "maximum"):
            if type(value.get(statistic)) in (int, float) and value[statistic] >= 0:
                metric(f"process_run.{name}.{statistic}", value[statistic], unit, "valid" if process.get("coverage", {}).get("raw_samples_verified") is True else "unknown")

    if (document.get("run_id") != manifest["run_id"] or draw_attribution.get("run_id") != manifest["run_id"]
            or draw_attribution.get("capture_id") != document.get("capture_id") or draw_attribution.get("correlation_valid") is not True):
        issue("evidence_binding_invalid", "Native evidence does not bind the freshly assessed run and capture.", "dependency_graph", True)
        report["status"] = "invalid"
        return report
    index = draw_index if draw_index is not None else draw_attribution.get("index") or {}
    spans = index.get("spans", [])
    operations = index.get("draws", [])
    commands = index.get("host_commands", [])
    queries = index.get("queries", [])
    notes, kinds, sources, contexts = defaultdict(lambda: defaultdict(list)), {}, {}, {}
    needed = {identifier for event in document.get("events", []) if event["kind"] == "link" and event.get("name") in RELATIONS
              for identifier in (event.get("subject"), event.get("related")) if identifier is not None}
    operation_ids = {row["operation_id"] for row in operations} if include_detail else set()
    cache_counts = Counter()
    dictionaries = {row["id"]: row for row in document.get("dictionaries", [])}
    event_links, counter_records, present_ordinals = [], [], defaultdict(set)
    query_devices, outcomes, terminal_ends = {}, {}, {}
    for position, event in enumerate(document.get("events", [])):
        subject = event.get("subject")
        if event["kind"] not in ("link", "attribute", "reference", "counter", "calibration") and subject is not None and (subject in needed or event["kind"] == "host_command"):
            previous = kinds.get(subject)
            if previous is not None and previous != event["kind"]:
                issue("evidence_entity_conflict", "Native entity kind changes across lifecycle records.", "dependency_graph", True)
            kinds[subject] = event["kind"]
            contexts[subject] = event["context"]
            if include_detail:
                sources.setdefault(subject, {"event_id": event["id"], "json_pointer": f"/events/{position}"})
            if event["status"] not in ("unknown", "pending"):
                outcomes[subject] = event["status"]
                terminal_ends[subject] = event.get("end_ns")
        if (event["kind"] == "reference" and event["status"] == "complete" and event.get("text") == "identity_only_no_lifecycle"
                and event.get("name") in ("driver", "device", "recorder", "native_queue") and subject in needed):
            kinds.setdefault(subject, event["name"])
            if include_detail:
                sources.setdefault(subject, {"event_id": event["id"], "json_pointer": f"/events/{position}"})
        if event["kind"] == "attribute" and event.get("name") in NOTE_NAMES and subject is not None:
            value = event["value"] if event["name"] in NUMERIC_NOTES else event.get("text")
            if subject in needed or subject in operation_ids:
                notes[subject][event["name"]].append({"value": value, "source": {"event_id": event["id"], "json_pointer": f"/events/{position}"}})
            if event["name"] in ("cache_outcome", "draw_cache_outcome", "dispatch_cache_outcome") and type(value) is str:
                cache_counts[f"{event['name']}.{value}"] += 1
        if event["kind"] == "link" and subject is not None and event.get("related") is not None:
            event_links.append(event)
        if event["kind"] == "counter":
            counter_records.append((position, event))
        if event["kind"] == "gpu_query" and subject is not None:
            query_devices[subject] = event["context"].get("device")
        if event["kind"] == "present" and subject is not None and event["data"].get("present_ordinal_status") == "observed":
            ordinal = _raw(event["data"].get("present_ordinal"), "observed present ordinal")
            present_ordinals[ordinal].add(subject)
    nodes, edges, relations = {}, [], []
    native_queues = defaultdict(set)
    for event in event_links:
        if event.get("name") == "queue_batch":
            native_queues[event["related"]].add(event["subject"])
    origin_capacity_loss = sum(event["value"] for _, event in counter_records if event.get("name") == "batch_origin_capacity_dropped")
    graph_unknown = bool(origin_capacity_loss) or draw_attribution.get("origin_graph_status") != "valid"
    if origin_capacity_loss:
        issue("evidence_batch_origin_capacity", "Batch origin membership exceeded the declared bounded table; dependency coverage remains incomplete.", "dependency_graph")
    for event in event_links:
        relation = event.get("name")
        if relation not in RELATIONS:
            continue
        first, last = event["subject"], event["related"]
        for identifier in (first, last):
            if identifier not in nodes:
                nodes[identifier] = ({"id": identifier, "kind": kinds.get(identifier, "external_or_metadata"),
                                     "source": sources.get(identifier), "status": "observed" if identifier in kinds else "unknown"} if include_detail else None)
        category = RELATIONS[relation]
        row = {"from": first, "to": last, "relation": relation, "category": category, "source_event_id": event["id"]}
        if include_detail:
            relations.append(row)
        if first not in kinds or last not in kinds:
            graph_unknown = True
            continue
        expected_kinds = {"operation_batch": ("operation", "batch"), "packet_batch": ("packet", "batch"),
            "job_batch": ("job", "batch"), "submission_batch": ("submission", "batch"),
            "packet_flip": ("packet", "flip"), "flip_depends_on_batch": ("batch", "flip"),
            "presents_flip": ("flip", "present"), "indirect_record": ("operation", "operation"),
            "queue_batch": ("native_queue", "batch")}.get(relation)
        if expected_kinds is not None and (kinds[first], kinds[last]) != expected_kinds:
            issue("evidence_relation_kind_invalid", f"{relation} contradicts known endpoint entity kinds.", "dependency_graph", True)
            continue
        if relation == "records_in" and (kinds[first] != "host_command" or kinds[last] != "batch"
                or contexts[first].get("batch") != last):
            issue("evidence_recording_join_invalid", "records_in contradicts its host command batch context.", "dependency_graph", True)
            continue
        if relation == "queue_after":
            first_queues, last_queues = native_queues[first], native_queues[last]
            if (kinds[first] != "batch" or kinds[last] != "batch" or len(first_queues) > 1 or len(last_queues) > 1
                    or first_queues and last_queues and first_queues != last_queues
                    or contexts[first].get("device") != contexts[last].get("device")):
                issue("evidence_queue_join_invalid", "Queue order contradicts explicit native queue/device membership.", "dependency_graph", True)
                continue
            if not first_queues or not last_queues:
                graph_unknown = True
        if category in ("measurement", "control_flow", "wait_observation"):
            if category == "wait_observation":
                values = {entry["value"] for entry in notes[first].get("wait_outcome", [])}
                if (outcomes.get(last) == "complete" and values & {"satisfied", "completed", "complete", "ready"}
                        and terminal_ends.get(last) is not None and terminal_ends.get(first) is not None
                        and terminal_ends[last] <= terminal_ends[first]):
                    edges.append({**row, "from": last, "to": first, "category": "observed_wait_completion"} if include_detail else (last, first))
                else:
                    graph_unknown = True
            continue
        edges.append(row if include_detail else (first, last))
    if len(nodes) > MAX_GRAPH_NODES or len(edges) > MAX_GRAPH_EDGES:
        issue("evidence_graph_capacity", "Dependency graph exceeds its explicit supported capacity.", "dependency_graph", True)
    elif not _dag(nodes, edges):
        issue("evidence_dependency_cycle", "Recorded dependency/containment edges contain a cycle; no dependency DAG can be claimed.", "dependency_graph", True)
    else:
        dimension("dependency_graph", "unknown" if graph_unknown or not edges else "valid",
                  "Only explicit recorded relationships are represented; completeness and critical path remain unavailable.")
    report["dependency_graph"] = {"node_count": len(nodes), "edge_count": len(edges),
        "scope": "explicit recorded link events; nested host-span containment remains in the native draw index",
        "critical_path_status": "unknown", "ownership_status": "unknown"}
    if include_detail:
        report["dependency_graph"].update(nodes=list(nodes.values()), edges=edges, observed_relations=relations)
    else:
        report["dependency_graph"]["detail_omitted"] = "Full dependency rows are opt-in; all recorded links were validated."
        edges.clear()
        nodes.clear()
    try:
        native_anchor = native_clock_anchor(document)
        calibrations, calibration_unknown = calibration_anchors(document)
        mappings, mapped = {}, 0
        for row in queries:
            mapping = map_gpu_interval(row, calibrations.get(query_devices.get(row["query_id"]), []))
            mapped += mapping["status"] == "valid"
            if include_detail:
                mappings[row["query_id"]] = mapping
        dimension("clock_mapping", "valid" if queries and mapped == len(queries) else "unknown",
                  *(calibration_unknown or ["Mapping retains conservative monotonic bounds; interpolation is a model only."]))
        metric("clock.mapped_query_count", mapped, "count")
        metric("clock.unmapped_query_count", len(queries) - mapped, "count")
        report["clock_mapping"] = {"query_count": len(queries), "mapped_query_count": mapped, "exact": False,
            "method": "raw same-device calibration with native host anchor and conservative monotonic brackets"}
        if include_detail:
            report["clock_mapping"]["queries"] = mappings
    except (ValueError, OverflowError) as error:
        native_anchor, mappings = None, {}
        issue("evidence_clock_invalid", str(error), "clock_mapping", True)
    per_thread, wait_intervals, cpu_functions = defaultdict(list), defaultdict(list), defaultdict(list)
    for span in spans:
        per_thread[span["thread_id"]].append((span["start_ns"], span["end_ns"]))
        if span["classification"] == "wait" or notes[span["span_id"]].get("wait_reason"):
            wait_intervals[span["thread_id"]].append((span["start_ns"], span["end_ns"]))
        cpu_functions[span["function"]].append(span.get("thread_cpu") or {"status": "not_requested", "cpu_ns": None})
    unions = {identifier: interval_union(rows) for identifier, rows in per_thread.items()}
    host_union = interval_union(row for rows in unions.values() for row in rows)
    waits = {identifier: interval_union(rows) for identifier, rows in wait_intervals.items()}
    dimension("cpu_elapsed", draw_attribution.get("cpu_status", "unknown"), "Elapsed scope coverage follows independent native validation.")
    if waits:
        dimension("waits", "valid" if draw_attribution.get("cpu_status") == "valid" else "unknown",
                  "Named waits include execution, scheduling and bookkeeping; no blocked/scheduled split is inferred.")
    cpu_summary = draw_attribution.get("summary", {}).get("thread_cpu_functions")
    if cpu_summary is None:
        cpu_summary = {}
        for name, rows in cpu_functions.items():
            measured = [row for row in rows if row.get("status") == "measured" and row.get("native_thread_binding") == "valid"]
            cpu_summary[name] = {"scopes": len(rows), "measured": len(measured),
                "cpu_ns": math.fsum(row["cpu_ns"] for row in measured), "cycles_measured": 0, "cycles": 0}
    measured_cpu = sum(row["measured"] for row in cpu_summary.values())
    for name, row in cpu_summary.items():
        if draw_attribution.get("function_availability", {}).get(name) == "valid":
            if row["measured"] == row["scopes"]:
                metric(f"thread_cpu.{name}.inclusive_ns", row["cpu_ns"], "ns")
            if row["cycles_measured"] == row["scopes"]:
                metric(f"thread_cycles.{name}.inclusive_cycles", row["cycles"], "cycles")
    for dimension_name, count_key in (("thread_cpu", "measured"), ("thread_cycles", "cycles_measured")):
        dimension(dimension_name, "valid" if cpu_summary and all(row[count_key] == row["scopes"]
            and draw_attribution.get("function_availability", {}).get(name) == "valid"
            for name, row in cpu_summary.items()) else "unknown",
            "Inclusive selected-scope observations require complete function coverage and bound native thread IDs; unsampled functions, self CPU and cycle-to-time conversion remain unavailable.")
    if measured_cpu:
        metric("thread_cpu.measured_scope_count", measured_cpu, "count")
    for frame in index.get("present_intervals", []) if include_detail else []:
        bounds = (frame["start_ns"], frame["end_ns"])
        covered = clip_union(host_union, bounds)
        uncovered = interval_uncovered(bounds, covered)
        ordinal = int(frame["opening_present"])
        report["frames"].append({"interval_index": frame["interval_index"], "opening_present": frame["opening_present"],
            "closing_present": frame["closing_present"], "start_ns": bounds[0], "end_ns": bounds[1],
            "host_scope_union_ns": _length(covered), "host_scope_uncovered_ns": _length(uncovered),
            "host_scope_uncovered_intervals": uncovered,
            "thread_scope_union_ns": {identifier: _length(clip_union(rows, bounds)) for identifier, rows in unions.items()},
            "named_wait_union_ns_by_thread": {identifier: _length(clip_union(rows, bounds)) for identifier, rows in waits.items()},
            "explicit_present_entity_ids": sorted(present_ordinals.get(ordinal, set()), key=int),
            "opening_present_entity_ids": sorted(present_ordinals.get(ordinal, set()), key=int),
            "closing_present_entity_ids": sorted(present_ordinals.get(int(frame["closing_present"]), set()), key=int),
            "causal_frame_membership": "unknown", "gpu_uncovered_time": None,
            "meaning": "selected host-clock activity and explicit endpoint IDs; no exclusive workload ownership"})
    if not include_detail:
        report["frames"] = draw_attribution.get("summary", {}).get("frames", [])
        report["summary"] = {key: value for key, value in draw_attribution.get("summary", {}).items() if key != "frames"}
        if any(frame["named_wait_union_ns_by_thread"] for frame in report["frames"]):
            dimension("waits", draw_attribution.get("cpu_status", "unknown"), "Named selected wait intervals; no scheduler/blocking split is inferred.")
    counter_groups, additive, operation_counters = defaultdict(list), Counter(), defaultdict(list)
    for position, event in counter_records:
        data, name, subject = event["data"], event.get("name"), event.get("subject")
        availability = data.get("availability")
        if availability == "observed":
            value = event.get("value")
            if type(value) is not int or value < 0 or not event.get("text"):
                issue("evidence_counter_invalid", "Observed workload counter lacks a value or unit.", "workload", True)
                continue
            row = {"name": name, "value": value, "unit": event["text"], "availability": "observed", "entity_id": subject,
                   "source": {"event_id": event["id"], "json_pointer": f"/events/{position}"}}
            operation = event["context"].get("operation")
            if operation is not None and include_detail:
                operation_counters[operation].append(row)
            if name in COPY_COUNTERS or name in COMPILE_COUNTERS or name == "indirect_argument_records_read":
                expected_unit = "bytes" if name in COPY_COUNTERS else "attempts" if name in COMPILE_COUNTERS else "records"
                if event["text"] != expected_unit:
                    issue("evidence_counter_unit_invalid", f"Observed counter {name} has a contradictory unit.", "workload", True)
                    continue
                additive[(name, event["text"])] += value
        else:
            counter_groups[(subject, name)].append(event)
    capture = document.get("capture", {})
    intact = (draw_attribution.get("coverage", {}).get("lost_event_count", 0) == 0
              and document.get("losses", {}).get("metadata", 0) == 0
              and draw_attribution.get("coverage", {}).get("boundaries") == "valid"
              and draw_attribution.get("coverage", {}).get("threads") == "valid"
              and capture.get("close_reason") == "window_closed"
              and capture.get("pending_spans", 0) == 0 and capture.get("pending_jobs", 0) == 0)
    workload = draw_attribution.get("metrics", {}).get("workload", {})
    if intact and workload:
        dimension("workload", "valid", "Complete counts cover retained instrumented operations and emitted host commands; they do not prove scene equivalence.")
        for name, value in workload.items():
            metric(f"workload.{name}", value, "count")
    for (name, unit), value in additive.items():
        category = "copies" if name in COPY_COUNTERS else "compilation" if name in COMPILE_COUNTERS else "workload"
        if category == "workload":
            metric(f"workload_counters.{name}", value, unit, "valid" if intact else "unknown")
            continue
        metric(f"{category}.{name}", value, unit, "valid" if intact else "unknown")
        dimension(category, "valid" if intact else "unknown", "Only explicitly observed additive counters are summed.")
    hardware_rows, hardware_counts, hardware_total, hardware_complete = [], Counter(), 0, True
    for (subject, name), records in counter_groups.items():
        pending = [row for row in records if row["data"].get("availability") == "pending"]
        terminal = [row for row in records if row["data"].get("availability") != "pending"]
        if len(pending) > 1 or len(terminal) > 1:
            issue("evidence_hardware_counter_duplicate", "Hardware counter has duplicate request or result records.", "gpu_counters", True)
            continue
        event = terminal[0] if terminal else records[0]
        if pending and terminal and any(pending[0].get(key) != terminal[0].get(key) for key in ("subject", "name", "text", "context")):
            issue("evidence_hardware_counter_binding", "Hardware counter lifecycle changes immutable command, unit or context.", "gpu_counters", True)
            continue
        rejected_request = event["data"].get("availability") in ("unsupported", "allocation_failed", "capacity_dropped")
        if not pending and intact and not rejected_request:
            issue("evidence_hardware_counter_registration", "Hardware counter result lacks a recorded request.", "gpu_counters", True)
            continue
        value = event["data"].get("counter_value")
        if event["data"].get("availability") == "available":
            value = _raw(value, "available hardware counter")
        elif value is not None:
            issue("evidence_hardware_counter_unavailable_value", "Unavailable hardware counter contains a fabricated value.", "gpu_counters", True)
            value = None
        hardware_total += 1
        hardware_counts[f"{name}.{event['data'].get('availability')}"] += 1
        hardware_complete = hardware_complete and bool(pending and terminal) and event["data"].get("availability") == "available"
        if include_detail:
            hardware_rows.append({"entity_id": subject, "name": name, "value": value, "unit": event.get("text"),
                              "availability": event["data"].get("availability"), "lifecycle_complete": bool(pending and terminal),
                              "request_rejected": rejected_request})
    if hardware_total and report["dimensions"]["gpu_counters"]["status"] != "invalid":
        complete = intact and hardware_complete
        dimension("gpu_counters", "valid" if complete else "unknown", "Hardware invocation counters are not shader-stage durations.")
    if cache_counts:
        dimension("cache_activity", "valid" if intact else "unknown", "Observed cache outcomes do not establish cache warmth or correctness.")
        for name, count in cache_counts.items():
            metric(f"cache_activity.{name}", count, "count", "valid" if intact else "unknown")
    span_map = {row["span_id"]: row for row in spans}
    query_map = {row["query_id"]: row for row in queries}
    report["sources"] = {"native_artifact": draw_attribution.get("artifact"),
        "canonical_events": "native sidecar /events; exact source pointers retain record identity"}
    report["operations"] = [{"operation_id": row["operation_id"], "status": row["status"], "kind": row["kind"],
        "host_span_ids": row["span_ids"], "gpu_query_ids": row["gpu_query_ids"], "pipeline_ids": row["pipeline_ids"],
        "shader_ids": row["shader_ids"], "observed_counters": operation_counters.get(row["operation_id"], []),
        "notes": dict(notes.get(row["operation_id"], {})), "source_events": row["source_events"],
        "time_window_membership": row.get("time_window_membership"), "frame_ownership": "unknown",
        "selected_cpu_scopes": [{key: span_map[identifier].get(key) for key in
            ("span_id", "function", "start_ns", "end_ns", "wall_ns", "self_ns", "classification", "thread_cpu", "thread_cycles", "source")}
            for identifier in row["span_ids"] if identifier in span_map],
        "whole_command_gpu_intervals": [{**query_map[identifier], "host_clock_mapping": mappings.get(identifier)}
            for identifier in row["gpu_query_ids"] if identifier in query_map],
        "shader_identities": [dictionaries[identifier] for identifier in row["shader_ids"] if identifier in dictionaries],
        "pipeline_identities": [dictionaries[identifier] for identifier in row["pipeline_ids"] if identifier in dictionaries]}
        for row in operations] if include_detail else []
    report["index_counts"] = {"frames": draw_attribution.get("summary", {}).get("frame_count", len(report["frames"])),
        "operations": len(operations), "dependency_nodes": report["dependency_graph"].get("node_count", 0),
        "dependency_edges": report["dependency_graph"].get("edge_count", 0), "gpu_clock_queries": len(queries)}
    report.setdefault("summary", {})["hardware_counter_availability"] = dict(hardware_counts)
    dimension("gpu_intervals", draw_attribution.get("gpu_status", "unknown"), "Recorded whole-command/shared intervals remain distinct from exclusive shader-stage costs.")
    external = external_evidence or {"profiles": [], "issues": [], "status": "unknown"}
    for finding in external.get("issues", []):
        report["issues"].append(dict(finding))
    external_summary, external_records = [], []
    command_map = {row["command_id"]: row for row in commands}
    operation_map = {row["operation_id"]: row for row in operations}
    for profile in external.get("profiles", []):
        value = profile["document"]
        if value.get("capture_id") is not None and value["capture_id"] != document["capture_id"]:
            issue("evidence_external_capture_binding", "External profile declares a different capture UUID.", "gpu_counters", True)
            continue
        clock = value["clock"]
        binding_valid = profile["process_binding"]["status"] == "valid"
        summary = {"kind": value["kind"], "artifact": profile["artifact"], "source_artifact": profile["source_artifact"],
            "process_binding": profile["process_binding"], "extraction_provenance": profile["extraction_provenance"],
            "record_counts": {name: len(rows) for name, rows in value["records"].items()}, "matched": 0, "unmatched": 0}
        if value["kind"] == "cpu_profile":
            samples = value["records"].get("samples", [])
            running = value["records"].get("context_switches", [])
            mapped_samples = sum(binding_valid and map_host_ticks(int(row["timestamp_ticks"]), clock["domain"], clock["ticks_per_second"], native_anchor) is not None for row in samples)
            metric("external.cpu_samples.imported_records", len(samples), "count")
            dimension("cpu_sampling", "valid" if samples and mapped_samples == len(samples) and profile["extraction_provenance"]["status"] == "valid" else "unknown",
                      "Sampling is an estimate; normalized extraction and process/clock identity remain explicit.")
            running_rows, running_by_thread = [], defaultdict(list)
            for row in running:
                first = map_host_ticks(int(row["start_ticks"]), clock["domain"], clock["ticks_per_second"], native_anchor) if binding_valid else None
                last = map_host_ticks(int(row["end_ticks"]), clock["domain"], clock["ticks_per_second"], native_anchor) if binding_valid else None
                running_rows.append({"source_record": row["id"], "native_thread_id": row["thread_id"], "cpu": row["cpu"],
                    "start": first, "end": last, "availability": "observed_normalized_interval" if first and last else "unknown"})
                if first and last:
                    running_by_thread[row["thread_id"]].append((first["lower_ns"], last["upper_ns"]))
            scheduler_union = {identifier: interval_union(rows) for identifier, rows in running_by_thread.items()}
            dimension("scheduler_running", "unknown", "Observed normalized running intervals have bounded clock mappings; extraction and exhaustive scheduler coverage remain unverified.")
            summary.update(mapped_samples=mapped_samples, running_intervals=len(running), symbol_resolution="unknown",
                cpu_sample_ownership=profile.get("cpu_sample_ownership"), symbol_receipts=profile.get("symbol_receipts", []))
            summary["sampled_functions"] = sampled_functions(profile.get("cpu_sample_ownership"), profile.get("symbol_receipts", []))
            summary["symbol_resolution"] = summary["sampled_functions"]["status"]
            external_records.append({"artifact": profile["artifact"], "scheduler_running_intervals": running_rows,
                "observed_running_union_ns_by_native_thread": {identifier: _length(rows) for identifier, rows in scheduler_union.items()},
                "sample_ownership": profile.get("cpu_sample_ownership"), "interpretation": "sampled range candidates and normalized scheduler observations; no function elapsed or blocked-time inference"})
        elif value["kind"] == "presentation_profile":
            presentations = value["records"].get("presentations", [])
            mapped = sum(binding_valid and map_host_ticks(int(row["timestamp_ticks"]), clock["domain"], clock["ticks_per_second"], native_anchor) is not None for row in presentations)
            summary.update(mapped_presentations=mapped, native_present_join="unavailable: external display IDs are different identifiers")
            metric("external.presentations.imported_records", len(presentations), "count")
            dimension("external_presentations", "valid" if presentations and mapped == len(presentations) and profile["extraction_provenance"]["status"] == "valid" else "unknown",
                      "Observed external presentations remain distinct endpoints from internal present intervals.")
        else:
            imported_rows = []
            transformation = profile["extraction_provenance"].get("status") == "valid" or profile["extraction_provenance"].get("transformation_status") == "valid"
            request = value.get("profile_request")
            target_commands = defaultdict(list)
            identity_only = binding_valid
            if request is not None:
                actual_process = (process_evidence or {}).get("process") or {}
                declared_process = value["process"]
                identity_only = (actual_process.get("identity_verified") is True
                    and actual_process.get("identity_binding") == "retained_child_handle"
                    and (process_evidence or {}).get("run_id") == manifest["run_id"]
                    and actual_process.get("pid") == declared_process.get("pid") == manifest["execution"].get("pid")
                    and actual_process.get("creation_filetime") == declared_process.get("creation_filetime")
                    and (actual_process.get("executable") or {}).get("sha256") == declared_process.get("executable_sha256"))
                native_descriptor = draw_attribution.get("artifact") or {}
                if any(request.get("native_artifact", {}).get(key) != native_descriptor.get(key) for key in ("path", "sha256", "size")):
                    issue("evidence_shader_request_binding", "Shader profiler request does not identify this verified native artifact.", "gpu_counters", True)
                    continue
                for target in request["targets"]:
                    command = command_map.get(target["command"])
                    operation = operation_map.get(target["operation"])
                    pipeline, variant, spirv = (dictionaries.get(target[key]) for key in ("pipeline", "shader_variant", "spirv"))
                    if (not command or not operation or command["operation_id"] != target["operation"]
                            or contexts.get(target["command"], {}).get("batch") != target["batch"]
                            or target["pipeline"] not in operation["pipeline_ids"]
                            or not pipeline or pipeline["kind"] != "pipeline" or target["shader_variant"] not in pipeline["references"]
                            or not variant or variant["kind"] != "shader_variant" or target["spirv"] not in variant["references"]
                            or not spirv or spirv["kind"] != "spirv" or spirv["key"] != target["spirv_sha256"]
                            or variant["fields"].get("stage") != target["stage"]):
                        issue("evidence_shader_request_target", "Requested profiler target contradicts the native command/pipeline/shader graph.", "gpu_counters", True)
                        continue
                    target_commands[target["command"]].append(target)
            for row in value["records"].get("gpu_counters", []) + value["records"].get("shader_metrics", []):
                label = parse_debug_label(row.get("debug_label"))
                matched = bool(identity_only and profile.get("source_integrity", {}).get("status") == "valid" and transformation
                    and label and label["run_id"] == manifest["run_id"]
                    and label["capture_id"] == document["capture_id"] and label["command"] in command_map)
                if matched:
                    command = command_map[label["command"]]
                    operation = operation_map.get(command["operation_id"])
                    matched = bool(operation and label["operation"] == command["operation_id"]
                                   and label["pipeline"] in operation["pipeline_ids"]
                                   and contexts.get(label["command"], {}).get("batch") == label["batch"])
                    shader_digest = row.get("shader_sha256")
                    stage = row.get("stage")
                    if shader_digest is not None and request is None:
                        matched = matched and any(identity["key"] == shader_digest
                            and (stage is None or identity["fields"].get("stage") == stage)
                            for identity in (dictionaries.get(identifier, {}) for identifier in operation["shader_ids"])
                            if identity)
                    elif stage is not None and request is None:
                        matched = matched and any(dictionaries.get(identifier, {}).get("fields", {}).get("stage") == stage
                                                  for identifier in operation["shader_ids"])
                    if request is not None:
                        matched = matched and row.get("shader_digest_scope") == "anyps5.final-spirv.v1" and any(target["operation"] == label["operation"] and target["batch"] == label["batch"]
                            and target["pipeline"] == label["pipeline"] and row.get("shader_sha256") == target["spirv_sha256"]
                            and row.get("stage") == target["stage"] for target in target_commands[label["command"]])
                summary["matched" if matched else "unmatched"] += 1
                if include_detail or len(imported_rows) < 32:
                    imported_rows.append({"source_record": row["id"], "name": row["name"], "value": row["value"],
                    "unit": row["unit"], "measurement_kind": row["measurement_kind"], "scope": row.get("scope", "exported_shader_row_aggregate"),
                    "availability": row["status"], "native_label_join": {"status": "valid" if matched else "unknown",
                        "identifiers": label if matched else None}, "source_artifact": profile["source_artifact"],
                    "extraction_provenance": profile["extraction_provenance"],
                    "meaning": "reported external hardware/sample observation; not an exclusive per-invocation shader-stage duration"})
            if include_detail:
                external_records.append({"artifact": profile["artifact"], "gpu_observations": imported_rows})
            else:
                summary["observation_examples"] = imported_rows
            summary["stage_duration_status"] = "unknown: imported hardware/sample scope is not per-invocation stage duration"
        external_summary.append(summary)
    report["external"] = {"profiles": external_summary, "process_observation_status": (process_evidence or {}).get("status", "unknown")}
    report["external_records"] = external_records
    report["hardware_counters"] = hardware_rows
    report["status"] = "invalid" if any(row["status"] == "invalid" for row in report["dimensions"].values()) or any(
        row["severity"] == "error" for row in report["issues"]) else "valid" if all(
        row["status"] == "valid" for row in report["dimensions"].values()) else "unknown"
    return report


def merge_measurement_workload(report, workload, manifest):
    if report is None:
        report = {"schema_version": 1, "document_type": "anyps5.evidence-report", "run_id": manifest["run_id"],
            "capture_id": None, "status": "unknown", "issues": [], "report_detail": "compact",
            "dimensions": {name: {"status": "unknown", "reasons": ["independent evidence is unavailable"]} for name in DIMENSIONS},
            "metrics": {}, "metric_availability": {}, "metric_units": {}, "limitations": list(LIMITATIONS)}
    else:
        report = {**report, "dimensions": dict(report["dimensions"]), "metrics": dict(report["metrics"]),
            "metric_availability": dict(report["metric_availability"]), "metric_units": dict(report.get("metric_units", {})),
            "issues": list(report["issues"])}
    status = workload["status"]
    values = workload.get("metrics", {})
    existing = report["dimensions"]["workload"]["status"]
    conflicts = []
    if status == "valid" and existing == "valid":
        for name, value in values.items():
            key = f"workload.{name}"
            previous = report["metrics"].get(key, 0 if name.startswith("operation_status.") else None)
            if previous is None or previous != value:
                conflicts.append(name)
    if conflicts:
        status = "invalid"
        finding = {"code": "workload_sources_disagree", "severity": "error",
            "message": "Independent detailed and lightweight workload counts disagree: " + ", ".join(conflicts)}
        report["issues"].append(finding)
        workload["issues"].append(finding)
        workload["status"] = "invalid"
        workload["metrics"] = {}
        workload["metric_units"] = {}
    if status != "valid" or existing != "valid":
        report["dimensions"]["workload"] = {"status": status,
            "reasons": ["Freshly validated selected host-observation ledger; scene, correctness and GPU execution remain unproved."]}
    if status == "valid":
        for name, value in values.items():
            key = f"workload.{name}"
            report["metrics"][key] = value
            report["metric_availability"][key] = "valid"
            report["metric_units"][key] = "count"
    if status != "valid":
        for key in report["metric_availability"]:
            if key.startswith("workload."):
                report["metric_availability"][key] = status
    if status == "invalid":
        report["status"] = "invalid"
    report["measurement_workload"] = {key: value for key, value in workload.items() if key != "frames"}
    return report


def compact_evidence(report):
    excluded = {"frames", "operations", "dependency_graph", "clock_mapping", "hardware_counters", "external_records"}
    result = {key: value for key, value in report.items() if key not in excluded}
    result["external"] = {**report.get("external", {}), "profiles": []}
    for profile in report.get("external", {}).get("profiles", []):
        value = dict(profile)
        ownership = profile.get("cpu_sample_ownership")
        if type(ownership) is dict:
            value["cpu_sample_ownership"] = {key: item for key, item in ownership.items()
                if key not in ("sample_examples", "module_offset_aggregates", "exporter_symbol_labels")}
            value["cpu_sample_ownership"]["index_counts"] = ownership.get("index_counts") or {key: len(ownership.get(key, []))
                for key in ("sample_examples", "module_offset_aggregates", "exporter_symbol_labels")}
        value["symbol_receipts"] = [{**{key: item for key, item in receipt.items() if key not in ("ranges", "function_ranges", "address_resolutions")},
            "resolution_counts": receipt.get("resolution_counts") or {"function_ranges": len(receipt.get("function_ranges", [])),
                "address_resolutions": len(receipt.get("address_resolutions", []))}}
                                     for receipt in profile.get("symbol_receipts", [])]
        result["external"]["profiles"].append(value)
    result["index_counts"] = report.get("index_counts") or {"frames": len(report.get("frames", [])), "operations": len(report.get("operations", [])),
        "dependency_nodes": report.get("dependency_graph", {}).get("node_count", len(report.get("dependency_graph", {}).get("nodes", []))),
        "dependency_edges": report.get("dependency_graph", {}).get("edge_count", len(report.get("dependency_graph", {}).get("edges", []))),
        "gpu_clock_queries": report.get("clock_mapping", {}).get("query_count", len(report.get("clock_mapping", {}).get("queries", {})))}
    result["report_detail"] = "compact"
    result["summary"] = {**report.get("summary", {}), "frames": report.get("summary", {}).get("frames", report.get("frames", [])[:1024]),
        "index_omitted": "Full operation, span, graph and query rows require explicit detail export; canonical raw evidence is preserved."}
    return result


def html_report(report, source_directory=None, output_directory=None):
    validate_evidence_report(report)
    escape = lambda value: html.escape(str(value), quote=True)
    artifact = report.get("sources", {}).get("native_artifact") or {}
    native_path = artifact.get("path")
    native_href = None
    if native_path is not None:
        from capture_schema import contained_path
        absolute = contained_path(Path(source_directory or "."), native_path)
        if output_directory is not None:
            try:
                native_href = quote(os.path.relpath(absolute, output_directory).replace("\\", "/"), safe="/")
            except ValueError:
                native_href = absolute.as_uri()
        else:
            native_href = quote(native_path, safe="/")

    def source_link(row):
        pointer = row["json_pointer"]
        if native_path is None:
            return f"<code>{escape(pointer)}</code>"
        return f"<a href='{escape(native_href + '#' + quote(pointer, safe='/'))}'><code>{escape(pointer)}</code></a>"
    rows = ["<!doctype html><html lang='en'><meta charset='utf-8'><title>Capture evidence</title>",
            "<style>body{font:16px system-ui;max-width:1200px;margin:2rem auto;padding:1rem}table{border-collapse:collapse;width:100%}td,th{border:1px solid #aaa;padding:.4rem;text-align:left}code{overflow-wrap:anywhere}</style>",
            f"<h1>Capture evidence: {escape(report['status'])}</h1><p>Run <code>{escape(report['run_id'])}</code></p>",
            "<p>Activity overlap does not establish frame ownership. Descriptive evidence supplies no performance verdict.</p>",
            "<h2>Availability</h2><table><tr><th>Dimension</th><th>Status</th><th>Reasons</th></tr>"]
    rows.extend(f"<tr><td>{escape(name)}</td><td>{escape(value['status'])}</td><td>{escape('; '.join(value['reasons']))}</td></tr>"
                for name, value in report["dimensions"].items())
    rows.append("</table><h2>Measured summaries</h2><pre>" + escape(json.dumps({"metrics": report["metrics"], "units": report.get("metric_units", {}),
        "availability": report["metric_availability"]}, indent=2)) + "</pre><table>")
    examples = report.get("summary", {}).get("source_examples", [])
    if examples:
        rows.append("</table><p>Bounded canonical operation source examples: " + ", ".join(
            escape(row["operation_id"]) + " " + source_link(row["source"]) for row in examples) + "</p><table>")
    rows.append("</table><h2>Measured host present intervals</h2><table><tr><th>Interval</th><th>Presents</th><th>Selected scope union (ns)</th><th>Uncovered selected scope time (ns)</th></tr>")
    rows.extend(f"<tr><td>{frame['interval_index']}</td><td>{escape(frame['opening_present'])}–{escape(frame['closing_present'])}</td><td>{frame['host_scope_union_ns']}</td><td>{frame['host_scope_uncovered_ns']}</td></tr>" for frame in report.get("frames", report.get("summary", {}).get("frames", [])))
    if report.get("report_detail") == "compact":
        rows.append("</table><p>Compact report: full span, operation and dependency rows are omitted. Canonical raw evidence is preserved; request full detail to export those rows.</p><table>")
    rows.append("</table><h2>Operations and source records</h2><table><tr><th>Operation</th><th>Kind/outcome</th><th>Shader identities</th><th>Selected CPU elapsed/self (ns)</th><th>Whole-command GPU intervals (ns)</th><th>Source records</th></tr>")
    for operation in report.get("operations", []):
        pointers = ", ".join(source_link(row) for row in operation["source_events"])
        shaders = "; ".join(f"{row['kind']} {row['fields'].get('stage', 'stage unknown')} {row['key']}" for row in operation.get("shader_identities", [])) or "unknown"
        scopes = "; ".join(f"{row['function']} ({row['classification']}): {row['wall_ns']}/{row['self_ns'] if row['self_ns'] is not None else 'unavailable'}; thread CPU {(row.get('thread_cpu') or {}).get('cpu_ns', 'unavailable')} ns ({(row.get('thread_cpu') or {}).get('status', 'unknown')})" for row in operation.get("selected_cpu_scopes", [])) or "unavailable"
        gpu = "; ".join(f"{row['query_id']}: {row['duration_ns'] if row['duration_ns'] is not None else 'unavailable'}; bounded host mapping {json.dumps(row.get('host_clock_mapping') or {'status': 'unknown'}, separators=(',', ':'))}" for row in operation.get("whole_command_gpu_intervals", [])) or "unavailable"
        rows.append(f"<tr><td>{escape(operation['operation_id'])}</td><td>{escape(operation['kind'])}/{escape(operation['status'])}</td><td><code>{escape(shaders)}</code></td><td>{escape(scopes)}</td><td>{escape(gpu)}</td><td>{pointers}</td></tr>")
    rows.append("</table><h2>Limitations</h2><ul>")
    rows.extend(f"<li>{escape(value)}</li>" for value in report["limitations"])
    rows.append("</ul><h2>Process resources</h2><pre>" + escape(json.dumps(report.get("process_resources", {}), indent=2)) + "</pre>")
    rows.append("<h2>Reviewed conditions</h2><pre>" + escape(json.dumps(report.get("conditions", {}), indent=2)) + "</pre>")
    rows.append("<h2>Log signature candidates</h2><pre>" + escape(json.dumps(report.get("log_review", {}), indent=2)) + "</pre>")
    rows.append("<h2>External profiler evidence</h2><pre>" + escape(json.dumps(report.get("external", {}), indent=2)) + "</pre>")
    rows.append("</html>")
    return "\n".join(rows) + "\n"


def report_from_assessment(report, *, json_path=None, html_path=None, source_directory=None):
    validate_evidence_report(report)
    if json_path is not None:
        atomic_write_json(json_path, report, replace=False)
    if html_path is not None:
        with Path(html_path).open("x", encoding="utf-8") as output:
            output.write(html_report(report, source_directory=source_directory or Path(html_path).parent,
                                     output_directory=Path(html_path).parent))
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description="Assess native and attached external evidence without launching or replaying a title.")
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--html-output", type=Path)
    parser.add_argument("--full-index", action="store_true")
    args = parser.parse_args(argv)
    try:
        from capture_health import assess_capture
        health = assess_capture(args.run_dir, include_draw_index=args.full_index)
        report = health.get("evidence")
        if report is None:
            raise SchemaError("no independently assessed native evidence report is available")
        report_from_assessment(report, json_path=args.json_output, html_path=args.html_output, source_directory=args.run_dir)
        print(json.dumps(compact_evidence(report), allow_nan=False))
        return 2 if report["status"] == "invalid" else 0 if report["status"] == "valid" else 1
    except (OSError, ValueError, OverflowError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
