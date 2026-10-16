import argparse
import json
import math
import re
import sys
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from pathlib import Path

from capture_schema import SchemaError, validate_draw_trace


LIMITATIONS = [
    "Coverage applies only to the producer's selected instrumented paths; uninstrumented work remains unknown.",
    "Host scope wall and self times measure elapsed intervals, including scheduling; they are not CPU usage.",
    "Asynchronous producer links do not establish a critical path or exclusive ownership of a frame.",
    "GPU query intervals measure their recorded command ranges, not exclusive shader-stage or shader-function execution.",
    "The GPU interval sum includes shared ranges and overlap; it is neither GPU busy time nor a per-frame cost.",
    "Overlapping thread and GPU intervals must not be summed into a wall-time percentage or a performance-win claim.",
    "Source-to-binary binding and actually loaded modules remain unverified.",
    "Dictionary digests are producer-reported identities; source and SPIR-V bytes are absent, so their content hashes are not recomputed here.",
]
WAIT_SCOPES = frozenset({"DrawPipeline.LockWait", "DrawPipeline.CapacityWait", "DrawPipeline.IdleWait",
                         "DrawPipeline.DrainWait", "Driver.FlipRoomWait", "Driver.RewindControlWait",
                         "Shader.SubmittedPreparationWait", "Shader.RegisteredPreparationWait",
                         "Driver.DrawGpuMutexWait", "Driver.DispatchGpuMutexWait", "Driver.DeviceSetupMutexWait",
                         "Driver.SubmissionOrderWait", "Driver.WorkerWait", "Shader.PreparationTicketWait",
                         "Shader.PreparationCloseWait", "Shader.PreparationIdleWait"})
DEFINITIONS = frozenset({"submission", "segment", "packet", "operation", "host_command", "batch",
                         "flip", "present", "job", "span", "gpu_query"})
GPU_FAILURES = frozenset({"allocation_failed", "capacity_dropped", "read_failed", "invalid_timestamp", "uncollected"})
MAX_DRAW_EVENTS = 8_388_608
MAX_DRAW_THREADS = 1024
MAX_DRAW_VIEW_EVENTS = MAX_DRAW_EVENTS + MAX_DRAW_THREADS
MAX_DRAW_DICTIONARIES = 262_144
MAX_DRAW_FILE_BYTES = 16 * 1024 ** 3
MAX_DRAW_VALUE_CHARS = 16 * 1024 ** 2
MAX_COMPACT_FRAMES = 1024
MAX_COMPACT_INTERVALS = 64


def validate_draw_capacity(event_count, dictionary_count, *, view_count=None):
    if (type(event_count) is not int or type(dictionary_count) is not int
            or event_count < 0 or dictionary_count < 0
            or event_count > MAX_DRAW_EVENTS or dictionary_count > MAX_DRAW_DICTIONARIES):
        raise SchemaError("draw sidecar exceeds supported bounded record capacity")
    if view_count is not None and (type(view_count) is not int or view_count < 0 or view_count > MAX_DRAW_VIEW_EVENTS):
        raise SchemaError("draw duplicated view exceeds supported bounded record capacity")


class _DrawJsonReader:
    def __init__(self, source):
        self.source = source
        self.buffer = ""
        self.position = 0
        self.eof = False
        self.characters = 0
        self.keys = {}
        self.decoder = json.JSONDecoder(object_pairs_hook=self.pairs, parse_constant=self.constant,
                                        parse_float=self.floating)

    def pairs(self, entries):
        result = {}
        for key, value in entries:
            if key in result:
                raise SchemaError(f"duplicate JSON key {key!r}")
            if key in self.keys:
                key = self.keys[key]
            elif len(self.keys) < 4096:
                self.keys[key] = key
            result[key] = value
        return result

    @staticmethod
    def constant(value):
        raise SchemaError(f"nonfinite JSON number {value}")

    @staticmethod
    def floating(value):
        result = float(value)
        if not math.isfinite(result):
            raise SchemaError("nonfinite JSON number")
        return result

    def fill(self):
        self.buffer = self.buffer[self.position:]
        self.position = 0
        chunk = self.source.read(65_536)
        self.characters += len(chunk)
        if self.characters > MAX_DRAW_FILE_BYTES:
            raise SchemaError("draw sidecar exceeds supported file size")
        self.buffer += chunk
        self.eof = not chunk

    def peek(self):
        while True:
            while self.position < len(self.buffer) and self.buffer[self.position] in " \t\r\n":
                self.position += 1
            if self.position < len(self.buffer):
                return self.buffer[self.position]
            if self.eof:
                return ""
            self.fill()

    def take(self, token):
        if self.peek() != token:
            raise SchemaError(f"draw JSON expected {token!r}")
        self.position += 1

    def value(self):
        if not self.peek():
            raise SchemaError("draw JSON ended before a value")
        while True:
            try:
                result, end = self.decoder.raw_decode(self.buffer, self.position)
            except json.JSONDecodeError as error:
                if self.eof:
                    raise SchemaError(f"invalid draw JSON: {error.msg}") from error
            else:
                if (type(result) in (int, float) and end < len(self.buffer)
                        and self.buffer[end] in "eE." and not self.eof):
                    if len(self.buffer) - self.position > MAX_DRAW_VALUE_CHARS:
                        raise SchemaError("draw JSON value exceeds supported item size")
                    self.fill()
                    continue
                if end < len(self.buffer) or self.eof:
                    if end - self.position > MAX_DRAW_VALUE_CHARS:
                        raise SchemaError("draw JSON value exceeds supported item size")
                    self.position = end
                    return result
            if len(self.buffer) - self.position > MAX_DRAW_VALUE_CHARS:
                raise SchemaError("draw JSON value exceeds supported item size")
            self.fill()

    def array(self, maximum, retain):
        result = [] if retain else None
        self.take("[")
        count = 0
        if self.peek() == "]":
            self.position += 1
            return result
        while True:
            item = self.value()
            count += 1
            if count > maximum:
                raise SchemaError("draw JSON array exceeds supported record capacity")
            if retain:
                result.append(item)
            token = self.peek()
            if token == "]":
                self.position += 1
                return result
            self.take(",")

    def document(self):
        result = {}
        self.take("{")
        if self.peek() == "}":
            self.position += 1
        else:
            while True:
                key = self.value()
                if type(key) is not str or key in result:
                    raise SchemaError("draw JSON root requires distinct string keys")
                self.take(":")
                if key in ("events", "traceEvents", "dictionaries", "boundaries", "threads"):
                    maximum = {"dictionaries": MAX_DRAW_DICTIONARIES, "boundaries": 65_536,
                               "threads": MAX_DRAW_THREADS, "traceEvents": MAX_DRAW_VIEW_EVENTS}.get(key, MAX_DRAW_EVENTS)
                    result[key] = self.array(maximum, key != "traceEvents")
                else:
                    result[key] = self.value()
                token = self.peek()
                if token == "}":
                    self.position += 1
                    break
                self.take(",")
        if self.peek():
            raise SchemaError("draw JSON has trailing content")
        declared_view = result.get("coverage", {}).get("perfetto_view_included")
        if declared_view is not None and (type(declared_view) is not bool or declared_view != ("traceEvents" in result)):
            raise SchemaError("declared Perfetto view presence differs from the document")
        result.pop("traceEvents", None)
        validate_draw_capacity(len(result.get("events", [])), len(result.get("dictionaries", [])))
        return result


def read_draw_trace(path):
    path = Path(path)
    if path.stat().st_size > MAX_DRAW_FILE_BYTES:
        raise SchemaError("draw sidecar exceeds supported file size")
    try:
        with path.open(encoding="utf-8") as source:
            return _DrawJsonReader(source).document()
    except (UnicodeError, RecursionError) as error:
        raise SchemaError(f"invalid draw JSON: {error}") from error


def _query_duration(data, status):
    begin, end = data["begin_gpu_ns"], data["end_gpu_ns"]
    fields = ("begin_gpu_ticks", "end_gpu_ticks", "period_ns", "duration_ns", "duration_origin",
              "absolute_time_precision", "timestamp_valid_bits", "timestamp_wraps")
    if status != "available":
        if begin is not None or end is not None or any(data.get(key) is not None for key in fields):
            raise SchemaError("unavailable GPU query must not fabricate timestamp measurements")
        return None, None, None
    if begin is None or end is None or end < begin:
        raise SchemaError("available GPU query lacks ordered finite timestamps")
    origin = data.get("duration_origin")
    if origin == "integer_tick_delta":
        first, last = int(data["begin_gpu_ticks"]), int(data["end_gpu_ticks"])
        delta = (last - first) & (2 ** 64 - 1)
        if delta >= 2 ** 63 or data["timestamp_wraps"] != (last < first):
            raise SchemaError("GPU query raw timestamp delta is ambiguous or its wrap flag is contradictory")
        expected = delta * data["period_ns"]
        if not math.isfinite(expected) or not math.isclose(data["duration_ns"], expected, rel_tol=1e-12, abs_tol=0):
            raise SchemaError("GPU query duration contradicts the integer tick delta")
        approximate_begin = float(first) * data["period_ns"]
        approximate_end = approximate_begin + expected
        for observed, coordinate in ((begin, approximate_begin), (end, approximate_end)):
            if not math.isfinite(coordinate) or not math.isclose(observed, coordinate, rel_tol=0,
                    abs_tol=2 * max(math.ulp(float(observed)), math.ulp(coordinate))):
                raise SchemaError("GPU query approximate coordinates contradict its raw timestamp identity")
        return expected, origin, data["absolute_time_precision"]
    expected = end - begin
    if origin == "approximate_endpoint_difference":
        if any(data.get(key) is not None for key in ("begin_gpu_ticks", "end_gpu_ticks", "period_ns",
                                                   "timestamp_valid_bits", "timestamp_wraps")):
            raise SchemaError("approximate GPU duration must not claim raw tick evidence")
        if not math.isclose(data["duration_ns"], expected, rel_tol=1e-12, abs_tol=0):
            raise SchemaError("GPU query duration contradicts its approximate endpoints")
    elif any(data.get(key) is not None for key in fields):
        raise SchemaError("GPU query precision fields require a supported duration origin")
    return expected, "approximate_endpoint_difference", data.get("absolute_time_precision", "unknown")


def _union(intervals):
    result = 0
    end = None
    for start, stop in sorted(intervals):
        if end is None or start >= end:
            result += stop - start
            end = stop
        elif stop > end:
            result += stop - end
            end = stop
    return result


def _unique(entries, key, name):
    result = {}
    for entry in entries:
        identifier = entry[key]
        if identifier in result:
            raise SchemaError(f"duplicate {name} ID {identifier}")
        result[identifier] = entry
    return result


def _check_dictionary_graph(dictionaries):
    remaining = {identifier: set(entry["references"]) for identifier, entry in dictionaries.items()}
    for identifier, references in remaining.items():
        if not references <= dictionaries.keys():
            raise SchemaError(f"dictionary {identifier} has an unresolved dictionary reference")
    reverse = defaultdict(set)
    for identifier, references in remaining.items():
        for target in references:
            reverse[target].add(identifier)
    ready = [identifier for identifier, references in remaining.items() if not references]
    count = 0
    while ready:
        identifier = ready.pop()
        count += 1
        for parent in reverse[identifier]:
            remaining[parent].remove(identifier)
            if not remaining[parent]:
                ready.append(parent)
    if count != len(remaining):
        raise SchemaError("dictionary reference graph contains a cycle")
    keys = {}
    for entry in dictionaries.values():
        key = (entry["kind"], entry["key"])
        identity = (entry["fields"], entry["references"])
        if key in keys and keys[key] != identity:
            raise SchemaError("dictionary key identifies conflicting metadata")
        keys[key] = identity
        fields = entry["fields"]
        if entry["kind"] in ("guest_shader", "compiler_key", "spirv", "shader_variant", "pipeline"):
            if fields.get("digest_algorithm") != "sha256" or re.fullmatch(r"[0-9a-f]{64}", entry["key"]) is None:
                raise SchemaError("shader and pipeline dictionary identities require SHA-256 digests")
            encodings = {"guest_shader": {"anyps5.guest-shader.v1"}, "compiler_key": {"anyps5.recompile-key.v1"},
                         "spirv": {"anyps5.final-spirv.v1"}, "shader_variant": {"anyps5.shader-variant.v1", "anyps5.builtin-shader-variant.v1"},
                         "pipeline": {"anyps5.graphics-pipeline.v1", "anyps5.compute-pipeline.v1", "anyps5.builtin-compute-pipeline.v1"}}
            if fields.get("encoding") not in encodings[entry["kind"]]:
                raise SchemaError("dictionary digest encoding is missing or unsupported")
            kinds = Counter(dictionaries[value]["kind"] for value in entry["references"])
            if entry["kind"] == "shader_variant" and (kinds["spirv"] != 1 or kinds["guest_shader"] > 1
                    or kinds["compiler_key"] > 1 or sum(kinds.values()) != kinds["spirv"] + kinds["guest_shader"] + kinds["compiler_key"]):
                raise SchemaError("shader variant must reference one final SPIR-V identity and optional guest/compiler identities")
            if entry["kind"] == "pipeline" and (not entry["references"] or set(kinds) != {"shader_variant"}):
                raise SchemaError("pipeline dictionary must reference shader variants")


def _source(index, event):
    return {"event_id": event["id"], "json_pointer": f"/events/{index}"}


def _thread_cpu(event, thread):
    data = event["data"]
    status = data.get("thread_cpu_status", "not_requested")
    fields = ("thread_cpu_begin_ticks", "thread_cpu_end_ticks", "thread_cpu_frequency", "thread_cpu_native_id", "thread_cpu_ns")
    if status not in ("not_requested", "measured", "unavailable", "failed", "nonmonotonic", "thread_changed", "clipped"):
        raise SchemaError("host scope thread CPU status is unsupported")
    if status != "measured":
        if any(data.get(key) is not None for key in fields):
            raise SchemaError("unmeasured thread CPU scope must not fabricate counters")
        return {"status": status, "cpu_ns": None, "native_thread_binding": "unknown"}
    values = []
    for key in fields[:-1]:
        value = data.get(key)
        if type(value) is not str or re.fullmatch(r"0|[1-9][0-9]{0,19}", value) is None or int(value) >= 2 ** 64:
            raise SchemaError("measured thread CPU requires canonical raw counter identities")
        values.append(int(value))
    first, last, frequency, native_id = values
    if last < first or frequency == 0 or native_id == 0 or event["end_ns"] is None:
        raise SchemaError("measured thread CPU scope lacks ordered counters and a closed interval")
    duration = data.get("thread_cpu_ns")
    expected = (last - first) * (1_000_000_000 / frequency)
    if type(duration) not in (int, float) or not math.isfinite(duration) or not math.isclose(duration, expected, rel_tol=1e-12, abs_tol=0):
        raise SchemaError("thread CPU duration contradicts its raw counter delta")
    declared = thread.get("native_thread_id")
    if declared is not None and declared != str(native_id):
        raise SchemaError("thread CPU native thread identity contradicts its thread ledger")
    return {"status": "measured", "cpu_ns": expected, "begin_ticks": str(first), "end_ticks": str(last),
            "frequency": str(frequency), "native_thread_id": str(native_id),
            "native_thread_binding": "valid" if declared is not None else "unknown",
            "meaning": "inclusive elapsed thread CPU counter interval; not scheduler, blocking or self CPU time"}


def _thread_cycles(event, thread):
    data = event["data"]
    status = data.get("thread_cycles_status", "not_requested")
    fields = ("thread_cycles_begin", "thread_cycles_end", "thread_cycles_delta", "thread_cycles_native_id")
    if status not in ("not_requested", "measured", "unavailable", "failed", "nonmonotonic", "thread_changed", "clipped"):
        raise SchemaError("host scope thread cycle status is unsupported")
    if status != "measured":
        if any(data.get(key) is not None for key in fields):
            raise SchemaError("unmeasured thread cycles must not fabricate raw counters")
        return {"status": status, "cycles": None, "native_thread_binding": "unknown"}
    values = []
    for key in fields:
        value = data.get(key)
        if type(value) is not str or re.fullmatch(r"0|[1-9][0-9]{0,19}", value) is None or int(value) >= 2 ** 64:
            raise SchemaError("measured thread cycles require canonical raw uint64 counters")
        values.append(int(value))
    first, last, delta, native_id = values
    if (last < first or delta != last - first or native_id == 0 or event["end_ns"] is None
            or data.get("thread_cycles_unit") != "cycles" or data.get("thread_cycles_source") != "query_thread_cycle_time"):
        raise SchemaError("measured thread cycle delta, source or units are contradictory")
    declared = thread.get("native_thread_id")
    if declared is not None and declared != str(native_id):
        raise SchemaError("thread cycle native identity contradicts its ledger")
    return {"status": "measured", "cycles": delta, "begin": str(first), "end": str(last),
            "native_thread_id": str(native_id), "native_thread_binding": "valid" if declared is not None else "unknown",
            "unit": "cycles", "time_conversion": "unavailable", "resolution": "unknown"}


def _merge_intervals(rows):
    merged = []
    for first, last in sorted(rows):
        if last <= first:
            continue
        if merged and first <= merged[-1][1]:
            merged[-1][1] = max(last, merged[-1][1])
        else:
            merged.append([first, last])
    return merged


def _clip_intervals(rows, first, last):
    start = max(0, bisect_right(rows, [first, math.inf]) - 1)
    result = []
    for index in range(start, len(rows)):
        opening, closing = rows[index]
        if opening >= last:
            break
        if closing > first:
            result.append([max(opening, first), min(closing, last)])
    return result


def _activity_summary(intervals, waits, boundaries):
    threads = {key: _merge_intervals(rows) for key, rows in intervals.items()}
    wait_unions = {key: _merge_intervals(rows) for key, rows in waits.items()}
    host = _merge_intervals(row for rows in threads.values() for row in rows)
    frames = []
    for index, (opening, closing) in enumerate(zip(boundaries, boundaries[1:])):
        if index >= MAX_COMPACT_FRAMES:
            break
        first, last = opening["ts_ns"], closing["ts_ns"]
        covered = _clip_intervals(host, first, last)
        cursor, uncovered = first, []
        for begin, end in covered:
            if begin > cursor:
                uncovered.append([cursor, begin])
            cursor = max(cursor, end)
        if cursor < last:
            uncovered.append([cursor, last])
        frames.append({"interval_index": index, "opening_present": opening["present"], "closing_present": closing["present"],
            "start_ns": first, "end_ns": last, "host_scope_union_ns": sum(b - a for a, b in covered),
            "host_scope_uncovered_ns": sum(b - a for a, b in uncovered),
            "host_scope_uncovered_intervals": uncovered[:MAX_COMPACT_INTERVALS],
            "uncovered_intervals_omitted": max(0, len(uncovered) - MAX_COMPACT_INTERVALS),
            "thread_scope_union_ns": {key: sum(b - a for a, b in _clip_intervals(rows, first, last)) for key, rows in threads.items()},
            "named_wait_union_ns_by_thread": {key: sum(b - a for a, b in _clip_intervals(rows, first, last)) for key, rows in wait_unions.items()},
            "causal_frame_membership": "unknown", "gpu_uncovered_time": None,
            "meaning": "selected host-clock activity; uncovered time is not CPU idle and overlap is not exclusive frame ownership"})
    return {"frames": frames, "frame_count": max(0, len(boundaries) - 1),
            "frames_omitted": max(0, len(boundaries) - 1 - MAX_COMPACT_FRAMES),
            "frame_summary_limit": MAX_COMPACT_FRAMES, "uncovered_interval_limit_per_frame": MAX_COMPACT_INTERVALS}


def _time_membership(timestamps, start, end):
    result = {"start_interval": None, "overlap_interval_range": None,
              "meaning": "host-clock activity within measured present intervals; not presented-frame ownership"}
    count = len(timestamps) - 1
    if count < 1:
        return result
    if timestamps[0] <= start < timestamps[-1]:
        result["start_interval"] = bisect_right(timestamps, start) - 1
    if end is None or end == start:
        if result["start_interval"] is not None:
            result["overlap_interval_range"] = [result["start_interval"], result["start_interval"]]
        return result
    if end > timestamps[0] and start < timestamps[-1]:
        first = max(0, bisect_right(timestamps, start) - 1)
        last = min(count - 1, bisect_left(timestamps, end) - 1)
        if first <= last:
            result["overlap_interval_range"] = [first, last]
    return result


def assess_draw_trace(document, manifest, *, trace_name, summary, trace, include_index=True, analysis_index=None):
    validate_draw_trace(document)
    capture = document["capture"]
    producer = document["producer"]
    if document["run_id"] != manifest["run_id"] or producer["pid"] != manifest["execution"].get("pid"):
        raise SchemaError("draw sidecar run or process identity differs from manifest")
    if manifest["environment"]["effective"].get("APS5_CAPTURE_RUN_ID") != document["run_id"]:
        raise SchemaError("draw sidecar run UUID is not bound to effective child environment")
    if capture.get("trace") != trace_name:
        raise SchemaError("draw sidecar does not bind the selected legacy trace basename")
    if any(capture[key] != manifest["capture"][key] for key in ("unit", "start", "length")):
        raise SchemaError("draw sidecar capture window differs from manifest")
    if producer["revision"] != summary["commit"] or producer["dirty"] != summary["dirty"]:
        raise SchemaError("draw and legacy trace producer revisions or dirty states differ")
    if not document["completion"]["output_complete"]:
        raise SchemaError("draw sidecar output is incomplete")
    validate_draw_capacity(len(document["events"]), len(document["dictionaries"]))
    events = _unique(document["events"], "id", "event")
    threads = _unique(document["threads"], "thread_id", "thread")
    dictionaries = _unique(document["dictionaries"], "id", "dictionary")
    _check_dictionary_graph(dictionaries)
    if document["completion"]["event_count"] != len(events):
        raise SchemaError("completion event count differs from canonical event array")
    result = {"status": "unknown", "identity_status": "valid", "cpu_status": "valid", "gpu_status": "unknown",
              "workload_status": "unknown",
              "origin_graph_status": "valid",
              "gpu_association_status": "valid",
              "run_id": document["run_id"], "capture_id": document["capture_id"],
              "correlation_valid": True, "scope": producer["coverage"], "limitations": list(LIMITATIONS),
              "issues": [], "metrics": {"workload": {}, "functions": {}, "shaders": {}, "gpu": {}},
              "function_availability": {},
              "report_detail": "full" if include_index else "compact",
              "function_totals_meaning": "elapsed totals for fully closed in-window selected scopes; unavailable names are omitted, not zero",
              "coverage": {"boundaries": "unknown", "threads": "unknown", "gpu_tail": "unknown"},
              "index": {"draws": [], "spans": [], "queries": [], "calibrations": [], "host_commands": [], "dictionaries": list(dictionaries.values()),
                        "external_references": [], "present_intervals": [],
                        "event_source": {"canonical_event_count": len(events),
                                         "json_pointer_template": "/events/{index}",
                                         "meaning": "canonical sidecar events remain authoritative; redundant generic event rows are omitted"},
                        "gpu_alignment": {"status": "unknown", "time_window_membership": None,
                                          "reason": "GPU query timestamps remain in their device clock; no CPU interval mapping is inferred"}}}

    def issue(code, message, *, area="cpu", invalid=False):
        result["issues"].append({"code": code, "severity": "error" if invalid else "warning", "message": message})
        key = f"{area}_status"
        if key in result and result[key] != "invalid":
            result[key] = "invalid" if invalid else "unknown"

    for limitation in document["limitations"]:
        if limitation not in result["limitations"]:
            result["limitations"].append(limitation)
    pending = sum(capture[key] for key in ("pending_spans", "pending_jobs", "pending_gpu_queries"))
    losses = document["losses"]
    total_loss = sum(losses.values())
    complete = capture["state"] == "complete" and document["completion"]["state"] == "complete"
    if complete and (pending or total_loss):
        raise SchemaError("complete draw capture contradicts pending work or loss counters")
    if not complete:
        issue("draw_capture_partial", "Detailed capture did not finish every evidence dimension; each dimension is assessed independently.", area="completion")
    if losses["events"] or losses["metadata"]:
        issue("draw_detail_loss", "Detailed event or dictionary records were lost.", area="identity", invalid=True)
        issue("draw_cpu_loss", "Lost detail prevents complete host timing attribution.", invalid=True)
    if losses["gpu_queries"]:
        issue("draw_gpu_loss", "GPU query coverage records loss.", area="gpu", invalid=True)
    counts = Counter(event["thread_id"] for event in events.values())
    if not counts.keys() <= threads.keys():
        raise SchemaError("event references a thread absent from the thread ledger")
    for identifier, thread in threads.items():
        if thread["attempted"] != thread["emitted"] + thread["lost"] or thread["emitted"] != counts[identifier]:
            raise SchemaError(f"thread {identifier} accounting does not match canonical events")
        if thread["last_event_ns"] < thread["registered_ns"]:
            raise SchemaError("thread ledger timestamp order is invalid")
        if not thread["acknowledged"]:
            issue("draw_thread_unacknowledged", f"Thread {identifier} did not acknowledge capture finalization.")
        if thread["lost"]:
            issue("draw_thread_loss", f"Thread {identifier} lost selected events.", invalid=True)
    if sum(thread["lost"] for thread in threads.values()) + losses["unassigned_events"] != losses["events"]:
        raise SchemaError("global event loss differs from per-thread loss accounting")
    result["coverage"].update(recorded_event_count=len(events), lost_event_count=losses["events"],
                              attempted_event_count=sum(thread["attempted"] for thread in threads.values())
                              + losses["unassigned_events"], lifecycle={})
    if all(thread["acknowledged"] for thread in threads.values()) and not losses["events"]:
        result["coverage"]["threads"] = "valid"

    boundaries = document["boundaries"]
    closed_window = (capture["close_reason"] == "window_closed"
                     and capture["open_ns"] is not None and capture["close_ns"] is not None
                     and capture["unit"] == "frames" and len(boundaries) == capture["length"] + 1)
    if complete or closed_window:
        if capture["open_ns"] is None or capture["close_ns"] is None or capture["close_ns"] <= capture["open_ns"]:
            raise SchemaError("complete capture requires ordered opening and closing timestamps")
        if not boundaries or boundaries[0]["ts_ns"] != capture["open_ns"] or boundaries[-1]["ts_ns"] != capture["close_ns"]:
            raise SchemaError("capture boundary timestamps disagree with opening or closing markers")
        if any(after["ts_ns"] < before["ts_ns"] for before, after in zip(boundaries, boundaries[1:])):
            raise SchemaError("capture boundaries are not ordered")
        if capture["unit"] == "frames":
            if len(boundaries) != capture["length"] + 1:
                raise SchemaError("fixed-frame detail boundary count differs from requested length")
            expected = [str(value) for value in range(capture["start"], capture["start"] + capture["length"] + 1)]
            if [entry["present"] for entry in boundaries] != expected:
                raise SchemaError("fixed-frame detail requires every opening, intermediate and closing boundary")
            if capture["open_present"] != expected[0] or capture["close_present"] != expected[-1]:
                raise SchemaError("capture present markers disagree with requested frame boundaries")
            elapsed = capture["close_ns"] - capture["open_ns"]
            if abs(elapsed / 1_000_000 - summary["window"]["ms"]) > 0.002:
                raise SchemaError("detail boundary duration differs from legacy frame summary")
            for boundary, present in zip(boundaries, trace["presents"]):
                if int(boundary["present"]) != present[0] or abs((boundary["ts_ns"] - capture["open_ns"]) / 1000 - present[1]) > 2:
                    raise SchemaError("detail and legacy trace present boundaries disagree")
            result["coverage"]["boundaries"] = "valid"
        else:
            issue("draw_seconds_boundary_unknown", "Seconds-window opening and workload equivalence remain unverified.")
    if result["coverage"]["boundaries"] != "valid" or capture["close_reason"] != "window_closed":
        issue("draw_window_incomplete", "A normally closed complete fixed-frame window is unavailable.")
        issue("draw_workload_window_unknown", "Whole-window workload coverage is unavailable.", area="identity")

    entities = defaultdict(list)
    references = {}
    links = defaultdict(set)
    identity_links = defaultdict(set)
    wait_subjects = {event["subject"] for event in events.values() if event["kind"] == "attribute" and event.get("name") == "wait_reason" and event.get("subject") is not None}
    measures = defaultdict(set)
    measurement_relations = defaultdict(dict)
    sources = {}
    operation_entities = defaultdict(set)
    for index, event in enumerate(document["events"]):
        if event["kind"] in ("operation", "host_command", "gpu_query", "calibration") or include_index and event["kind"] == "span":
            sources[event["id"]] = _source(index, event)
        if event["end_ns"] is not None and event["end_ns"] < event["ts_ns"]:
            raise SchemaError("event end precedes its beginning")
        if event["kind"] == "calibration":
            result["index"]["calibrations"].append({"device_id": event["context"]["device"], "status": event["status"],
                "cpu_ns": event["ts_ns"], "gpu_ns": event["data"]["begin_gpu_ns"],
                "deviation_ns": event["data"]["deviation_ns"], "period_ns": event["data"]["period_ns"],
                "valid_bits": event["data"]["valid_bits"], "source": sources[event["id"]]})
        if event["kind"] in DEFINITIONS and event["subject"] is not None:
            entities[event["subject"]].append(event)
        if event["kind"] == "reference" and event["subject"] is not None:
            previous = references.get(event["subject"])
            if previous is None or event["status"] == "complete" and event.get("text") == "identity_only_no_lifecycle":
                references[event["subject"]] = event
        if event["kind"] in ("link", "attribute") and event["subject"] is not None and event["related"] is not None:
            expected_kind = {"pipeline": "pipeline", "shader_stage": "shader_variant"}.get(event["name"])
            if expected_kind is not None and event["related"] in dictionaries and dictionaries[event["related"]]["kind"] != expected_kind:
                raise SchemaError("typed metadata link references the wrong dictionary kind")
            links[event["subject"]].add(event["related"])
            if event["name"] in ("pipeline", "shader_stage", "main", "stencil_replay", "back_faces", "second_channel", "indirect_record"):
                identity_links[event["subject"]].add(event["related"])
            if event["name"] in ("measures", "presentation_path"):
                measures[event["subject"]].add(event["related"])
                measurement_relations[event["subject"]][event["related"]] = event["name"]
    for identifier, records in entities.items():
        if identifier in identity_links and records[0]["kind"] in ("span", "host_command") and records[0]["context"]["operation"] is not None:
            operation_entities[records[0]["context"]["operation"]].add(identifier)
    known = entities.keys() | dictionaries.keys() | references.keys()
    unresolved = set()
    for event in events.values():
        targets = [event["subject"], event["related"], event["parent_span"],
                   *[value for name, value in event["context"].items() if name not in ("queue", "packet_offset", "opcode")]]
        unresolved.update(value for value in targets if value is not None and value not in known)
    declarations = {identifier for identifier, event in references.items() if event["status"] == "complete"
                    and event.get("text") == "identity_only_no_lifecycle" and event["name"] in ("driver", "device", "recorder", "native_queue")}
    external = set(references) - entities.keys() - dictionaries.keys() - declarations
    if unresolved or external:
        issue("draw_external_identity", "Pre-window or uninstrumented origin references remain explicit unknowns; measured shader and timing identities are assessed separately.", area="origin_graph")
    result["index"]["external_references"] = [{"id": identifier, "reason": references[identifier].get("text")
                                                if identifier in references else "unresolved entity reference"}
                                               for identifier in sorted(external | unresolved, key=int)]
    for identifier, records in entities.items():
        if len({event["kind"] for event in records}) != 1 or identifier in dictionaries:
            raise SchemaError("entity ID identifies conflicting canonical kinds")

    span_records = {}
    for identifier, records in entities.items():
        if records[0]["kind"] != "span":
            continue
        ended = [event for event in records if event["end_ns"] is not None]
        if len(ended) > 1:
            raise SchemaError("span has multiple terminal interval records")
        if not ended:
            issue("draw_span_unclosed", f"Span {identifier} has no terminal interval.")
            continue
        event = ended[0]
        if not event["name"]:
            raise SchemaError("host span lacks a stable function or scope name")
        if event["status"] == "unknown" or event.get("text") == "clipped_at_window_close":
            issue("draw_span_clipped", f"Span {identifier} is observationally clipped, not a complete function interval.")
        span_records[identifier] = event
    children = defaultdict(list)
    resolved_parents = set()
    for identifier in span_records:
        visited = set()
        parent = identifier
        while parent in span_records and parent not in resolved_parents:
            if parent in visited:
                raise SchemaError("host span parent graph contains a cycle")
            visited.add(parent)
            parent = span_records[parent]["parent_span"]
        resolved_parents.update(visited)
    for identifier, event in span_records.items():
        parent = event["parent_span"]
        if parent == identifier:
            raise SchemaError("span cannot parent itself")
        if parent in entities and entities[parent][0]["kind"] != "span":
            raise SchemaError("span parent references a known non-span entity")
        if parent in span_records:
            owner = span_records[parent]
            if owner["thread_id"] == event["thread_id"]:
                if not owner["ts_ns"] <= event["ts_ns"] <= event["end_ns"] <= owner["end_ns"]:
                    raise SchemaError("same-thread child span lies outside its parent interval")
                children[parent].append((event["ts_ns"], event["end_ns"]))
    functions = defaultdict(lambda: {"calls": 0, "wall_ns": 0, "self_ns": 0, "wait_ns": 0, "boundary_crossing": 0, "clipped": 0})
    cpu_functions = defaultdict(lambda: {"scopes": 0, "measured": 0, "cpu_ns": 0, "cycles_measured": 0, "cycles": 0})
    scope_intervals, wait_intervals = defaultdict(list), defaultdict(list)
    operation_spans = defaultdict(list)
    for identifier, event in span_records.items():
        wall = event["end_ns"] - event["ts_ns"]
        self_time = wall - _union(children[identifier])
        crosses = (capture["open_ns"] is None or capture["close_ns"] is None
                   or event["ts_ns"] < capture["open_ns"] or event["end_ns"] > capture["close_ns"])
        wait = event["name"] in WAIT_SCOPES or identifier in wait_subjects
        cpu = _thread_cpu(event, threads[event["thread_id"]])
        cycles = _thread_cycles(event, threads[event["thread_id"]])
        observation = cpu_functions[event["name"]]
        observation["scopes"] += 1
        if cpu["status"] == "measured" and cpu["native_thread_binding"] == "valid":
            observation["measured"] += 1
            observation["cpu_ns"] += cpu["cpu_ns"]
        if cycles["status"] == "measured" and cycles["native_thread_binding"] == "valid":
            observation["cycles_measured"] += 1
            observation["cycles"] += cycles["cycles"]
        scope_intervals[event["thread_id"]].append((event["ts_ns"], event["end_ns"]))
        if wait:
            wait_intervals[event["thread_id"]].append((event["ts_ns"], event["end_ns"]))
        if include_index:
            row = {"span_id": identifier, "function": event["name"], "classification": "wait" if wait else "host_elapsed",
               "thread_id": event["thread_id"], "parent_span": event["parent_span"],
               "operation_id": event["context"]["operation"], "job_id": event["context"]["job"],
               "start_ns": event["ts_ns"], "end_ns": event["end_ns"], "wall_ns": wall,
               "self_ns": self_time if not losses["events"] else None,
               "self_status": "valid" if not losses["events"] else "unknown",
               "boundary_crossing": crosses, "status": event["status"], "source": sources[event["id"]]}
            row["thread_cpu"] = cpu
            row["thread_cycles"] = cycles
            result["index"]["spans"].append(row)
            if row["operation_id"] is not None:
                operation_spans[row["operation_id"]].append(identifier)
        aggregate = functions[event["name"]]
        if event["status"] == "unknown" or event.get("text") == "clipped_at_window_close":
            aggregate["clipped"] += 1
        if crosses:
            aggregate["boundary_crossing"] += 1
        else:
            aggregate["calls"] += 1
            aggregate["wall_ns"] += wall
            aggregate["self_ns"] += self_time
            aggregate["wait_ns"] += wall if wait else 0
    if any(entry["boundary_crossing"] for entry in functions.values()):
        result["limitations"].append("Boundary-crossing host intervals remain indexed but are excluded from complete-window function totals.")
    result["summary"] = _activity_summary(scope_intervals, wait_intervals, boundaries if result["coverage"]["boundaries"] == "valid" else [])
    result["summary"]["thread_cpu_functions"] = {name: {**row,
        "cpu_ns": row["cpu_ns"] if row["measured"] else None,
        "cycles": row["cycles"] if row["cycles_measured"] else None,
        "cpu_status": "valid" if row["measured"] == row["scopes"] else "unknown",
        "cycles_status": "valid" if row["cycles_measured"] == row["scopes"] else "unknown",
        "meaning": "inclusive sums of measured retained scopes; complete function metrics require independent function availability; counter zero is quantized observation, not proof of no work"}
        for name, row in cpu_functions.items()}
    span_count = len(span_records)
    span_records.clear()
    scope_intervals.clear()
    wait_intervals.clear()
    children.clear()

    queries = {}
    operation_queries = defaultdict(list)
    measured_commands = set()
    for identifier, records in entities.items():
        if records[0]["kind"] != "gpu_query":
            continue
        terminals = [event for event in records if event["status"] != "pending"]
        pending_records = [event for event in records if event["status"] == "pending"]
        if len(pending_records) > 1:
            raise SchemaError("GPU query has multiple registration records")
        if len(terminals) > 1:
            raise SchemaError("GPU query has multiple terminal result records")
        binding = records[0]
        for member in records[1:]:
            if (member["related"] != binding["related"] or member["context"] != binding["context"]
                    or member["name"] != binding["name"] or member["data"]["slot"] != binding["data"]["slot"]
                    or member["data"]["shared"] != binding["data"]["shared"]):
                raise SchemaError("GPU query lifecycle changes its immutable batch, operation, slot, sharing or name")
        event = terminals[0] if terminals else records[-1]
        if not pending_records and not losses["events"]:
            raise SchemaError("GPU query result has no recorded registration or declared event loss")
        if not terminals and complete:
            raise SchemaError("complete draw capture has an unfinished GPU query lifecycle")
        lifecycle_valid = bool(pending_records and terminals)
        duration, duration_origin, absolute_precision = _query_duration(event["data"], event["status"])
        row = {"query_id": identifier, "operation_id": event["context"]["operation"], "batch_id": event["related"],
               "status": event["status"], "duration_ns": duration, "shared": event["data"]["shared"],
               "lifecycle_status": "valid" if lifecycle_valid else "unknown",
               "duration_origin": duration_origin, "absolute_time_precision": absolute_precision,
               "begin_gpu_ticks": event["data"].get("begin_gpu_ticks"),
               "end_gpu_ticks": event["data"].get("end_gpu_ticks"), "period_ns": event["data"].get("period_ns"),
               "timestamp_valid_bits": event["data"].get("timestamp_valid_bits"),
               "timestamp_wraps": event["data"].get("timestamp_wraps"),
               "source": sources[event["id"]], "measures": sorted(measures[identifier], key=int)}
        measured = row["measures"]
        association = bool(measured) and lifecycle_valid
        for target in measured:
            if target not in entities:
                association = False
                continue
            members = entities[target]
            kind = members[0]["kind"]
            if kind == "host_command":
                if measurement_relations[identifier][target] != "measures":
                    raise SchemaError("host command query requires an explicit measures relation")
                if any(member["context"]["operation"] != row["operation_id"] for member in members):
                    raise SchemaError("GPU query and measured host command have different operation identities")
                for member in members:
                    command_batch = member["context"].get("batch")
                    if command_batch is None:
                        association = False
                    elif command_batch != row["batch_id"]:
                        raise SchemaError("GPU query and measured host command have different batch identities")
                    command_device, query_device = member["context"].get("device"), event["context"].get("device")
                    if command_device is not None and query_device is not None and command_device != query_device:
                        raise SchemaError("GPU query and measured host command have different device identities")
                measured_commands.add(target)
            elif kind == "present":
                if (row["shared"] is not True or event["context"]["present"] != target or event["name"] != "present_blit"
                        or measurement_relations[identifier][target] != "presentation_path"):
                    raise SchemaError("present-range query must explicitly share and bind the measured present identity")
            else:
                raise SchemaError("GPU query measures an unsupported known entity kind")
        row["association_status"] = "valid" if association else "unknown"
        if not association:
            issue("draw_gpu_association_unknown", f"Query {identifier} lacks complete recorded command/present linkage.", area="gpu_association")
        queries[identifier] = row
        if row["operation_id"] is not None:
            operation_queries[row["operation_id"]].append(row)
        if event["status"] in GPU_FAILURES:
            issue("draw_gpu_query_failure", f"GPU query {identifier}: {event['status']}.", area="gpu", invalid=True)
        elif event["status"] != "available":
            issue("draw_gpu_query_unavailable", f"GPU query {identifier}: {event['status']}.", area="gpu")
    result["index"]["queries"] = list(queries.values())
    result["coverage"]["lifecycle"]["gpu_queries"] = {
        "complete": sum(row["lifecycle_status"] == "valid" for row in queries.values()),
        "incomplete": sum(row["lifecycle_status"] != "valid" for row in queries.values())}
    for identifier, records in entities.items():
        if records[0]["kind"] != "host_command":
            continue
        beginnings = [entry for entry in records if entry["status"] == "unknown"]
        terminals = [entry for entry in records if entry["status"] != "unknown"]
        if len(beginnings) > 1 or len(terminals) > 1:
            raise SchemaError("host command has duplicate recording begins or terminal outcomes")
        if (not beginnings or not terminals) and not losses["events"]:
            raise SchemaError("host command requires one recording begin and one terminal outcome")
        binding = records[0]
        for member in records[1:]:
            if (member["thread_id"] != binding["thread_id"]
                    or any(member[key] != binding[key] for key in ("name", "related", "context", "value"))
                    or any(member["data"][key] != binding["data"][key] for key in ("requested_count", "count_dynamic"))):
                raise SchemaError("host command lifecycle changes immutable recording or request descriptors")
        lifecycle_valid = bool(beginnings and terminals)
        event = terminals[0] if terminals else beginnings[0]
        if lifecycle_valid and event["ts_ns"] < beginnings[0]["ts_ns"]:
            raise SchemaError("host command terminal precedes its recording begin")
        if not lifecycle_valid:
            issue("draw_host_command_lifecycle_incomplete", f"Host command {identifier} has a lost lifecycle record; elapsed recording time is unavailable.")
        result["index"]["host_commands"].append({"command_id": identifier, "name": event["name"],
            "operation_id": event["context"]["operation"], "status": event["status"],
            "lifecycle_status": "valid" if lifecycle_valid else "unknown",
            "recording_begin_ns": beginnings[0]["ts_ns"] if lifecycle_valid else None,
            "recording_end_ns": event["ts_ns"] if lifecycle_valid else None,
            "elapsed_ns": event["ts_ns"] - beginnings[0]["ts_ns"] if lifecycle_valid else None,
            "requested_count": event["data"]["requested_count"], "count_dynamic": event["data"]["count_dynamic"],
            "resolved_indirect_count": None, "source": sources[event["id"]],
            "source_events": [sources[entry["id"]] for entry in records]})
        if identifier not in measured_commands:
            issue("draw_command_query_missing", f"Recorded host command {identifier} has no linked GPU query range.", area="gpu_association")
    result["coverage"]["lifecycle"]["host_commands"] = {
        "complete": sum(row["lifecycle_status"] == "valid" for row in result["index"]["host_commands"]),
        "incomplete": sum(row["lifecycle_status"] != "valid" for row in result["index"]["host_commands"])}
    if (complete and queries and all(row["status"] == "available" for row in queries.values())
            and not losses["gpu_queries"] and not capture["pending_gpu_queries"] and result["gpu_status"] != "invalid"):
        result["gpu_status"] = "valid"
        result["coverage"]["gpu_tail"] = "valid" if complete else "unknown"
    elif not queries:
        issue("draw_gpu_queries_absent", "No per-command GPU query evidence was recorded.", area="gpu")
    if capture["pending_spans"] or capture["pending_jobs"]:
        issue("draw_host_work_pending", "Host scopes or queued jobs remained pending at finalization.")
    if capture["pending_gpu_queries"]:
        issue("draw_gpu_work_pending", "GPU queries remained pending after the bounded drain deadline.", area="gpu")

    shader_counts = Counter()
    operations = {}
    for identifier, records in entities.items():
        if records[0]["kind"] != "operation":
            continue
        names = {event["name"] for event in records if event["name"] is not None}
        if len(names) > 1:
            raise SchemaError("operation identity has conflicting names")
        terminal = [entry for entry in records if entry["status"] not in ("unknown", "pending")]
        if len(terminal) > 1:
            raise SchemaError("operation has multiple terminal lifecycle states")
        event = terminal[-1] if terminal else records[-1]
        begin_observed = any(entry["status"] == "unknown" or entry["end_ns"] is not None for entry in records)
        lifecycle_valid = bool(begin_observed and terminal)
        related_dictionaries = set()
        stack = [identifier, *operation_entities[identifier]]
        visited = set()
        while stack:
            node = stack.pop()
            if node in visited:
                continue
            visited.add(node)
            if node in dictionaries:
                related_dictionaries.add(node)
                stack.extend(dictionaries[node]["references"])
            stack.extend(identity_links[node])
        shader_ids = sorted((key for key in related_dictionaries if dictionaries[key]["kind"] in ("shader_variant", "guest_shader", "spirv")), key=int)
        pipeline_ids = sorted((key for key in related_dictionaries if dictionaries[key]["kind"] == "pipeline"), key=int)
        gpu = operation_queries[identifier]
        host = operation_spans[identifier]
        name = next(iter(names), "unknown")
        draw_kind = "dispatch" if "dispatch" in name.lower() else "draw" if "draw" in name.lower() else "other"
        if draw_kind in ("draw", "dispatch") and not shader_ids:
            issue("draw_shader_identity_missing", f"Operation {identifier} has no recorded shader dictionary linkage.", area="identity")
        for identity in {f"{dictionaries[shader]['kind']}:{dictionaries[shader]['key']}" for shader in shader_ids}:
            shader_counts[identity] += 1
        operations[identifier] = {"operation_id": identifier, "kind": draw_kind, "name": name,
                                  "context": event["context"], "status": event["status"], "shader_ids": shader_ids,
                                  "lifecycle_status": "valid" if lifecycle_valid else "unknown",
                                  "pipeline_ids": pipeline_ids, "span_ids": host, "gpu_query_ids": [row["query_id"] for row in gpu],
                                  "gpu_interval_sum_ns": sum(row["duration_ns"] for row in gpu)
                                  if gpu and not losses["events"] and all(row["duration_ns"] is not None and row["shared"] is False
                                                 and row["association_status"] == "valid" for row in gpu) else None,
                                  "gpu_interval_meaning": "sum of recorded nonshared intervals; overlap is possible; not GPU busy time",
                                  "source_events": [sources[row["id"]] for row in records]}
    result["index"]["draws"] = list(operations.values())
    result["summary"]["source_examples"] = [{"operation_id": row["operation_id"], "status": row["status"],
        "source": row["source_events"][0]} for row in list(operations.values())[:8] if row["source_events"]]
    result["coverage"]["lifecycle"]["operations"] = {
        "complete": sum(row["lifecycle_status"] == "valid" for row in operations.values()),
        "incomplete": sum(row["lifecycle_status"] != "valid" for row in operations.values())}
    if result["coverage"]["boundaries"] == "valid":
        timestamps = [boundary["ts_ns"] for boundary in boundaries]
        for index, (opening, closing) in enumerate(zip(boundaries, boundaries[1:])):
            result["index"]["present_intervals"].append({"interval_index": index,
                "opening_present": opening["present"], "closing_present": closing["present"],
                "start_ns": opening["ts_ns"], "end_ns": closing["ts_ns"],
                "opening_source": {"json_pointer": f"/boundaries/{index}"},
                "closing_source": {"json_pointer": f"/boundaries/{index + 1}"},
                "meaning": "measured host present interval; not exclusive ownership of work"})
        for row in result["index"]["spans"]:
            row["time_window_membership"] = _time_membership(timestamps, row["start_ns"], row["end_ns"])
        for identifier, row in operations.items():
            records = entities[identifier]
            beginnings = [event for event in records if event["status"] == "unknown" or event["end_ns"] is not None]
            row["start_ns"] = min(event["ts_ns"] for event in records) if beginnings or not losses["events"] else None
            ends = [event["end_ns"] for event in records if event["end_ns"] is not None]
            terminal = [event for event in records if event["status"] != "unknown"]
            row["end_ns"] = max(ends) if ends else max(event["ts_ns"] for event in terminal) if terminal and len(records) > 1 else None
            row["time_window_membership"] = (_time_membership(timestamps, row["start_ns"], row["end_ns"])
                                               if row["start_ns"] is not None else None)
    observed_counts = {"operations": len(operations),
        "requested_draw_operations": sum(row["kind"] == "draw" and "indirect_record" not in row["name"].lower() for row in operations.values()),
        "indirect_record_operations": sum("indirect_record" in row["name"].lower() for row in operations.values()),
        "dispatch_operations": sum(row["kind"] == "dispatch" for row in operations.values()),
        "host_draw_commands": sum("draw" in (row["name"] or "").lower() for row in result["index"]["host_commands"]),
        "host_dispatch_commands": sum("dispatch" in (row["name"] or "").lower() for row in result["index"]["host_commands"]),
        "host_spans": span_count, "gpu_queries": len(queries)}
    observed_counts.update({f"operation_status.{status}": count for status, count
                            in Counter(row["status"] for row in operations.values()).items()})
    result["observed_counts"] = {"meaning": "counts of retained records and entities; lost work is unknown",
                                 "values": observed_counts}
    aggregate_valid = (result["coverage"]["boundaries"] == "valid" and closed_window
        and not losses["events"] and not losses["metadata"]
        and not capture["pending_spans"] and not capture["pending_jobs"]
        and all(thread["acknowledged"] for thread in threads.values()))
    workload_valid = (aggregate_valid
        and all(row["lifecycle_status"] == "valid" for row in operations.values())
        and all(row["lifecycle_status"] == "valid" and row["status"] == "complete"
                and row["operation_id"] in operations for row in result["index"]["host_commands"]))
    result["workload_status"] = "valid" if workload_valid else "unknown"
    result["workload_meaning"] = ("Counts of retained instrumented operation attempts and complete host-command recordings; "
        "not exclusive frame ownership, resolved indirect execution counts or shader identity completeness")
    if workload_valid:
        result["metrics"]["workload"] = dict(observed_counts)
    for name, totals in functions.items():
        available = aggregate_valid and totals["calls"] > 0 and not totals["clipped"] and not totals["boundary_crossing"]
        result["function_availability"][name] = "valid" if available else "unknown"
        if not available:
            continue
        for key in ("wall_ns", "self_ns", "wait_ns"):
            result["metrics"]["functions"][f"{name}.{key[:-3]}_ms"] = totals[key] / 1_000_000
        result["metrics"]["functions"][f"{name}.calls"] = totals["calls"]
    if result["identity_status"] == "valid":
        result["metrics"]["shaders"] = dict(shader_counts)
    if result["gpu_status"] == "valid":
        result["metrics"]["gpu"] = {"sum_of_recorded_intervals_ms_including_shared_and_overlap": sum(row["duration_ns"] for row in queries.values()) / 1_000_000,
                                    "available_queries": len(queries)}
    statuses = [result[key] for key in ("identity_status", "cpu_status", "gpu_status", "gpu_association_status")]
    result["status"] = "invalid" if "invalid" in statuses else "valid" if all(status == "valid" for status in statuses) and complete else "unknown"
    result["index_counts"] = {"spans": span_count, "operations": len(operations), "queries": len(queries),
                              "host_commands": len(result["index"]["host_commands"]), "dictionaries": len(dictionaries)}
    if analysis_index is not None:
        analysis_index.update(result["index"])
    if not include_index:
        result.pop("index")
        result["index_omitted"] = "Full derived detail is opt-in; canonical raw records and exact event pointer template remain available."
        result["event_source"] = {"canonical_event_count": len(events), "json_pointer_template": "/events/{index}"}
    return result


def report_from_health(run_dir, health):
    detail = health.get("draw_attribution")
    return {"schema_version": 1, "document_type": "anyps5.draw-report", "run_id": health["run_id"],
            "status": "invalid" if health["status"] == "invalid" else detail["status"] if detail is not None else "unknown",
            "capture_health_status": health["status"], "detail": detail,
            "report_detail": "full" if detail is not None and "index" in detail else "compact",
            "issues": health["issues"], "limitations": detail["limitations"] if detail is not None else list(LIMITATIONS),
            "source_directory": str(Path(run_dir).resolve())}


def report_capture(run_dir, *, full_index=False):
    from capture_health import assess_capture
    return report_from_health(run_dir, assess_capture(run_dir, include_draw_index=full_index))


def _cell(value):
    return str(value).replace("|", "&#124;").replace("<", "&lt;").replace(">", "&gt;").replace("\r", " ").replace("\n", " ")


def iter_text_report(report):
    yield f"Draw attribution: {report['status']}; run {report['run_id']}\n\n"
    detail = report.get("detail")
    if detail is None:
        yield "No independently validated draw sidecar is available; legacy attribution remains unknown.\n"
    else:
        yield (f"Shader identity: {detail['identity_status']}; host scopes: {detail['cpu_status']}; GPU measurements: {detail['gpu_status']}; "
               f"command associations: {detail['gpu_association_status']}; origin graph: {detail['origin_graph_status']}\n")
        counts = detail.get("observed_counts")
        if counts is not None:
            yield f"Retained observations only: {_cell(json.dumps(counts['values'], sort_keys=True))}; lost work remains unknown.\n"
        yield "\n| Host present interval | Open present | Close present | Start (ns) | End (ns) |\n| --- | --- | --- | ---: | ---: |\n"
        for interval in detail.get("index", {}).get("present_intervals", detail.get("summary", {}).get("frames", [])):
            yield (f"| {interval['interval_index']} | {interval['opening_present']} | {interval['closing_present']} | "
                   f"{interval['start_ns']} | {interval['end_ns']} |\n")
        yield "Host interval membership describes timing overlap, never presented-frame ownership. GPU interval membership is unavailable.\n"
        yield "\n| Operation | Kind | Shader dictionary IDs | Host span IDs | GPU query IDs | Source event IDs |\n| --- | --- | --- | --- | --- | --- |\n"
        artifact = Path(report["source_directory"]) / detail.get("artifact", {}).get("path", "")
        if "index" not in detail:
            yield "Compact report: detailed span/operation rows are omitted; canonical raw evidence is preserved. Use --full-index for explicit export.\n"
            yield f"Validated function summaries: {_cell(json.dumps(detail.get('metrics', {}).get('functions', {}), sort_keys=True))}\n"
        for draw in detail.get("index", {}).get("draws", []):
            links = [f"[{source['event_id']}](<{artifact.as_posix()}#{source['json_pointer']}>)" for source in draw["source_events"]]
            yield (f"| {draw['operation_id']} | {_cell(draw['name'])} | {', '.join(draw['shader_ids']) or 'unknown'} | "
                   f"{', '.join(draw['span_ids']) or 'unknown'} | {', '.join(draw['gpu_query_ids']) or 'unavailable'} | {', '.join(links)} |\n")
        yield "\n| Function / scope | Class | Wall (ms) | Self (ms) | Source |\n| --- | --- | ---: | ---: | --- |\n"
        for span in detail.get("index", {}).get("spans", []):
            source = span["source"]
            self_text = f"{span['self_ns'] / 1_000_000:.6f}" if span["self_ns"] is not None else "unavailable"
            yield (f"| {_cell(span['function'])} | {span['classification']} | {span['wall_ns'] / 1_000_000:.6f} | "
                   f"{self_text} | [{source['event_id']}](<{artifact.as_posix()}#{source['json_pointer']}>) |\n")
    yield "\n"
    for item in report["issues"]:
        yield f"- {_cell(item['code'])}: {_cell(item['message'])}\n"
    yield "\n"
    for value in report["limitations"]:
        yield f"- {_cell(value)}\n"


def text_report(report):
    return "".join(iter_text_report(report))


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="Independently validate and index selected per-draw evidence; no replay or performance verdict.")
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--full-index", action="store_true")
    args = parser.parse_args(argv)
    try:
        report = report_capture(args.run_dir, full_index=args.full_index)
        if args.json:
            json.dump(report, sys.stdout, indent=2, ensure_ascii=False, allow_nan=False)
            sys.stdout.write("\n")
        else:
            sys.stdout.writelines(iter_text_report(report))
    except (OSError, ValueError, OverflowError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")
    return 2 if report["status"] == "invalid" else 0 if report["status"] == "valid" else 1


if __name__ == "__main__":
    raise SystemExit(main())
