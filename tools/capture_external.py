import argparse
import csv
import hashlib
import io
import json
import math
import re
import sys
from decimal import Decimal, InvalidOperation
from fractions import Fraction
from pathlib import Path, PurePosixPath
from uuid import UUID

from capture_schema import SchemaError, atomic_write_json, canonical_sha256, contained_path, read_json, sha256_file


EXTERNAL_TYPE = "anyps5.external-evidence"
MAX_EXTERNAL_BYTES = 256 * 1024 ** 2
MAX_EXTERNAL_RECORDS = 1_048_576
PROFILE_KINDS = frozenset({"cpu_profile", "gpu_profile", "presentation_profile"})
LABEL = re.compile(r"aps5 run=([0-9a-f-]{36}) capture=([0-9a-f-]{36}) op=([0-9]+) batch=([0-9]+) pipeline=([0-9]+) command=([0-9]+)")
LIMITATIONS = [
    "Normalized ETW/WPR and Nsight records are an explicit interchange protocol, not native ETL or vendor report parsers.",
    "Source hashes establish preserved bytes, not the correctness of an unverified extraction or symbolization tool.",
    "CPU samples estimate sampled activity; they do not measure every invocation or replace observed thread CPU time.",
    "External display identifiers are distinct from native flip and present identifiers.",
    "Module snapshots establish sampled address-range candidates, not continuous module lifetime or guest-function ownership.",
]


def _object(value, name):
    if type(value) is not dict:
        raise SchemaError(f"{name} must be an object")
    return value


def _text(value, name):
    if type(value) is not str or not value.strip() or "\0" in value:
        raise SchemaError(f"{name} must be a nonempty string")
    return value


def _uint(value, name, minimum=0):
    if type(value) is not int or not minimum <= value <= 2 ** 64 - 1:
        raise SchemaError(f"{name} must be a uint64 integer")
    return value


def _ticks(value, name, nullable=False):
    if value is None and nullable:
        return None
    if type(value) is not str or not re.fullmatch(r"0|[1-9][0-9]{0,19}", value) or int(value) >= 2 ** 64:
        raise SchemaError(f"{name} must be a canonical uint64 decimal string")
    return int(value)


def _number(value, name, nullable=False):
    if value is None and nullable:
        return None
    try:
        valid = type(value) in (int, float) and math.isfinite(value) and value >= 0
    except OverflowError:
        valid = False
    if not valid:
        raise SchemaError(f"{name} must be a finite nonnegative number")
    return value


def _hash(value, name, nullable=False):
    if value is None and nullable:
        return None
    if type(value) is not str or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise SchemaError(f"{name} must be a SHA-256 hexadecimal string")
    return value


def _uuid(value, name):
    try:
        valid = str(UUID(value)) == value
    except (ValueError, TypeError, AttributeError):
        valid = False
    if not valid:
        raise SchemaError(f"{name} must be a canonical UUID")


def _relative_source(value):
    _text(value, "external source artifact")
    path = PurePosixPath(value)
    if (path.is_absolute() or "\\" in value or ":" in value or any(part.casefold() in ("", ".", "..", "dump", "fakelib") for part in value.split("/"))
            or not path.parts or path.parts[0] not in ("external", "collectors")):
        raise SchemaError("profiling source must be a contained path below external/ or collectors/")
    if path.suffix.casefold() in (".exe", ".dll", ".prx", ".elf", ".self", ".bin"):
        raise SchemaError("external profiling sources must not be runtime or game executables")
    return value


def parse_debug_label(value):
    if type(value) is not str:
        return None
    match = LABEL.fullmatch(value)
    if match is None:
        return None
    run_id, capture_id, operation, batch, pipeline, command = match.groups()
    _uuid(run_id, "debug label run_id")
    _uuid(capture_id, "debug label capture_id")
    for name, identifier in (("operation", operation), ("batch", batch), ("pipeline", pipeline), ("command", command)):
        _ticks(identifier, f"debug label {name}")
    return {"run_id": run_id, "capture_id": capture_id, "operation": operation, "batch": batch, "pipeline": pipeline, "command": command}


def validate_external_document(document):
    _object(document, "external evidence")
    if document.get("document_type") != EXTERNAL_TYPE or type(document.get("schema_version")) is not int or document["schema_version"] != 1:
        raise SchemaError("unsupported external evidence protocol")
    _uuid(document.get("run_id"), "external run_id")
    if document.get("capture_id") is not None:
        _uuid(document["capture_id"], "external capture_id")
    if type(document.get("kind")) is not str or document["kind"] not in PROFILE_KINDS:
        raise SchemaError("unsupported external profile kind")
    process = _object(document.get("process"), "external process")
    _uint(process.get("pid"), "external process.pid", 1)
    _ticks(process.get("creation_filetime"), "external process.creation_filetime", nullable=True)
    _hash(process.get("executable_sha256"), "external process.executable_sha256", nullable=True)
    producer = _object(document.get("producer"), "external producer")
    for name in ("tool", "version", "adapter", "adapter_version", "native_format"):
        _text(producer.get(name), f"external producer.{name}")
    if producer.get("normalized_protocol") is not True:
        raise SchemaError("external evidence must declare its normalized protocol")
    source = _object(document.get("source"), "external source")
    _relative_source(source.get("artifact"))
    _hash(source.get("sha256"), "external source.sha256")
    _uint(source.get("size"), "external source.size")
    clock = _object(document.get("clock"), "external clock")
    _text(clock.get("domain"), "external clock.domain")
    _uint(clock.get("ticks_per_second"), "external clock.ticks_per_second", 1)
    anchors = clock.get("anchors")
    if type(anchors) is not list or len(anchors) > 4096:
        raise SchemaError("external clock anchors must be a bounded array")
    for anchor in anchors:
        _object(anchor, "external clock anchor")
        _ticks(anchor.get("external_ticks"), "external clock anchor.external_ticks")
        if type(anchor.get("trace_ns")) is not int:
            raise SchemaError("external clock anchor.trace_ns must be an integer")
        _number(anchor.get("uncertainty_ns"), "external clock anchor.uncertainty_ns")
        _text(anchor.get("source_record"), "external clock anchor.source_record")
    coverage = _object(document.get("coverage"), "external coverage")
    if coverage.get("state") not in ("complete", "partial", "unknown"):
        raise SchemaError("external coverage state is invalid")
    _text(coverage.get("scope"), "external coverage.scope")
    start = _ticks(coverage.get("start_ticks"), "external coverage.start_ticks", nullable=True)
    end = _ticks(coverage.get("end_ticks"), "external coverage.end_ticks", nullable=True)
    if start is not None and end is not None and end < start:
        raise SchemaError("external coverage timestamps are reversed")
    if coverage.get("lost_records") is not None:
        _uint(coverage["lost_records"], "external coverage.lost_records")
    records = _object(document.get("records"), "external records")
    total = 0
    allowed = {"cpu_profile": {"samples", "context_switches"}, "gpu_profile": {"gpu_counters", "shader_metrics"},
               "presentation_profile": {"presentations"}}[document["kind"]]
    identifiers = set()
    for kind, rows in records.items():
        if kind not in allowed or type(rows) is not list:
            raise SchemaError("external record array does not match its profile kind")
        total += len(rows)
        if total > MAX_EXTERNAL_RECORDS:
            raise SchemaError("external evidence exceeds its record bound")
        for row in rows:
            _object(row, "external record")
            identifier = _text(row.get("id"), "external record.id")
            if identifier in identifiers:
                raise SchemaError("external record IDs must be unique")
            identifiers.add(identifier)
            if kind == "context_switches":
                first = _ticks(row.get("start_ticks"), "context switch start")
                last = _ticks(row.get("end_ticks"), "context switch end")
                if last < first or row.get("state") != "running":
                    raise SchemaError("context-switch intervals must be observed ordered running intervals")
                _uint(row.get("cpu"), "context switch cpu")
            elif kind != "shader_metrics":
                _ticks(row.get("timestamp_ticks"), "external record timestamp")
            if kind in ("samples", "context_switches"):
                _ticks(row.get("thread_id"), "external native thread ID")
            if kind == "samples":
                _uint(row.get("weight_samples"), "CPU sample weight", 1)
                _number(row.get("weight_ns"), "CPU estimated sample time", nullable=True)
                stack = row.get("stack")
                if type(stack) is not list or len(stack) > 256:
                    raise SchemaError("CPU sample stack must be a bounded address array")
                for frame in stack:
                    _object(frame, "CPU stack frame")
                    _ticks(frame.get("address"), "CPU stack instruction pointer")
                    if frame.get("symbol") is not None:
                        _text(frame["symbol"], "declared CPU symbol")
            if kind in ("gpu_counters", "shader_metrics"):
                for name in ("name", "unit", "scope", "measurement_kind"):
                    _text(row.get(name), f"GPU counter.{name}")
                if row.get("status") not in ("measured", "unavailable", "not_requested", "failed"):
                    raise SchemaError("GPU external measurement status is invalid")
                _number(row.get("value"), "GPU external value", nullable=True)
                if row["status"] == "measured" and row.get("value") is None or row["status"] != "measured" and row.get("value") is not None:
                    raise SchemaError("GPU external availability contradicts its value")
                if row.get("debug_label") is not None:
                    _text(row["debug_label"], "GPU debug label")
                if row.get("shader_sha256") is not None:
                    _hash(row["shader_sha256"], "external shader digest")
                if row.get("stage") is not None:
                    _text(row["stage"], "external shader stage")
            if kind == "shader_metrics":
                from capture_gpu_profile import METRIC_UNITS, STAGES

                if "timestamp_ticks" in row or row["scope"] != "exported_shader_row_aggregate":
                    raise SchemaError("aggregated shader rows cannot invent timestamps or invocation scope")
                if row["measurement_kind"] not in METRIC_UNITS or row["unit"] not in METRIC_UNITS[row["measurement_kind"]]:
                    raise SchemaError("unsupported sampled shader metric kind or unit")
                if row.get("stage") is not None and row["stage"] not in STAGES:
                    raise SchemaError("unsupported explicit shader stage")
                if (row.get("shader_sha256") is not None and row.get("shader_digest_scope") != "anyps5.final-spirv.v1"
                        or row.get("shader_sha256") is None and row.get("shader_digest_scope") is not None):
                    raise SchemaError("exported shader digest must identify the explicit native domain-specific SPIR-V scope")
                if row["unit"] == "percent" and row["value"] is not None and row["value"] > 100:
                    raise SchemaError("shader sample percent exceeds 100")
                if row["unit"] in ("samples", "count", "cycles", "bytes", "instructions", "thread_instructions") and row["value"] is not None:
                    _uint(row["value"], "shader observed count")
                _text(row.get("shader_label"), "exported shader label")
                if row.get("vendor_shader_hash") is not None:
                    _text(row["vendor_shader_hash"], "opaque vendor shader hash")
                _uint(row.get("source_record"), "shader CSV source record", 1)
                lines = _object(row.get("source_lines"), "shader CSV source lines")
                first = _uint(lines.get("first"), "shader CSV first line", 2)
                if _uint(lines.get("last"), "shader CSV last line", 2) < first:
                    raise SchemaError("shader source line interval is reversed")
            if kind == "presentations":
                for name in ("swapchain", "present_mode"):
                    _text(row.get(name), f"external presentation.{name}")
                metrics = _object(row.get("metrics"), "external presentation metrics")
                for name, value in metrics.items():
                    _text(name, "external presentation metric name")
                    _number(value, "external presentation metric", nullable=True)
    if "shader_metrics" in records:
        from capture_gpu_profile import ADAPTER, validate_profile_request

        if producer["adapter"] != ADAPTER or set(records) != {"shader_metrics"}:
            raise SchemaError("shader table aggregates require the dedicated verifiable CSV adapter")
        request = validate_profile_request(document.get("profile_request"))
        if (request["run_id"] != document["run_id"] or request["capture_id"] != document.get("capture_id")
                or any(request["process"].get(key) != process.get(key) for key in ("pid", "creation_filetime", "executable_sha256"))):
            raise SchemaError("shader profile request contradicts its normalized envelope")
        if (clock["domain"] != "unavailable_aggregated_shader_table" or clock["ticks_per_second"] != 1 or anchors
                or coverage["state"] != "unknown" or start is not None or end is not None or coverage.get("lost_records") is not None):
            raise SchemaError("shader CSV alone cannot establish timestamps, clock anchors, coverage or loss")
    return document


def _csv_number(value, name):
    if value is None or not value.strip() or value.strip().upper() in ("NA", "N/A"):
        return None
    try:
        result = Decimal(value)
    except InvalidOperation as error:
        raise SchemaError(f"invalid PresentMon {name}") from error
    if not result.is_finite() or result < 0:
        raise SchemaError(f"invalid PresentMon {name}")
    return result


def _csv_rows(reader):
    try:
        yield from reader
    except csv.Error as error:
        raise SchemaError("PresentMon CSV contains an invalid or oversized field") from error


def normalize_presentmon_csv(text, *, run_id, pid, tool_version, source_artifact, source_sha256, source_size,
                             frequency=None, process=None, clock_anchors=None):
    reader = csv.DictReader(io.StringIO(text.lstrip("\ufeff")), strict=True)
    try:
        fields = reader.fieldnames or []
    except csv.Error as error:
        raise SchemaError("PresentMon CSV header is invalid or oversized") from error
    if not 1 <= len(fields) <= 256 or len(fields) != len(set(fields)) or not {"ProcessID", "SwapChainAddress", "PresentMode"} <= set(fields):
        raise SchemaError("PresentMon CSV headers are missing or duplicated")
    time_columns = [name for name in ("CPUStartQPC", "CPUStartTime", "CPUStartQPCTime") if name in fields]
    if len(time_columns) != 1:
        raise SchemaError("PresentMon CSV requires exactly one supported CPU start-time column")
    column = time_columns[0]
    if column == "CPUStartQPC" and (type(frequency) is not int or frequency <= 0):
        raise SchemaError("PresentMon raw QPC import requires its observed frequency")
    domain = "windows_qpc" if column == "CPUStartQPC" else "windows_qpc_ns" if column == "CPUStartQPCTime" else "presentmon_relative_ns"
    tick_frequency = frequency if column == "CPUStartQPC" else 1_000_000_000
    rows, excluded = [], 0
    metric_fields = ("MsCPUBusy", "MsCPUWait", "MsGPULatency", "MsGPUTime", "MsGPUBusy", "MsGPUWait",
                     "DisplayLatency", "DisplayedTime", "MsBetweenPresents", "MsInPresentAPI", "MsUntilDisplayed")
    for source_row, row in enumerate(_csv_rows(reader), 2):
        if source_row - 1 > MAX_EXTERNAL_RECORDS:
            raise SchemaError("PresentMon CSV exceeds its row bound")
        if None in row or any(value is None or len(value) > 65_536 for value in row.values()):
            raise SchemaError("PresentMon CSV row does not match its headers")
        try:
            row_pid = int(row["ProcessID"])
        except (ValueError, TypeError) as error:
            raise SchemaError("PresentMon ProcessID must be an integer") from error
        if not 0 < row_pid < 2 ** 32:
            raise SchemaError("PresentMon ProcessID is outside the native PID range")
        if row_pid != pid:
            excluded += 1
            continue
        value = _csv_number(row[column], column)
        if value is None:
            raise SchemaError("PresentMon CPU start timestamp is unavailable")
        scaled = value if column == "CPUStartQPC" else value * 1_000_000
        if scaled != scaled.to_integral_value():
            raise SchemaError("PresentMon time cannot be represented by the declared integer clock")
        metrics = {name: float(number) if number is not None else None
                   for name in metric_fields if name in fields
                   for number in [_csv_number(row[name], name)]}
        rows.append({"id": f"csv:{source_row}", "source_row": source_row, "timestamp_ticks": str(int(scaled)),
                     "swapchain": row["SwapChainAddress"], "present_mode": row["PresentMode"],
                     "external_present_id": row.get("PresentId") or None, "metrics": metrics})
    timestamps = [int(row["timestamp_ticks"]) for row in rows]
    binding = {"pid": pid, "creation_filetime": None, "executable_sha256": None}
    if process is not None:
        binding.update(process)
    document = {"schema_version": 1, "document_type": EXTERNAL_TYPE, "kind": "presentation_profile",
                "run_id": run_id, "process": binding,
                "producer": {"tool": "PresentMon", "version": tool_version, "adapter": "presentmon_csv_v1",
                             "adapter_version": "1", "native_format": "documented_console_csv", "normalized_protocol": True},
                "source": {"artifact": source_artifact, "sha256": source_sha256, "size": source_size},
                "clock": {"domain": domain, "ticks_per_second": tick_frequency, "anchors": clock_anchors or []},
                "coverage": {"state": "unknown", "scope": "observed PresentMon rows for selected PID; display endpoints differ from internal presents",
                             "start_ticks": str(min(timestamps)) if timestamps else None,
                             "end_ticks": str(max(timestamps)) if timestamps else None, "lost_records": None},
                "records": {"presentations": rows}, "adapter_metadata": {"timestamp_column": column,
                    "time_values_unit": "milliseconds_except_raw_QPC", "excluded_other_pid_rows": excluded}}
    return validate_external_document(document)


def _identity_binding(document, manifest, process_evidence):
    if document["run_id"] != manifest["run_id"] or document["process"]["pid"] != manifest["execution"].get("pid"):
        raise SchemaError("external run or PID does not match the captured child")
    actual = (process_evidence or {}).get("process") or {}
    _object(actual, "observed external process binding")
    if actual.get("executable") is not None:
        _object(actual["executable"], "observed process executable")
    declared = document["process"]
    if (process_evidence or {}).get("status") == "invalid" or actual.get("identity_verified") is not True or not actual.get("executable"):
        return {"status": "unknown", "reasons": ["independently observed process creation and executable identity are unavailable"]}
    if declared.get("creation_filetime") is None or declared.get("executable_sha256") is None:
        return {"status": "unknown", "reasons": ["external evidence does not establish process creation and executable identity"]}
    if (declared["creation_filetime"] != actual.get("creation_filetime")
            or declared["executable_sha256"] != actual["executable"].get("sha256") or declared["pid"] != actual.get("pid")):
        raise SchemaError("external evidence process creation or executable identity contradicts observed child")
    if (process_evidence.get("run_id") != manifest["run_id"] or actual.get("identity_binding") != "retained_child_handle"):
        return {"status": "unknown", "reasons": ["retained-child process observation binding is unavailable"]}
    return {"status": "valid", "reasons": ["matching retained-child run, PID, creation and executable identity"],
            "scope": "process identity only; no timestamp, profiler extraction or live-scope assertion"}


def _binding(document, manifest, process_evidence):
    identity = _identity_binding(document, manifest, process_evidence)
    if identity["status"] != "valid":
        return {**identity, "identity_status": identity["status"], "identity_reasons": identity["reasons"]}
    identity_fields = {"identity_status": "valid", "identity_reasons": identity["reasons"]}
    clock, observed = document["clock"], process_evidence.get("clock") or {}
    anchors = observed.get("anchors") or []
    same_ticks = clock["domain"] in ("windows_qpc", "query_performance_counter") and clock["ticks_per_second"] == observed.get("ticks_per_second")
    scaled_qpc = clock["domain"] == "windows_qpc_ns" and clock["ticks_per_second"] == 1_000_000_000 and type(observed.get("ticks_per_second")) is int and observed["ticks_per_second"] > 0
    if observed.get("domain") != "windows_qpc" or not (same_ticks or scaled_qpc) or not anchors:
        return {"status": "unknown", **identity_fields, "reasons": ["profile timestamps lack a same-clock observed process interval"]}
    first, last = min(int(row["ticks"]) for row in anchors), max(int(row["ticks"]) for row in anchors)
    if scaled_qpc:
        first, last = Fraction(first * 1_000_000_000, observed["ticks_per_second"]), Fraction(last * 1_000_000_000, observed["ticks_per_second"])
    timestamps = [int(row[key]) for rows in document["records"].values() for row in rows
                  for key in ("timestamp_ticks", "start_ticks", "end_ticks") if row.get(key) is not None]
    if any(tick < first or tick > last for tick in timestamps):
        return {"status": "unknown", **identity_fields, "reasons": ["profile records extend outside the independently observed child interval"]}
    return {"status": "valid", **identity_fields, "reasons": ["matching retained-child identity and timestamps within sampled same-clock process bounds"],
            "scope": "sampled process interval; not proof of unverified native extraction"}


def _private_artifact_path(run_dir, relative, *, must_exist=False):
    from capture_process import observation_path

    root = observation_path(str(Path(run_dir).absolute()))
    observation_path(str(root / relative))
    return observation_path(str(contained_path(root, relative, must_exist=must_exist)))


def _symbol_maps(run_dir, manifest, verified_artifacts, process_evidence=None):
    maps = []
    for descriptor in manifest["artifacts"]:
        if descriptor["kind"] != "symbol_receipt":
            continue
        path = _private_artifact_path(run_dir, descriptor["path"], must_exist=True)
        if (descriptor["size"] > 4 * 1024 ** 2 or path.stat().st_size != descriptor["size"]
                or sha256_file(path) != descriptor["sha256"]
                or verified_artifacts is not None and descriptor["path"] not in verified_artifacts):
            raise SchemaError("symbol receipt bytes are unverified or oversized")
        receipt = _object(read_json(path), "symbol receipt")
        if receipt.get("document_type") != "anyps5.symbol-receipt" or receipt.get("schema_version") != 1:
            raise SchemaError("unsupported symbol receipt protocol")
        image = _object(receipt.get("image"), "symbol receipt image")
        _hash(image.get("sha256"), "symbol receipt image hash")
        _uint(image.get("size"), "symbol receipt image size")
        matched_modules = [module for module in (process_evidence or {}).get("modules", [])
                           if (process_evidence or {}).get("status") != "invalid"
                           and module.get("file_identity") == {key: image.get(key) for key in ("size", "sha256")}]
        assessed = {"status": "unknown", "binding": "unknown", "function_ranges": [], "address_resolutions": [],
                    "reason": "receipt image has no matching independently observed sampled module identity", "issues": []}
        if matched_modules:
            from capture_symbols import assess_symbol_receipt

            assessed = assess_symbol_receipt(receipt, observed_image=matched_modules[0]["file_identity"])
        selected = [{"start": int(row["start_rva"]), "end": int(row["end_rva"]), "name": row["name"]}
                    for row in assessed["function_ranges"]] if assessed["status"] == "valid" else []
        if path.stat().st_size != descriptor["size"] or sha256_file(path) != descriptor["sha256"]:
            raise SchemaError("symbol receipt changed during assessment")
        maps.append({**assessed, "image": assessed.get("image") or image, "ranges": selected, "artifact": dict(descriptor),
                     "matched_observed_module_ids": [module.get("module_id") for module in matched_modules]})
    return maps


def _shader_adapter_input(run_dir, document, key, descriptors, verified_artifacts):
    from capture_process import observation_path
    from capture_tabular import _read_bounded

    descriptor = _object(document.get(key), f"shader {key}")
    relative = _text(descriptor.get("path"), f"shader {key} path")
    if key == "collector_receipt_artifact":
        if ("\\" in relative or ":" in relative or not relative.startswith(("external/", "collectors/"))
                or any(part.casefold() in ("", ".", "..", "dump", "fakelib", "app0") for part in relative.split("/"))):
            raise SchemaError("GPU collector receipt path must be contained below external/ or collectors/")
    else:
        _relative_source(relative)
    selected = descriptors.get(relative)
    if (selected is None or relative not in verified_artifacts
            or any(descriptor.get(name) != selected[name] for name in ("size", "sha256")) or selected["size"] > 1024 ** 2
            or Path(relative).suffix.casefold() != ".json"):
        raise SchemaError(f"shader {key} does not identify a bounded verified JSON manifest artifact")
    observation_path(str(Path(run_dir) / relative))
    path = observation_path(str(contained_path(run_dir, relative, must_exist=True)))
    raw = _read_bounded(path, 1024 ** 2)
    if len(raw) != selected["size"] or hashlib.sha256(raw).hexdigest() != selected["sha256"]:
        raise SchemaError(f"shader {key} integrity is invalid")
    from capture_schema import loads_json

    value = loads_json(raw.decode("utf-8-sig"), context=key)
    return value, path, raw


def _shader_collector_receipt(run_dir, document, request, descriptors, verified_artifacts):
    from capture_gpu_profile import inspect_collector_receipt
    from capture_process import observation_path
    from capture_tabular import _read_bounded

    if document.get("collector_receipt_artifact") is None:
        return {"status": "missing_proof", "tool_authenticity": "unknown", "scope_provenance": "unknown"}
    receipt, path, raw = _shader_adapter_input(run_dir, document, "collector_receipt_artifact", descriptors, verified_artifacts)
    assessment = inspect_collector_receipt(receipt, request)
    references = receipt.get("artifacts", [])
    if type(references) is not list or len(references) > 1024:
        raise SchemaError("GPU collector artifact references must be a bounded list")
    references = list(references)
    for command in receipt["commands"]:
        references.extend(command[key] for key in ("stdout", "stderr") if command.get(key) is not None)
    bound = []
    for reference in references:
        _object(reference, "GPU collector artifact reference")
        relative = _text(reference.get("path"), "GPU collector artifact path")
        if (PurePosixPath(relative).is_absolute() or "\\" in relative or ":" in relative
                or any(part.casefold() in ("", ".", "..", "dump", "fakelib", "app0") for part in relative.split("/"))):
            raise SchemaError("GPU collector artifact must be a contained private evidence path")
        observation_path(str(Path(run_dir) / relative))
        selected = descriptors.get(relative)
        if (selected is None or relative not in verified_artifacts
                or any(reference.get(key) != selected[key] for key in ("size", "sha256"))):
            raise SchemaError("GPU collector related artifact does not bind verified manifest bytes")
        bound.append(relative)
    if _read_bounded(path, 1024 ** 2) != raw:
        raise SchemaError("GPU collector receipt changed during assessment")
    assessment["artifact_integrity"] = "valid"
    assessment["related_artifact_paths"] = sorted(set(bound))
    assessment["source_csv_in_receipt"] = document["source"]["artifact"] in bound
    return assessment


def load_external_evidence(run_dir, manifest, *, verified_artifacts=None, process_evidence=None):
    run_dir = Path(run_dir)
    result = {"schema_version": 1, "document_type": "anyps5.external-assessment", "run_id": manifest["run_id"],
              "status": "unknown", "profiles": [], "issues": [], "limitations": list(LIMITATIONS)}
    if verified_artifacts is None:
        verified_artifacts = {}
        try:
            for item in manifest["artifacts"]:
                path = _private_artifact_path(run_dir, item["path"], must_exist=True)
                if path.stat().st_size != item["size"] or sha256_file(path) != item["sha256"]:
                    raise SchemaError("manifest artifact integrity failed during independent external assessment")
                verified_artifacts[item["path"]] = path
        except (OSError, ValueError) as error:
            result["status"] = "invalid"
            result["issues"].append({"code": "external_artifact_invalid", "severity": "error", "message": str(error)})
            return result
    if process_evidence is None:
        from capture_process import assess_process_observation
        process_evidence = assess_process_observation(run_dir, manifest, verified_artifacts or {})
    descriptors = {item["path"]: item for item in manifest["artifacts"]}
    for descriptor in manifest["artifacts"]:
        if descriptor["kind"] not in PROFILE_KINDS:
            continue
        try:
            path = _private_artifact_path(run_dir, descriptor["path"], must_exist=True)
            if path.stat().st_size > MAX_EXTERNAL_BYTES or path.stat().st_size != descriptor["size"] or sha256_file(path) != descriptor["sha256"]:
                raise SchemaError("external normalized artifact integrity or size is invalid")
            if verified_artifacts is not None and descriptor["path"] not in verified_artifacts:
                raise SchemaError("external normalized artifact was not independently selected as usable")
            document = validate_external_document(read_json(path))
            if document["kind"] != descriptor["kind"]:
                raise SchemaError("external artifact kind contradicts its normalized protocol")
            source = document["source"]
            source_descriptor = descriptors.get(source["artifact"])
            if source_descriptor is None or any(source[key] != source_descriptor[key] for key in ("sha256", "size")):
                raise SchemaError("external source does not identify an explicit matching manifest artifact")
            source_path = _private_artifact_path(run_dir, source["artifact"], must_exist=True)
            if (source_path.stat().st_size != source["size"] or sha256_file(source_path) != source["sha256"]
                    or verified_artifacts is not None and source["artifact"] not in verified_artifacts):
                raise SchemaError("external source artifact integrity is invalid")
            binding = _binding(document, manifest, process_evidence)
            extraction = {"status": "unknown", "method": "normalized_bridge", "reasons": ["native vendor extraction is not independently parsed"]}
            if document["producer"]["adapter"] == "presentmon_csv_v1":
                if source["size"] > MAX_EXTERNAL_BYTES:
                    raise SchemaError("PresentMon CSV exceeds the supported adapter size")
                parsed = normalize_presentmon_csv(source_path.read_bytes().decode("utf-8"), run_id=document["run_id"],
                    pid=document["process"]["pid"], tool_version=document["producer"]["version"],
                    source_artifact=source["artifact"], source_sha256=source["sha256"], source_size=source["size"],
                    frequency=document["clock"]["ticks_per_second"], process=document["process"],
                    clock_anchors=document["clock"]["anchors"])
                if any(canonical_sha256(parsed[key]) != canonical_sha256(document[key]) for key in ("records", "clock", "adapter_metadata")):
                    raise SchemaError("normalized presentations contradict the preserved native CSV")
                extraction = {"status": "valid", "method": "documented_presentmon_csv", "reasons": []}
            if document["producer"]["adapter"] == "explicit_table_v1":
                from capture_tabular import MAX_TABLE_BYTES, normalize_exported_table
                if source["size"] > MAX_TABLE_BYTES:
                    raise SchemaError("exported table exceeds the supported 64 MiB adapter size")
                mapping_descriptor = _object(document.get("mapping_artifact"), "export mapping artifact")
                mapping_path = mapping_descriptor.get("path")
                selected_mapping = descriptors.get(mapping_path)
                if (selected_mapping is None or mapping_path not in verified_artifacts
                        or any(mapping_descriptor.get(key) != selected_mapping[key] for key in ("size", "sha256"))
                        or selected_mapping["size"] > 1024 ** 2):
                    raise SchemaError("export mapping does not bind an explicit verified manifest artifact")
                mapping_file = _private_artifact_path(run_dir, mapping_path, must_exist=True)
                if mapping_file.stat().st_size != selected_mapping["size"] or sha256_file(mapping_file) != selected_mapping["sha256"]:
                    raise SchemaError("export mapping artifact integrity is invalid")
                mapping = read_json(mapping_file)
                parsed = normalize_exported_table(source_path.read_bytes().decode("utf-8"), mapping,
                    run_id=document["run_id"], process=document["process"], source_artifact=source["artifact"],
                    source_sha256=source["sha256"], source_size=source["size"])
                if any(canonical_sha256(parsed[key]) != canonical_sha256(document[key]) for key in ("records", "clock", "producer", "adapter_metadata")):
                    raise SchemaError("normalized evidence contradicts the preserved table and explicit mapping")
                if mapping_file.stat().st_size != selected_mapping["size"] or sha256_file(mapping_file) != selected_mapping["sha256"]:
                    raise SchemaError("export mapping changed during assessment")
                extraction = {"status": "unknown", "method": "explicit_export_table", "transformation_status": "valid",
                    "reasons": ["selected CSV transformation is verified; original vendor extraction and aggregation remain unverified"]}
            if document["producer"]["adapter"] == "nsight_shader_csv_v1":
                from capture_gpu_profile import MAX_TABLE_BYTES, normalize_nsight_shader_csv
                from capture_process import observation_path
                from capture_tabular import _read_bounded

                if source["size"] > MAX_TABLE_BYTES or source_path.suffix.casefold() != ".csv":
                    raise SchemaError("Nsight shader source requires bounded preserved CSV")
                observation_path(str(run_dir / source["artifact"]))
                observation_path(str(source_path))
                mapping, mapping_file, mapping_raw = _shader_adapter_input(run_dir, document, "mapping_artifact", descriptors, verified_artifacts)
                request, request_file, request_raw = _shader_adapter_input(run_dir, document, "request_artifact", descriptors, verified_artifacts)
                native = request.get("native_artifact")
                _object(native, "shader request native artifact")
                native_path = _text(native.get("path"), "shader request native artifact path")
                native_descriptor = descriptors.get(native_path)
                if (native_descriptor is None or native_path not in verified_artifacts
                        or native_descriptor["kind"] != "draw_trace"
                        or any(native.get(key) != native_descriptor[key] for key in ("size", "sha256"))):
                    raise SchemaError("shader request does not bind the selected verified native draw trace")
                parsed = normalize_nsight_shader_csv(_read_bounded(source_path, MAX_TABLE_BYTES).decode("utf-8"), mapping,
                    request=request, source_artifact=source["artifact"], source_sha256=source["sha256"], source_size=source["size"])
                if any(canonical_sha256(parsed[key]) != canonical_sha256(document[key])
                       for key in ("records", "clock", "coverage", "producer", "adapter_metadata", "profile_request")):
                    raise SchemaError("shader profile contradicts its preserved CSV, mapping or request")
                if (_read_bounded(mapping_file, 1024 ** 2) != mapping_raw or _read_bounded(request_file, 1024 ** 2) != request_raw):
                    raise SchemaError("shader mapping or request changed during assessment")
                extraction = {"status": "unknown", "method": "nsight_shader_profiler_csv", "transformation_status": "valid",
                    "tool_identity": "missing_proof", "scope_provenance": "unknown", "loss_accounting": "missing_proof",
                    "reasons": ["preserved CSV transformation and request are verified; vendor extraction, tool execution, sampled range and live/replay scope remain unverified"]}
                extraction["collector_receipt"] = _shader_collector_receipt(run_dir, document, request, descriptors, verified_artifacts)
            ownership, symbols = None, []
            if document["kind"] == "cpu_profile":
                from capture_process import assess_cpu_sample_ownership
                ownership = assess_cpu_sample_ownership(document, process_evidence, artifact=dict(descriptor))
                symbols = _symbol_maps(run_dir, manifest, verified_artifacts, process_evidence)
            if path.stat().st_size != descriptor["size"] or sha256_file(path) != descriptor["sha256"]:
                raise SchemaError("normalized evidence changed during assessment")
            if source_path.stat().st_size != source["size"] or sha256_file(source_path) != source["sha256"]:
                raise SchemaError("raw external source changed during assessment")
            result["profiles"].append({"document": document, "artifact": dict(descriptor), "source_artifact": dict(source_descriptor),
                "source_integrity": {"status": "valid", "reasons": []}, "process_binding": binding,
                "extraction_provenance": extraction, "cpu_sample_ownership": ownership, "symbol_receipts": symbols})
        except (OSError, ValueError, UnicodeError, OverflowError) as error:
            result["issues"].append({"code": "external_evidence_invalid", "severity": "error", "message": f"{descriptor['path']}: {error}"})
    result["status"] = "invalid" if result["issues"] else "valid" if result["profiles"] and all(
        profile["process_binding"]["status"] == "valid" and profile["extraction_provenance"]["status"] == "valid"
        for profile in result["profiles"]) else "unknown"
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description="Import documented PresentMon CSV or inspect normalized external profiling evidence; never launch a profiler.")
    subparsers = parser.add_subparsers(dest="command", required=True)
    normalize = subparsers.add_parser("normalize-presentmon")
    for name in ("input", "output", "manifest"):
        normalize.add_argument(f"--{name}", required=True, type=Path)
    normalize.add_argument("--tool-version", required=True)
    normalize.add_argument("--source-artifact", required=True)
    normalize.add_argument("--qpc-frequency", type=int)
    normalize.add_argument("--process-creation-filetime")
    normalize.add_argument("--executable-sha256")
    inspect = subparsers.add_parser("assess")
    inspect.add_argument("run_dir", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "normalize-presentmon":
            if args.input.suffix.casefold() != ".csv" or any(part.casefold() == "fakelib" for part in args.input.resolve().parts):
                raise SchemaError("PresentMon input must be an explicit CSV outside forbidden platform-library paths")
            if args.input.stat().st_size > MAX_EXTERNAL_BYTES:
                raise SchemaError("PresentMon CSV exceeds the supported adapter size")
            manifest = read_json(args.manifest)
            data = args.input.read_bytes()
            document = normalize_presentmon_csv(data.decode("utf-8-sig"), run_id=manifest["run_id"],
                pid=manifest["execution"]["pid"], tool_version=args.tool_version, source_artifact=args.source_artifact,
                source_sha256=hashlib.sha256(data).hexdigest(), source_size=len(data), frequency=args.qpc_frequency,
                process={"creation_filetime": args.process_creation_filetime, "executable_sha256": args.executable_sha256})
            atomic_write_json(args.output, document, replace=False)
            print(str(args.output))
            return 0
        manifest = read_json(args.run_dir / "manifest.json")
        result = load_external_evidence(args.run_dir, manifest)
        json.dump(result, sys.stdout, indent=2, allow_nan=False)
        sys.stdout.write("\n")
        return 2 if result["status"] == "invalid" else 0 if result["status"] == "valid" else 1
    except (OSError, ValueError, UnicodeError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
