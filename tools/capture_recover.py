import argparse
import json
import sys
from pathlib import Path
from uuid import UUID

from capture_schema import SchemaError, atomic_write_json, contained_path, loads_json, read_json, sha256_file


MAX_JOURNAL_BYTES = 16 * 1024 ** 3
MAX_LINE_BYTES = 64 * 1024 ** 2
MAX_EVENTS = 8_388_608
MAX_RETAINED_EVENTS = 65_536
KINDS = frozenset({"header", "thread", "dictionary", "events", "boundary", "checkpoint", "footer"})
EVENT_KINDS = frozenset({"submission", "segment", "packet", "operation", "host_command", "batch", "flip", "present",
    "link", "job", "span", "boundary", "calibration", "gpu_query", "attribute", "reference", "counter"})


def _uint(value, name, maximum=2 ** 64 - 1):
    if type(value) is not int or not 0 <= value <= maximum:
        raise SchemaError(f"{name} must be a bounded unsigned integer")
    return value


def _identifier(value, name):
    if type(value) is not str or not value.isascii() or not value.isdigit() or str(int(value)) != value or int(value) >= 2 ** 64:
        raise SchemaError(f"{name} must be a canonical uint64 decimal string")
    return value


def _uuid(value, name):
    try:
        valid = str(UUID(value)) == value
    except (ValueError, TypeError, AttributeError):
        valid = False
    if not valid:
        raise SchemaError(f"{name} must be a canonical UUID")


def _event(event):
    if event.get("kind") not in EVENT_KINDS or type(event.get("context")) is not dict or type(event.get("data")) is not dict:
        raise SchemaError("journal event kind or canonical payload is invalid")
    lifecycle = ("unknown", "complete", "skipped", "rejected", "failed", "cancelled")
    query = ("pending", "available", "unsupported", "allocation_failed", "capacity_dropped", "read_failed", "invalid_timestamp", "uncollected")
    if event.get("status") not in (query if event["kind"] == "gpu_query" else lifecycle):
        raise SchemaError("journal event status does not match its kind")
    _uint(event.get("value"), "journal event value")
    for name in ("subject", "related", "parent_span"):
        if event.get(name) is not None:
            _identifier(event[name], f"journal event {name}")
    for name in ("name", "text"):
        if event.get(name) is not None and (type(event[name]) is not str or "\0" in event[name]):
            raise SchemaError("journal event text field is invalid")
    for name in ("driver", "submission", "segment", "packet", "operation", "device", "batch", "job", "flip", "present"):
        if event["context"].get(name) is not None:
            _identifier(event["context"][name], f"journal context {name}")
    for name in ("queue", "packet_offset", "opcode"):
        if event["context"].get(name) is not None:
            _uint(event["context"][name], f"journal context {name}")
        elif name == "packet_offset":
            raise SchemaError("journal context packet_offset is absent")


def recover_journal(path, manifest, *, descriptor=None, max_retained_events=MAX_RETAINED_EVENTS):
    path = Path(path)
    _uint(max_retained_events, "retained event bound", MAX_RETAINED_EVENTS)
    expected_pid = manifest["execution"].get("pid")
    expected_name = f"aps5-draw-{manifest['run_id']}-{expected_pid}.journal.jsonl"
    report = {"schema_version": 1, "document_type": "anyps5.journal-recovery", "run_id": manifest["run_id"],
              "capture_id": None, "status": "unknown", "capture_eligibility": "invalid", "complete": False,
              "final_sidecar_required": True, "committed_lines": 0, "committed_bytes": 0,
              "truncated_tail_bytes": 0, "event_count": 0, "retained_events": [], "omitted_event_count": 0,
              "threads": [], "dictionaries": [], "boundaries": [], "last_checkpoint": None,
              "footer": None, "issues": [], "source": {"path": path.name, "sha256": None, "size": None},
              "manifest_descriptor_status": "recorded" if descriptor is not None else "unavailable",
              "limitations": ["A committed journal prefix never proves a complete capture or replaces the final sidecar.",
                  "Newline commits a record; the journal has no per-record checksum or filesystem corruption proof.",
                  "Retained events are bounded observations, not a complete-window workload or timing aggregate."]}
    try:
        if path.name != expected_name:
            raise SchemaError("journal filename does not match the run UUID and actual child PID")
        if path.stat().st_size > MAX_JOURNAL_BYTES:
            raise SchemaError("journal exceeds its 16 GiB byte bound")
        size, digest = path.stat().st_size, sha256_file(path)
        report["source"].update(size=size, sha256=digest)
        if descriptor is not None and (descriptor.get("kind") != "draw_journal" or descriptor.get("sha256") != digest or descriptor.get("size") != size):
            raise SchemaError("journal descriptor integrity or artifact kind is invalid")
        sequence, event_ids, dictionary_ids, thread_ids, offsets = 0, set(), set(), set(), {}
        header, footer, byte_budget = None, False, None
        with path.open("rb") as source:
            while True:
                line = source.readline(MAX_LINE_BYTES + 1)
                if not line:
                    break
                if len(line) > MAX_LINE_BYTES:
                    raise SchemaError("journal record exceeds its 64 MiB line bound")
                if byte_budget is not None and report["committed_bytes"] + len(line) > byte_budget:
                    raise SchemaError("journal input exceeds its declared byte budget")
                if not line.endswith(b"\n"):
                    report["truncated_tail_bytes"] = len(line)
                    break
                if footer:
                    raise SchemaError("committed records follow the journal footer")
                envelope = loads_json(line.decode("utf-8"), context=f"journal line {sequence + 1}")
                if type(envelope) is not dict or type(envelope.get("schema_version")) is not int or envelope["schema_version"] != 1 or envelope.get("document_type") != "anyps5.draw-journal":
                    raise SchemaError("unsupported journal envelope")
                if envelope.get("run_id") != manifest["run_id"] or envelope.get("pid") != expected_pid or type(envelope.get("pid")) is not int:
                    raise SchemaError("journal record does not bind the actual captured child")
                _uuid(envelope.get("capture_id"), "journal capture UUID")
                if envelope["capture_id"] == manifest["run_id"]:
                    raise SchemaError("journal capture UUID must differ from the run UUID")
                if report["capture_id"] is not None and envelope["capture_id"] != report["capture_id"]:
                    raise SchemaError("journal capture UUID changes across records")
                report["capture_id"] = envelope["capture_id"]
                if type(envelope.get("sequence")) is not int or envelope["sequence"] != sequence:
                    raise SchemaError("journal sequence is duplicated, missing or reordered")
                kind, data = envelope.get("kind"), envelope.get("data")
                if kind not in KINDS or type(data) is not dict:
                    raise SchemaError("journal kind or data is invalid")
                if sequence == 0 and kind != "header" or sequence > 0 and kind == "header":
                    raise SchemaError("journal requires exactly one initial header")
                if kind == "header":
                    header = data
                    window = {key: data.get(key) for key in ("unit", "start", "length")}
                    if (data.get("time_unit") != "ns" or data.get("clock_domain") != "steady_clock"
                            or window != manifest["capture"]
                            or data.get("batch_events") != 4096):
                        raise SchemaError("journal header does not match the recorded window or protocol bounds")
                    byte_budget = _uint(data.get("maximum_bytes"), "journal declared byte budget", MAX_JOURNAL_BYTES)
                    if byte_budget == 0:
                        raise SchemaError("journal declared byte budget must be positive")
                    report["declared_maximum_bytes"] = byte_budget
                    if size > byte_budget:
                        raise SchemaError("journal input exceeds its declared byte budget")
                    _identifier(data.get("origin_steady_ns"), "journal steady-clock origin")
                    _uint(window["start"], "journal capture start")
                    _uint(window["length"], "journal capture length")
                    report["window"] = window
                elif kind == "thread":
                    identifier = _identifier(data.get("thread_id"), "journal thread ID")
                    if identifier in thread_ids or len(thread_ids) >= 1024:
                        raise SchemaError("journal thread definitions are duplicated or exceed their bound")
                    thread_ids.add(identifier)
                    if type(data.get("name")) is not str or type(data.get("registered_ns")) is not int:
                        raise SchemaError("journal thread definition is malformed")
                    if data.get("native_thread_id") is not None:
                        _identifier(data["native_thread_id"], "journal native thread ID")
                    offsets[identifier] = 0
                    report["threads"].append(data)
                elif kind == "dictionary":
                    identifier = _identifier(data.get("id"), "journal dictionary ID")
                    if identifier in dictionary_ids or len(dictionary_ids) >= 262_144:
                        raise SchemaError("journal dictionaries are duplicated or exceed their bound")
                    dictionary_ids.add(identifier)
                    if (type(data.get("kind")) is not str or type(data.get("key")) is not str or type(data.get("fields")) is not dict
                            or any(type(key) is not str or type(value) is not str for key, value in data["fields"].items())):
                        raise SchemaError("journal dictionary definition is malformed")
                    if type(data.get("references")) is not list:
                        raise SchemaError("journal dictionary references must be an array")
                    for reference in data["references"]:
                        _identifier(reference, "journal dictionary reference")
                    report["dictionaries"].append(data)
                elif kind == "boundary":
                    present = int(_identifier(data.get("present"), "journal present ordinal"))
                    if type(data.get("ts_ns")) is not int or len(report["boundaries"]) >= 65_536:
                        raise SchemaError("journal boundary timestamp or capacity is invalid")
                    if report["boundaries"] and (present != int(report["boundaries"][-1]["present"]) + 1 or data["ts_ns"] < report["boundaries"][-1]["ts_ns"]):
                        raise SchemaError("journal boundaries are reversed or noncontiguous")
                    report["boundaries"].append(data)
                elif kind == "events":
                    thread = _identifier(data.get("thread_id"), "journal batch thread ID")
                    events = data.get("events")
                    if (thread not in thread_ids or type(events) is not list or not 0 < len(events) <= 4096
                            or type(data.get("count")) is not int or data["count"] != len(events)
                            or type(data.get("first_index")) is not int or data["first_index"] != offsets[thread]):
                        raise SchemaError("journal event batch thread, index or count is inconsistent")
                    for index, event in enumerate(events):
                        if type(event) is not dict or event.get("thread_id") != thread:
                            raise SchemaError("journal event does not match its registered batch thread")
                        identifier = _identifier(event.get("id"), "journal event ID")
                        if identifier in event_ids or len(event_ids) >= MAX_EVENTS:
                            raise SchemaError("journal event IDs are duplicated or exceed the canonical bound")
                        event_ids.add(identifier)
                        _event(event)
                        if type(event.get("ts_ns")) is not int or event.get("end_ns") is not None and (type(event["end_ns"]) is not int or event["end_ns"] < event["ts_ns"]):
                            raise SchemaError("journal event interval is invalid")
                        if len(report["retained_events"]) < max_retained_events:
                            report["retained_events"].append({"event": event, "source": {"line": sequence + 1,
                                "json_pointer": f"/data/events/{index}", "event_id": identifier}})
                        report["event_count"] += 1
                    offsets[thread] += len(events)
                elif kind == "checkpoint":
                    if data.get("complete") is not False or data.get("state") not in ("recording", "draining", "frozen"):
                        raise SchemaError("journal checkpoint falsely claims complete or has invalid state")
                    _uint(data.get("emitted_events"), "journal checkpoint event count", MAX_EVENTS)
                    if data["emitted_events"] != report["event_count"]:
                        raise SchemaError("journal checkpoint emitted count contradicts committed event rows")
                    for field in ("pending_spans", "pending_jobs", "pending_gpu_queries"):
                        _uint(data.get(field), f"journal checkpoint {field}")
                    losses = data.get("losses")
                    if type(losses) is not dict:
                        raise SchemaError("journal checkpoint loss ledger is missing")
                    for field in ("events", "metadata", "gpu_queries"):
                        _uint(losses.get(field), f"journal loss {field}")
                    report["last_checkpoint"] = data
                elif kind == "footer":
                    if (data.get("complete") is not False or data.get("final_sidecar_required") is not True
                            or data.get("stop_reason") not in ("exhausted", "complete", "io_failed", "byte_budget")):
                        raise SchemaError("journal footer falsely claims a complete final capture")
                    _uint(data.get("emitted_events"), "journal footer event count", MAX_EVENTS)
                    if data["emitted_events"] != report["event_count"]:
                        raise SchemaError("journal footer emitted count contradicts committed event rows")
                    report["footer"], footer = data, True
                sequence += 1
                report["committed_lines"] = sequence
                report["committed_bytes"] += len(line)
        if header is None:
            raise SchemaError("journal has no committed header")
        if path.stat().st_size != size or sha256_file(path) != digest:
            raise SchemaError("journal changed while its committed prefix was inspected")
        report["omitted_event_count"] = report["event_count"] - len(report["retained_events"])
        report["issues"].append({"code": "journal_prefix_only", "severity": "warning",
            "message": "Recovered committed records remain partial and ineligible for complete-window comparisons."})
    except (OSError, ValueError, UnicodeError, OverflowError) as error:
        report["status"] = "invalid"
        report["issues"].append({"code": "journal_recovery_invalid", "severity": "error", "message": str(error)})
    return report


def recover_run(run_dir, *, manifest=None, journal=None, max_retained_events=MAX_RETAINED_EVENTS):
    run_dir = Path(run_dir)
    manifest = manifest or read_json(run_dir / "manifest.json")
    descriptors = [item for item in manifest["artifacts"] if item["kind"] == "draw_journal"]
    if journal is not None:
        selected = contained_path(run_dir, journal, must_exist=True)
        matches = [item for item in descriptors if item["path"] == journal]
        return recover_journal(selected, manifest, descriptor=matches[0] if len(matches) == 1 else None,
                               max_retained_events=max_retained_events)
    if len(descriptors) != 1:
        raise SchemaError("recovery requires exactly one explicitly recorded draw_journal artifact")
    return recover_journal(contained_path(run_dir, descriptors[0]["path"], must_exist=True), manifest,
                           descriptor=descriptors[0], max_retained_events=max_retained_events)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Inspect committed crash-journal records; never promote a journal prefix to a complete capture.")
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--json-output", type=Path, required=True)
    parser.add_argument("--journal", help="Explicit contained journal path for a crashed run whose manifest has no finalized inventory")
    args = parser.parse_args(argv)
    try:
        report = recover_run(args.run_dir, journal=args.journal)
        atomic_write_json(args.json_output, report, replace=False)
        print(json.dumps({key: report[key] for key in ("status", "event_count", "committed_lines", "complete", "capture_eligibility")}))
        return 2 if report["status"] == "invalid" else 1
    except (OSError, ValueError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
