import hashlib
import json
import math
import os
import re
import tempfile
from datetime import datetime, timedelta
from pathlib import Path, PureWindowsPath
from uuid import UUID


SCHEMA_VERSION = 1
MANIFEST_TYPE = "anyps5.run-manifest"
HEALTH_TYPE = "anyps5.capture-health"
COMPARISON_TYPE = "anyps5.capture-comparison"
DRAW_TRACE_TYPE = "anyps5.draw-trace"
WORKLOAD_KEYS = ("requested_draw_operations", "dispatch_operations", "indirect_record_operations",
    "host_draw_commands", "host_dispatch_commands", "operation_status.complete", "operation_status.skipped",
    "operation_status.rejected", "operation_status.failed", "operation_status.cancelled", "host_commands_completed",
    "host_commands_failed", "jobs_started", "jobs_completed", "jobs_failed")
ENVIRONMENT_PREFIXES = ("APS5_", "ANYPS5_", "VK_")


class SchemaError(ValueError):
    pass


def _object(value, name):
    if type(value) is not dict:
        raise SchemaError(f"{name} must be an object")
    return value


def _array(value, name):
    if type(value) is not list:
        raise SchemaError(f"{name} must be an array")
    return value


def _string(value, name):
    if type(value) is not str or not value.strip() or "\x00" in value:
        raise SchemaError(f"{name} must be a nonempty string without NUL")
    return value


def _boolean(value, name):
    if type(value) is not bool:
        raise SchemaError(f"{name} must be a boolean")
    return value


def _integer(value, name, minimum=None):
    if type(value) is not int or (minimum is not None and value < minimum):
        suffix = f" >= {minimum}" if minimum is not None else ""
        raise SchemaError(f"{name} must be an integer{suffix}")
    return value


def number(value, name, minimum=None):
    try:
        valid = type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        valid = False
    if not valid or (minimum is not None and value < minimum):
        suffix = f" >= {minimum}" if minimum is not None else ""
        raise SchemaError(f"{name} must be a finite number{suffix}")
    return value


def _choice(value, name, choices):
    if type(value) is not str or value not in choices:
        raise SchemaError(f"{name} must be one of {', '.join(choices)}")
    return value


def _hash(value, name, length=64):
    if type(value) is not str or re.fullmatch(f"[0-9a-f]{{{length}}}", value) is None:
        raise SchemaError(f"{name} must be a lowercase {length}-digit hexadecimal hash")
    return value


def _uuid(value, name):
    _string(value, name)
    try:
        valid = str(UUID(value)) == value
    except ValueError:
        valid = False
    if not valid:
        raise SchemaError(f"{name} must be a canonical lowercase hyphenated UUID")
    return value


def _timestamp(value, name, nullable=False):
    if value is None and nullable:
        return None
    _string(value, name)
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise SchemaError(f"{name} must be a UTC ISO timestamp") from error
    if "T" not in value or parsed.utcoffset() != timedelta(0):
        raise SchemaError(f"{name} must be a UTC ISO timestamp")
    return parsed


def _json_value(value, name="JSON"):
    if value is None or type(value) in (str, bool, int):
        return
    if type(value) is float:
        number(value, name)
        return
    if type(value) is list:
        for index, item in enumerate(value):
            _json_value(item, f"{name}[{index}]")
        return
    if type(value) is dict:
        for key, item in value.items():
            if type(key) is not str:
                raise SchemaError(f"{name} keys must be strings")
            _json_value(item, f"{name}.{key}")
        return
    raise SchemaError(f"{name} contains a value that is not JSON")


def _unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise SchemaError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _constant(value):
    raise SchemaError(f"nonfinite JSON number {value}")


def loads_json(text, *, context="JSON"):
    try:
        value = json.loads(text, object_pairs_hook=_unique_pairs, parse_constant=_constant)
        _json_value(value)
        return value
    except (ValueError, TypeError, RecursionError) as error:
        raise SchemaError(f"{context}: {error}") from error


def read_json(path):
    try:
        return loads_json(Path(path).read_text(encoding="utf-8"), context=str(path))
    except UnicodeError as error:
        raise SchemaError(f"{path}: JSON must be UTF-8") from error


def canonical_json_bytes(value):
    try:
        _json_value(value)
        return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
                          allow_nan=False).encode("utf-8")
    except (ValueError, TypeError, UnicodeError, RecursionError) as error:
        raise SchemaError(f"cannot encode canonical JSON: {error}") from error


def canonical_sha256(value):
    return hashlib.sha256(canonical_json_bytes(value)).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _relative_path(value, name):
    _string(value, name)
    windows = PureWindowsPath(value)
    normalized = value.replace("\\", "/")
    parts = normalized.split("/")
    if windows.drive or windows.root or normalized.startswith("/") or ":" in value:
        raise SchemaError(f"{name} must be a relative contained path")
    if any(part in ("", ".", "..") or part.endswith((" ", ".")) for part in parts):
        raise SchemaError(f"{name} must not contain empty, dot, traversal or ambiguous components")
    devices = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)),
               *(f"LPT{i}" for i in range(1, 10))}
    if any(part.split(".", 1)[0].upper() in devices for part in parts):
        raise SchemaError(f"{name} must not contain Windows device names")
    return Path(*parts)


def contained_path(run_dir, relative_path, *, must_exist=False):
    relative = _relative_path(relative_path, "artifact.path")
    try:
        root = Path(run_dir).resolve(strict=True)
        if not root.is_dir():
            raise SchemaError("run directory must be a directory")
        path = (root / relative).resolve(strict=must_exist)
        if not path.is_relative_to(root) or path == root:
            raise SchemaError("artifact.path escapes the run directory")
    except (OSError, RuntimeError) as error:
        raise SchemaError(f"cannot resolve artifact.path: {error}") from error
    return path


def atomic_write_json(path, value, *, replace=True):
    try:
        _json_value(value)
    except (ValueError, TypeError, UnicodeError, RecursionError) as error:
        raise SchemaError(f"cannot encode canonical JSON: {error}") from error
    target = Path(path)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{target.name}.", suffix=".tmp", dir=target.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            try:
                json.dump(value, output, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
                          allow_nan=False)
                output.write("\n")
            except (ValueError, TypeError, UnicodeError, RecursionError) as error:
                raise SchemaError(f"cannot encode canonical JSON: {error}") from error
            output.flush()
            os.fsync(output.fileno())
        if replace:
            os.replace(temporary, target)
        else:
            os.link(temporary, target)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def _absolute_path(value, name):
    _string(value, name)
    if not Path(value).is_absolute() and not PureWindowsPath(value).is_absolute():
        raise SchemaError(f"{name} must be an absolute path")
    return value


def _strings(value, name, unique=False):
    values = _array(value, name)
    for index, item in enumerate(values):
        _string(item, f"{name}[{index}]")
    if unique and len(values) != len(set(values)):
        raise SchemaError(f"{name} must not contain duplicates")
    return values


def validate_capture(value):
    capture = _object(value, "capture")
    _choice(capture.get("unit"), "capture.unit", ("frames", "seconds"))
    _integer(capture.get("start"), "capture.start", 0)
    _integer(capture.get("length"), "capture.length", 1)
    if capture["start"] > 2 ** 64 - 1 or capture["length"] > 2 ** 64 - 1:
        raise SchemaError("capture.start and capture.length must fit uint64_t")
    return value


def capture_spec(capture):
    validate_capture(capture)
    return f"{capture['start']}:{capture['length']}"


def _header(value, document_type):
    _object(value, "document")
    _json_value(value)
    if type(value.get("schema_version")) is not int or value["schema_version"] != SCHEMA_VERSION:
        raise SchemaError(f"schema_version must be {SCHEMA_VERSION}")
    if value.get("document_type") != document_type:
        raise SchemaError(f"document_type must be {document_type!r}")


def validate_artifact(value, run_dir=None):
    artifact = _object(value, "artifact")
    _string(artifact.get("kind"), "artifact.kind")
    _relative_path(artifact.get("path"), "artifact.path")
    _hash(artifact.get("sha256"), "artifact.sha256")
    _integer(artifact.get("size"), "artifact.size", 0)
    if "trace" in artifact:
        _string(artifact["trace"], "artifact.trace")
    if run_dir is not None:
        contained_path(run_dir, artifact["path"])
    return value


def _metadata(value, name):
    if type(value) is str:
        _string(value, name)
    elif type(value) in (dict, list):
        if not value:
            raise SchemaError(f"{name} must not be empty")
    else:
        raise SchemaError(f"{name} must be a nonempty string, object or array")


def validate_cache(value):
    cache = _object(value, "settings.cache")
    for key in ("shader", "pipeline", "driver"):
        state = cache.get(key)
        if type(state) is dict:
            state = state.get("state")
        _choice(state, f"settings.cache.{key}", ("cold", "warm", "disabled", "unknown"))
    return value


def _path_identity(value):
    if PureWindowsPath(value).drive:
        return str(PureWindowsPath(value)).casefold()
    return os.path.normcase(os.path.normpath(value))


def _validate_source(source):
    source = _object(source, "source")
    _absolute_path(source.get("root"), "source.root")
    _hash(source.get("commit"), "source.commit", 40)
    _boolean(source.get("dirty"), "source.dirty")
    _hash(source.get("diff_sha256"), "source.diff_sha256")
    paths = []
    for item in _array(source.get("untracked"), "source.untracked"):
        _object(item, "source.untracked entry")
        _relative_path(item.get("path"), "source.untracked.path")
        _hash(item.get("sha256"), "source.untracked.sha256")
        _integer(item.get("size"), "source.untracked.size", 0)
        paths.append(os.path.normcase(item["path"].replace("\\", "/")))
    if len(paths) != len(set(paths)):
        raise SchemaError("source.untracked contains duplicate paths")
    submodule_paths = []
    for item in _array(source.get("submodules"), "source.submodules"):
        _object(item, "source.submodule")
        _relative_path(item.get("path"), "source.submodule.path")
        _hash(item.get("index_commit"), "source.submodule.index_commit", 40)
        _boolean(item.get("initialized"), "source.submodule.initialized")
        submodule_paths.append(os.path.normcase(item["path"].replace("\\", "/")))
        if item["initialized"]:
            _validate_source(item.get("source"))
        elif "source" in item:
            raise SchemaError("uninitialized submodules cannot have an inspected source record")
    if len(submodule_paths) != len(set(submodule_paths)):
        raise SchemaError("source.submodules contains duplicate paths")
    for key in ("fingerprint_sha256", "index_diff_sha256", "worktree_diff_sha256"):
        if key in source:
            _hash(source[key], f"source.{key}")


def validate_collectors_policy(value):
    if value is None:
        return value
    _object(value, "collectors")
    allowed = {"enabled", "kind", "wpr", "profile", "max_duration_seconds", "max_trace_bytes", "command_timeout_seconds", "export"}
    if set(value) - allowed:
        raise SchemaError("unsupported collector policy fields")
    _boolean(value.get("enabled"), "collectors.enabled")
    if not value["enabled"]:
        if set(value) != {"enabled"}:
            raise SchemaError("disabled collectors cannot carry executable settings")
        return value
    _choice(value.get("kind"), "collectors.kind", ("wpr_cpu",))
    _choice(value.get("profile", "CPU"), "collectors.profile", ("CPU",))
    _absolute_path(value.get("wpr"), "collectors.wpr")
    if PureWindowsPath(value["wpr"]).name.casefold() != "wpr.exe":
        raise SchemaError("collector path must identify wpr.exe")
    for name, default, minimum, maximum in (("max_duration_seconds", 120, 1, 3600),
            ("max_trace_bytes", 512 * 1024 ** 2, 1024 ** 2, 8 * 1024 ** 3), ("command_timeout_seconds", 30, 1, 120)):
        count = _integer(value.get(name, default), f"collectors.{name}", minimum)
        if count > maximum:
            raise SchemaError(f"collectors.{name} exceeds its bound")
    export = value.get("export")
    if export is not None:
        _object(export, "collectors.export")
        if set(export) - {"tool", "profile", "table_file", "mapping"}:
            raise SchemaError("unsupported collector export fields")
        for name in ("tool", "profile"):
            _absolute_path(export.get(name), f"collectors.export.{name}")
        if (PureWindowsPath(export["tool"]).name.casefold() != "wpaexporter.exe"
                or PureWindowsPath(export["profile"]).suffix.casefold() != ".wpaprofile"):
            raise SchemaError("collector export requires explicit WPAExporter and saved WPA profile paths")
        table = _string(export.get("table_file"), "collectors.export.table_file")
        if Path(table).name != table or any(token in table for token in ("/", "\\", ":")) or not table.casefold().endswith(".csv"):
            raise SchemaError("collector export table must be one CSV basename")
        if export.get("mapping") is not None:
            _absolute_path(export["mapping"], "collectors.export.mapping")
    return value


def validate_manifest(value, run_dir=None):
    _header(value, MANIFEST_TYPE)
    _uuid(value.get("run_id"), "run_id")
    created = _timestamp(value.get("created_at"), "created_at")
    for key in ("started_at", "ended_at", "exit_code"):
        if key not in value:
            raise SchemaError(f"{key} is required, using null when unknown")
    started = _timestamp(value["started_at"], "started_at", nullable=True)
    ended = _timestamp(value["ended_at"], "ended_at", nullable=True)
    state = _choice(value.get("state"), "state", ("planned", "running", "completed", "failed", "interrupted"))
    exit_code = value["exit_code"]
    if exit_code is not None:
        _integer(exit_code, "exit_code")
    if (started is not None and started < created) or (ended is not None and ended < (started or created)):
        raise SchemaError("lifecycle timestamps must be chronological")
    if state == "planned" and any(item is not None for item in (started, ended, exit_code)):
        raise SchemaError("planned runs cannot have execution timestamps or an exit code")
    if state == "running" and (started is None or ended is not None or exit_code is not None):
        raise SchemaError("running runs require started_at and no end or exit code")
    if state in ("completed", "failed", "interrupted") and ended is None:
        raise SchemaError("terminal runs require ended_at")
    if state == "completed" and (started is None or exit_code != 0):
        raise SchemaError("completed runs require started_at and exit_code 0")
    _validate_source(value.get("source"))
    build = _object(value.get("build"), "build")
    if type(build.get("compiler")) not in (str, dict):
        raise SchemaError("build.compiler must be a nonempty string or object")
    _metadata(build.get("compiler"), "build.compiler")
    _string(build.get("build_type"), "build.build_type")
    _object(build.get("options"), "build.options")
    _choice(build.get("provenance"), "build.provenance", ("declared_configuration",))
    _choice(build.get("source_binary_binding"), "build.source_binary_binding", ("unverified",))
    if build.get("receipt_sha256") is not None:
        _hash(build["receipt_sha256"], "build.receipt_sha256")
    runtime = _object(value.get("runtime"), "runtime")
    _choice(runtime.get("inventory_kind"), "runtime.inventory_kind", ("declared_files",))
    if runtime.get("loaded_modules_verified") is not False:
        raise SchemaError("declared_files inventory cannot verify loaded modules")
    runtime_paths = []
    executables = 0
    for item in _array(runtime.get("files"), "runtime.files"):
        _object(item, "runtime.files entry")
        role = _choice(item.get("role"), "runtime.files.role", ("executable", "library"))
        executables += role == "executable"
        _absolute_path(item.get("path"), "runtime.files.path")
        _hash(item.get("sha256"), "runtime.files.sha256")
        _integer(item.get("size"), "runtime.files.size", 0)
        runtime_paths.append(_path_identity(item["path"]))
    if executables != 1 or len(runtime_paths) != len(set(runtime_paths)):
        raise SchemaError("runtime.files requires one executable and no duplicate paths")
    machine = _object(value.get("machine"), "machine")
    for key in ("id", "cpu", "gpu", "driver", "os"):
        _string(machine.get(key), f"machine.{key}")
    _choice(machine.get("provenance"), "machine.provenance", ("declared_configuration",))
    title = _object(value.get("title"), "title")
    for key in ("id", "version"):
        _string(title.get(key), f"title.{key}")
    scene = _object(value.get("scene"), "scene")
    _string(scene.get("id"), "scene.id")
    _object(scene.get("recipe"), "scene.recipe")
    _hash(scene.get("recipe_sha256"), "scene.recipe_sha256")
    if scene["recipe_sha256"] != canonical_sha256(scene["recipe"]):
        raise SchemaError("scene.recipe_sha256 does not match canonical recipe")
    settings = _object(value.get("settings"), "settings")
    for key in ("resolution", "presentation"):
        _metadata(settings.get(key), f"settings.{key}")
    validate_cache(settings.get("cache"))
    diagnostics = _object(settings.get("diagnostics"), "settings.diagnostics")
    if "collectors" in value:
        validate_collectors_policy(value["collectors"])
    _choice(diagnostics.get("mode"), "settings.diagnostics.mode", ("measurement", "attribution", "forensic"))
    _object(diagnostics.get("options"), "settings.diagnostics.options")
    if "report_detail" in diagnostics["options"]:
        _choice(diagnostics["options"]["report_detail"], "settings.diagnostics.options.report_detail", ("compact", "full"))
    relinker = _object(settings.get("relinker"), "settings.relinker")
    _strings(relinker.get("flags"), "settings.relinker.flags")
    environment = _object(value.get("environment"), "environment")
    effective = _object(environment.get("effective"), "environment.effective")
    names = []
    for key, item in effective.items():
        _string(key, "environment name")
        if not key.upper().startswith(ENVIRONMENT_PREFIXES) or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key) is None:
            raise SchemaError("environment.effective may contain only relevant variable names")
        if type(item) is not str or "\x00" in item:
            raise SchemaError("environment values must be strings without NUL")
        names.append(key.casefold())
    if len(names) != len(set(names)):
        raise SchemaError("environment names must be unique ignoring case")
    normalized_environment = {name.upper(): item for name, item in effective.items()}
    workload = normalized_environment.get("APS5_TRACE_WORKLOAD")
    if workload not in (None, "0", "1"):
        raise SchemaError("APS5_TRACE_WORKLOAD must be 0 or 1")
    if workload == "1" and (value.get("capture", {}).get("unit") != "frames"
            or normalized_environment.get("APS5_CAPTURE_RUN_ID") != value["run_id"]):
        raise SchemaError("measurement workload requires a frame window and bound run UUID")
    detail = normalized_environment.get("APS5_TRACE_DETAIL")
    configured_detail = diagnostics["options"].get("trace_detail")
    if detail not in (None, "draw") or configured_detail not in (None, "draw"):
        raise SchemaError("only opt-in draw trace detail is supported")
    if configured_detail == "draw" and detail != "draw":
        raise SchemaError("configured draw detail must be present in the effective environment")
    if detail == "draw" and normalized_environment.get("APS5_CAPTURE_RUN_ID") != value["run_id"]:
        raise SchemaError("draw detail requires APS5_CAPTURE_RUN_ID bound to the manifest run UUID")
    if "APS5_CAPTURE_RUN_ID" in normalized_environment and normalized_environment["APS5_CAPTURE_RUN_ID"] != value["run_id"]:
        raise SchemaError("APS5_CAPTURE_RUN_ID must match the manifest run UUID")
    removed = _strings(environment.get("removed_inherited"), "environment.removed_inherited")
    if any(not key.upper().startswith(ENVIRONMENT_PREFIXES)
           or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key) is None for key in removed):
        raise SchemaError("removed_inherited may contain only relevant variable names")
    validate_capture(value.get("capture"))
    artifact_paths = []
    for item in _array(value.get("artifacts"), "artifacts"):
        validate_artifact(item, run_dir)
        artifact_paths.append(item["path"].replace("\\", "/").casefold())
    if len(artifact_paths) != len(set(artifact_paths)):
        raise SchemaError("artifacts contains duplicate paths")
    execution = _object(value.get("execution"), "execution")
    command = _array(execution.get("command"), "execution.command")
    if not command:
        raise SchemaError("execution.command must not be empty")
    for argument in command:
        if type(argument) is not str or "\x00" in argument:
            raise SchemaError("execution.command arguments must be strings without NUL")
    _absolute_path(command[0], "execution.command[0]")
    executable = next(item["path"] for item in runtime["files"] if item["role"] == "executable")
    if _path_identity(command[0]) != _path_identity(executable):
        raise SchemaError("execution.command[0] must identify the declared runtime executable")
    _absolute_path(execution.get("cwd"), "execution.cwd")
    if execution.get("pid") is not None:
        _integer(execution["pid"], "execution.pid", 1)
    for error_key in ("observation_errors", "collector_errors"):
        for error in _array(execution.get(error_key, []), f"execution.{error_key}"):
            _object(error, "execution.observation_errors entry")
            for name in ("phase", "type"):
                _string(error.get(name), f"execution.observation_errors.{name}")
            if type(error.get("message")) is not str:
                raise SchemaError("execution.observation_errors.message must be a string")
    return value


def validate_health(value):
    _header(value, HEALTH_TYPE)
    _uuid(value.get("run_id"), "run_id")
    statuses = ("valid", "invalid", "unknown")
    status = _choice(value.get("status"), "status", statuses)
    frames = _object(value.get("frame_metrics"), "frame_metrics")
    frame_status = _choice(frames.get("status"), "frame_metrics.status", statuses)
    metrics = _object(frames.get("metrics"), "frame_metrics.metrics")
    if frame_status == "valid" and not metrics:
        raise SchemaError("valid frame metrics must contain measurements")
    for key, item in metrics.items():
        _string(key, "frame metric name")
        if item is not None:
            number(item, f"frame_metrics.metrics.{key}", 0)
    if "presents" in frames and frames["presents"] is not None:
        _integer(frames["presents"], "frame_metrics.presents", 1 if frame_status == "valid" else 0)
    if frames.get("duration_ms") is not None:
        number(frames["duration_ms"], "frame_metrics.duration_ms", 0)
    attribution = _object(value.get("detailed_attribution"), "detailed_attribution")
    attribution_status = _choice(attribution.get("status"), "detailed_attribution.status", statuses)
    _strings(attribution.get("limitations"), "detailed_attribution.limitations")
    provenance = _object(value.get("provenance"), "provenance")
    provenance_status = _choice(provenance.get("status"), "provenance.status", statuses)
    if provenance.get("loaded_modules_verified") is not False:
        raise SchemaError("v1 health cannot verify observed loaded modules")
    _choice(provenance.get("source_binary_binding"), "provenance.source_binary_binding", ("unverified",))
    errors = False
    for issue in _array(value.get("issues"), "issues"):
        _object(issue, "issue")
        _string(issue.get("code"), "issue.code")
        severity = _choice(issue.get("severity"), "issue.severity", ("error", "warning", "info"))
        _string(issue.get("message"), "issue.message")
        errors |= severity == "error"
    if status == "valid" and (errors or frame_status != "valid"):
        raise SchemaError("valid health requires valid frame metrics and no error issues")
    if status != "invalid" and (errors or "invalid" in (frame_status, attribution_status, provenance_status)):
        raise SchemaError("health with error issues or invalid dimensions must have invalid status")
    if "draw_attribution" in value:
        validate_draw_attribution(value["draw_attribution"])
    if "measurement_workload" in value:
        workload = _object(value["measurement_workload"], "measurement_workload")
        _choice(workload.get("status"), "measurement_workload.status", statuses)
        if workload.get("run_id") != value["run_id"]:
            raise SchemaError("measurement workload run differs from health")
        _boolean(workload.get("correlation_valid"), "measurement_workload.correlation_valid")
        for name, metric in _object(workload.get("metrics"), "measurement_workload.metrics").items():
            draw_uint(metric, f"measurement_workload.metrics.{name}")
            if workload.get("metric_units", {}).get(name) != "count" or workload["status"] != "valid":
                raise SchemaError("measurement workload metrics require valid count evidence")
    if "evidence" in value:
        validate_evidence_report(value["evidence"])
        if value["evidence"]["run_id"] != value["run_id"]:
            raise SchemaError("evidence report run identity differs from capture health")
        if value["evidence"]["status"] == "invalid" and status != "invalid":
            raise SchemaError("invalid evidence requires invalid capture health")
    return value


def validate_comparison(value):
    _header(value, COMPARISON_TYPE)
    _choice(value.get("status"), "status", ("invalid", "incomparable", "insufficient", "comparable"))
    _strings(value.get("reasons"), "reasons")
    _strings(value.get("limitations"), "limitations")
    for key in ("baseline", "candidate", "metrics"):
        _object(value.get(key), key)
    return value


def draw_id(value, name, *, nullable=False, zero=False):
    if value is None and nullable:
        return None
    if (type(value) is not str or len(value) > 20 or re.fullmatch(r"0|[1-9][0-9]*", value) is None
            or int(value) > 2 ** 64 - 1 or not zero and value == "0"):
        raise SchemaError(f"{name} must be a canonical uint64 decimal string")
    return value


def draw_uint(value, name, *, nullable=False):
    if value is None and nullable:
        return None
    _integer(value, name, 0)
    if value > 2 ** 64 - 1:
        raise SchemaError(f"{name} must fit uint64_t")
    return value


def draw_ns(value, name, *, nullable=False):
    if value is None and nullable:
        return None
    _integer(value, name)
    if not -(2 ** 63) <= value <= 2 ** 63 - 1:
        raise SchemaError(f"{name} must fit signed int64_t nanoseconds")
    return value


def _validate_draw_clock(clock):
    _object(clock, "draw.clock")
    _choice(clock.get("domain"), "draw.clock.domain", ("steady_clock",))
    _choice(clock.get("time_unit"), "draw.clock.time_unit", ("ns",))
    draw_id(clock.get("origin_steady_ns"), "draw.clock.origin_steady_ns", zero=True)
    anchor = _object(clock.get("native_anchor"), "draw.clock.native_anchor")
    domain = _choice(anchor.get("domain"), "draw.clock.native_anchor.domain",
                     ("query_performance_counter", "clock_monotonic", "unknown"))
    status = _choice(anchor.get("status"), "draw.clock.native_anchor.status", ("available", "unavailable"))
    before = draw_ns(anchor.get("steady_before_ns"), "draw.clock.native_anchor.steady_before_ns")
    after = draw_ns(anchor.get("steady_after_ns"), "draw.clock.native_anchor.steady_after_ns")
    if before > after:
        raise SchemaError("native clock sampling bracket is reversed")
    for name in ("ticks", "frequency"):
        draw_id(anchor.get(name), f"draw.clock.native_anchor.{name}", nullable=status == "unavailable", zero=name == "ticks")
    if status == "available" and domain == "unknown":
        raise SchemaError("available native clock anchor requires a known clock domain")
    if status == "unavailable" and any(anchor.get(name) is not None for name in ("ticks", "frequency")):
        raise SchemaError("unavailable native clock anchor cannot contain measured ticks")


def _validate_draw_extended_data(event):
    data = event["data"]
    if data.get("present_ordinal_status") is not None or data.get("present_ordinal") is not None:
        status = _choice(data.get("present_ordinal_status"), "draw.present_ordinal_status", ("observed", "unavailable"))
        if event["kind"] != "present":
            raise SchemaError("present ordinal requires a present record")
        draw_id(data.get("present_ordinal"), "draw.present_ordinal", nullable=status == "unavailable", zero=True)
        if status == "observed" and int(data["present_ordinal"]) != event["value"]:
            raise SchemaError("present ordinal contradicts its recorded value")
        if status == "unavailable" and (data.get("present_ordinal") is not None or event["value"] != 0):
            raise SchemaError("unavailable present ordinal cannot report a measured value")
    if data.get("completion_domain") is not None:
        domain = _choice(data["completion_domain"], "draw.completion_domain", (
            "gpu_fence_observed", "native_queue_submit_returned", "native_submit_or_fence_failure", "cancelled_before_completion"))
        expected = {"complete": "gpu_fence_observed", "unknown": "native_queue_submit_returned",
                    "failed": "native_submit_or_fence_failure"}.get(event["status"], "cancelled_before_completion")
        if event["kind"] != "batch" or event["name"] != "batch_submission" or domain != expected:
            raise SchemaError("batch completion domain contradicts its recorded lifecycle")
    if event["kind"] == "calibration":
        period = data.get("period_ns")
        if period is not None:
            number(period, "draw.calibration.period_ns", 0)
        elif event["status"] != "failed":
            raise SchemaError("only failed calibration may omit its timestamp period")
        bits = draw_uint(data.get("valid_bits"), "draw.calibration.valid_bits")
        if bits > 64 or data.get("end_gpu_ns") is not None:
            raise SchemaError("calibration requires at most 64 timestamp bits and no interval end")
        if data.get("deviation_ns") is not None:
            number(data["deviation_ns"], "draw.calibration.deviation_ns", 0)
        if event["status"] == "complete" and (data.get("begin_gpu_ns") is None
                or data.get("deviation_ns") is None or period is None or period <= 0 or bits == 0):
            raise SchemaError("complete calibration requires a GPU timestamp, uncertainty, period and valid bits")
    cpu_fields = ("thread_cpu_begin_ticks", "thread_cpu_end_ticks", "thread_cpu_frequency", "thread_cpu_native_id")
    if "thread_cpu_status" in data or any(data.get(name) is not None for name in cpu_fields + ("thread_cpu_ns",)):
        status = _choice(data.get("thread_cpu_status"), "draw.thread_cpu_status",
                         ("not_requested", "measured", "unavailable", "failed", "nonmonotonic", "thread_changed", "clipped"))
        for name in cpu_fields:
            draw_id(data.get(name), f"draw.{name}", nullable=status != "measured", zero=name.endswith("_ticks"))
        if status == "measured":
            if event["kind"] != "span" or event["end_ns"] is None:
                raise SchemaError("thread CPU measurement requires a host span with both endpoints")
            measured = number(data.get("thread_cpu_ns"), "draw.thread_cpu_ns", 0)
            ticks = int(data["thread_cpu_end_ticks"]) - int(data["thread_cpu_begin_ticks"])
            if ticks < 0:
                raise SchemaError("measured thread CPU ticks are nonmonotonic")
            expected = ticks * 1_000_000_000 / int(data["thread_cpu_frequency"])
            if not math.isclose(measured, expected, rel_tol=1e-12, abs_tol=0.001):
                raise SchemaError("thread CPU duration contradicts its raw tick delta")
        elif any(data.get(name) is not None for name in cpu_fields + ("thread_cpu_ns",)):
            raise SchemaError("unavailable thread CPU time cannot contain measured values")
    cycle_fields = ("thread_cycles_begin", "thread_cycles_end", "thread_cycles_delta", "thread_cycles_native_id")
    if "thread_cycles_status" in data or any(data.get(name) is not None for name in cycle_fields):
        status = _choice(data.get("thread_cycles_status"), "draw.thread_cycles_status",
            ("not_requested", "measured", "unavailable", "failed", "nonmonotonic", "thread_changed", "clipped"))
        if event["kind"] != "span":
            raise SchemaError("thread cycle observations require a host span")
        if status == "measured":
            for name in cycle_fields:
                draw_id(data.get(name), f"draw.{name}", zero=name != "thread_cycles_native_id")
            if (event["end_ns"] is None or int(data["thread_cycles_end"]) < int(data["thread_cycles_begin"])
                    or int(data["thread_cycles_delta"]) != int(data["thread_cycles_end"]) - int(data["thread_cycles_begin"])
                    or data.get("thread_cycles_unit") != "cycles" or data.get("thread_cycles_source") != "query_thread_cycle_time"):
                raise SchemaError("thread cycles contradict their raw delta, source or unit")
        elif any(data.get(name) is not None for name in cycle_fields):
            raise SchemaError("unavailable thread cycles cannot contain measured counters")
    calibration_fields = ("calibration_gpu_ticks", "calibration_host_ticks",
                          "calibration_conversion_host_ticks", "calibration_host_frequency",
                          "calibration_steady_before_ns", "calibration_steady_after_ns")
    if any(data.get(name) is not None for name in calibration_fields):
        if event["kind"] != "calibration" or event["status"] != "complete":
            raise SchemaError("raw calibration anchors require a completed calibration record")
        for name in calibration_fields[:4]:
            draw_id(data.get(name), f"draw.{name}", zero=name != "calibration_host_frequency")
        _string(data.get("calibration_host_domain"), "draw.calibration_host_domain")
        before = draw_ns(data.get("calibration_steady_before_ns"), "draw.calibration_steady_before_ns")
        after = draw_ns(data.get("calibration_steady_after_ns"), "draw.calibration_steady_after_ns")
        if before > after:
            raise SchemaError("raw calibration steady-clock bracket is reversed")
    if event["kind"] == "counter":
        availability = _choice(data.get("availability"), "draw.counter.availability",
                               ("observed", "pending", "available", "unsupported", "allocation_failed", "capacity_dropped",
                                "read_failed", "invalid_timestamp", "uncollected"))
        available = availability in ("observed", "available")
        draw_id(data.get("counter_value"), "draw.counter.counter_value", nullable=not available, zero=True)
        _string(event.get("name"), "draw.counter.name")
        _string(event.get("text"), "draw.counter.unit")
        if available:
            if event["status"] != "complete" or int(data["counter_value"]) != event["value"]:
                raise SchemaError("available counter contradicts its lifecycle or value")
        elif data.get("counter_value") is not None or event["status"] != "unknown" or event["value"] != 0:
            raise SchemaError("unavailable counter cannot report a measured value")


def validate_draw_trace(value):
    _header(value, DRAW_TRACE_TYPE)
    _uuid(value.get("run_id"), "draw.run_id")
    _uuid(value.get("capture_id"), "draw.capture_id")
    if value["run_id"] == value["capture_id"]:
        raise SchemaError("draw capture_id must differ from run_id")
    producer = _object(value.get("producer"), "draw.producer")
    _integer(producer.get("pid"), "draw.producer.pid", 1)
    _string(producer.get("revision"), "draw.producer.revision")
    if "dirty" not in producer or producer["dirty"] is not None and type(producer["dirty"]) is not bool:
        raise SchemaError("draw.producer.dirty must be a boolean or null")
    _choice(producer.get("coverage"), "draw.producer.coverage", ("selected instrumented paths only",))
    if "clock" in value:
        _validate_draw_clock(value["clock"])
    capture = _object(value.get("capture"), "draw.capture")
    validate_capture(capture)
    _choice(capture.get("state"), "draw.capture.state", ("complete", "partial", "failed"))
    for name in ("open_present", "close_present"):
        draw_id(capture.get(name), f"draw.capture.{name}", nullable=True, zero=True)
    for name in ("open_ns", "close_ns"):
        draw_ns(capture.get(name), f"draw.capture.{name}", nullable=True)
    if capture.get("trace") is not None:
        _string(capture["trace"], "draw.capture.trace")
        if Path(capture["trace"]).name != capture["trace"] or "/" in capture["trace"] or "\\" in capture["trace"]:
            raise SchemaError("draw.capture.trace must identify a trace basename")
    for name in ("drain_deadline_ms", "pending_spans", "pending_jobs", "pending_gpu_queries"):
        draw_uint(capture.get(name), f"draw.capture.{name}")
    _string(capture.get("close_reason"), "draw.capture.close_reason")
    for boundary in _array(value.get("boundaries"), "draw.boundaries"):
        _object(boundary, "draw.boundary")
        draw_id(boundary.get("present"), "draw.boundary.present", zero=True)
        draw_ns(boundary.get("ts_ns"), "draw.boundary.ts_ns")
    for thread in _array(value.get("threads"), "draw.threads"):
        _object(thread, "draw.thread")
        draw_id(thread.get("thread_id"), "draw.thread.thread_id")
        _string(thread.get("name"), "draw.thread.name")
        for name in ("attempted", "emitted", "lost"):
            draw_uint(thread.get(name), f"draw.thread.{name}")
        for name in ("registered_ns", "last_event_ns"):
            draw_ns(thread.get(name), f"draw.thread.{name}")
        _boolean(thread.get("acknowledged"), "draw.thread.acknowledged")
        if "native_thread_id" in thread or "native_thread_id_source" in thread:
            source = _choice(thread.get("native_thread_id_source"), "draw.thread.native_thread_id_source",
                             ("windows_thread_id", "linux_gettid", "unavailable"))
            draw_id(thread.get("native_thread_id"), "draw.thread.native_thread_id", nullable=source == "unavailable")
            if source == "unavailable" and thread.get("native_thread_id") is not None:
                raise SchemaError("unavailable native thread identity cannot have a native ID")
    for entry in _array(value.get("dictionaries"), "draw.dictionaries"):
        _object(entry, "draw.dictionary")
        draw_id(entry.get("id"), "draw.dictionary.id")
        _string(entry.get("kind"), "draw.dictionary.kind")
        _string(entry.get("key"), "draw.dictionary.key")
        fields = _object(entry.get("fields"), "draw.dictionary.fields")
        for name, item in fields.items():
            _string(name, "draw.dictionary field name")
            if type(item) is not str or "\x00" in item:
                raise SchemaError("draw.dictionary fields must map names to strings without NUL")
        for reference in _array(entry.get("references"), "draw.dictionary.references"):
            draw_id(reference, "draw.dictionary.reference")
    lifecycle_statuses = ("unknown", "complete", "skipped", "rejected", "failed", "cancelled")
    query_statuses = ("pending", "available", "unsupported", "allocation_failed", "capacity_dropped", "read_failed",
                      "invalid_timestamp", "uncollected")
    for event in _array(value.get("events"), "draw.events"):
        _object(event, "draw.event")
        for name in ("id", "thread_id"):
            draw_id(event.get(name), f"draw.event.{name}")
        _choice(event.get("kind"), "draw.event.kind", ("submission", "segment", "packet", "operation", "host_command",
                "batch", "flip", "present", "link", "job", "span", "boundary", "calibration", "gpu_query", "attribute", "reference", "counter"))
        draw_ns(event.get("ts_ns"), "draw.event.ts_ns")
        draw_ns(event.get("end_ns"), "draw.event.end_ns", nullable=True)
        for name in ("subject", "related", "parent_span"):
            draw_id(event.get(name), f"draw.event.{name}", nullable=True)
        if event.get("name") is not None:
            _string(event["name"], "draw.event.name")
        if event.get("text") is not None and (type(event["text"]) is not str or "\x00" in event["text"]):
            raise SchemaError("draw.event.text must be a string without NUL or null")
        draw_uint(event.get("value"), "draw.event.value")
        _choice(event.get("status"), "draw.event.status", query_statuses if event["kind"] == "gpu_query" else lifecycle_statuses)
        context = _object(event.get("context"), "draw.event.context")
        for name in ("driver", "submission", "segment", "packet", "operation", "device", "batch", "job", "flip", "present"):
            draw_id(context.get(name), f"draw.event.context.{name}", nullable=True)
        for name in ("queue", "packet_offset", "opcode"):
            draw_uint(context.get(name), f"draw.event.context.{name}", nullable=name in ("queue", "opcode"))
        data = _object(event.get("data"), "draw.event.data")
        for name in ("begin_gpu_ns", "end_gpu_ns"):
            if data.get(name) is not None:
                number(data[name], f"draw.event.data.{name}", 0)
        for name in ("begin_gpu_ticks", "end_gpu_ticks"):
            draw_id(data.get(name), f"draw.event.data.{name}", nullable=True, zero=True)
        for name in ("period_ns", "duration_ns"):
            if data.get(name) is not None:
                number(data[name], f"draw.event.data.{name}", 0)
        origin = data.get("duration_origin")
        if origin is not None:
            _choice(origin, "draw.event.data.duration_origin", ("integer_tick_delta", "approximate_endpoint_difference"))
        if data.get("absolute_time_precision") is not None:
            _choice(data["absolute_time_precision"], "draw.event.data.absolute_time_precision", ("approximate_binary64",))
        if data.get("timestamp_valid_bits") is not None:
            bits = draw_uint(data["timestamp_valid_bits"], "draw.event.data.timestamp_valid_bits")
            if bits != 64:
                raise SchemaError("precise GPU query intervals currently require 64 timestamp bits")
        if data.get("timestamp_wraps") is not None:
            _boolean(data["timestamp_wraps"], "draw.event.data.timestamp_wraps")
        if origin is not None and (data.get("duration_ns") is None or data.get("absolute_time_precision") is None):
            raise SchemaError("GPU duration origin requires its duration and absolute-coordinate precision")
        if origin == "integer_tick_delta" and (data.get("begin_gpu_ticks") is None
                or data.get("end_gpu_ticks") is None or data.get("period_ns") is None or data["period_ns"] <= 0
                or data.get("timestamp_valid_bits") != 64 or data.get("timestamp_wraps") is None):
            raise SchemaError("integer GPU duration requires raw timestamp ticks, wrap evidence and a positive period")
        draw_uint(data.get("slot"), "draw.event.data.slot", nullable=True)
        if data.get("shared") is not None:
            _boolean(data["shared"], "draw.event.data.shared")
        if event["kind"] == "host_command":
            draw_uint(data.get("requested_count"), "draw.event.data.requested_count")
            _boolean(data.get("count_dynamic"), "draw.event.data.count_dynamic")
        _validate_draw_extended_data(event)
    losses = _object(value.get("losses"), "draw.losses")
    for name in ("events", "metadata", "gpu_queries", "unassigned_events"):
        draw_uint(losses.get(name), f"draw.losses.{name}")
    _strings(value.get("limitations"), "draw.limitations")
    completion = _object(value.get("completion"), "draw.completion")
    _choice(completion.get("state"), "draw.completion.state", ("complete", "partial", "failed"))
    draw_uint(completion.get("event_count"), "draw.completion.event_count")
    _boolean(completion.get("output_complete"), "draw.completion.output_complete")
    if "coverage" in value:
        coverage = _object(value["coverage"], "draw.coverage")
        for name in ("perfetto_view_included", "thread_cycles_requested"):
            if name in coverage:
                _boolean(coverage[name], f"draw.coverage.{name}")
        for name, expected in (("records_per_thread_scope", "per_thread"), ("maximum_events_scope", "all_threads")):
            if name in coverage:
                _choice(coverage[name], f"draw.coverage.{name}", (expected,))
        if coverage.get("thread_cycles_requested") is False and any(
                event["data"].get("thread_cycles_status", "not_requested") != "not_requested" for event in value["events"]):
            raise SchemaError("thread cycles were recorded without requested coverage")
        if coverage.get("thread_cycles_requested") is True and any(
                event["kind"] == "span" and event["data"].get("thread_cycles_status", "not_requested") == "not_requested"
                for event in value["events"]):
            raise SchemaError("requested thread cycles need an explicit availability on every span")
        if "maximum_boundaries" in coverage:
            maximum = draw_uint(coverage["maximum_boundaries"], "draw.coverage.maximum_boundaries")
            if maximum == 0 or len(value["boundaries"]) > maximum:
                raise SchemaError("draw boundary records exceed their declared capacity")
        if "maximum_events" in coverage:
            maximum = draw_uint(coverage["maximum_events"], "draw.coverage.maximum_events")
            if maximum == 0 or len(value["events"]) > maximum:
                raise SchemaError("draw event records exceed their declared capacity")
    return value


def validate_measurement_workload(value):
    _header(value, "anyps5.measurement-workload")
    _uuid(value.get("run_id"), "workload.run_id")
    producer = _object(value.get("producer"), "workload.producer")
    _integer(producer.get("pid"), "workload.producer.pid", 1)
    _string(producer.get("revision"), "workload.producer.revision")
    if "dirty" not in producer:
        raise SchemaError("workload producer requires explicit nullable dirty state")
    if producer.get("dirty") is not None:
        _boolean(producer["dirty"], "workload.producer.dirty")
    _string(value.get("trace"), "workload.trace")
    if Path(value["trace"]).name != value["trace"] or "/" in value["trace"] or "\\" in value["trace"]:
        raise SchemaError("workload trace must be a basename")
    clock = _object(value.get("clock"), "workload.clock")
    if clock != {"domain": "steady_clock", "unit": "ns", "origin": "implementation_defined_steady_epoch"}:
        raise SchemaError("workload clock metadata is unsupported")
    capture = _object(value.get("capture"), "workload.capture")
    _string(capture.get("spec"), "workload.capture.spec")
    for name in ("open_present", "close_present", "open_ns", "close_ns"):
        draw_uint(capture.get(name), f"workload.capture.{name}")
    coverage = _object(value.get("coverage"), "workload.coverage")
    for name in ("closed", "overflow", "lifetime_error"):
        _boolean(coverage.get(name), f"workload.coverage.{name}")
    draw_uint(coverage.get("lost_intervals"), "workload.coverage.lost_intervals")
    maximum = draw_uint(coverage.get("maximum_intervals"), "workload.coverage.maximum_intervals")
    if not 1 <= maximum <= 65_536:
        raise SchemaError("workload interval capacity is unsupported")

    def counts(row):
        _object(row, "workload counts")
        if set(row) != set(WORKLOAD_KEYS):
            raise SchemaError("workload requires every typed counter without extra names")
        for name, count in row.items():
            draw_uint(count, f"workload counts.{name}")

    def pending(row):
        if type(row) is not list or len(row) != 3:
            raise SchemaError("workload pending requires operation, command and job counts")
        for count in row:
            draw_uint(count, "workload pending count")

    counts(value.get("counts"))
    pending(coverage.get("pending_at_open"))
    pending(coverage.get("pending_at_close"))
    frames = _array(value.get("frames"), "workload.frames")
    if len(frames) > maximum:
        raise SchemaError("workload rows exceed declared capacity")
    totals = dict.fromkeys(WORKLOAD_KEYS, 0)
    previous = None
    for row in frames:
        _object(row, "workload frame")
        for name in ("present", "begin_ns", "end_ns"):
            draw_uint(row.get(name), f"workload.frame.{name}")
        if row["end_ns"] <= row["begin_ns"]:
            raise SchemaError("workload frame interval is not positive")
        counts(row.get("counts"))
        pending(row.get("pending_at_begin"))
        pending(row.get("pending_at_end"))
        if previous is not None and (row["present"] != previous["present"] + 1 or row["begin_ns"] != previous["end_ns"]
                or row["pending_at_begin"] != previous["pending_at_end"]):
            raise SchemaError("workload retained intervals are not contiguous")
        if not coverage["overflow"] and not coverage["lifetime_error"]:
            starts = [sum(row["counts"][key] for key in WORKLOAD_KEYS[:3]),
                      row["counts"]["host_draw_commands"] + row["counts"]["host_dispatch_commands"], row["counts"]["jobs_started"]]
            ends = [sum(row["counts"][key] for key in WORKLOAD_KEYS[5:10]),
                    row["counts"]["host_commands_completed"] + row["counts"]["host_commands_failed"],
                    row["counts"]["jobs_completed"] + row["counts"]["jobs_failed"]]
            if any(row["pending_at_begin"][index] + starts[index] - ends[index] != row["pending_at_end"][index] for index in range(3)):
                raise SchemaError("workload lifecycle counts contradict pending balances")
        for name in WORKLOAD_KEYS:
            totals[name] += row["counts"][name]
        previous = row
    if not coverage["lost_intervals"] and not coverage["overflow"] and totals != value["counts"]:
        raise SchemaError("workload totals differ from retained row sums")
    if frames and (frames[0]["present"] != capture["open_present"] or frames[0]["begin_ns"] != capture["open_ns"]
            or frames[0]["pending_at_begin"] != coverage["pending_at_open"]):
        raise SchemaError("workload first row contradicts opening markers")
    if coverage["closed"] and not coverage["lost_intervals"] and frames and (
            frames[-1]["present"] + 1 != capture["close_present"] or frames[-1]["end_ns"] != capture["close_ns"]
            or frames[-1]["pending_at_end"] != coverage["pending_at_close"]):
        raise SchemaError("workload last row contradicts closing markers")
    return value


def validate_draw_attribution(value):
    _object(value, "draw_attribution")
    statuses = ("valid", "invalid", "unknown")
    for name in ("status", "identity_status", "cpu_status", "gpu_status"):
        _choice(value.get(name), f"draw_attribution.{name}", statuses)
    for name in ("origin_graph_status", "gpu_association_status", "workload_status"):
        if name in value:
            _choice(value[name], f"draw_attribution.{name}", statuses)
    _uuid(value.get("run_id"), "draw_attribution.run_id")
    _uuid(value.get("capture_id"), "draw_attribution.capture_id")
    _boolean(value.get("correlation_valid"), "draw_attribution.correlation_valid")
    _strings(value.get("limitations"), "draw_attribution.limitations")
    for category, metrics in _object(value.get("metrics"), "draw_attribution.metrics").items():
        _string(category, "draw metric category")
        for name, metric in _object(metrics, f"draw_attribution.metrics.{category}").items():
            _string(name, "draw metric name")
            if metric is not None:
                number(metric, f"draw_attribution.metrics.{category}.{name}", 0)
    if "index" in value:
        index = _object(value["index"], "draw_attribution.index")
        if "event_source" in index:
            source = _object(index["event_source"], "draw_attribution.index.event_source")
            draw_uint(source.get("canonical_event_count"), "draw_attribution.index.event_source.canonical_event_count")
            if source.get("json_pointer_template") != "/events/{index}":
                raise SchemaError("draw event source requires canonical array pointers")
            _string(source.get("meaning"), "draw_attribution.index.event_source.meaning")
    if "observed_counts" in value:
        counts = _object(value["observed_counts"], "draw_attribution.observed_counts")
        _string(counts.get("meaning"), "draw_attribution.observed_counts.meaning")
        for name, count in _object(counts.get("values"), "draw_attribution.observed_counts.values").items():
            _string(name, "draw observed count name")
            draw_uint(count, "draw observed count")
    if "function_availability" in value:
        for name, status in _object(value["function_availability"], "draw_attribution.function_availability").items():
            _string(name, "draw function name")
            _choice(status, "draw function availability", statuses)
    return value


def validate_draw_report(value):
    _header(value, "anyps5.draw-report")
    _uuid(value.get("run_id"), "draw_report.run_id")
    _choice(value.get("status"), "draw_report.status", ("valid", "invalid", "unknown"))
    _strings(value.get("limitations"), "draw_report.limitations")
    if value.get("detail") is not None:
        validate_draw_attribution(value["detail"])
    return value


def validate_evidence_report(value):
    _header(value, "anyps5.evidence-report")
    _uuid(value.get("run_id"), "evidence.run_id")
    if value.get("capture_id") is not None:
        _uuid(value["capture_id"], "evidence.capture_id")
    statuses = ("valid", "unknown", "invalid")
    status = _choice(value.get("status"), "evidence.status", statuses)
    invalid_dimension = False
    for name, dimension in _object(value.get("dimensions"), "evidence.dimensions").items():
        _string(name, "evidence dimension name")
        _object(dimension, "evidence dimension")
        invalid_dimension |= _choice(dimension.get("status"), "evidence dimension status", statuses) == "invalid"
        _strings(dimension.get("reasons"), "evidence dimension reasons")
    errors = False
    for issue in _array(value.get("issues"), "evidence.issues"):
        _object(issue, "evidence issue")
        _string(issue.get("code"), "evidence issue code")
        _string(issue.get("message"), "evidence issue message")
        errors |= _choice(issue.get("severity"), "evidence issue severity", ("error", "warning", "info")) == "error"
    metrics = _object(value.get("metrics"), "evidence.metrics")
    availability = _object(value.get("metric_availability"), "evidence.metric_availability")
    for name, metric in metrics.items():
        _string(name, "evidence metric name")
        metric_status = _choice(availability.get(name), "evidence metric availability", statuses)
        if metric is None:
            if metric_status == "valid":
                raise SchemaError("available evidence metric requires a measurement")
        else:
            number(metric, "evidence metric")
    for name, metric_status in availability.items():
        _string(name, "evidence metric name")
        _choice(metric_status, "evidence metric availability", statuses)
        if metric_status == "valid" and name not in metrics:
            raise SchemaError("available evidence metric is absent")
    if "metric_units" in value:
        for name, unit in _object(value["metric_units"], "evidence.metric_units").items():
            _string(name, "evidence metric unit name")
            _string(unit, "evidence metric unit")
            if name not in metrics:
                raise SchemaError("metric unit identifies an absent evidence metric")
    if status != "invalid" and (errors or invalid_dimension):
        raise SchemaError("invalid evidence dimensions or errors require invalid report status")
    return value


def validate_document(value, run_dir=None):
    _object(value, "document")
    validators = {MANIFEST_TYPE: validate_manifest, HEALTH_TYPE: validate_health,
                  COMPARISON_TYPE: validate_comparison, DRAW_TRACE_TYPE: validate_draw_trace,
                  "anyps5.draw-report": validate_draw_report,
                  "anyps5.measurement-workload": validate_measurement_workload,
                  "anyps5.evidence-report": validate_evidence_report}
    document_type = value.get("document_type")
    if type(document_type) is not str or document_type not in validators:
        raise SchemaError("unknown document_type")
    if document_type == MANIFEST_TYPE:
        return validate_manifest(value, run_dir)
    return validators[document_type](value)
