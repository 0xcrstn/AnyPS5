import copy
import ctypes
import datetime
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import threading
import time
from uuid import UUID, uuid4

from capture_process import WindowsProcess, identity_file, observation_path
from capture_schema import SchemaError, atomic_write_json, contained_path, read_json


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def bounded_integer(value, name, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise SchemaError(f"{name} must be an integer in {minimum}..{maximum}")
    return value


def configured_path(value, check_files):
    if check_files:
        return observation_path(value)
    if type(value) is not str or not value or "\0" in value or not Path(value).is_absolute():
        raise SchemaError("collector paths must be explicit absolute paths")
    path = Path(os.path.abspath(value))
    if any(part.casefold() in ("dump", "fakelib") for part in path.parts):
        raise SchemaError("collector refuses dump/fakelib paths")
    return path


def validate_collectors_config(value, *, check_files=True):
    if value is None:
        return None
    if type(value) is not dict or set(value) - {"enabled", "kind", "wpr", "profile", "max_duration_seconds", "max_trace_bytes", "command_timeout_seconds", "export"}:
        raise SchemaError("unsupported collector configuration")
    if type(value.get("enabled")) is not bool:
        raise SchemaError("collectors.enabled must be explicit boolean")
    if not value["enabled"]:
        if set(value) != {"enabled"}:
            raise SchemaError("disabled collector must not carry executable settings")
        return {"enabled": False}
    if value.get("kind") != "wpr_cpu" or value.get("profile", "CPU") != "CPU":
        raise SchemaError("only the documented WPR CPU profile is supported")
    tool = configured_path(value.get("wpr", ""), check_files)
    if tool.name.casefold() != "wpr.exe" or check_files and not tool.is_file():
        raise SchemaError("collector requires an explicit existing wpr.exe")
    result = {"enabled": True, "kind": "wpr_cpu", "wpr": str(tool), "profile": "CPU"}
    for key, default, minimum, maximum in (("max_duration_seconds", 120, 1, 3600), ("max_trace_bytes", 512 * 1024**2, 1024**2, 8 * 1024**3), ("command_timeout_seconds", 30, 1, 120)):
        result[key] = bounded_integer(value.get(key, default), key, minimum, maximum)
    export = value.get("export")
    result["export"] = None
    if export is not None:
        if type(export) is not dict or set(export) - {"tool", "profile", "table_file", "mapping"}:
            raise SchemaError("unsupported collector export configuration")
        exporter = configured_path(export.get("tool", ""), check_files)
        profile = configured_path(export.get("profile", ""), check_files)
        table = export.get("table_file")
        if exporter.name.casefold() != "wpaexporter.exe" or profile.suffix.casefold() != ".wpaprofile" or check_files and (not exporter.is_file() or not profile.is_file() or profile.stat().st_size > 4 * 1024**2):
            raise SchemaError("export requires existing WPAExporter and an explicit saved WPA profile")
        if type(table) is not str or PurePosixPath(table).name != table or "\\" in table or not table.casefold().endswith(".csv") or table in (".", "..") or ":" in table:
            raise SchemaError("export table_file must be one explicit CSV basename")
        result["export"] = {"tool": str(exporter), "profile": str(profile), "table_file": table, "mapping": None}
        if export.get("mapping") is not None:
            mapping = configured_path(export["mapping"], check_files)
            if check_files:
                if not mapping.is_file() or mapping.stat().st_size > 1024**2:
                    raise SchemaError("export mapping must be an explicit bounded existing JSON file")
                from capture_tabular import validate_mapping
                validate_mapping(read_json(mapping))
            result["export"]["mapping"] = str(mapping)
    return result


def tool_identity(path):
    selected = observation_path(str(path))
    version = None
    if os.name == "nt":
        from ctypes import wintypes
        library = ctypes.WinDLL("version", use_last_error=True)
        library.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
        library.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
        library.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
        unused = wintypes.DWORD()
        size = library.GetFileVersionInfoSizeW(str(selected), ctypes.byref(unused))
        if 0 < size <= 4 * 1024**2:
            buffer = ctypes.create_string_buffer(size)
            if library.GetFileVersionInfoW(str(selected), 0, size, buffer):
                pointer, length = ctypes.c_void_p(), wintypes.UINT()
                if library.VerQueryValueW(buffer, "\\", ctypes.byref(pointer), ctypes.byref(length)) and length.value >= 52:
                    fields = (wintypes.DWORD * 13).from_address(pointer.value)
                    if fields[0] == 0xFEEF04BD:
                        version = ".".join(str(part) for part in (fields[2] >> 16, fields[2] & 65535, fields[3] >> 16, fields[3] & 65535))
    return {"path": str(selected), **identity_file(selected), "file_version": version, "version_source": "pe_version_resource" if version else "unavailable"}


def artifact(run_dir, path, kind):
    return {"kind": kind, "path": Path(path).relative_to(run_dir).as_posix(), **identity_file(path)}


def directory_bytes(directory):
    total = 0
    count = 0
    for base, folders, files in os.walk(directory, followlinks=False):
        observation_path(str(Path(base)))
        for name in folders + files:
            count += 1
            if count > 8192:
                raise OSError("collector output entry capacity exceeded")
            selected = observation_path(str(Path(base) / name))
            if selected.is_file():
                total += selected.stat().st_size
    return total


def run_command(argv, *, cwd, output_prefix, timeout_seconds=30, max_output_bytes=4 * 1024**2, max_generated_bytes=None):
    argv = list(argv)
    if not argv or any(type(value) is not str or "\0" in value for value in argv):
        raise SchemaError("collector command requires explicit argv strings")
    selected = tool_identity(argv[0])
    prefix = Path(output_prefix)
    stdout_path, stderr_path = Path(str(prefix) + ".stdout.txt"), Path(str(prefix) + ".stderr.txt")
    receipt = {"argv": argv, "tool": selected, "started_at": utc_now(), "ended_at": None, "returncode": None, "status": "unknown"}
    environment = dict(os.environ)
    for name in ("DEBUGINFOD_URLS", "_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH"):
        environment[name] = ""
    with stdout_path.open("xb", buffering=0) as stdout, stderr_path.open("xb", buffering=0) as stderr:
        child = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                 env=environment, shell=False, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        deadline = time.monotonic() + timeout_seconds
        try:
            while child.poll() is None:
                generated = directory_bytes(cwd) if max_generated_bytes is not None else 0
                if stdout_path.stat().st_size + stderr_path.stat().st_size > max_output_bytes or max_generated_bytes is not None and generated > max_generated_bytes:
                    receipt["status"] = "output_limit"
                    child.terminate()
                    break
                if time.monotonic() >= deadline:
                    receipt["status"] = "timeout"
                    child.terminate()
                    break
                time.sleep(0.05)
            try:
                receipt["returncode"] = child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                receipt["returncode"] = child.wait(timeout=5)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
    receipt.update(ended_at=utc_now(), stdout={"path": str(stdout_path), **identity_file(stdout_path)}, stderr={"path": str(stderr_path), **identity_file(stderr_path)})
    if tool_identity(argv[0]) != selected:
        receipt["status"] = "tool_changed"
    elif receipt["status"] == "unknown":
        receipt["status"] = "completed" if receipt["returncode"] == 0 else "failed"
    if stdout_path.stat().st_size + stderr_path.stat().st_size > max_output_bytes:
        receipt["status"] = "output_limit"
    return receipt


class WindowsSessions:
    def query(self):
        if os.name != "nt":
            raise OSError("ETW session inventory requires Windows")
        from ctypes import wintypes
        class Wnode(ctypes.Structure):
            _fields_ = [("size", wintypes.ULONG), ("provider", wintypes.ULONG), ("context", ctypes.c_ulonglong), ("timestamp", ctypes.c_longlong), ("guid", ctypes.c_ubyte * 16), ("clock", wintypes.ULONG), ("flags", wintypes.ULONG)]
        class Properties(ctypes.Structure):
            _fields_ = [("wnode", Wnode)] + [(name, wintypes.ULONG) for name in ("buffer_size", "minimum", "maximum", "max_file", "mode", "flush", "enable_flags", "age", "buffers", "free", "events_lost", "written", "log_buffers_lost", "realtime_buffers_lost")] + [("logger_thread", wintypes.HANDLE), ("file_offset", wintypes.ULONG), ("name_offset", wintypes.ULONG)]
        library = ctypes.WinDLL("advapi32", use_last_error=True)
        library.QueryAllTracesW.argtypes = [ctypes.POINTER(ctypes.POINTER(Properties)), wintypes.ULONG, ctypes.POINTER(wintypes.ULONG)]
        library.QueryAllTracesW.restype = wintypes.ULONG
        capacity = 64
        for unused in range(2):
            buffers = [ctypes.create_string_buffer(ctypes.sizeof(Properties) + 8192) for index in range(capacity)]
            pointers = (ctypes.POINTER(Properties) * capacity)()
            for index, buffer in enumerate(buffers):
                pointer = ctypes.cast(buffer, ctypes.POINTER(Properties))
                pointer.contents.wnode.size = len(buffer)
                pointer.contents.name_offset = ctypes.sizeof(Properties)
                pointer.contents.file_offset = ctypes.sizeof(Properties) + 4096
                pointers[index] = pointer
            count = wintypes.ULONG()
            status = library.QueryAllTracesW(pointers, capacity, ctypes.byref(count))
            if status == 234 and capacity < count.value <= 1024:
                capacity = count.value
                continue
            if status:
                raise ctypes.WinError(status)
            if count.value > capacity:
                raise OSError("ETW session inventory exceeded capacity")
            result = []
            for index in range(count.value):
                row = pointers[index].contents
                if row.name_offset + 2 > len(buffers[index]) or row.file_offset + 2 > len(buffers[index]):
                    raise OSError("ETW session inventory offsets invalid")
                base = ctypes.addressof(buffers[index])
                name = ctypes.wstring_at(base + row.name_offset, (len(buffers[index]) - row.name_offset) // 2).split("\0", 1)[0]
                filename = ctypes.wstring_at(base + row.file_offset, (len(buffers[index]) - row.file_offset) // 2).split("\0", 1)[0]
                result.append({"name": name, "session_id": str(row.wnode.context), "guid": str(UUID(bytes_le=bytes(row.wnode.guid))), "clock_type": row.wnode.clock,
                               "enable_flags": row.enable_flags, "file": filename, "events_lost": row.events_lost, "log_buffers_lost": row.log_buffers_lost, "realtime_buffers_lost": row.realtime_buffers_lost})
            return result
        raise OSError("ETW session inventory could not be bounded")


def session_identity(row):
    return row["name"], row["session_id"], row["guid"]


class CollectorSession:
    def __init__(self, run_dir, manifest, config, *, process, command_runner=None, session_backend=None):
        self.run_dir, self.manifest, self.process = Path(run_dir), manifest, process
        self.config = copy.deepcopy(config)
        self.command_runner = command_runner or run_command
        self.session_backend = session_backend or WindowsSessions()
        self.instance = "AnyPS5-" + str(uuid4())
        self.directory = self.run_dir / "collectors"
        self.backend = None
        self.owned = []
        self.before = None
        self.start_attempted = False
        self.started = False
        self.finished = False
        self.lock = threading.RLock()
        self.artifacts = []
        self.receipt = {"schema_version": 1, "document_type": "anyps5.collector-receipt", "run_id": manifest["run_id"], "instance_name": self.instance, "run_directory": str(self.run_dir.absolute()),
                        "process": None, "state": "planned", "status": "planned", "commands": [], "errors": [], "ownership": {"state": "unknown", "preexisting_sessions": [], "owned_sessions": []},
                        "coverage": {"state": "unknown", "scope": "system-wide WPR CPU recording; normalized rows must be filtered to retained child", "start_qpc": None, "end_qpc": None, "frequency": None,
                                     "lost_events": None, "full_lifetime": False, "bounds": "duration and file size checked at poll/command intervals; not kernel hard limits"},
                        "limitations": ["WPR may require existing administrator or Performance Log Users rights; no elevation is requested.", "Visible session queries do not enumerate private/inaccessible ETW sessions.",
                                        "A successful tool command and hashes do not authenticate its implementation or prove export completeness.", "No process-relative clock anchors, zero loss or debug symbols are inferred."]}

    def save(self):
        atomic_write_json(self.directory / "receipt.json", self.receipt)

    def command(self, arguments, *, cwd=None, generated_cap=None):
        prefix = self.directory / f"command-{len(self.receipt['commands']):04d}"
        try:
            result = self.command_runner(arguments, cwd=str(cwd or self.directory), output_prefix=prefix,
                                         timeout_seconds=self.config["command_timeout_seconds"], max_generated_bytes=generated_cap)
        except Exception as error:
            result = {"argv": list(arguments), "status": "failed", "returncode": None, "error": {"type": type(error).__name__, "message": str(error)}}
            for name in ("stdout", "stderr"):
                path = Path(str(prefix) + f".{name}.txt")
                if path.is_file():
                    result[name] = {"path": str(path), **identity_file(path)}
            for name in ("stdout", "stderr"):
                if name in result:
                    result[name]["path"] = Path(result[name]["path"]).relative_to(self.run_dir).as_posix()
            self.receipt["commands"].append(result)
            self.save()
            raise
        for name in ("stdout", "stderr"):
            if name in result:
                result[name]["path"] = Path(result[name]["path"]).relative_to(self.run_dir).as_posix()
        self.receipt["commands"].append(result)
        self.save()
        return result

    def discover_owned(self):
        if self.before is None:
            raise SchemaError("collector lacks pre-start session inventory")
        current = self.session_backend.query()
        old = {row["name"]: session_identity(row) for row in self.before}
        if any(row["name"] in old and session_identity(row) != old[row["name"]] for row in current):
            raise SchemaError("preexisting session identity changed during WPR start")
        previous = {row["name"]: row for row in self.before}
        configuration_fields = ("enable_flags", "file", "clock_type")
        if any(row["name"] in previous and any(row.get(key) != previous[row["name"]].get(key) for key in configuration_fields) for row in current):
            raise SchemaError("preexisting session configuration changed; refusing unrelated session control")
        owned = [row for row in current if self.instance.casefold() in row["name"].casefold() and row["name"] not in old]
        if owned:
            self.owned = owned
            self.receipt["ownership"].update(state="verified_unique_instance", owned_sessions=copy.deepcopy(owned))
        return owned

    def start(self):
        with self.lock:
            if self.started or self.finished:
                raise SchemaError("collector session cannot be restarted")
            self.directory.mkdir(exist_ok=False)
            (self.directory / "temporary").mkdir()
            self.started = True
            self.started_monotonic = time.monotonic()
            self.save()
            try:
                if self.process.poll() is not None:
                    raise SchemaError("collector refuses a child that has already exited")
                self.backend = WindowsProcess(self.process.pid, process_handle=int(self.process._handle))
                identity = self.backend.identity()
                expected = next(row for row in self.manifest["runtime"]["files"] if row["role"] == "executable")
                if identity["pid"] != self.manifest["execution"]["pid"] or os.path.normcase(identity["path"]) != os.path.normcase(expected["path"]) or identity_file(expected["path"]) != {key: expected[key] for key in ("size", "sha256")}:
                    raise SchemaError("collector retained-child identity differs from runtime")
                self.receipt["process"] = {"pid": identity["pid"], "creation_filetime": identity["creation_filetime"], "executable_sha256": expected["sha256"], "identity_binding": "retained_child_handle"}
                before = self.session_backend.query()
                self.before = copy.deepcopy(before)
                self.receipt["ownership"]["preexisting_sessions"] = before
                if any(self.instance.casefold() in row["name"].casefold() or row.get("enable_flags", 0) for row in before):
                    raise SchemaError("existing kernel/instance recording prevents isolated WPR start")
                self.start_attempted = True
                self.receipt["start_attempted"] = True
                self.save()
                result = self.command([self.config["wpr"], "-start", "CPU", "-filemode", "-recordtempto", str(self.directory / "temporary"), "-instancename", self.instance])
                self.discover_owned()
                if result["status"] != "completed" or not self.owned:
                    raise SchemaError("WPR start failed or unique-session ownership was not verified")
                self.receipt["coverage"].update(start_qpc=str(self.backend.ticks()), frequency=self.backend.frequency)
                self.receipt["coverage"]["qpc_scope"] = "observer after start command and before stop command; not native ETL first/last event bounds"
                self.receipt["state"] = "recording"
                self.receipt["status"] = "recording"
                self.save()
            except Exception as error:
                self.receipt["state"] = "failed"
                self.receipt["status"] = "failed"
                self.receipt["errors"].append({"phase": "start", "type": type(error).__name__, "message": str(error)})
                if self.start_attempted and not self.owned:
                    try:
                        self.discover_owned()
                    except Exception as discovery_error:
                        self.receipt["errors"].append({"phase": "start", "type": type(discovery_error).__name__, "message": "owned-session discovery failed: " + str(discovery_error)})
                self.save()
                raise

    def poll(self):
        with self.lock:
            if self.finished or self.receipt["state"] != "recording":
                return
            total = directory_bytes(self.directory / "temporary")
            if time.monotonic() - self.started_monotonic >= self.config["max_duration_seconds"] or total >= self.config["max_trace_bytes"]:
                self.receipt["stop_reason"] = "duration_limit" if time.monotonic() - self.started_monotonic >= self.config["max_duration_seconds"] else "trace_size_limit"
                self.finish()

    def export(self, raw):
        config = self.config["export"]
        if config is None:
            return
        profile = observation_path(config["profile"])
        profile_identity = identity_file(profile)
        from capture_process import copy_attachment
        archived_profile = self.directory / "export-profile.wpaProfile"
        if copy_attachment(profile, archived_profile) != profile_identity:
            raise SchemaError("WPA export profile changed while preserving")
        profile_descriptor = artifact(self.run_dir, archived_profile, "collector_profile")
        self.artifacts.append(profile_descriptor)
        destination = self.directory / "export"
        destination.mkdir()
        result = self.command([config["tool"], "-i", str(raw), "-profile", str(profile), "-outputfolder", str(destination)], cwd=destination, generated_cap=self.config["max_trace_bytes"])
        self.receipt["export_profile"] = {"path": str(profile), **profile_identity, "artifact": {key: profile_descriptor[key] for key in ("path", "size", "sha256")}}
        if result["status"] != "completed" or identity_file(profile) != profile_identity:
            raise SchemaError("WPA export failed or profile changed")
        table = contained_path(destination, config["table_file"], must_exist=True)
        if table.stat().st_size > min(self.config["max_trace_bytes"], (64 if config["mapping"] else 256) * 1024**2):
            raise SchemaError("selected WPA CSV exceeds bounded import size")
        self.artifacts.append(artifact(self.run_dir, table, "collector_export"))
        if config["mapping"]:
            from capture_tabular import normalize_exported_table
            mapping_path = observation_path(config["mapping"])
            mapping_identity = identity_file(mapping_path)
            mapping = read_json(mapping_path)
            source_identity = identity_file(table)
            text = table.read_bytes().decode("utf-8")
            process = self.receipt["process"]
            document = normalize_exported_table(text, mapping, run_id=self.manifest["run_id"], process=process,
                         source_artifact=table.relative_to(self.run_dir).as_posix(), source_sha256=source_identity["sha256"], source_size=source_identity["size"])
            if document["kind"] != "cpu_profile" or identity_file(table) != source_identity or identity_file(mapping_path) != mapping_identity:
                raise SchemaError("unsupported/changed WPA CPU table export")
            normalized = self.directory / "cpu-profile.json"
            mapping_copy = self.directory / "table-mapping.json"
            with mapping_copy.open("xb") as stream:
                stream.write(mapping_path.read_bytes())
            if identity_file(mapping_copy) != mapping_identity:
                raise SchemaError("mapping changed while preserving")
            document["mapping_artifact"] = {"path": mapping_copy.relative_to(self.run_dir).as_posix(), **mapping_identity}
            atomic_write_json(normalized, document, replace=False)
            self.artifacts.extend([artifact(self.run_dir, normalized, "cpu_profile"), artifact(self.run_dir, mapping_copy, "external_mapping")])
            self.receipt["normalization"] = {"state": "explicit_table_mapping", "clock_anchors": "unknown", "loss": "unknown", "raw_source": table.relative_to(self.run_dir).as_posix()}

    def finish(self):
        with self.lock:
            if self.finished:
                return self.receipt
            self.finished = True
            if not self.started:
                return self.receipt
            try:
                if self.start_attempted and not self.owned:
                    if not self.discover_owned():
                        self.receipt["ownership"]["state"] = "unresolved_start_outcome"
                        raise SchemaError("WPR start was attempted but no owned session can be positively identified; cleanup remains unresolved")
                if self.owned:
                    now = self.session_backend.query()
                    current = {session_identity(row): row for row in now}
                    if any(session_identity(row) not in current for row in self.owned):
                        raise SchemaError("owned ETW session identity changed; refusing any stop")
                    self.receipt["coverage"]["observed_pre_stop_events_lost"] = sum(current[session_identity(row)]["events_lost"] for row in self.owned)
                    self.receipt["coverage"]["end_qpc"] = str(self.backend.ticks())
                    self.receipt["coverage"]["loss_scope"] = "last pre-stop ETW query; final export loss remains unknown"
                    raw = self.directory / "cpu.etl"
                    result = self.command([self.config["wpr"], "-stop", str(raw), "AnyPS5 explicit capture", "-skipPdbGen", "-instancename", self.instance], generated_cap=self.config["max_trace_bytes"] * 3)
                    remaining = {session_identity(row) for row in self.session_backend.query()}
                    if result["status"] != "completed" or any(session_identity(row) in remaining for row in self.owned):
                        raise SchemaError("WPR stop did not verifiably end owned sessions")
                    self.receipt["ownership"]["state"] = "verified_stopped"
                    self.receipt["state"] = "stopped"
                    self.receipt["status"] = "stopped"
                    if not raw.is_file() or raw.stat().st_size > self.config["max_trace_bytes"]:
                        raise SchemaError("ETL missing or over declared final file budget")
                    self.artifacts.append(artifact(self.run_dir, raw, "raw_etw"))
                    self.export(raw)
            except Exception as error:
                self.receipt["state"] = "failed"
                self.receipt["status"] = "failed"
                self.receipt["errors"].append({"phase": "finish", "type": type(error).__name__, "message": str(error)})
                if self.owned:
                    self.receipt["ownership"]["state"] = "stop_not_verified"
            finally:
                if self.backend is not None:
                    try:
                        self.backend.close()
                    except Exception as error:
                        self.receipt["errors"].append({"phase": "finish", "type": type(error).__name__, "message": "retained-child handle cleanup failed: " + str(error)})
                self.receipt["finished_at"] = utc_now()
                if self.receipt["errors"]:
                    self.receipt["state"] = "failed"
                    self.receipt["status"] = "failed"
                self.receipt["artifacts"] = copy.deepcopy(self.artifacts)
                self.save()
                self.artifacts.append(artifact(self.run_dir, self.directory / "receipt.json", "collector_receipt"))
                for command in self.receipt["commands"]:
                    for name in ("stdout", "stderr"):
                        if name in command:
                            self.artifacts.append(artifact(self.run_dir, contained_path(self.run_dir, command[name]["path"], must_exist=True), "collector_log"))
            return self.receipt


def assess_collector_receipt(run_dir, manifest, *, verified_artifacts=None, process_evidence=None):
    result = {"run_id": manifest["run_id"], "status": "unknown", "tool_identity_status": "unknown", "issues": [], "tool_identities": [], "process": None, "coverage": {}, "artifact": None,
              "ownership": "unknown", "tool_identity_verification": "preserved_receipt_only_not_current_binary_rehash",
              "limitations": ["Recorded executable hashes are preserved identities, not tool authenticity or proof of execution.", "Current installed tools are not consulted for historical assessment.",
                              "Historical session ownership and final ETW completeness cannot be independently replayed from a receipt."]}
    try:
        if not (manifest.get("collectors") or {}).get("enabled"):
            return result
        candidates = [row for row in manifest.get("artifacts", []) if row["kind"] == "collector_receipt"]
        if not candidates:
            result["issues"].append({"severity": "warning", "code": "collector_receipt_missing", "message": "Requested collector receipt is unavailable."})
            return result
        if len(candidates) != 1:
            raise SchemaError("collector requires one preserved receipt")
        descriptor = candidates[0]
        inventory = {row["path"]: row for row in manifest["artifacts"]}
        run = Path(run_dir)
        def verify(value, maximum=None):
            path = value.get("path")
            if path not in inventory or verified_artifacts is not None and path not in verified_artifacts:
                raise SchemaError("collector referenced artifact is not in verified manifest inventory")
            item = inventory[path]
            if any(item[key] != value.get(key) for key in ("size", "sha256")) or maximum is not None and item["size"] > maximum:
                raise SchemaError("collector referenced descriptor differs or exceeds its bound")
            selected = contained_path(run, path, must_exist=True)
            if identity_file(selected) != {key: item[key] for key in ("size", "sha256")}:
                raise SchemaError("collector artifact hash differs from manifest")
            return selected
        path = verify(descriptor, 4 * 1024**2)
        receipt = read_json(path)
        if type(receipt) is not dict or receipt.get("schema_version") != 1 or receipt.get("document_type") != "anyps5.collector-receipt" or receipt.get("run_id") != manifest["run_id"]:
            raise SchemaError("collector receipt protocol/run binding invalid")
        result["artifact"] = descriptor
        if process_evidence is None:
            from capture_process import assess_process_observation
            usable = {key: contained_path(run, key, must_exist=True) for key, row in inventory.items() if row["kind"] in ("process_observation", "process_samples") and (verified_artifacts is None or key in verified_artifacts)}
            process_evidence = assess_process_observation(run, manifest, usable)
        actual = process_evidence.get("process") or {}
        declared = receipt.get("process") or {}
        if process_evidence.get("status") == "invalid" or process_evidence.get("run_id") != manifest["run_id"] or not actual.get("identity_verified") or actual.get("identity_binding") != "retained_child_handle":
            result["issues"].append({"severity": "warning", "code": "collector_process_binding_unavailable", "message": "Fresh retained-child process evidence is unavailable."})
            return result
        if declared.get("identity_binding") != "retained_child_handle" or any(declared.get(key) != actual.get(key) for key in ("pid", "creation_filetime")) or declared.get("executable_sha256") != actual["executable"]["sha256"]:
            raise SchemaError("collector process differs from freshly assessed child")
        instance = receipt.get("instance_name", "")
        if not instance.startswith("AnyPS5-") or str(UUID(instance[7:])) != instance[7:]:
            raise SchemaError("collector instance must be unique canonical UUID")
        config = manifest["collectors"]
        original_root = receipt.get("run_directory")
        original_trace = manifest.get("environment", {}).get("effective", {}).get("APS5_TRACE_DIR")
        if not original_root and original_trace:
            original_root = str(Path(original_trace).parent)
        if type(original_root) is not str or not Path(original_root).is_absolute():
            raise SchemaError("collector original run directory unavailable")
        original_root = Path(original_root)
        if original_trace and os.path.normcase(str(Path(original_trace).parent)) != os.path.normcase(str(original_root)):
            raise SchemaError("collector original run directory contradicts capture trace root")
        commands = receipt.get("commands")
        if type(commands) is not list or not 1 <= len(commands) <= 16:
            raise SchemaError("collector command inventory is missing or exceeds its bound")
        roles = []
        tools = {}
        for command in commands:
            argv = command.get("argv")
            tool = command.get("tool")
            if type(argv) is not list or not argv or any(type(value) is not str or "\0" in value for value in argv) or type(tool) is not dict:
                raise SchemaError("collector command/tool record invalid")
            if tool.get("path") != argv[0] or type(tool.get("size")) is not int or tool["size"] <= 0 or re.fullmatch(r"[0-9a-f]{64}", str(tool.get("sha256"))) is None:
                raise SchemaError("collector recorded tool identity invalid")
            if tool.get("file_version") is not None and (type(tool["file_version"]) is not str or not tool["file_version"].strip() or len(tool["file_version"]) > 256):
                raise SchemaError("collector version metadata invalid")
            if tool["path"] in tools and tools[tool["path"]] != tool:
                raise SchemaError("collector tool identity changed between command roles")
            tools[tool["path"]] = tool
            if argv == [config["wpr"], "-start", "CPU", "-filemode", "-recordtempto", str(original_root / "collectors" / "temporary"), "-instancename", instance]:
                role = "wpr_start"
            elif argv == [config["wpr"], "-stop", str(original_root / "collectors" / "cpu.etl"), "AnyPS5 explicit capture", "-skipPdbGen", "-instancename", instance]:
                role = "wpr_stop"
            elif config.get("export") and argv == [config["export"]["tool"], "-i", str(original_root / "collectors" / "cpu.etl"), "-profile", config["export"]["profile"], "-outputfolder", str(original_root / "collectors" / "export")]:
                role = "wpa_export"
            else:
                raise SchemaError("collector argv is not an owned explicit supported command role")
            roles.append(role)
            for name in ("stdout", "stderr"):
                if name not in command:
                    raise SchemaError("collector command output unavailable")
                verify(command[name], 16 * 1024**2)
            if command.get("status") != "completed" or command.get("returncode") != 0:
                raise SchemaError("collector command did not complete successfully")
            result["tool_identities"].append({"role": role, **{key: tool.get(key) for key in ("path", "size", "sha256", "file_version", "version_source")}})
        expected_roles = ["wpr_start", "wpr_stop", "wpa_export"] if config.get("export") else ["wpr_start", "wpr_stop"]
        if roles != expected_roles:
            raise SchemaError("collector command role sequence invalid")
        archived = {value["path"]: value for value in receipt.get("artifacts", [])}
        required = {"collectors/cpu.etl": "raw_etw"}
        if config.get("export"):
            required[f"collectors/export/{config['export']['table_file']}"] = "collector_export"
            required["collectors/export-profile.wpaProfile"] = "collector_profile"
            profile = receipt.get("export_profile") or {}
            profile_artifact = profile.get("artifact") or {}
            if profile.get("path") != config["export"]["profile"] or any(profile.get(key) != profile_artifact.get(key) for key in ("size", "sha256")):
                raise SchemaError("archived WPA profile does not bind recorded export configuration")
            verify(profile_artifact, 4 * 1024**2)
            if config["export"].get("mapping"):
                required.update({"collectors/table-mapping.json": "external_mapping", "collectors/cpu-profile.json": "cpu_profile"})
        for relative, kind in required.items():
            if relative not in archived or archived[relative].get("kind") != kind:
                raise SchemaError("requested collector output missing from preserved receipt")
        for value in archived.values():
            verify(value)
        if receipt.get("errors") or receipt.get("status") != "stopped" or receipt.get("ownership", {}).get("state") != "verified_stopped":
            raise SchemaError("collector receipt does not record a successful owned stop")
        ownership = receipt["ownership"]
        owned = ownership.get("owned_sessions")
        previous = ownership.get("preexisting_sessions")
        if type(owned) is not list or not 1 <= len(owned) <= 1024 or type(previous) is not list or len(previous) > 1024:
            raise SchemaError("collector recorded ownership inventory invalid")
        old_names = {row["name"] for row in previous}
        for row in owned:
            if instance.casefold() not in row["name"].casefold() or row["name"] in old_names or re.fullmatch(r"[1-9][0-9]*", row["session_id"]) is None or str(UUID(row["guid"])) != row["guid"]:
                raise SchemaError("collector recorded ownership contradicts unique new instance")
        coverage = receipt.get("coverage")
        if type(coverage) is not dict or coverage.get("full_lifetime") is not False:
            raise SchemaError("collector coverage must preserve bounded lifetime caveat")
        for key in ("start_qpc", "end_qpc"):
            if type(coverage.get(key)) is not str or re.fullmatch(r"0|[1-9][0-9]{0,19}", coverage[key]) is None or int(coverage[key]) >= 2**64:
                raise SchemaError("collector observed QPC bounds invalid")
        if int(coverage["end_qpc"]) < int(coverage["start_qpc"]) or type(coverage.get("frequency")) is not int or coverage["frequency"] <= 0:
            raise SchemaError("collector observed QPC frequency/order invalid")
        verify(descriptor, 4 * 1024**2)
        result.update(status="valid", tool_identity_status="valid", process=declared, coverage=receipt.get("coverage", {}), ownership="recorded_unique_instance_not_independently_replayed")
    except OSError as error:
        result["issues"].append({"severity": "warning", "code": "collector_inputs_unavailable", "message": str(error)})
    except (ValueError, TypeError, KeyError, AttributeError) as error:
        result.update(status="invalid", tool_identity_status="invalid")
        result["issues"].append({"severity": "error", "code": "collector_receipt_invalid", "message": str(error)})
    return result
