import argparse
import gzip
import json
import math
import re
import sys
from datetime import datetime, timedelta
from pathlib import Path

from capture_manifest import file_identity, safe_path
from capture_schema import (SchemaError, capture_spec, contained_path, loads_json, read_json,
                            validate_evidence_report, validate_manifest, validate_measurement_workload, WORKLOAD_KEYS)


PERCENTILES = ("p50", "p90", "p99", "max")
LIMITATIONS = [
    "Slices shorter than 20 microseconds are folded; their individual scope and ordering are unavailable.",
    "The current producer omits program ranges.",
    "GPU work collected after the window closes can be absent; the GPU tail is not proven complete.",
    "The producer does not establish per-thread coverage or account for every uninstrumented thread.",
    "GPU query availability and missing completion records are not exhaustively recorded in the trace summary.",
    "The closing present boundary is omitted from trace instants; the final frame interval is not independently observed in the trace.",
    "A false trace_dropped flag establishes neither complete work coverage nor causal attribution.",
]


def _number(value, name, *, positive=False, signed=False):
    try:
        valid = type(value) in (int, float) and math.isfinite(value) and (signed or value >= 0)
    except OverflowError:
        valid = False
    if not valid or (positive and value == 0):
        qualifier = "positive " if positive else "" if signed else "non-negative "
        raise SchemaError(f"{name} must be a finite {qualifier}number")
    return value


def _integer(value, name, *, positive=False):
    if type(value) is not int or value < (1 if positive else 0):
        raise SchemaError(f"{name} must be a {'positive' if positive else 'non-negative'} integer")
    _number(value, name)
    return value


def _object(value, name):
    if not isinstance(value, dict):
        raise SchemaError(f"{name} must be an object")
    return value


def _verification_valid(result):
    if (not isinstance(result, dict) or type(result.get("verified")) is not bool
            or not isinstance(result.get("findings"), list) or not isinstance(result.get("checked_at"), str)):
        return False
    try:
        timestamp = datetime.fromisoformat(result["checked_at"].replace("Z", "+00:00"))
        if "T" not in result["checked_at"] or timestamp.utcoffset() != timedelta(0):
            return False
    except ValueError:
        return False
    for finding in result["findings"]:
        if (not isinstance(finding, dict) or not isinstance(finding.get("path"), str)
                or finding.get("reason") not in ("missing", "changed", "unreadable", "unsafe")
                or not isinstance(finding.get("expected"), dict)):
            return False
        for identity in (finding["expected"], finding.get("actual")):
            if identity is None:
                continue
            if (not isinstance(identity, dict) or type(identity.get("size")) is not int or identity["size"] < 0
                    or not isinstance(identity.get("sha256"), str)
                    or re.fullmatch(r"[0-9a-f]{64}", identity["sha256"]) is None):
                return False
    return True


def _summary(path, selector):
    records = []
    with path.open(encoding="utf-8") as source:
        for index, line in enumerate(source, 1):
            if not line.strip():
                continue
            record = _object(loads_json(line, context=f"{path.name}:{index}"), "summary")
            if record.get("trace") == selector:
                records.append(record)
    if len(records) != 1:
        raise SchemaError(f"expected one summary matching selected trace {selector!r}, found {len(records)}")
    summary = records[0]
    for key in ("commit", "trace", "gpu", "driver", "os", "resolution"):
        if not isinstance(summary.get(key), str):
            raise SchemaError(f"summary.{key} must be a string")
    if "dirty" not in summary or (summary["dirty"] is not None and type(summary["dirty"]) is not bool):
        raise SchemaError("summary.dirty must be present as a boolean or null")
    env = _object(summary.get("env"), "summary.env")
    if any(not isinstance(key, str) or not isinstance(value, str) for key, value in env.items()):
        raise SchemaError("summary.env must map strings to strings")
    window = _object(summary.get("window"), "summary.window")
    if not isinstance(window.get("spec"), str):
        raise SchemaError("summary.window.spec must be a string")
    _integer(window.get("first_present"), "summary.window.first_present")
    duration = _number(window.get("ms"), "summary.window.ms", positive=True)
    count = _integer(summary.get("presents"), "summary.presents", positive=True)
    frames = _object(summary.get("frame_ms"), "summary.frame_ms")
    values = [_number(frames.get(key), f"summary.frame_ms.{key}") for key in PERCENTILES]
    if values != sorted(values):
        raise SchemaError("summary.frame_ms must satisfy p50 <= p90 <= p99 <= max")
    if duration / count > values[-1] + 0.002 or values[-1] > duration + 0.002:
        raise SchemaError("summary frame maximum is inconsistent with its count and duration")
    for key in ("worker_busy_pct", "top_phases_ms_per_frame"):
        for name, value in _object(summary.get(key), f"summary.{key}").items():
            _number(value, f"summary.{key}.{name}")
    _number(summary.get("gpu_busy_ms_per_present"), "summary.gpu_busy_ms_per_present")
    if type(summary.get("trace_dropped")) is not bool:
        raise SchemaError("summary.trace_dropped must be a boolean")
    for name, dropped in _object(summary.get("trace_dropped_threads", {}), "summary.trace_dropped_threads").items():
        dropped = _object(dropped, f"summary.trace_dropped_threads.{name}")
        if type(dropped.get("totals_full")) is not bool:
            raise SchemaError("dropped-thread totals_full must be a boolean")
        if "events_lost" in dropped:
            _integer(dropped["events_lost"], "dropped-thread events_lost")
        if "events_full_after_s" in dropped:
            _number(dropped["events_full_after_s"], "dropped-thread events_full_after_s")
            if dropped["events_full_after_s"] > duration / 1000 + 0.001:
                raise SchemaError("dropped-thread cutoff lies outside the summary window")
    return summary


def _trace(path):
    opener = gzip.open if path.suffix.lower() == ".gz" else Path.open
    with opener(path, "rt", encoding="utf-8") as source:
        trace = _object(loads_json(source.read(), context=path.name), "trace")
    if "run_id" in trace and not isinstance(trace["run_id"], str):
        raise SchemaError("trace.run_id must be a string when present")
    if "schema_version" in trace and (type(trace["schema_version"]) is not int or trace["schema_version"] != 1):
        raise SchemaError("Capture artifact schema version is unsupported")
    events = trace.get("traceEvents")
    if not isinstance(events, list):
        raise SchemaError("trace.traceEvents must be an array")
    process_ids = set()
    thread_names = {}
    presents = []
    losses = []
    gpu_events = 0
    for event in events:
        event = _object(event, "trace event")
        if not isinstance(event.get("name"), str) or not isinstance(event.get("ph"), str):
            raise SchemaError("trace events require string name and ph")
        timestamp = _number(event.get("ts"), "trace event.ts", signed=True)
        pid = _integer(event.get("pid"), "trace event.pid")
        tid = _integer(event.get("tid"), "trace event.tid")
        process_ids.add(pid)
        args = _object(event.get("args", {}), "trace event.args")
        if event["ph"] == "X":
            _number(event.get("dur"), "trace event.dur")
            _number(timestamp + event["dur"], "trace event end", signed=True)
        if event["ph"] == "C":
            for key, value in args.items():
                _number(value, f"trace counter.{key}")
        if event["ph"] == "M" and event["name"] == "thread_name":
            if not isinstance(args.get("name"), str):
                raise SchemaError("thread metadata requires a string name")
            if tid in thread_names and thread_names[tid] != args["name"]:
                raise SchemaError("conflicting names for a trace thread")
            thread_names[tid] = args["name"]
        if event["name"] == "present":
            if event["ph"] != "i":
                raise SchemaError("present events must be instant events")
            presents.append((_integer(args.get("present"), "trace present id"), timestamp, tid))
        if event["name"] == "trace buffer full":
            losses.append(_integer(args.get("events_lost"), "trace events_lost", positive=True))
    if len(process_ids) != 1:
        raise SchemaError("trace must identify exactly one process")
    for event in events:
        if event["ph"] == "X" and thread_names.get(event["tid"]) == "gpu":
            gpu_events += 1
    return {"presents": presents, "pid": next(iter(process_ids)), "thread_names": thread_names,
            "run_id": trace.get("run_id"), "schema_version": trace.get("schema_version"),
            "events_lost": sum(losses), "gpu_events": gpu_events, "event_count": len(events)}


def assess_capture(run_dir, manifest=None, *, include_draw_index=False):
    run_dir = safe_path(str(Path(run_dir).absolute()), "run directory", resolve=True)
    if manifest is None:
        manifest = read_json(run_dir / "manifest.json")
    validate_manifest(manifest, run_dir=run_dir)
    report = {
        "schema_version": 1, "document_type": "anyps5.capture-health", "run_id": manifest["run_id"],
        "status": "unknown", "frame_metrics": {"status": "unknown", "metrics": {}},
        "detailed_attribution": {"status": "unknown", "limitations": list(LIMITATIONS)},
        "provenance": {"status": "unknown", "loaded_modules_verified": False,
                       "source_binary_binding": "unverified", "runtime_files_verified": False,
                       "runtime_boundaries_verified": False, "producer_revision_verified": False},
        "issues": [], "artifacts_selected": [], "coverage": {
            "requested_window": manifest["capture"], "capture_completed": False,
            "thread_coverage": "unknown", "gpu_tail": "unknown", "program_ranges": "unavailable",
            "query_availability": "unknown", "all_artifacts_complete": "unknown",
        },
    }

    def issue(code, message, *, area="frame", severity="error"):
        report["issues"].append({"code": code, "severity": severity, "message": message})
        if severity == "error":
            key = {"frame": "frame_metrics", "attribution": "detailed_attribution", "provenance": "provenance"}[area]
            report[key]["status"] = "invalid"

    if manifest["state"] != "completed" or manifest["exit_code"] != 0:
        issue("run_incomplete", "Run did not complete successfully; preserved evidence cannot pass strict comparison.")
    execution = manifest.get("execution", {})
    collector_errors = execution.get("collector_errors", [])
    report["coverage"]["collectors"] = {"status": "invalid" if collector_errors else "unknown",
        "scope": "requested collector lifecycle; native frame timing is assessed independently"}
    for collector_error in collector_errors:
        issue("collector_collection_failed", f"{collector_error.get('phase', 'unknown phase')}: "
              f"{collector_error.get('type', 'error')}: {collector_error.get('message', 'no detail')}", area="provenance")
    for observation_error in execution.get("observation_errors", []):
        issue("observation_collection_failed", f"{observation_error.get('phase', 'unknown phase')}: "
              f"{observation_error.get('type', 'error')}: {observation_error.get('message', 'no detail')}",
              area="provenance", severity="warning")
    if execution.get("artifact_findings"):
        issue("artifact_selection_findings", "Runner recorded mixed, ambiguous, or unmatched capture artifacts.")
    termination = execution.get("termination", {})
    if isinstance(termination, dict) and termination.get("kind") in ("interrupt", "interrupted", "timeout", "external", "terminated"):
        issue("run_terminated", "The run records interruption or external termination.")
    inventory = execution.get("inventory_verification", {})
    if not isinstance(inventory, dict):
        issue("runtime_verification_invalid", "Runtime inventory verification must be an object.", area="provenance")
        inventory = {}
    for boundary in ("before", "after"):
        result = inventory.get(boundary)
        if result is None:
            issue("runtime_verification_missing", f"No usable {boundary}-execution runtime verification was recorded.",
                  area="provenance", severity="warning")
        elif not _verification_valid(result):
            issue("runtime_verification_invalid", f"Malformed {boundary}-execution runtime verification.", area="provenance")
        elif not result["verified"] or result["findings"]:
            issue("runtime_verification_failed", f"{boundary.capitalize()}-execution runtime verification reported missing or changed files.",
                  area="provenance")
    report["provenance"]["runtime_boundaries_verified"] = all(
        _verification_valid(inventory.get(boundary)) and inventory[boundary].get("verified") is True
        and inventory[boundary].get("findings") == [] for boundary in ("before", "after"))
    report["provenance"]["runtime_files_verified"] = report["provenance"]["runtime_boundaries_verified"]
    report["provenance"]["runtime_verification_scope"] = "recorded_before_and_after_execution"
    issue("source_binary_binding_unverified", "Inspected source and declared build configuration do not prove which source produced the runtime files.",
          area="provenance", severity="warning")
    issue("loaded_modules_unverified", "Declared file hashes do not prove which modules the process loaded.",
          area="provenance", severity="warning")
    usable = {}
    for descriptor in manifest["artifacts"]:
        try:
            path = contained_path(run_dir, descriptor["path"], must_exist=True)
            if file_identity(path) != {key: descriptor[key] for key in ("size", "sha256")}:
                raise SchemaError("artifact content does not match its recorded size and SHA-256")
            usable[descriptor["path"]] = path
        except (OSError, ValueError) as error:
            issue("artifact_integrity", f"{descriptor['path']}: {error}")
    process_evidence, external_evidence = _assess_auxiliary_evidence(run_dir, manifest, usable, report, issue)
    missing_completions = 0
    for descriptor in manifest["artifacts"]:
        if descriptor["kind"] != "log" or descriptor["path"] not in usable:
            continue
        try:
            with usable[descriptor["path"]].open(encoding="utf-8", errors="replace") as source:
                for line in source:
                    missing_completions += sum(int(value) for value in re.findall(r"(\d+) batches without a completion record", line))
                    if "[trace]" in line and re.search(r"(?:writing|appending).* failed:", line):
                        issue("capture_output_failure", "The recorded run log reports a trace or summary output failure.")
            if file_identity(usable[descriptor["path"]]) != {key: descriptor[key] for key in ("size", "sha256")}:
                issue("artifact_integrity", "Run log changed while reading capture evidence.")
        except OSError as error:
            issue("artifact_integrity", f"{descriptor['path']}: {error}")
    if missing_completions:
        report["coverage"]["run_log_missing_completion_records"] = missing_completions
        issue("gpu_completion_loss_observed", "Run log reports missing GPU completion records; their exact overlap with the selected window is unknown.",
              area="attribution", severity="warning")
    summaries = [item for item in manifest["artifacts"] if item["kind"] == "summary"]
    traces = [item for item in manifest["artifacts"] if item["kind"] == "trace"]
    if len(summaries) != 1 or len(traces) != 1:
        issue("artifact_selection", f"Expected exactly one explicit summary and trace descriptor; found {len(summaries)} summaries and {len(traces)} traces.")
    else:
        summary_descriptor, trace_descriptor = summaries[0], traces[0]
        selected_timing = [dict(summary_descriptor), dict(trace_descriptor)]
        report["artifacts_selected"].extend(selected_timing)
        if all(item["path"] in usable for item in selected_timing):
            try:
                trace_name = Path(trace_descriptor["path"]).name
                selector = summary_descriptor.get("trace", trace_name)
                if selector != trace_name or "/" in selector or "\\" in selector:
                    raise SchemaError("summary selector must identify the explicit trace basename")
                summary = _summary(usable[summary_descriptor["path"]], selector)
                trace = _trace(usable[trace_descriptor["path"]])
                for descriptor in selected_timing:
                    if file_identity(usable[descriptor["path"]]) != {key: descriptor[key] for key in ("size", "sha256")}:
                        raise SchemaError("selected artifact changed while reading capture evidence")
                _assess_selected(manifest, summary, trace, report, issue)
                _assess_draw_sidecar(run_dir, manifest, usable, summary, trace, trace_name,
                                     report, issue, include_draw_index=include_draw_index,
                                     process_evidence=process_evidence, external_evidence=external_evidence)
                _assess_workload_sidecar(run_dir, manifest, usable, summary, trace, trace_name, report, issue)
            except (OSError, ValueError, EOFError, OverflowError, UnicodeError) as error:
                issue("capture_evidence_invalid", str(error))
    if report["frame_metrics"]["status"] == "unknown" and report["coverage"]["capture_completed"]:
        if manifest["capture"]["unit"] == "frames":
            report["frame_metrics"]["status"] = "valid"
    if any(item["severity"] == "error" for item in report["issues"]):
        report["status"] = "invalid"
    return report


def _assess_workload_sidecar(run_dir, manifest, usable, summary, trace, trace_name, report, issue):
    selected = [item for item in manifest["artifacts"] if item["kind"] == "measurement_workload"]
    requested = {key.upper(): value for key, value in manifest["environment"]["effective"].items()}.get("APS5_TRACE_WORKLOAD") == "1"
    if not selected and not requested:
        return
    result = {"status": "unknown", "run_id": manifest["run_id"], "metrics": {}, "metric_units": {},
        "observed_counts": {}, "coverage": {}, "issues": [], "correlation_valid": False,
        "scope": "selected host-observed request, command and deferred-job paths; not GPU execution or scene proof"}
    report["measurement_workload"] = result
    expected = f"aps5-workload-{manifest['run_id']}-{manifest['execution'].get('pid')}.json.gz"
    try:
        if not requested or len(selected) != 1 or Path(selected[0]["path"]).name != expected:
            raise SchemaError("expected one explicitly requested workload sidecar matching run UUID and child PID")
        descriptor = selected[0]
        path = usable.get(descriptor["path"])
        if path is None or descriptor.get("trace") != trace_name:
            raise SchemaError("workload descriptor is unavailable or does not link the selected trace")
        if descriptor["size"] > 64 * 1024 ** 2:
            raise SchemaError("compressed workload sidecar exceeds 64 MiB")
        with gzip.open(path, "rb") as source:
            payload = source.read(64 * 1024 ** 2 + 1)
        if len(payload) > 64 * 1024 ** 2:
            raise SchemaError("workload sidecar exceeds 64 MiB decoded bound")
        document = validate_measurement_workload(loads_json(payload.decode("utf-8"), context="measurement workload"))
        if file_identity(path) != {key: descriptor[key] for key in ("size", "sha256")}:
            raise SchemaError("workload sidecar changed during assessment")
        producer, capture, coverage = document["producer"], document["capture"], document["coverage"]
        requested_capture = manifest["capture"]
        revision = producer["revision"]
        if (document["run_id"] != manifest["run_id"] or producer["pid"] != manifest["execution"].get("pid")
                or document["trace"] != trace_name or revision != summary.get("commit")
                or producer["dirty"] != summary.get("dirty") or re.fullmatch(r"[0-9a-fA-F]{7,40}", revision) is None
                or not manifest["source"]["commit"].startswith(revision.lower())):
            raise SchemaError("workload run/process/trace/revision binding disagrees with capture evidence")
        spec = f"frames {capture_spec(requested_capture)}"
        if (requested_capture["unit"] != "frames" or capture["spec"] != spec
                or capture["open_present"] != requested_capture["start"]):
            raise SchemaError("workload opening window disagrees with the manifest")
        rows = document["frames"]
        if coverage["closed"] and not coverage["lost_intervals"]:
            if (len(rows) != requested_capture["length"] or capture["close_present"] != requested_capture["start"] + requested_capture["length"]
                    or abs((capture["close_ns"] - capture["open_ns"]) / 1_000_000 - summary["window"]["ms"]) > 0.002):
                raise SchemaError("workload closed window disagrees with exact frame count or duration")
        for row, present in zip(rows, trace["presents"]):
            if row["present"] != present[0] or abs((row["begin_ns"] - capture["open_ns"]) / 1000 - present[1]) > 2:
                raise SchemaError("workload boundary differs from the selected timing trace")
        result["correlation_valid"] = True
        result.update(observed_counts=dict(document["counts"]), coverage=dict(coverage), artifact=dict(descriptor))
        failure_keys = ("operation_status.rejected", "operation_status.failed", "operation_status.cancelled", "host_commands_failed", "jobs_failed")
        complete = (coverage["closed"] and not coverage["lost_intervals"] and not coverage["overflow"] and not coverage["lifetime_error"]
            and not any(coverage["pending_at_open"] + coverage["pending_at_close"])
            and not any(document["counts"][key] for key in failure_keys)
            and report["frame_metrics"]["status"] != "invalid" and report["coverage"]["capture_completed"]
            and manifest["state"] == "completed" and manifest["exit_code"] == 0)
        result["status"] = "valid" if complete else "unknown"
        if complete:
            keys = WORKLOAD_KEYS[:5] + WORKLOAD_KEYS[5:10]
            result["metrics"] = {key: document["counts"][key] for key in keys}
            result["metric_units"] = dict.fromkeys(keys, "count")
        else:
            result["issues"].append({"code": "workload_coverage_unknown", "severity": "warning",
                "message": "Incomplete window, losses, pending work or failed outcomes prevent complete workload equivalence."})
        report["artifacts_selected"].append(dict(descriptor))
    except (OSError, ValueError, EOFError, UnicodeError, OverflowError) as error:
        result["status"] = "invalid"
        result["issues"].append({"code": "measurement_workload_invalid", "severity": "error", "message": str(error)})
    from capture_evidence import merge_measurement_workload
    report["evidence"] = merge_measurement_workload(report.get("evidence"), result, manifest)
    for finding in result["issues"]:
        issue(finding["code"], finding["message"], area="attribution", severity=finding["severity"])


def _assess_auxiliary_evidence(run_dir, manifest, usable, report, issue):
    from capture_external import load_external_evidence
    from capture_process import assess_process_observation

    process = assess_process_observation(run_dir, manifest, usable)
    report["process_observation"] = process
    from capture_collectors import assess_collector_receipt
    collectors = assess_collector_receipt(run_dir, manifest, verified_artifacts=usable, process_evidence=process)
    report["collector_observation"] = collectors
    external = load_external_evidence(run_dir, manifest, verified_artifacts=usable, process_evidence=process)
    report["external_evidence"] = {key: value for key, value in external.items() if key != "profiles"}
    report["external_evidence"]["profiles"] = [
        {**{key: value for key, value in profile.items() if key != "document"},
         "kind": profile["document"]["kind"],
         "record_count": sum(len(rows) for rows in profile["document"]["records"].values())}
        for profile in external.get("profiles", [])]
    report["provenance"]["observed_module_snapshot_status"] = process["status"]
    report["provenance"]["observed_module_scope"] = "sampled mappings; continuous module lifetime remains unknown"
    report["provenance"]["process_creation_identity_verified"] = (
        process["status"] == "valid" and (process.get("process") or {}).get("identity_verified") is True)
    for observation in (process, external, collectors):
        for finding in observation.get("issues", []):
            issue(finding["code"], finding["message"], area="provenance", severity=finding["severity"])
    selected_kinds = {"process_observation", "process_samples", "cpu_profile", "gpu_profile", "presentation_profile"}
    if any(item["kind"] == "condition_receipt" for item in manifest["artifacts"]):
        selected_kinds.add("condition_receipt")
        _assess_conditions(run_dir, manifest, report, issue)
    if any(item["kind"] == "log" and item["path"] in usable for item in manifest["artifacts"]):
        from capture_issues import assess_run_issues, compact_issue_report
        try:
            issues = assess_run_issues(run_dir, manifest)
            report["log_review"] = compact_issue_report(issues)
        except (OSError, ValueError, OverflowError) as error:
            issue("log_review_invalid", str(error), area="provenance")
    report["artifacts_selected"].extend(dict(item) for item in manifest["artifacts"]
                                        if item["kind"] in selected_kinds and item["path"] in usable)
    return process, external


def _assess_conditions(run_dir, manifest, report, issue, *, capture_id=None):
    from capture_experiment import assess_run_conditions
    try:
        conditions = assess_run_conditions(run_dir, manifest=manifest, capture_id=capture_id)
        report["conditions"] = conditions
        if conditions["status"] == "invalid":
            issue("condition_evidence_invalid", "A reviewed condition conflicts with its required evidence.", area="provenance")
        for finding in conditions.get("issues", []):
            issue(finding["code"], finding["message"], area="provenance", severity=finding["severity"])
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
        report["conditions"] = {"schema_version": 1, "document_type": "anyps5.condition-report",
                                "run_id": manifest["run_id"], "status": "invalid", "dimensions": {}}
        issue("condition_evidence_invalid", str(error), area="provenance")


def _assess_draw_sidecar(run_dir, manifest, usable, summary, trace, trace_name,
                        report, issue, *, include_draw_index, process_evidence=None, external_evidence=None):
    from capture_draws import assess_draw_trace, read_draw_trace
    sidecars = [item for item in manifest["artifacts"] if item["kind"] == "draw_trace"]
    requested = manifest["environment"]["effective"].get("APS5_TRACE_DETAIL") == "draw"
    if not requested and not sidecars:
        return
    if not requested:
        issue("draw_detail_unrequested", "Draw sidecar was supplied without matching opt-in configuration.", area="attribution")
        return
    expected = f"aps5-draw-{manifest['run_id']}-{manifest['execution'].get('pid')}.json"
    if len(sidecars) != 1 or Path(sidecars[0]["path"]).name != expected:
        issue("draw_artifact_selection", "Expected exactly one draw sidecar matching the recorded run UUID and child PID.",
              area="attribution")
        return
    descriptor = sidecars[0]
    if descriptor["path"] not in usable or descriptor.get("trace") != trace_name:
        issue("draw_artifact_link", "Draw sidecar descriptor does not identify the selected trace or has invalid integrity.",
              area="attribution")
        return
    try:
        path = usable[descriptor["path"]]
        document = read_draw_trace(path)
        analysis_index = {}
        result = assess_draw_trace(document, manifest, trace_name=trace_name, summary=summary, trace=trace,
                                   include_index=include_draw_index, analysis_index=analysis_index)
        result["artifact"] = dict(descriptor)
        if "conditions" in report and report["conditions"]["status"] != "invalid":
            _assess_conditions(run_dir, manifest, report, issue, capture_id=document["capture_id"])
        from capture_evidence import assess_evidence, compact_evidence
        evidence = validate_evidence_report(assess_evidence(document, manifest, result,
            external_evidence=external_evidence, process_evidence=process_evidence,
            condition_evidence=report.get("conditions"), log_review=report.get("log_review"), draw_index=analysis_index))
        if file_identity(path) != {key: descriptor[key] for key in ("size", "sha256")}:
            raise SchemaError("draw sidecar changed while reading evidence")
    except (OSError, ValueError, OverflowError) as error:
        issue("draw_evidence_invalid", str(error), area="attribution")
        return
    report["artifacts_selected"].append(dict(descriptor))
    report["evidence"] = evidence if include_draw_index else compact_evidence(evidence)
    for finding in evidence["issues"]:
        issue(finding["code"], finding["message"], area="attribution", severity=finding["severity"])
    result["artifact"] = dict(descriptor)
    if not include_draw_index:
        result.pop("index", None)
    report["draw_attribution"] = result
    report["legacy_attribution"] = {**report["detailed_attribution"],
                                      "limitations": list(report["detailed_attribution"]["limitations"])}
    legacy_codes = {"trace_data_loss", "loss_flag_inconsistent", "gpu_query_loss", "gpu_track_missing", "legacy_coverage_unknown"}
    for finding in report["issues"]:
        if finding["code"] in legacy_codes:
            finding["evidence_scope"] = "legacy_trace"
            if finding["severity"] == "error":
                finding["legacy_severity"] = "error"
                finding["severity"] = "warning"
    report["coverage"].update(draw_identity=result["identity_status"], cpu_scopes=result["cpu_status"],
                               gpu_queries=result["gpu_status"], origin_graph=result["origin_graph_status"],
                               gpu_command_association=result["gpu_association_status"],
                               draw_capture_id=result["capture_id"])
    for finding in result["issues"]:
        issue(finding["code"], finding["message"], area="attribution", severity=finding["severity"])
    report["detailed_attribution"] = {"status": result["status"], "limitations": result["limitations"],
                                       "scope": "selected instrumented paths only"}
    if evidence["status"] == "invalid":
        report["detailed_attribution"]["status"] = "invalid"


def _assess_selected(manifest, summary, trace, report, issue):
    capture = manifest["capture"]
    expected_spec = f"{capture['unit']} {capture_spec(capture)}"
    if summary["window"]["spec"] != expected_spec:
        raise SchemaError("Summary window does not match the requested capture specification")
    if "run_id" in summary and summary["run_id"] != manifest["run_id"]:
        raise SchemaError("Summary run identity does not match the manifest")
    if "schema_version" in summary and (type(summary["schema_version"]) is not int or summary["schema_version"] != 1):
        raise SchemaError("Capture artifact schema version is unsupported")
    if trace["run_id"] is not None and trace["run_id"] != manifest["run_id"]:
        raise SchemaError("Trace run identity does not match the manifest")
    observed_revision = summary["commit"].lower()
    if re.fullmatch(r"[0-9a-f]{7,40}", observed_revision) is None:
        issue("producer_revision_unknown", "Producer build revision is unavailable or too abbreviated to correlate with the inspected source.",
              area="provenance", severity="warning")
    elif not manifest["source"]["commit"].startswith(observed_revision):
        issue("source_revision_mismatch", "Summary build revision differs from the inspected source revision.", area="provenance")
    else:
        report["provenance"]["producer_revision_verified"] = True
        if len(observed_revision) != 40:
            issue("producer_revision_abbreviated", "Producer revision is an abbreviated matching source prefix; it is not a full build identity.",
                  area="provenance", severity="warning")
    if summary.get("dirty") is not False or manifest["source"]["dirty"]:
        issue("source_dirty_or_unknown", "Source or producer build state is dirty or unknown; commit alone does not identify the runtime build.",
              area="provenance", severity="warning")
    pid = manifest.get("execution", {}).get("pid")
    report["provenance"]["process_identity_verified"] = pid is not None and pid == trace["pid"]
    if pid is None:
        issue("process_identity_unverified", "No child process ID was recorded to correlate the selected trace.",
              area="provenance", severity="warning")
    if pid is not None and pid != trace["pid"]:
        raise SchemaError("Trace process identity does not match the recorded child process")
    records = trace["presents"]
    first = summary["window"]["first_present"]
    count = summary["presents"]
    if len(records) != count or [item[0] for item in records] != list(range(first, first + count)):
        raise SchemaError("Trace present count or sequence differs from the summary")
    timestamps = [item[1] for item in records]
    if timestamps != sorted(timestamps) or not timestamps or abs(timestamps[0]) > 0.001:
        raise SchemaError("Trace present timestamps are not ordered from the window origin")
    if len({item[2] for item in records}) != 1:
        raise SchemaError("Trace present instants identify multiple presenter threads")
    duration = summary["window"]["ms"]
    if timestamps[-1] > duration * 1000 + 1:
        raise SchemaError("Trace present instant lies past the summary closing boundary")
    for before, after in zip(timestamps, timestamps[1:]):
        if (after - before) / 1000 > summary["frame_ms"]["max"] + 0.002:
            raise SchemaError("Trace observed frame interval exceeds the summary maximum")
    reconstructed = sorted([(after - before) / 1000 for before, after in zip(timestamps, timestamps[1:])]
                           + [max(0.0, duration - timestamps[-1] / 1000)])
    for key, fraction in (("p50", 0.5), ("p90", 0.9), ("p99", 0.99), ("max", 1.0)):
        expected = reconstructed[min(count - 1, int(fraction * count))]
        if abs(expected - summary["frame_ms"][key]) > 0.002:
            raise SchemaError(f"Summary frame_ms.{key} contradicts the trace intervals and summary closing boundary")
    report["coverage"]["quantile_check"] = "trace_intervals_plus_final_interval_reconstructed_from_summary"
    if capture["unit"] == "frames":
        if first != capture["start"] or count != capture["length"]:
            raise SchemaError("Capture frame start or length differs from the requested window")
    else:
        if duration + 0.001 < capture["length"] * 1000:
            raise SchemaError("Seconds capture duration is shorter than the requested length")
        issue("seconds_start_unverified", "Legacy traces omit absolute elapsed opening time; the requested seconds start cannot be independently verified.", severity="warning")
        issue("seconds_present_quantization", "Seconds windows close at a present after the requested duration from opening; boundary overshoot is present-dependent.", severity="warning")
    environment = {name.upper(): value for name, value in manifest.get("environment", {}).get("effective", {}).items()
                   if name.upper().startswith("APS5_")}
    expected_key = "APS5_TRACE_FRAMES" if capture["unit"] == "frames" else "APS5_TRACE_SECONDS"
    if summary["env"].get(expected_key) != capture_spec(capture):
        raise SchemaError("Summary environment does not record the requested capture window")
    observed_environment = {name.upper(): value for name, value in summary["env"].items()}
    if len(observed_environment) != len(summary["env"]):
        raise SchemaError("Summary environment repeats variable names ignoring case")
    for name in sorted(environment.keys() | observed_environment.keys()):
        if name not in environment or name not in observed_environment:
            issue("environment_mismatch", f"Summary and manifest APS5 environment presence differs for {name}.", area="provenance")
        else:
            expected = "<path>" if "/" in environment[name] or "\\" in environment[name] else environment[name]
            if observed_environment[name] != expected:
                issue("environment_mismatch", f"Summary environment differs for {name}.", area="provenance")
    report["observed_context"] = {key: summary[key] for key in ("gpu", "driver", "os", "resolution", "commit", "dirty", "env")}
    report["frame_metrics"].update({
        "metrics": {**{f"frame_ms.{key}": summary["frame_ms"][key] for key in PERCENTILES},
                    "mean_frame_ms": duration / count}, "presents": count, "duration_ms": duration,
    })
    report["coverage"].update({"capture_completed": True, "present_instants": count,
                               "thread_count_recorded": len(trace["thread_names"]),
                               "gpu_events_recorded": trace["gpu_events"],
                               "summary_trace_dropped": summary["trace_dropped"]})
    dropped_threads = summary.get("trace_dropped_threads", {})
    dropped_details = any(item.get("events_lost", 0) or item["totals_full"] for item in dropped_threads.values())
    if summary["trace_dropped"] or dropped_details or trace["events_lost"]:
        issue("trace_data_loss", "Trace event buffers or phase totals report lost data; detailed attribution is incomplete.", area="attribution")
    if not summary["trace_dropped"] and (dropped_details or trace["events_lost"]):
        issue("loss_flag_inconsistent", "Trace loss evidence contradicts the summary trace_dropped flag.", area="attribution")
    if trace["gpu_events"] == 0:
        issue("gpu_track_missing", "No GPU duration ranges are recorded; GPU detail is unavailable.", area="attribution", severity="warning")
    for key in ("gpu_queries_missing", "gpu_queries_dropped", "gpu_unread", "missing_completion_records"):
        if key in summary and _integer(summary[key], f"summary.{key}"):
            issue("gpu_query_loss", f"Summary reports {key}; GPU attribution is incomplete.", area="attribution")
    issue("legacy_coverage_unknown", "Current producer coverage limitations prevent a complete detailed-attribution verdict.",
          area="attribution", severity="warning")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Assess explicitly selected run evidence without launching or replaying it.")
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--json", action="store_true", help="Write the JSON health report (the default)")
    args = parser.parse_args(argv)
    try:
        report = assess_capture(args.run_dir)
        output = json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False, allow_nan=False) + "\n"
    except (OSError, ValueError, OverflowError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")
    sys.stdout.write(output)
    return 2 if report["status"] == "invalid" else 0


if __name__ == "__main__":
    sys.exit(main())
