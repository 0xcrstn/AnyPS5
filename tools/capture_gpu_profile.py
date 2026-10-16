import argparse
import copy
import csv
import hashlib
import io
import re
import sys
from pathlib import Path, PurePosixPath

from capture_schema import SchemaError, atomic_write_json, canonical_json_bytes, canonical_sha256, loads_json
from capture_tabular import MAX_CELL, MAX_COLUMNS, MAX_TABLE_BYTES, MAX_TABLE_ROWS, _number, _read_bounded


REQUEST_TYPE = "anyps5.gpu-profile-request"
MAPPING_TYPE = "anyps5.nsight-shader-mapping"
ADAPTER = "nsight_shader_csv_v1"
MAX_SHADER_RECORDS = 262_144
MAX_SHADER_RECORD_BYTES = 192 * 1024 ** 2
GRAPH_IDS = ("operation", "batch", "command", "pipeline", "shader_variant", "spirv")
STAGES = frozenset(("vertex", "fragment", "geometry", "tessellation_control", "tessellation_evaluation", "task", "mesh", "compute"))
METRIC_UNITS = {"warp_samples": {"samples"}, "stall_samples": {"samples"}, "sample_percent": {"percent"},
                "counter": {"count", "cycles", "bytes", "instructions", "thread_instructions", "threads_per_warp"}}
DOCUMENTATION = {
    "capture_cli": "https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-overview.html",
    "launch_cli": "https://docs.nvidia.com/nsight-graphics/UserGuide/launch-application-overview.html",
    "shader_csv": "https://docs.nvidia.com/nsight-graphics/UserGuide/shader-profiler.html",
    "replay_scope": "https://docs.nvidia.com/nsight-graphics/UserGuide/graphics-capture-ui.html",
    "systems_export": "https://docs.nvidia.com/nsight-systems/AnalysisGuide/index.html",
}
LIMITATIONS = [
    "Warp and stall samples describe sampled resident-warp activity, not exclusive stage time or invocation elapsed time.",
    "The exported current table can aggregate shaders, pipelines, calls and selected ranges; hierarchical rows are not additive.",
    "Vendor shader hashes are opaque and are never equated to native SPIR-V SHA-256 digests.",
    "Native anyps5.final-spirv.v1 identities hash a domain, length and little-endian words; a raw bytecode-file SHA-256 is a different digest.",
    "Live Replay profiles replayed work; a declared live scope does not establish original-process capture provenance.",
    "CSV transformation does not authenticate the profiler executable, source report, selected range, clock or dropped samples.",
    "A request references native targets but cannot select the same future command IDs or prove their occurrence in a new launch.",
    "Nsight Systems timeline and GPU device metrics do not provide isolated Vulkan shader-stage invocation timing.",
]


def _object(value, name, allowed, required):
    if type(value) is not dict or set(value) - set(allowed) or not set(required) <= set(value):
        raise SchemaError(f"{name} has missing or unsupported fields")
    return value


def _text(value, name, maximum=4096):
    if type(value) is not str or not value.strip() or len(value) > maximum or "\0" in value:
        raise SchemaError(f"{name} must be bounded nonempty text")
    return value


def _choice(value, name, choices):
    if type(value) is not str or value not in choices:
        raise SchemaError(f"{name} is unsupported")
    return value


def _integer(value, name, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise SchemaError(f"{name} is outside its integer bound")
    return value


def validate_profile_request(request):
    from capture_external import _hash, _ticks, _uuid

    _object(request, "GPU profile request", {"schema_version", "document_type", "run_id", "capture_id", "process",
            "native_artifact", "targets", "tool", "scope", "collection"},
            {"schema_version", "document_type", "run_id", "capture_id", "process", "native_artifact", "targets", "tool", "scope", "collection"})
    if type(request["schema_version"]) is not int or request["schema_version"] != 1 or request["document_type"] != REQUEST_TYPE:
        raise SchemaError("unsupported GPU profile request protocol")
    for key in ("run_id", "capture_id"):
        _uuid(request[key], f"GPU request {key}")
    if request["run_id"] == request["capture_id"]:
        raise SchemaError("GPU request capture UUID must differ from run UUID")
    process = _object(request["process"], "GPU request process", {"pid", "creation_filetime", "executable_sha256"}, {"pid"})
    _integer(process["pid"], "GPU request PID", 1, 2 ** 32 - 1)
    _ticks(process.get("creation_filetime"), "GPU request creation FILETIME", nullable=True)
    _hash(process.get("executable_sha256"), "GPU request executable hash", nullable=True)
    artifact = _object(request["native_artifact"], "GPU request native artifact", {"path", "sha256", "size"}, {"path", "sha256", "size"})
    artifact_path = _text(artifact["path"], "native artifact path")
    if (PurePosixPath(artifact_path).is_absolute() or "\\" in artifact_path or ":" in artifact_path
            or any(part.casefold() in ("", ".", "..", "dump", "fakelib", "app0") for part in artifact_path.split("/"))):
        raise SchemaError("native artifact requires a safe relative evidence path")
    _hash(artifact["sha256"], "GPU request native artifact hash")
    _integer(artifact["size"], "GPU request native artifact size", 0, 2 ** 64 - 1)
    if type(request["targets"]) is not list or not 1 <= len(request["targets"]) <= 256:
        raise SchemaError("GPU profile request requires 1..256 explicit native shader targets")
    seen = set()
    for target in request["targets"]:
        _object(target, "GPU shader target", {*GRAPH_IDS, "stage", "spirv_sha256"}, {*GRAPH_IDS, "stage", "spirv_sha256"})
        for key in GRAPH_IDS:
            if _ticks(target[key], f"GPU target {key}") == 0:
                raise SchemaError("GPU target IDs must identify nonzero native entities")
        _choice(target["stage"], "GPU target stage", STAGES)
        _hash(target["spirv_sha256"], "GPU target SPIR-V hash")
        identity = tuple(target[key] for key in (*GRAPH_IDS, "stage", "spirv_sha256"))
        if identity in seen:
            raise SchemaError("duplicate GPU shader target")
        seen.add(identity)
    tool = _object(request["tool"], "GPU request tool", {"name", "version"}, {"name", "version"})
    if tool["name"] != "NVIDIA Nsight Graphics":
        raise SchemaError("shader profiling request requires NVIDIA Nsight Graphics")
    _text(tool["version"], "GPU request tool version", 256)
    _choice(request["scope"], "GPU request scope", {"live", "replay", "unknown"})
    collection = _object(request["collection"], "GPU request collection", {"start_after_frames", "limit_to_frames", "max_duration_ms"},
                         {"start_after_frames", "limit_to_frames", "max_duration_ms"})
    _integer(collection["start_after_frames"], "GPU trace start presents", 0, 2 ** 32 - 1)
    _integer(collection["limit_to_frames"], "GPU trace frame count", 1, 15)
    _integer(collection["max_duration_ms"], "GPU trace maximum duration", 1, 240_000)
    return request


def build_profile_packet(request, *, ngfx, target_executable, working_directory, output_directory):
    from capture_process import observation_path

    validate_profile_request(request)
    paths = [observation_path(str(value)) for value in (ngfx, target_executable, working_directory, output_directory)]
    executable, target, working, output = paths
    if executable.name.casefold() != "ngfx.exe" or target.suffix.casefold() != ".exe":
        raise SchemaError("Windows GPU Trace packet requires explicit ngfx.exe and target executable paths")
    collection = request["collection"]
    arguments = [str(executable), "--activity", "GPU Trace Profiler", "--platform", "Windows", "--exe", str(target),
        "--dir", str(working), "--output-dir", str(output), "--start-after-frames", str(collection["start_after_frames"]),
        "--limit-to-frames", str(collection["limit_to_frames"]), "--max-duration-ms", str(collection["max_duration_ms"]),
        "--real-time-shader-profiler", "--set-gpu-clocks", "unaltered", "--auto-export"]
    labels = sorted({f"aps5 run={request['run_id']} capture={request['capture_id']} op={row['operation']} "
                     f"batch={row['batch']} pipeline={row['pipeline']} command={row['command']}" for row in request["targets"]})
    return {"schema_version": 1, "document_type": "anyps5.gpu-profile-packet", "request": copy.deepcopy(request),
        "request_sha256": canonical_sha256(request), "status": "requested", "executed": False,
        "commands": [{"purpose": "inspect_installed_version_options", "argv": [str(executable), "--help-all"]},
                     {"purpose": "collect_live_gpu_trace", "argv": arguments}] if request["scope"] == "live" else [],
        "required_observed_environment": {"APS5_TRACE_DETAIL": "draw", "APS5_TRACE_GPU_LABELS": "1", "APS5_CAPTURE_RUN_ID": request["run_id"]},
        "target_labels": labels, "native_target_binding": "missing_proof", "tool_identity": "missing_proof",
        "execution_gate": {"status": "missing_proof", "installed_version_supported": "unknown",
            "required_receipts": ["tool executable hash and version resource", "installed --help-all output containing every planned option",
                "architecture-compatible shader-profiler metric set", "explicit future launch approval"],
            "capture_scope": "tool-defined presents, not assumed identical to the native 100-frame window"},
        "capabilities": {"shader_samples": "requested", "stall_samples": "requested", "execution_counters": "requested",
            "exact_stage_elapsed": "unsupported", "native_report_parser": "unsupported", "automatic_shader_csv_export": "unsupported",
            "replay_automation": "unsupported", "clock_alignment": "missing_proof", "loss_accounting": "missing_proof"},
        "steps": ["Verify installed ngfx help and version against the preserved tool receipt before any separately authorized execution.",
                  "Select an architecture-compatible shader-profiler metric set in the installed tool; preserve all chosen options and source report.",
                  "Preserve the actual child PID, creation time, executable hash, run/capture identity and complete collector log.",
                  "Select the debug-label range and shader/pipeline rows in Shader Profiler; use its Save as CSV action.",
                  "Preserve the untouched CSV and create a mapping from the exported headers; do not add IDs or timestamps to the CSV.",
                  "Import with this adapter and independently join observed labels and shader identities to the same native capture.",
                  "Retain loss warnings, sample settings, clock mode and original-versus-Live-Replay scope as separate proof."],
        "documentation": dict(DOCUMENTATION), "limitations": list(LIMITATIONS)}


def validate_shader_mapping(mapping):
    _object(mapping, "Nsight shader mapping", {"schema_version", "document_type", "tool_version", "csv", "columns", "row_types", "stage_values", "metrics", "spirv_digest_scope"},
            {"schema_version", "document_type", "tool_version", "csv", "columns", "row_types", "stage_values", "metrics"})
    if type(mapping["schema_version"]) is not int or mapping["schema_version"] != 1 or mapping["document_type"] != MAPPING_TYPE:
        raise SchemaError("unsupported Nsight shader mapping protocol")
    _text(mapping["tool_version"], "Nsight mapping version", 256)
    dialect = _object(mapping["csv"], "shader CSV dialect", {"delimiter", "quotechar", "encoding"}, {"delimiter", "quotechar", "encoding"})
    for key in ("delimiter", "quotechar"):
        if type(dialect[key]) is not str or len(dialect[key]) != 1 or dialect[key] in ("\0", "\r", "\n"):
            raise SchemaError("shader CSV delimiter and quote must be single safe characters")
    if dialect["delimiter"] == dialect["quotechar"] or dialect["encoding"] not in ("utf-8", "utf-8-sig"):
        raise SchemaError("unsupported shader CSV encoding or dialect")
    columns = _object(mapping["columns"], "shader CSV columns", {"row_type", "shader_label", "vendor_shader_hash", "debug_label", "spirv_sha256"},
                      {"row_type", "shader_label"})
    for key, value in columns.items():
        _text(value, f"shader CSV header {key}", 256)
    if "spirv_sha256" in columns:
        if mapping.get("spirv_digest_scope") != "anyps5.final-spirv.v1":
            raise SchemaError("explicit native SPIR-V identity column requires its domain-specific digest scope")
    elif "spirv_digest_scope" in mapping:
        raise SchemaError("SPIR-V digest scope requires an actual mapped identity column")
    if len(set(columns.values())) != len(columns):
        raise SchemaError("shader semantic fields require distinct actual columns")
    row_types = _object(mapping["row_types"], "shader row types", mapping["row_types"] if type(mapping["row_types"]) is dict else {}, {})
    if not 1 <= len(row_types) <= 64:
        raise SchemaError("shader row types require a bounded explicit value map")
    for key, value in row_types.items():
        _text(key, "exported shader row type", 256)
        _choice(value, "shader row classification", {"shader", "pipeline", "aggregate", "unattributed"})
    stage_values = _object(mapping["stage_values"], "shader stage values", row_types, {})
    for key, value in stage_values.items():
        if row_types[key] != "shader":
            raise SchemaError("pipeline and aggregate rows cannot be reclassified as shader stages")
        _choice(value, "shader stage", STAGES)
    metrics = mapping["metrics"]
    if type(metrics) is not list or not 1 <= len(metrics) <= 64:
        raise SchemaError("shader mapping requires 1..64 explicit metric columns")
    seen = set(columns.values())
    for metric in metrics:
        _object(metric, "shader metric mapping", {"column", "name", "unit", "measurement_kind", "unavailable_values"},
                {"column", "name", "unit", "measurement_kind", "unavailable_values"})
        for key in ("column", "name"):
            _text(metric[key], f"shader metric {key}", 256)
        kind = _choice(metric["measurement_kind"], "shader metric kind", METRIC_UNITS)
        _choice(metric["unit"], "shader metric unit", METRIC_UNITS[kind])
        if metric["column"] in seen:
            raise SchemaError("each mapped shader metric requires its own real column")
        seen.add(metric["column"])
        missing = metric["unavailable_values"]
        if type(missing) is not list or len(missing) > 32 or any(type(value) is not str or len(value) > 256 or "\0" in value for value in missing):
            raise SchemaError("unavailable shader cells require bounded explicit text tokens")
    return mapping


def normalize_nsight_shader_csv(text, mapping, *, request, source_artifact, source_sha256, source_size):
    from capture_external import _hash, _relative_source, parse_debug_label, validate_external_document

    validate_profile_request(request)
    validate_shader_mapping(mapping)
    if mapping["tool_version"] != request["tool"]["version"]:
        raise SchemaError("shader mapping version contradicts the requested tool version")
    if type(text) is not str or "\0" in text:
        raise SchemaError("shader table must be UTF-8 text without NUL")
    try:
        raw = text.encode("utf-8")
    except UnicodeError as error:
        raise SchemaError("shader CSV is not valid UTF-8") from error
    if len(raw) > MAX_TABLE_BYTES or len(raw) != source_size or hashlib.sha256(raw).hexdigest() != source_sha256:
        raise SchemaError("shader CSV bytes contradict their preserved identity or size bound")
    _relative_source(source_artifact)
    if Path(source_artifact).suffix.casefold() != ".csv":
        raise SchemaError("Nsight Shader Profiler adapter requires preserved CSV")
    dialect, columns = mapping["csv"], mapping["columns"]
    if text.startswith("\ufeff"):
        if dialect["encoding"] != "utf-8-sig":
            raise SchemaError("shader CSV BOM requires utf-8-sig mapping")
        text = text[1:]
    records, excluded, source_rows, record_bytes = [], {key: 0 for key in ("pipeline", "aggregate", "unattributed")}, 0, 0
    reader = csv.reader(io.StringIO(text, newline=""), delimiter=dialect["delimiter"], quotechar=dialect["quotechar"], strict=True)
    try:
        headers = next(reader, [])
        required = set(columns.values()) | {metric["column"] for metric in mapping["metrics"]}
        if (not 1 <= len(headers) <= MAX_COLUMNS or len(headers) != len(set(headers)) or not required <= set(headers)
                or any(not value or len(value) > MAX_CELL for value in headers)):
            raise SchemaError("shader CSV headers are missing, duplicated or oversized")
        previous_line = reader.line_num
        for source_rows, values in enumerate(reader, 1):
            first, last = previous_line + 1, reader.line_num
            previous_line = last
            if source_rows > MAX_TABLE_ROWS or len(values) != len(headers) or any(len(value) > MAX_CELL for value in values):
                raise SchemaError("shader CSV row exceeds bounds or does not match its headers")
            observed = dict(zip(headers, values))
            row_type = observed[columns["row_type"]].strip()
            category = mapping["row_types"].get(row_type)
            if category is None:
                raise SchemaError("exported shader row type lacks an explicit classification")
            if category != "shader":
                excluded[category] += 1
                continue
            name = _text(observed[columns["shader_label"]].strip(), "observed shader name", 4096)
            vendor_hash = (observed[columns["vendor_shader_hash"]].strip() or None) if "vendor_shader_hash" in columns else None
            label = (observed[columns["debug_label"]].strip() or None) if "debug_label" in columns else None
            digest = (observed[columns["spirv_sha256"]].strip() or None) if "spirv_sha256" in columns else None
            if vendor_hash is not None:
                _text(vendor_hash, "opaque vendor shader hash", 256)
            if digest is not None:
                _hash(digest, "explicit exported SPIR-V SHA-256")
            if label is not None:
                _text(label, "observed debug label", 512)
                parse_debug_label(label)
            for index, metric in enumerate(mapping["metrics"]):
                cell = observed[metric["column"]].strip()
                value = None if cell in metric["unavailable_values"] else _number(cell, "shader metric")
                if metric["unit"] == "percent" and value is not None and value > 100:
                    raise SchemaError("shader sample percentage exceeds 100")
                if metric["unit"] in ("samples", "count", "cycles", "bytes", "instructions", "thread_instructions") and value is not None:
                    if not re.fullmatch(r"[0-9]{1,20}", cell) or int(cell) >= 2 ** 64:
                        raise SchemaError("shader count requires an exact exported uint64 decimal integer")
                    value = int(cell)
                record = {"id": f"shader_csv:{source_rows}:{index}", "source_record": source_rows,
                    "source_lines": {"first": first, "last": last}, "shader_label": name, "vendor_shader_hash": vendor_hash,
                    "stage": mapping["stage_values"].get(row_type), "debug_label": label, "shader_sha256": digest,
                    "shader_digest_scope": mapping.get("spirv_digest_scope") if digest is not None else None,
                    "name": metric["name"], "unit": metric["unit"], "measurement_kind": metric["measurement_kind"],
                    "scope": "exported_shader_row_aggregate", "status": "measured" if value is not None else "unavailable", "value": value}
                record_bytes += len(canonical_json_bytes(record)) + 1
                if len(records) >= MAX_SHADER_RECORDS or record_bytes > MAX_SHADER_RECORD_BYTES:
                    raise SchemaError("shader metric expansion exceeds the supported record or normalized-byte bound")
                records.append(record)
    except csv.Error as error:
        raise SchemaError("shader CSV contains an invalid or oversized field") from error
    observed_counts = {kind: sum(row["measurement_kind"] == kind and row["status"] == "measured" for row in records) for kind in METRIC_UNITS}
    document = {"schema_version": 1, "document_type": "anyps5.external-evidence", "kind": "gpu_profile",
        "run_id": request["run_id"], "capture_id": request["capture_id"],
        "process": {key: request["process"].get(key) for key in ("pid", "creation_filetime", "executable_sha256")},
        "producer": {"tool": "NVIDIA Nsight Graphics", "version": mapping["tool_version"], "adapter": ADAPTER,
            "adapter_version": "1", "native_format": "shader_profiler_exported_csv", "normalized_protocol": True},
        "source": {"artifact": source_artifact, "sha256": source_sha256, "size": source_size},
        "clock": {"domain": "unavailable_aggregated_shader_table", "ticks_per_second": 1, "anchors": []},
        "coverage": {"state": "unknown", "scope": "exported_current_shader_table_selection_only", "start_ticks": None,
                     "end_ticks": None, "lost_records": None}, "records": {"shader_metrics": records},
        "profile_request": copy.deepcopy(request), "adapter_metadata": {"mapping_sha256": canonical_sha256(mapping),
            "request_sha256": canonical_sha256(request), "source_rows": source_rows, "selected_metric_rows": len(records),
            "excluded_row_types": excluded, "profile_scope": request["scope"], "scope_provenance": "unknown",
            "tool_identity": "missing_proof", "native_target_binding": "missing_proof", "clock_alignment": "unsupported",
            "lost_records": None, "loss_accounting": "missing_proof", "aggregation": "exported_current_table_unknown_hierarchy_and_range",
            "capabilities": {kind: {"requested": True, "observed_rows": count, "status": "observed" if count else "missing_proof"}
                             for kind, count in observed_counts.items()},
            "exact_stage_elapsed": "unsupported", "limitations": list(LIMITATIONS), "documentation": dict(DOCUMENTATION)}}
    return validate_external_document(document)


def _artifact_path(run_dir, relative, suffix, *, must_exist=False, collector=False):
    from capture_external import _relative_source
    from capture_process import observation_path
    from capture_schema import contained_path

    if collector:
        _text(relative, "GPU collector receipt path")
        if ("\\" in relative or ":" in relative or not relative.startswith(("external/", "collectors/"))
                or any(part.casefold() in ("", ".", "..", "dump", "fakelib", "app0") for part in relative.split("/"))):
            raise SchemaError("GPU collector receipt requires a contained external/ or collectors/ path")
    else:
        _relative_source(relative)
    if Path(relative).suffix.casefold() != suffix:
        raise SchemaError(f"GPU profile artifact requires {suffix} suffix")
    observation_path(str(run_dir / relative))
    return observation_path(str(contained_path(run_dir, relative, must_exist=must_exist)))


def inspect_collector_receipt(receipt, request):
    from capture_external import _hash, _ticks, _uuid

    validate_profile_request(request)
    if (type(receipt) is not dict or receipt.get("document_type") != "anyps5.collector-receipt"
            or type(receipt.get("schema_version")) is not int or receipt["schema_version"] != 1):
        raise SchemaError("unsupported GPU collector receipt")
    _uuid(receipt.get("run_id"), "GPU collector run UUID")
    process = receipt.get("process")
    if type(process) is not dict or receipt["run_id"] != request["run_id"]:
        raise SchemaError("GPU collector receipt does not identify the requested run")
    for key in ("pid", "creation_filetime", "executable_sha256"):
        if process.get(key) != request["process"].get(key):
            raise SchemaError("GPU collector receipt process identity contradicts the profiling request")
    commands = receipt.get("commands")
    if type(commands) is not list or len(commands) > 256:
        raise SchemaError("GPU collector command ledger must be bounded")
    recorded = []
    for command in commands:
        if type(command) is not dict or type(command.get("argv")) is not list or not 1 <= len(command["argv"]) <= 256:
            raise SchemaError("GPU collector requires a bounded recorded argument vector")
        for argument in command["argv"]:
            _text(argument, "GPU collector recorded argument", 32_768)
        tool = command.get("tool")
        if type(tool) is not dict:
            raise SchemaError("GPU collector tool identity is unavailable")
        _text(tool.get("path"), "GPU collector tool path")
        _hash(tool.get("sha256"), "GPU collector tool hash")
        _integer(tool.get("size"), "GPU collector tool size", 0, 2 ** 64 - 1)
        if tool.get("file_version") is not None:
            _text(tool["file_version"], "GPU collector file version", 4096)
        code = command.get("returncode")
        if code is not None:
            _integer(code, "GPU collector return code", -(2 ** 31), 2 ** 32 - 1)
        recorded.append({"tool_sha256": tool["sha256"], "tool_size": tool["size"], "file_version": tool.get("file_version"),
                         "recorded_tool_name": tool["path"].replace("\\", "/").rsplit("/", 1)[-1],
                         "recorded_status": command.get("status", "unknown"),
                         "returncode": code, "stdout_present": command.get("stdout") is not None,
                         "stderr_present": command.get("stderr") is not None})
    coverage = receipt.get("coverage")
    if type(coverage) is not dict:
        raise SchemaError("GPU collector coverage receipt is unavailable")
    lost = coverage.get("lost_events")
    if lost is not None:
        _integer(lost, "GPU collector reported lost events", 0, 2 ** 64 - 1)
    for key in ("start_qpc", "end_qpc"):
        _ticks(coverage.get(key), f"GPU collector {key}", nullable=True)
    return {"status": "recorded", "recorded_collector_status": receipt.get("status", "unknown"),
            "recorded_collector_state": receipt.get("state", "unknown"), "commands": recorded, "reported_lost_events": lost,
            "loss_scope": coverage.get("loss_scope", "unknown"),
            "reported_scope": coverage.get("scope"), "scope_provenance": "unknown", "tool_authenticity": "unknown",
            "extraction_provenance": "unknown", "clock_alignment": "unknown",
            "limitations": ["Preserved receipt facts do not authenticate vendor implementation, shader CSV extraction, selected range or live/replay scope.",
                            "Collector-reported loss is not independently reconciled shader sampler loss."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Prepare a documented Nsight request or import preserved Shader Profiler CSV; never execute NVIDIA tooling.")
    parser.add_argument("--run-dir", required=True, type=Path)
    subparsers = parser.add_subparsers(dest="command", required=True)
    plan = subparsers.add_parser("plan")
    plan.add_argument("--ngfx", required=True)
    plan.add_argument("--target-exe", required=True)
    plan.add_argument("--working-dir", required=True)
    plan.add_argument("--output-dir", required=True)
    normalize = subparsers.add_parser("normalize")
    normalize.add_argument("--source", required=True)
    normalize.add_argument("--mapping", required=True)
    normalize.add_argument("--collector-receipt")
    for selected in (plan, normalize):
        selected.add_argument("--request", required=True)
        selected.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    try:
        from capture_process import observation_path

        run_dir = observation_path(str(args.run_dir))
        request_path = _artifact_path(run_dir, args.request, ".json", must_exist=True)
        output = _artifact_path(run_dir, args.output, ".json")
        if args.command == "normalize":
            source_path = _artifact_path(run_dir, args.source, ".csv", must_exist=True)
            mapping_path = _artifact_path(run_dir, args.mapping, ".json", must_exist=True)
            receipt_path = (_artifact_path(run_dir, args.collector_receipt, ".json", must_exist=True, collector=True)
                            if args.collector_receipt is not None else None)
        request_raw = _read_bounded(request_path, 1024 ** 2)
        request = loads_json(request_raw.decode("utf-8-sig"), context="GPU profile request")
        inputs = [(request_path, request_raw, 1024 ** 2)]
        if args.command == "plan":
            document = build_profile_packet(request, ngfx=args.ngfx, target_executable=args.target_exe,
                working_directory=args.working_dir, output_directory=args.output_dir)
        else:
            raw = _read_bounded(source_path, MAX_TABLE_BYTES)
            mapping_raw = _read_bounded(mapping_path, 1024 ** 2)
            mapping = loads_json(mapping_raw.decode("utf-8-sig"), context="Nsight shader mapping")
            document = normalize_nsight_shader_csv(raw.decode("utf-8"), mapping, request=request,
                source_artifact=args.source, source_sha256=hashlib.sha256(raw).hexdigest(), source_size=len(raw))
            document["mapping_artifact"] = {"path": args.mapping, "sha256": hashlib.sha256(mapping_raw).hexdigest(), "size": len(mapping_raw)}
            inputs.extend([(source_path, raw, MAX_TABLE_BYTES), (mapping_path, mapping_raw, 1024 ** 2)])
            if receipt_path is not None:
                receipt_raw = _read_bounded(receipt_path, 1024 ** 2)
                inspect_collector_receipt(loads_json(receipt_raw.decode("utf-8-sig"), context="GPU collector receipt"), request)
                document["collector_receipt_artifact"] = {"path": args.collector_receipt,
                    "sha256": hashlib.sha256(receipt_raw).hexdigest(), "size": len(receipt_raw)}
                inputs.append((receipt_path, receipt_raw, 1024 ** 2))
        document["request_artifact"] = {"path": args.request, "sha256": hashlib.sha256(request_raw).hexdigest(), "size": len(request_raw)}
        if any(output == path for path, raw, bound in inputs):
            raise SchemaError("GPU profile output must not overwrite a preserved input")
        if any(_read_bounded(path, bound) != raw for path, raw, bound in inputs):
            raise SchemaError("GPU profiling inputs changed during transformation")
        atomic_write_json(output, document, replace=False)
        print("GPU profiling packet saved; collection and native extraction remain unverified." if args.command == "plan"
              else "Shader CSV transformed; target binding, extraction, coverage and live scope require independent proof.")
        return 0
    except (OSError, UnicodeError, ValueError, OverflowError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
