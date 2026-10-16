import argparse
import bisect
import copy
import ctypes
import datetime
import json
import os
import re
from pathlib import Path
import shutil
import stat
import subprocess
import time

from capture_schema import SchemaError, atomic_write_json, canonical_sha256, contained_path, read_json


PROFILE_KINDS = frozenset(("cpu_profile", "gpu_profile", "presentation_profile"))


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def observation_path(value):
    if not isinstance(value, str) or not value or "\0" in value or not Path(value).is_absolute():
        raise SchemaError("observation paths must be explicit absolute paths")
    path = Path(os.path.abspath(value))
    for candidate in (path, *path.parents):
        if any(part.casefold() in ("dump", "fakelib") for part in candidate.parts):
            raise SchemaError("observation refuses dump/fakelib access")
        if candidate.exists():
            info = candidate.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise SchemaError("observation refuses symbolic links and reparse points")
    return path


def identity_file(path):
    from capture_manifest import file_identity
    return file_identity(observation_path(str(path)))


def integer(value, name, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise SchemaError(f"{name} must be an integer in {minimum}..{maximum}")
    return value


def validate_observation_config(value):
    if value is None:
        return None
    if not isinstance(value, dict) or type(value.get("enabled")) is not bool:
        raise SchemaError("observation.enabled must be explicit boolean")
    result = copy.deepcopy(value)
    result["interval_ms"] = integer(result.get("interval_ms", 1000), "observation.interval_ms", 50, 60000)
    result["max_samples"] = integer(result.get("max_samples", 4096), "observation.max_samples", 1, 100000)
    result["max_module_hash_bytes"] = integer(result.get("max_module_hash_bytes", 512 * 1024 * 1024), "observation.max_module_hash_bytes", 1, 4 * 1024**3)
    result["max_total_hash_bytes"] = integer(result.get("max_total_hash_bytes", 2 * 1024**3), "observation.max_total_hash_bytes", 1, 16 * 1024**3)
    result["max_output_bytes"] = integer(result.get("max_output_bytes", 64 * 1024**2), "observation.max_output_bytes", 1024, 2 * 1024**3)
    for key in ("hash_roots", "effective_settings_files", "build_receipts", "symbol_receipts"):
        items = result.get(key, [])
        if not isinstance(items, list) or len(items) > 256:
            raise SchemaError(f"observation.{key} must be a bounded explicit path list")
        result[key] = [str(observation_path(item)) for item in items]
    caches = result.get("cache_locations", [])
    if not isinstance(caches, list) or len(caches) > 64:
        raise SchemaError("observation.cache_locations must be a bounded list")
    for cache in caches:
        if not isinstance(cache, dict) or cache.get("kind") not in ("shader", "pipeline", "driver"):
            raise SchemaError("cache location kind must be shader, pipeline or driver")
        cache["path"] = str(observation_path(cache.get("path")))
        cache["max_files"] = integer(cache.get("max_files", 1024), "cache.max_files", 1, 100000)
        cache["max_bytes"] = integer(cache.get("max_bytes", 256 * 1024**2), "cache.max_bytes", 1, 4 * 1024**3)
    result["cache_locations"] = caches
    profiles = result.get("profiling_evidence", [])
    if not isinstance(profiles, list) or len(profiles) > 64:
        raise SchemaError("observation.profiling_evidence must be a bounded list")
    for profile in profiles:
        if not isinstance(profile, dict) or profile.get("kind") not in PROFILE_KINDS or profile.get("format") != "anyps5.external-evidence/v1":
            raise SchemaError("profiling evidence requires a normalized supported anyps5.external-evidence/v1 profile")
        for key in ("path", "source_path"):
            profile[key] = str(observation_path(profile.get(key)))
    result["profiling_evidence"] = profiles
    return result


def descriptor(kind, relative, run_dir):
    path = contained_path(run_dir, relative, must_exist=True)
    return {"kind": kind, "path": relative, **identity_file(path)}


def snapshot_locations(config, phase):
    records = []
    for selected in config["cache_locations"]:
        root = observation_path(selected["path"])
        record = {"kind": selected["kind"], "path": str(root), "files": [], "status": "observed", "errors": [], "state": "unknown"}
        total = 0
        try:
            if not root.exists():
                record["status"] = "missing"
            elif root.is_file():
                candidates = [root]
            else:
                candidates = []
                visited = 0
                for directory, folders, files in os.walk(root, followlinks=False):
                    folders.sort()
                    files.sort()
                    for name in (*folders, *files):
                        visited += 1
                        observation_path(str(Path(directory) / name))
                    if visited > selected["max_files"] * 8 or len(candidates) + len(files) > selected["max_files"]:
                        record["status"] = "partial"
                        record["errors"].append("entry_capacity")
                        break
                    candidates.extend(Path(directory) / name for name in files)
            for path in candidates if root.exists() else []:
                observation_path(str(path))
                size = path.stat().st_size
                if len(record["files"]) >= selected["max_files"] or total + size > selected["max_bytes"]:
                    record["status"] = "partial"
                    record["errors"].append("hash_capacity")
                    break
                value = identity_file(path)
                total += value["size"]
                record["files"].append({"path": path.name if root.is_file() else path.relative_to(root).as_posix(), **value})
        except (OSError, ValueError) as error:
            record["status"] = "error"
            record["errors"].append(str(error))
        record["hashed_bytes"] = total
        record["inventory_sha256"] = canonical_sha256(record["files"])
        records.append(record)
    settings = []
    for item in config["effective_settings_files"]:
        try:
            settings.append({"path": item, "status": "observed_file_identity", **identity_file(item), "effective_consumption_verified": False})
        except (OSError, ValueError) as error:
            settings.append({"path": item, "status": "error", "error": str(error), "effective_consumption_verified": False})
    return {"schema_version": 1, "document_type": "anyps5.cache-snapshot", "phase": phase, "observed_at": utc_now(),
            "locations": records, "settings_files": settings, "policy_inferred_from_contents": False}


class WindowsProcess:
    def __init__(self, pid, *, process_handle=None):
        if os.name != "nt":
            raise OSError("native process observation is supported only on Windows")
        from ctypes import wintypes
        self.types = wintypes
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        if process_handle is None:
            raise OSError("exact launched-child handle is required for process observation")
        self.kernel.GetCurrentProcess.restype = wintypes.HANDLE
        self.kernel.DuplicateHandle.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        duplicate = wintypes.HANDLE()
        current = self.kernel.GetCurrentProcess()
        self.check(self.kernel.DuplicateHandle(current, process_handle, current, ctypes.byref(duplicate), 0, False, 2))
        self.handle = duplicate
        self.kernel.GetProcessId.argtypes = [wintypes.HANDLE]
        self.kernel.GetProcessId.restype = wintypes.DWORD
        if self.kernel.GetProcessId(self.handle) != pid:
            self.kernel.CloseHandle(self.handle)
            self.handle = None
            raise OSError("retained child handle PID differs from launched process")
        self.bound_to_child_handle = True
        self.pid = pid
        self.kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, *([ctypes.POINTER(wintypes.FILETIME)] * 4)]
        self.kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        self.kernel.K32EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
        self.kernel.K32GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
        self.kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        self.kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        self.kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        self.kernel.QueryPerformanceFrequency.argtypes = [ctypes.POINTER(ctypes.c_longlong)]
        self.kernel.QueryPerformanceCounter.argtypes = [ctypes.POINTER(ctypes.c_longlong)]
        frequency = ctypes.c_longlong()
        if not self.kernel.QueryPerformanceFrequency(ctypes.byref(frequency)):
            self.kernel.CloseHandle(self.handle)
            self.handle = None
            raise ctypes.WinError(ctypes.get_last_error())
        self.frequency = frequency.value

    @staticmethod
    def check(result):
        if not result:
            raise ctypes.WinError(ctypes.get_last_error())

    def ticks(self):
        value = ctypes.c_longlong()
        self.check(self.kernel.QueryPerformanceCounter(ctypes.byref(value)))
        return value.value

    def identity(self):
        values = [self.types.FILETIME() for unused in range(4)]
        self.check(self.kernel.GetProcessTimes(self.handle, *(ctypes.byref(value) for value in values)))
        name = ctypes.create_unicode_buffer(32768)
        length = self.types.DWORD(len(name))
        self.check(self.kernel.QueryFullProcessImageNameW(self.handle, 0, name, ctypes.byref(length)))
        counts = [(value.dwHighDateTime << 32) | value.dwLowDateTime for value in values]
        return {"pid": self.pid, "creation_filetime": str(counts[0]), "path": name.value,
                "kernel_cpu_100ns": counts[2], "user_cpu_100ns": counts[3]}

    def sample(self):
        from ctypes import wintypes
        class ModuleInfo(ctypes.Structure):
            _fields_ = [("base", ctypes.c_void_p), ("size", wintypes.DWORD), ("entry", ctypes.c_void_p)]
        class ThreadEntry(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("tid", wintypes.DWORD), ("pid", wintypes.DWORD), ("priority", wintypes.LONG), ("delta", wintypes.LONG), ("flags", wintypes.DWORD)]
        class Memory(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("faults", wintypes.DWORD)] + [(name, ctypes.c_size_t) for name in ("peak_working_set", "working_set", "peak_paged", "paged", "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile", "private_bytes")]
        class GlobalMemory(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("load", wintypes.DWORD)] + [(name, ctypes.c_ulonglong) for name in ("total_physical", "available_physical", "total_pagefile", "available_pagefile", "total_virtual", "available_virtual", "extended_virtual")]
        before_ns = time.perf_counter_ns()
        begin = self.ticks()
        code = wintypes.DWORD()
        self.check(self.kernel.GetExitCodeProcess(self.handle, ctypes.byref(code)))
        if code.value != 259:
            raise ProcessLookupError("observed process has exited")
        identity = self.identity()
        modules = (wintypes.HMODULE * 4096)()
        needed = wintypes.DWORD()
        self.check(self.kernel.K32EnumProcessModulesEx(self.handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3))
        if needed.value > ctypes.sizeof(modules):
            raise OSError("module enumeration capacity exceeded")
        self.kernel.K32GetModuleInformation.argtypes = [wintypes.HANDLE, wintypes.HMODULE, ctypes.POINTER(ModuleInfo), wintypes.DWORD]
        result = []
        for module in modules[:needed.value // ctypes.sizeof(wintypes.HMODULE)]:
            info = ModuleInfo()
            path = ctypes.create_unicode_buffer(32768)
            self.check(self.kernel.K32GetModuleInformation(self.handle, module, ctypes.byref(info), ctypes.sizeof(info)))
            length = self.kernel.K32GetModuleFileNameExW(self.handle, module, path, len(path))
            self.check(length)
            if length >= len(path) - 1:
                raise OSError("module path enumeration was truncated")
            result.append({"path": path.value, "base_address": str(info.base), "image_size": info.size})
        snapshot = self.kernel.CreateToolhelp32Snapshot(4, 0)
        if snapshot in (None, ctypes.c_void_p(-1).value):
            raise ctypes.WinError(ctypes.get_last_error())
        threads = []
        self.kernel.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry)]
        self.kernel.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry)]
        try:
            entry = ThreadEntry()
            entry.size = ctypes.sizeof(entry)
            available = self.kernel.Thread32First(snapshot, ctypes.byref(entry))
            while available:
                if entry.pid == self.pid:
                    threads.append(str(entry.tid))
                available = self.kernel.Thread32Next(snapshot, ctypes.byref(entry))
            if ctypes.get_last_error() not in (0, 18):
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            self.kernel.CloseHandle(snapshot)
        memory, system = Memory(), GlobalMemory()
        memory.size, system.size = ctypes.sizeof(memory), ctypes.sizeof(system)
        self.kernel.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD]
        self.kernel.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(GlobalMemory)]
        self.check(self.kernel.K32GetProcessMemoryInfo(self.handle, ctypes.byref(memory), memory.size))
        self.check(self.kernel.GlobalMemoryStatusEx(ctypes.byref(system)))
        end = self.ticks()
        self.check(self.kernel.GetExitCodeProcess(self.handle, ctypes.byref(code)))
        if code.value != 259:
            raise ProcessLookupError("exact child exited during process observation")
        return {"timestamp_ticks": str(begin), "end_ticks": str(end), "identity": identity, "modules": result, "native_thread_ids": threads,
                "anchor": {"ticks": str(begin), "observer_before_ns": before_ns, "observer_after_ns": time.perf_counter_ns()},
                "resources": {"working_set_bytes": memory.working_set, "private_bytes": memory.private_bytes, "page_fault_count": memory.faults,
                              "system_total_physical_bytes": system.total_physical, "system_available_physical_bytes": system.available_physical,
                              "system_available_commit_bytes": system.available_pagefile, "kernel_cpu_100ns": identity["kernel_cpu_100ns"], "user_cpu_100ns": identity["user_cpu_100ns"]}}

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


class ProcessObserver:
    def __init__(self, run_dir, manifest, config, *, process=None, backend_factory=WindowsProcess, expected_creation_filetime=None):
        self.run_dir = Path(run_dir)
        self.manifest = manifest
        self.config = config
        self.backend = None
        self.expected_creation_filetime = expected_creation_filetime
        self.module_hashes = {}
        self.modules = {}
        self.threads = {}
        self.last_present = set()
        self.resource_values = {}
        self.sample_count = 0
        self.attempt_count = 0
        self.output_bytes = 0
        self.output_failed = False
        self.hash_bytes = 0
        self.last_ticks = None
        self.maximum_gap = 0
        directory = self.run_dir / "observations"
        directory.mkdir(exist_ok=True)
        self.sample_path = directory / "samples.jsonl"
        self.stream = self.sample_path.open("xb", buffering=0)
        self.report = {"schema_version": 1, "document_type": "anyps5.process-observation", "run_id": manifest["run_id"],
                       "process": {"pid": manifest["execution"]["pid"], "creation_filetime": None, "executable": None, "identity_verified": False, "identity_binding": "unknown"},
                       "clock": {"domain": "windows_qpc", "ticks_per_second": None, "anchors": []}, "errors": [],
                       "coverage": {"full_lifetime": False, "sample_count": 0, "interval_ms": config["interval_ms"], "max_samples": config["max_samples"], "sampling_status": "unknown"}}
        try:
            if backend_factory is WindowsProcess:
                retained = getattr(process, "_handle", None)
                self.backend = backend_factory(manifest["execution"]["pid"], process_handle=int(retained) if retained is not None else None)
                self.expected_creation_filetime = self.backend.identity()["creation_filetime"]
            else:
                self.backend = backend_factory(manifest["execution"]["pid"])
            if getattr(self.backend, "bound_to_child_handle", False) is not True or self.expected_creation_filetime is None:
                self.backend.close()
                self.backend = None
                raise OSError("observation backend lacks exact retained-child creation identity")
            self.report["clock"]["ticks_per_second"] = self.backend.frequency
        except (OSError, ValueError) as error:
            self.report["errors"].append({"phase": "open", "type": type(error).__name__, "message": str(error)})

    def hash_module(self, item):
        path = Path(item["path"])
        result = {**item, "file_identity": None, "hash_status": "not_selected"}
        selected = any(path.is_relative_to(Path(root)) for root in self.config["hash_roots"])
        if selected:
            try:
                path = observation_path(str(path))
                info = path.stat()
                key = (str(path), info.st_size, info.st_mtime_ns, info.st_ctime_ns)
                if key not in self.module_hashes and (info.st_size > self.config["max_module_hash_bytes"] or self.hash_bytes + info.st_size > self.config["max_total_hash_bytes"] or len(self.module_hashes) >= 8192):
                    result["hash_status"] = "capacity_exceeded"
                else:
                    if key not in self.module_hashes:
                        self.module_hashes[key] = identity_file(path)
                        self.hash_bytes += self.module_hashes[key]["size"]
                    result.update(file_identity=self.module_hashes[key], hash_status="hashed_disk_file")
            except (OSError, ValueError) as error:
                result.update(hash_status="error", error=str(error))
        result["module_id"] = canonical_sha256({key: result[key] for key in ("path", "base_address", "image_size", "file_identity")})
        return result

    def sample(self):
        if self.backend is None or self.output_failed or self.attempt_count >= self.config["max_samples"]:
            return
        self.attempt_count += 1
        try:
            sample = self.backend.sample()
            identity = sample["identity"]
            if identity.get("pid") != self.manifest["execution"]["pid"] or identity.get("creation_filetime") != self.expected_creation_filetime:
                raise ValueError("observed PID/creation time differs from retained launched child")
            expected = self.manifest["execution"]["command"][0]
            if os.path.normcase(os.path.abspath(identity["path"])) != os.path.normcase(os.path.abspath(expected)):
                raise ValueError("observed executable path differs from launched child")
            recorded = self.report["process"]
            if recorded["creation_filetime"] is not None and recorded["creation_filetime"] != identity["creation_filetime"]:
                raise ValueError("process creation time changed; possible PID reuse")
            creation = int(identity["creation_filetime"])
            lower = datetime.datetime.fromisoformat(self.manifest["created_at"].replace("Z", "+00:00"))
            upper = datetime.datetime.fromisoformat(self.manifest["started_at"].replace("Z", "+00:00"))
            epoch = datetime.datetime(1601, 1, 1, tzinfo=datetime.timezone.utc)
            actual = epoch + datetime.timedelta(microseconds=creation // 10)
            if not lower - datetime.timedelta(seconds=1) <= actual <= upper + datetime.timedelta(seconds=1):
                raise ValueError("process creation time is outside the launch bound")
            if recorded["executable"] is None:
                executable = identity_file(expected)
                declared = next(item for item in self.manifest["runtime"]["files"] if item["role"] == "executable")
                if executable != {key: declared[key] for key in ("size", "sha256")}:
                    raise ValueError("observed executable disk identity differs from run inventory")
                recorded.update(creation_filetime=identity["creation_filetime"], executable={"path": expected, **executable}, identity_verified=True, identity_binding="retained_child_handle")
            ticks = int(sample["timestamp_ticks"])
            if self.last_ticks is not None:
                if ticks < self.last_ticks:
                    raise ValueError("sample clock moved backwards")
            materialized = [self.hash_module(item) for item in sample["modules"]]
            if len(set(self.modules) | {item["module_id"] for item in materialized}) > 16384 or len(set(self.threads) | set(sample["native_thread_ids"])) > 65536:
                raise ValueError("cumulative module or thread observation capacity exceeded")
            sample["modules"] = materialized
            sample["run_id"] = self.manifest["run_id"]
            sample["executable"] = recorded["executable"]
            sample["clock"] = {"domain": "windows_qpc", "ticks_per_second": self.backend.frequency}
            sample["process_binding"] = {"kind": "retained_child_handle", "pid": self.manifest["execution"]["pid"], "creation_filetime": self.expected_creation_filetime}
            encoded = (json.dumps(sample, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")
            encoded_size = len(encoded)
            if self.output_bytes + encoded_size > self.config["max_output_bytes"]:
                self.attempt_count = self.config["max_samples"]
                raise ValueError("process sample output capacity exceeded")
            try:
                written = self.stream.write(encoded)
                if written != encoded_size:
                    raise OSError("process sample output write was incomplete")
                self.stream.flush()
            except OSError:
                self.output_failed = True
                raise
            self.output_bytes += encoded_size
            if self.last_ticks is not None:
                self.maximum_gap = max(self.maximum_gap, ticks - self.last_ticks)
            self.last_ticks = ticks
            present = {item["module_id"] for item in materialized}
            for item in materialized:
                key = item["module_id"]
                if key not in self.modules:
                    self.modules[key] = {**item, "first_seen_ticks": str(ticks), "last_seen_ticks": str(ticks), "absence_observed_ticks": None, "presence_intervals": []}
                entry = self.modules[key]
                if key not in self.last_present:
                    entry["presence_intervals"].append({"first_seen_ticks": str(ticks), "last_seen_ticks": str(ticks), "absence_observed_ticks": None})
                entry["last_seen_ticks"] = str(ticks)
                entry["absence_observed_ticks"] = None
                entry["presence_intervals"][-1]["last_seen_ticks"] = str(ticks)
            for key in self.last_present - present:
                self.modules[key]["absence_observed_ticks"] = str(ticks)
                self.modules[key]["presence_intervals"][-1]["absence_observed_ticks"] = str(ticks)
            self.last_present = present
            for native_id in sample["native_thread_ids"]:
                entry = self.threads.setdefault(native_id, {"native_thread_id": native_id, "native_thread_id_source": "windows_thread_id", "first_seen_ticks": str(ticks)})
                entry["last_seen_ticks"] = str(ticks)
            for name, value in sample["resources"].items():
                entry = self.resource_values.setdefault(name, {"first": value, "last": value, "minimum": value, "maximum": value})
                entry.update(last=value, minimum=min(entry["minimum"], value), maximum=max(entry["maximum"], value))
            self.sample_count += 1
            self.report["clock"]["anchors"] = [sample["anchor"]] if self.sample_count == 1 else [self.report["clock"]["anchors"][0], sample["anchor"]]
        except ProcessLookupError:
            self.report["coverage"]["sampling_status"] = "process_exited"
        except (OSError, ValueError) as error:
            if len(self.report["errors"]) < 128:
                self.report["errors"].append({"phase": "sample", "type": type(error).__name__, "message": str(error)})

    def wait(self, process, *, poll_callback=None):
        sampling = True
        next_sample = None
        interval = self.config["interval_ms"] / 1000
        while poll_callback is not None or (self.backend is not None and not self.output_failed and self.attempt_count < self.config["max_samples"]):
            if poll_callback is not None:
                poll_callback()
            if sampling and (poll_callback is None or next_sample is None or time.monotonic() >= next_sample):
                try:
                    self.sample()
                except Exception as error:
                    self.report["errors"].append({"phase": "sample", "type": type(error).__name__, "message": str(error)})
                    if poll_callback is None:
                        return process.wait()
                    sampling = False
                if poll_callback is not None:
                    next_sample = time.monotonic() + interval
            timeout = interval
            if poll_callback is not None:
                timeout = min(0.25, max(0.001, next_sample - time.monotonic())) if sampling and next_sample is not None else 0.25
            try:
                return process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                pass
        return process.wait()

    def finish(self):
        try:
            self.stream.close()
        except OSError as error:
            self.output_failed = True
            if len(self.report["errors"]) < 128:
                self.report["errors"].append({"phase": "output_close", "type": type(error).__name__, "message": str(error)})
        finally:
            if self.backend is not None:
                self.backend.close()
        coverage = self.report["coverage"]
        coverage.update(sample_count=self.sample_count, attempt_count=self.attempt_count,
                        sample_bytes=self.sample_path.stat().st_size, complete_sample_bytes=self.output_bytes,
                        sample_output_failed=self.output_failed, hashed_bytes=self.hash_bytes,
                        max_observed_gap_ticks=str(self.maximum_gap),
                        sampling_status="output_failed" if self.output_failed else "sample_cap" if self.attempt_count >= self.config["max_samples"] else "partial" if self.report["errors"] else "observed")
        self.report.update(modules=list(self.modules.values()), threads=list(self.threads.values()), resources=self.resource_values,
                           observed_at=utc_now(), limitations=["Sampled presence does not prove continuous or full-lifetime module coverage.",
                           "Disk hashes do not establish complete in-memory bytes or source-to-binary binding.", "Native OS thread IDs are valid only within this process creation lifetime.",
                           "Native thread IDs may be reused; sampled presence is not a complete thread lifecycle.",
                           "Resource samples and CPU counters are not per-function execution measurements."])
        atomic_write_json(self.run_dir / "observations" / "process.json", self.report, replace=False)
        return self.report


def resolve_address(address, observation, *, at_ticks=None):
    unknown = {"status": "unknown", "module": None, "offset": None, "symbol": None}
    if type(address) is not int or not 0 <= address < 2**64 or at_ticks is None:
        return {**unknown, "reason": "address_or_observation_time_unavailable"}
    try:
        tick = int(at_ticks)
        candidates = []
        for module in observation.get("modules", []):
            base, size = int(module["base_address"]), module["image_size"]
            if not base <= address < base + size:
                continue
            for interval in module["presence_intervals"]:
                first, last = int(interval["first_seen_ticks"]), int(interval["last_seen_ticks"])
                if first <= tick <= last:
                    candidates.append(module)
                    break
        if len(candidates) != 1:
            return {**unknown, "reason": "unmapped_dynamic_or_ambiguous_sampled_range"}
        module = candidates[0]
        return {"status": "sampled_range_candidate", "module": {key: module[key] for key in ("module_id", "path", "base_address", "image_size", "file_identity")},
                "offset": str(address - int(module["base_address"])), "symbol": None, "symbol_status": "unknown",
                "temporal_scope": "sampled_presence_bounds_do_not_prove_continuous_mapping", "observation_ticks": str(tick)}
    except (KeyError, TypeError, ValueError):
        return {**unknown, "reason": "invalid_observation_range"}


def validate_process_observation(value, manifest):
    if not isinstance(value, dict) or value.get("schema_version") != 1 or value.get("document_type") != "anyps5.process-observation" or value.get("run_id") != manifest["run_id"]:
        raise SchemaError("process observation header/run identity mismatch")
    process = value.get("process", {})
    if not isinstance(process, dict) or not isinstance(value.get("coverage"), dict) or not isinstance(value.get("clock"), dict):
        raise SchemaError("process observation identity/clock/coverage must be objects")
    if type(process.get("pid")) is not int or process["pid"] != manifest["execution"].get("pid") or type(process.get("identity_verified")) is not bool:
        raise SchemaError("process observation PID differs from launched child")
    if value.get("coverage", {}).get("full_lifetime") is not False:
        raise SchemaError("sampled process evidence cannot establish full-lifetime coverage")
    if process.get("identity_verified") is True:
        if process.get("identity_binding") != "retained_child_handle":
            raise SchemaError("verified process identity requires retained-child handle binding")
        creation = process.get("creation_filetime")
        if not isinstance(creation, str) or re.fullmatch(r"[1-9][0-9]*", creation) is None or int(creation) >= 2**64:
            raise SchemaError("process creation FILETIME must be canonical uint64")
        epoch = datetime.datetime(1601, 1, 1, tzinfo=datetime.timezone.utc)
        observed = epoch + datetime.timedelta(microseconds=int(creation) // 10)
        lower = datetime.datetime.fromisoformat(manifest["created_at"].replace("Z", "+00:00"))
        upper = datetime.datetime.fromisoformat(manifest["started_at"].replace("Z", "+00:00"))
        if not lower - datetime.timedelta(seconds=1) <= observed <= upper + datetime.timedelta(seconds=1):
            raise SchemaError("observed process creation time is outside recorded launch bounds")
        executable = process.get("executable", {})
        if not isinstance(executable, dict) or not isinstance(executable.get("path"), str):
            raise SchemaError("observed executable identity must be an object with path")
        declared = next(item for item in manifest["runtime"]["files"] if item["role"] == "executable")
        if os.path.normcase(executable.get("path", "")) != os.path.normcase(declared["path"]) or any(executable.get(key) != declared[key] for key in ("size", "sha256")):
            raise SchemaError("observed executable identity differs from declared runtime")
        if type(value.get("clock", {}).get("ticks_per_second")) is not int or value["clock"]["ticks_per_second"] <= 0:
            raise SchemaError("process observation clock frequency is unavailable")
    modules = value.get("modules")
    if not isinstance(modules, list) or len(modules) > 16384:
        raise SchemaError("invalid observed module inventory")
    seen = set()
    for module in modules:
        if not isinstance(module, dict):
            raise SchemaError("observed module must be an object")
        base = module.get("base_address")
        if not isinstance(base, str) or re.fullmatch(r"0|[1-9][0-9]*", base) is None:
            raise SchemaError("module base must be canonical uint64 string")
        size = integer(module.get("image_size"), "module.image_size", 1, 2**64 - 1)
        if int(base) + size > 2**64 or module.get("module_id") in seen:
            raise SchemaError("module mapping overflows or repeats identity")
        seen.add(module.get("module_id"))
        if not isinstance(module.get("path"), str) or not Path(module["path"]).is_absolute():
            raise SchemaError("observed module path must be absolute")
        if module.get("module_id") != canonical_sha256({key: module.get(key) for key in ("path", "base_address", "image_size", "file_identity")}):
            raise SchemaError("observed module identity fields do not match module ID")
        file = module.get("file_identity")
        if file is not None and (not isinstance(file, dict) or type(file.get("size")) is not int or file["size"] < 0 or not isinstance(file.get("sha256"), str) or re.fullmatch(r"[0-9a-f]{64}", file["sha256"]) is None):
            raise SchemaError("invalid observed module disk identity")
        intervals = module.get("presence_intervals")
        if not isinstance(intervals, list):
            raise SchemaError("sampled module presence intervals must be an array")
        for interval in intervals:
            if not isinstance(interval, dict):
                raise SchemaError("sampled module presence interval must be an object")
            if int(interval["first_seen_ticks"]) > int(interval["last_seen_ticks"]):
                raise SchemaError("module sampled lifetime bounds are reversed")
            absent = interval.get("absence_observed_ticks")
            if absent is not None and int(absent) < int(interval["last_seen_ticks"]):
                raise SchemaError("module absence precedes its last observation")
    return value


def reconstruct_samples(path, manifest, *, max_samples, max_bytes):
    if path.stat().st_size > max_bytes:
        raise SchemaError("process samples exceed configured byte capacity")
    modules, threads, resources = {}, {}, {}
    resource_sources = {}
    previous = set()
    process = None
    clock = None
    anchors = []
    count = 0
    physical_line = 0
    last_tick = None
    maximum_gap = 0
    with path.open(encoding="utf-8") as stream:
        while True:
            line = stream.readline(16 * 1024**2 + 1)
            if not line:
                break
            physical_line += 1
            if len(line) > 16 * 1024**2:
                raise SchemaError("process sample line exceeds bounded record size")
            if not line.strip():
                continue
            count += 1
            if count > max_samples:
                raise SchemaError("process samples exceed configured count capacity")
            from capture_schema import loads_json
            sample = loads_json(line, context=f"process_samples:{physical_line}")
            if not isinstance(sample, dict):
                raise SchemaError("raw process sample must be an object")
            identity = sample.get("identity", {})
            if not isinstance(identity, dict):
                raise SchemaError("raw process identity must be an object")
            binding = sample.get("process_binding", {})
            if not isinstance(binding, dict) or binding.get("kind") != "retained_child_handle" or binding.get("pid") != identity.get("pid") or binding.get("creation_filetime") != identity.get("creation_filetime"):
                raise SchemaError("raw process sample lacks exact retained-child binding")
            supplied = {"pid": identity.get("pid"), "creation_filetime": identity.get("creation_filetime"), "executable": sample.get("executable"), "identity_verified": True, "identity_binding": "retained_child_handle"}
            current_clock = sample.get("clock", {})
            probe = {"schema_version": 1, "document_type": "anyps5.process-observation", "run_id": sample.get("run_id"), "process": supplied,
                     "clock": current_clock, "coverage": {"full_lifetime": False}, "modules": []}
            validate_process_observation(probe, manifest)
            if os.path.normcase(identity.get("path", "")) != os.path.normcase(supplied["executable"]["path"]):
                raise SchemaError("raw process path differs from observed executable identity")
            if process is not None and process != supplied:
                raise SchemaError("raw sample process identity changed")
            if clock is not None and clock != current_clock:
                raise SchemaError("raw sample clock identity changed")
            process, clock = supplied, current_clock
            tick_text = sample.get("timestamp_ticks")
            if not isinstance(tick_text, str) or re.fullmatch(r"0|[1-9][0-9]*", tick_text) is None:
                raise SchemaError("raw sample timestamp must be canonical decimal ticks")
            tick = int(tick_text)
            end = sample.get("end_ticks")
            if not isinstance(end, str) or re.fullmatch(r"0|[1-9][0-9]*", end) is None or int(end) < tick:
                raise SchemaError("raw sample query bracket is reversed")
            if last_tick is not None:
                if tick < last_tick:
                    raise SchemaError("raw sample clock moved backwards")
                maximum_gap = max(maximum_gap, tick - last_tick)
            last_tick = tick
            anchor = sample.get("anchor", {})
            if not isinstance(anchor, dict) or anchor.get("ticks") != tick_text or type(anchor.get("observer_before_ns")) is not int or type(anchor.get("observer_after_ns")) is not int or anchor["observer_before_ns"] > anchor["observer_after_ns"]:
                raise SchemaError("raw sample clock anchor is malformed")
            anchors = [anchor] if not anchors else [anchors[0], anchor]
            present = set()
            if not isinstance(sample.get("modules"), list) or len(sample["modules"]) > 4096:
                raise SchemaError("raw module inventory must be a bounded array")
            for item in sample["modules"]:
                if not isinstance(item, dict):
                    raise SchemaError("raw module inventory entry must be an object")
                candidate = {**item, "presence_intervals": []}
                validate_process_observation({**probe, "modules": [candidate]}, manifest)
                key = item["module_id"]
                if key in present:
                    raise SchemaError("module repeats within raw sample")
                present.add(key)
                entry = modules.setdefault(key, {**item, "first_seen_ticks": tick_text, "last_seen_ticks": tick_text, "absence_observed_ticks": None, "presence_intervals": []})
                if key not in previous:
                    entry["presence_intervals"].append({"first_seen_ticks": tick_text, "last_seen_ticks": tick_text, "absence_observed_ticks": None})
                entry["last_seen_ticks"] = tick_text
                entry["absence_observed_ticks"] = None
                entry["presence_intervals"][-1]["last_seen_ticks"] = tick_text
            for key in previous - present:
                modules[key]["absence_observed_ticks"] = tick_text
                modules[key]["presence_intervals"][-1]["absence_observed_ticks"] = tick_text
            previous = present
            ids = sample.get("native_thread_ids")
            if not isinstance(ids, list) or len(ids) > 65536 or len(set(ids)) != len(ids):
                raise SchemaError("raw native thread inventory is invalid")
            for native in ids:
                if not isinstance(native, str) or re.fullmatch(r"[1-9][0-9]*", native) is None or int(native) >= 2**32:
                    raise SchemaError("raw native Windows thread ID is invalid")
                entry = threads.setdefault(native, {"native_thread_id": native, "native_thread_id_source": "windows_thread_id", "first_seen_ticks": tick_text})
                entry["last_seen_ticks"] = tick_text
            if not isinstance(sample.get("resources"), dict):
                raise SchemaError("raw resources must be an object")
            for name, value in sample["resources"].items():
                integer(value, f"resources.{name}", 0, 2**64 - 1)
                entry = resources.setdefault(name, {"first": value, "last": value, "minimum": value, "maximum": value})
                sources = resource_sources.setdefault(name, {"first": physical_line, "last": physical_line, "minimum": physical_line, "maximum": physical_line})
                sources["last"] = physical_line
                if value < entry["minimum"]:
                    sources["minimum"] = physical_line
                if value > entry["maximum"]:
                    sources["maximum"] = physical_line
                entry.update(last=value, minimum=min(entry["minimum"], value), maximum=max(entry["maximum"], value))
            if len(modules) > 16384 or len(threads) > 65536:
                raise SchemaError("raw cumulative observation inventory exceeds capacity")
    return {"process": process, "clock": {**clock, "anchors": anchors} if clock else None, "modules": list(modules.values()), "threads": list(threads.values()),
            "resources": resources, "resource_sources": resource_sources, "sample_count": count, "maximum_gap_ticks": str(maximum_gap)}


def assess_process_observation(run_dir, manifest, usable_artifacts):
    result = {"status": "unknown", "issues": [], "run_id": manifest["run_id"], "process": None, "clock": None, "modules": [], "threads": [], "resources": {}, "coverage": {}, "artifact": None}
    candidates = [item for item in manifest.get("artifacts", []) if item["kind"] == "process_observation"]
    if not candidates:
        return result
    try:
        if len(candidates) != 1:
            raise SchemaError("multiple process observation reports")
        item = candidates[0]
        if item["path"] not in usable_artifacts:
            raise SchemaError("process observation artifact was not verified")
        path = contained_path(run_dir, item["path"], must_exist=True)
        expected = {key: item[key] for key in ("size", "sha256")}
        if identity_file(path) != expected or item["size"] > 32 * 1024**2:
            raise SchemaError("process observation artifact hash/size mismatch")
        value = validate_process_observation(read_json(path), manifest)
        raw_candidates = [row for row in manifest["artifacts"] if row["kind"] == "process_samples"]
        if len(raw_candidates) != 1 or raw_candidates[0]["path"] not in usable_artifacts:
            raise SchemaError("primary process sample artifact is missing or unverified")
        raw = raw_candidates[0]
        raw_path = contained_path(run_dir, raw["path"], must_exist=True)
        raw_identity = {key: raw[key] for key in ("size", "sha256")}
        if identity_file(raw_path) != raw_identity:
            raise SchemaError("primary process samples hash mismatch")
        policy = validate_observation_config(manifest.get("observation"))
        derived = reconstruct_samples(raw_path, manifest, max_samples=policy["max_samples"] if policy else 100000,
                                      max_bytes=policy["max_output_bytes"] if policy else 64 * 1024**2)
        if identity_file(raw_path) != raw_identity:
            raise SchemaError("primary process samples changed while assessing")
        for key in ("process", "clock", "modules", "threads", "resources"):
            if derived["sample_count"] and value.get(key) != derived[key]:
                raise SchemaError(f"derived process report {key} differs from raw samples")
        if value["coverage"].get("sample_count") != derived["sample_count"] or value["coverage"].get("max_observed_gap_ticks") != derived["maximum_gap_ticks"]:
            raise SchemaError("saved sample count/gap differs from raw observations")
        if identity_file(path) != expected:
            raise SchemaError("process observation changed during assessment")
        result.update({key: derived.get(key) for key in ("process", "clock", "modules", "threads", "resources")})
        result["coverage"] = {**value["coverage"], "sample_count": derived["sample_count"], "max_observed_gap_ticks": derived["maximum_gap_ticks"], "raw_samples_verified": True, "raw_samples_artifact": raw["path"]}
        result["sample_artifact"] = raw
        result["artifact"] = item
        result["status"] = "valid" if derived["process"] is not None and not value.get("errors") and derived["sample_count"] > 0 else "unknown"
        result["resources"] = {}
        for name, entry in derived["resources"].items():
            counter = name.endswith("_cpu_100ns") or name == "page_fault_count"
            result["resources"][name] = {**entry, "delta": entry["last"] - entry["first"] if not counter or entry["last"] >= entry["first"] else None,
                "meaning": "cumulative_process_counter_delta_not_function_time" if counter else "sampled_resource_gauge_not_per_frame",
                "source_records": {key: {"artifact": raw["path"], "line": line} for key, line in derived["resource_sources"][name].items()}}
        result["receipt_matches"] = assess_receipt_matches(run_dir, manifest, usable_artifacts, derived["modules"])
        for receipt in result["receipt_matches"]:
            if receipt["status"] == "invalid":
                result["status"] = "invalid"
                result["issues"].append({"severity": "error", "code": "process_receipt_invalid", "message": receipt["error"]})
        result["issues"].append({"severity": "warning", "code": "process_sampling_lifetime_unknown", "message": "Sampled modules and native thread IDs do not establish continuous/full-lifetime coverage."})
        for error in value.get("errors", []):
            result["issues"].append({"severity": "warning", "code": "process_observation_error", "message": str(error)})
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        result["status"] = "invalid"
        result["issues"].append({"severity": "error", "code": "process_observation_invalid", "message": str(error)})
    return result


def assess_cpu_sample_ownership(document, process_evidence, *, artifact=None, max_samples=100000, max_frames=64):
    result = {"status": "unknown", "sample_count": 0, "stack_frame_count": 0, "module_offset_aggregates": [], "sample_examples": [],
              "exporter_symbol_labels": [], "unknown_counts": {}, "issues": [],
              "limitations": ["Sample counts are observations, not function invocation counts or precise function timings.",
                              "All stack frames can share one sample weight; aggregates are not additive CPU time or exclusive self time.",
                              "Module presence bounds are sampled candidates, not continuously observed mappings; native TIDs can be reused.",
                              "Exporter-provided symbol labels remain unverified; no symbol or unwind correctness is inferred."]}
    try:
        if not isinstance(document, dict) or document.get("kind") != "cpu_profile" or not isinstance(process_evidence, dict):
            raise SchemaError("CPU ownership requires normalized CPU and fresh process evidence")
        actual = process_evidence.get("process") or {}
        expected = document.get("process") or {}
        if process_evidence.get("status") == "invalid" or not actual.get("identity_verified") or actual.get("identity_binding") != "retained_child_handle":
            raise SchemaError("fresh retained-child process binding is unavailable")
        if document.get("run_id") != process_evidence.get("run_id") or any(expected.get(key) != actual.get(key) for key in ("pid", "creation_filetime")) or expected.get("executable_sha256") != actual["executable"]["sha256"]:
            raise SchemaError("CPU sample process identity does not match observations")
        clock, native_clock = document.get("clock") or {}, process_evidence.get("clock") or {}
        qpc_domains = {"windows_qpc", "query_performance_counter"}
        if clock.get("domain") not in qpc_domains or native_clock.get("domain") not in qpc_domains or clock.get("ticks_per_second") != native_clock.get("ticks_per_second"):
            raise SchemaError("CPU sample and process observation clocks are not explicitly identical")
        records = document.get("records") or {}
        samples = records.get("samples")
        if not isinstance(samples, list) or len(samples) > max_samples:
            raise SchemaError("CPU sample inventory exceeds bounded assessment capacity")
        modules = sorted(process_evidence.get("modules", []), key=lambda row: int(row["base_address"]))
        bases = [int(row["base_address"]) for row in modules]
        prefix_ends = []
        for row in modules:
            prefix_ends.append(max(prefix_ends[-1] if prefix_ends else 0, int(row["base_address"]) + row["image_size"]))
        threads = {row["native_thread_id"]: row for row in process_evidence.get("threads", [])}
        aggregates = {}
        labels = {}
        unknown = {}
        profile_path = artifact.get("path") if isinstance(artifact, dict) else str(artifact) if artifact else None
        for index, sample in enumerate(samples):
            if not isinstance(sample, dict) or not isinstance(sample.get("stack"), list) or len(sample["stack"]) > max_frames:
                raise SchemaError("CPU sample stack exceeds bounded frame capacity")
            tick = int(sample["timestamp_ticks"])
            native = str(sample["thread_id"])
            thread = threads.get(native)
            thread_known = thread is not None and int(thread["first_seen_ticks"]) <= tick <= int(thread["last_seen_ticks"])
            weight = integer(sample.get("weight_samples"), "CPU sample weight", 1, 2**63 - 1)
            source = {"artifact": profile_path, "record": f"records.samples[{index}]", "sample_id": sample.get("id"), "raw_source_artifact": document.get("source", {}).get("artifact")}
            example = {"sample_id": sample.get("id"), "timestamp_ticks": str(tick), "native_thread_id": native, "thread_presence": "sampled_bounds_candidate" if thread_known else "unknown", "source": source, "stack": []}
            hit_keys = set()
            for frame in sample["stack"]:
                address = int(frame["address"])
                candidates = []
                cursor = bisect.bisect_right(bases, address) - 1
                while cursor >= 0 and prefix_ends[cursor] > address:
                    module = modules[cursor]
                    if int(module["base_address"]) <= address < int(module["base_address"]) + module["image_size"]:
                        candidates.append(module)
                    cursor -= 1
                resolved = resolve_address(address, {"modules": candidates}, at_ticks=tick) if thread_known else {"status": "unknown", "reason": "native_thread_not_observed_at_sample_bounds", "module": None, "offset": None, "symbol": None}
                resolved["observation_artifact"] = process_evidence.get("artifact")
                resolved["process_samples_artifact"] = process_evidence.get("sample_artifact")
                result["stack_frame_count"] += 1
                if resolved["status"] == "sampled_range_candidate":
                    module = resolved["module"]
                    key = (module["module_id"], resolved["offset"])
                    entry = aggregates.setdefault(key, {"module": module, "offset": resolved["offset"], "symbol": None, "symbol_status": "unknown", "stack_occurrences": 0,
                                                        "weighted_stack_occurrences": 0, "sample_hits": 0, "weighted_sample_hits": 0, "source_examples": []})
                    entry["stack_occurrences"] += 1
                    entry["weighted_stack_occurrences"] += weight
                    if key not in hit_keys:
                        entry["sample_hits"] += 1
                        entry["weighted_sample_hits"] += weight
                        hit_keys.add(key)
                    if len(entry["source_examples"]) < 4:
                        entry["source_examples"].append(source)
                else:
                    reason = resolved.get("reason", "unknown")
                    unknown[reason] = unknown.get(reason, 0) + 1
                label = frame.get("symbol")
                if label is not None and len(labels) < 4096:
                    labels.setdefault((str(address), label), {"address": str(address), "label": label, "status": "exporter_declared_unverified", "source": source})
                if len(result["sample_examples"]) < 128:
                    example["stack"].append({"address": str(address), "resolution": resolved, "exporter_symbol": label, "exporter_symbol_status": "unverified" if label else "unknown"})
            result["sample_count"] += 1
            if len(result["sample_examples"]) < 128:
                result["sample_examples"].append(example)
            if len(aggregates) > 100000:
                raise SchemaError("CPU module/offset aggregation capacity exceeded")
        result.update(status="valid", module_offset_aggregates=list(aggregates.values()), exporter_symbol_labels=list(labels.values()), unknown_counts=unknown)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        result["issues"].append({"severity": "warning", "code": "cpu_sample_ownership_unknown", "message": str(error)})
    return result


def assess_receipt_matches(run_dir, manifest, usable_artifacts, modules):
    matches = []
    for artifact in manifest.get("artifacts", []):
        if artifact["kind"] not in ("build_receipt", "symbol_receipt"):
            continue
        result = {"artifact": artifact["path"], "kind": artifact["kind"], "matched_observed_module_ids": [], "source_binary_binding": "unverified", "symbol_binding": "unverified"}
        try:
            if artifact["path"] not in usable_artifacts or artifact["size"] > 4 * 1024**2:
                raise SchemaError("receipt is missing, unverified or oversized")
            path = contained_path(run_dir, artifact["path"], must_exist=True)
            expected = {key: artifact[key] for key in ("size", "sha256")}
            if identity_file(path) != expected:
                raise SchemaError("receipt hash mismatch")
            value = read_json(path)
            if not isinstance(value, dict) or value.get("schema_version") != 1 or value.get("document_type") != "anyps5." + artifact["kind"].replace("_", "-"):
                raise SchemaError("receipt protocol mismatch")
            images = value.get("outputs", []) if artifact["kind"] == "build_receipt" else [value.get("image", {})]
            if not isinstance(images, list) or len(images) > 4096:
                raise SchemaError("receipt output inventory must be bounded")
            for image in images:
                if not isinstance(image, dict) or not isinstance(image.get("sha256"), str) or re.fullmatch(r"[0-9a-f]{64}", image["sha256"]) is None:
                    raise SchemaError("receipt output identity is invalid")
                result["matched_observed_module_ids"].extend(module["module_id"] for module in modules if module.get("file_identity") == {key: image.get(key) for key in ("size", "sha256")})
            if artifact["kind"] == "symbol_receipt" and value.get("binding") != "declared_pair_not_verified_debug_id":
                from capture_symbols import assess_symbol_receipt
                matched = next((module["file_identity"] for module in modules
                    if module["module_id"] in result["matched_observed_module_ids"]), None)
                assessment = assess_symbol_receipt(value, observed_image=matched)
                if assessment["status"] == "invalid":
                    raise SchemaError(assessment.get("reason") or "fresh symbol receipt assessment failed")
                result["symbol_assessment"] = {"status": assessment["status"], "binding": assessment["binding"],
                    "function_range_count": len(assessment["function_ranges"]),
                    "exact_address_count": len(assessment["address_resolutions"]),
                    "matched_observed_image": matched is not None}
                if assessment["status"] == "valid":
                    result["symbol_binding"] = assessment["binding"]
                    result["symbol_resolution"] = "fresh_metadata_assessment"
            if identity_file(path) != expected:
                raise SchemaError("receipt changed during assessment")
            result["status"] = "observed_image_hash_matches" if result["matched_observed_module_ids"] else "unknown"
        except (OSError, ValueError, TypeError, KeyError) as error:
            result.update(status="invalid", error=str(error))
        matches.append(result)
    return matches


def snapshot_artifacts(run_dir, manifest, config, phase):
    directory = Path(run_dir) / "observations"
    directory.mkdir(exist_ok=True)
    relative = f"observations/cache-{phase}.json"
    value = snapshot_locations(config, phase)
    value["run_id"] = manifest["run_id"]
    atomic_write_json(Path(run_dir) / relative, value, replace=False)
    return [descriptor("cache_snapshot", relative, run_dir)]


def copy_attachment(source, destination):
    source = observation_path(str(source))
    identity = identity_file(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
    if identity_file(source) != identity or identity_file(destination) != identity:
        raise SchemaError("attachment changed during copying")
    return identity


def attach_observation_evidence(run_dir, manifest, config):
    artifacts = []
    for kind in ("build_receipt", "symbol_receipt"):
        for index, source in enumerate(config[kind + "s"]):
            if Path(source).stat().st_size > 4 * 1024**2:
                raise SchemaError("receipt size exceeds bounded JSON limit")
            before = identity_file(source)
            value = read_json(source)
            if not isinstance(value, dict) or value.get("document_type") != "anyps5." + kind.replace("_", "-") or value.get("schema_version") != 1:
                raise SchemaError("receipt document type/version mismatch")
            relative = f"external/{kind}-{index:04d}.json"
            if copy_attachment(source, contained_path(run_dir, relative)) != before:
                raise SchemaError("receipt changed after protocol validation")
            artifacts.append(descriptor(kind, relative, run_dir))
    for index, plan in enumerate(config["profiling_evidence"]):
        value = read_json(plan["path"])
        if not isinstance(value, dict) or value.get("kind") != plan["kind"]:
            raise SchemaError("normalized profile kind differs from configured attachment kind")
        artifacts.extend(attach_profile_files(run_dir, manifest, plan["path"], plan["source_path"], index=index))
    return artifacts


def profile_dependency_artifacts(run_dir, manifest, value):
    known = {item["path"]: item for item in manifest["artifacts"]}
    selected = {}

    def existing(reference, kind, *, limit=None, prefixes=None, suffix=None):
        if not isinstance(reference, dict):
            raise SchemaError("profile dependency descriptor must be an object")
        relative = reference.get("path")
        if (not isinstance(relative, str) or "\\" in relative or ":" in relative
                or any(part.casefold() in ("", ".", "..", "dump", "fakelib", "app0") for part in relative.split("/"))
                or prefixes is not None and not relative.startswith(prefixes)):
            raise SchemaError("profile dependency must name an explicit contained evidence path")
        if suffix is not None and Path(relative).suffix.casefold() != suffix:
            raise SchemaError("profile auxiliary evidence must be JSON")
        expected = {key: reference.get(key) for key in ("size", "sha256")}
        if (type(expected["size"]) is not int or expected["size"] < 0
                or limit is not None and expected["size"] > limit
                or not isinstance(expected["sha256"], str) or re.fullmatch(r"[0-9a-f]{64}", expected["sha256"]) is None):
            raise SchemaError("profile dependency size/hash is invalid or exceeds its bound")
        previous = known.get(relative)
        if previous is None and not relative.startswith(("external/", "collectors/")):
            raise SchemaError("related profile evidence outside external/ or collectors/ must already be inventoried")
        if previous is not None and any(previous[key] != expected[key] for key in expected):
            raise SchemaError("profile dependency contradicts existing manifest identity")
        observation_path(str(Path(run_dir) / relative))
        path = observation_path(str(contained_path(run_dir, relative, must_exist=True)))
        if path.stat().st_size != expected["size"] or limit is not None and path.stat().st_size > limit:
            raise SchemaError("profile dependency actual size differs or exceeds its bound")
        if identity_file(path) != expected:
            raise SchemaError("profile dependency differs from its preserved byte identity")
        descriptor = {"kind": previous["kind"] if previous is not None else kind, "path": relative, **expected}
        if relative in selected:
            if any(selected[relative][key] != expected[key] for key in expected):
                raise SchemaError("profile dependency is declared inconsistently")
            if previous is None and kind == "collector_log":
                descriptor["kind"] = kind
            else:
                descriptor = selected[relative]
        selected[relative] = descriptor
        return path

    producer = value.get("producer", {})
    if not isinstance(producer, dict):
        raise SchemaError("profile producer must be an object")
    adapter = producer.get("adapter")
    if adapter is not None and not isinstance(adapter, str):
        raise SchemaError("profile adapter must be an explicit string")
    required = {"explicit_table_v1": ("mapping_artifact",),
                "nsight_shader_csv_v1": ("mapping_artifact", "request_artifact")}.get(adapter, ())
    for key in required:
        if value.get(key) is None:
            raise SchemaError("normalized profile is missing required " + key)
    documents = {}
    for key, kind in (("mapping_artifact", "external_mapping"), ("request_artifact", "gpu_profile_request"),
                      ("collector_receipt_artifact", "collector_receipt")):
        if value.get(key) is None:
            continue
        path = existing(value[key], kind, limit=1024**2, prefixes=("external/", "collectors/"), suffix=".json")
        documents[key] = read_json(path)
        if not isinstance(documents[key], dict):
            raise SchemaError("profile auxiliary JSON must be an object")
    request = documents.get("request_artifact")
    if request is not None:
        from capture_gpu_profile import validate_profile_request
        validate_profile_request(request)
        if request["run_id"] != manifest["run_id"] or any(request["process"].get(key) != value["process"].get(key)
                for key in ("pid", "creation_filetime", "executable_sha256")):
            raise SchemaError("GPU request differs from the attached profile process/run identity")
        native = request.get("native_artifact")
        if not isinstance(native, dict) or known.get(native.get("path"), {}).get("kind") != "draw_trace":
            raise SchemaError("GPU request must bind an existing native draw trace artifact")
        existing(native, "draw_trace")
    receipt = documents.get("collector_receipt_artifact")
    if receipt is not None:
        if request is None:
            raise SchemaError("collector receipt attachment requires its explicit GPU request")
        from capture_gpu_profile import inspect_collector_receipt
        inspect_collector_receipt(receipt, request)
        references = receipt.get("artifacts", [])
        if not isinstance(references, list) or len(references) > 1024:
            raise SchemaError("collector related artifact list exceeds its bound")
        references = [(reference, "collector_evidence") for reference in references]
        for command in receipt["commands"]:
            references.extend((command[key], "collector_log") for key in ("stdout", "stderr") if command.get(key) is not None)
        for reference, kind in references:
            existing(reference, kind)
    for item in selected.values():
        if identity_file(contained_path(run_dir, item["path"], must_exist=True)) != {key: item[key] for key in ("size", "sha256")}:
            raise SchemaError("profile dependency changed during validation")
    return list(selected.values())


def attach_profile_files(run_dir, manifest, profile, raw_source, *, index):
    profile, raw_source = observation_path(str(profile)), observation_path(str(raw_source))
    if profile.stat().st_size > 256 * 1024**2:
        raise SchemaError("normalized profile exceeds bounded attachment limit")
    profile_identity = identity_file(profile)
    value = read_json(profile)
    kind = value.get("kind") if isinstance(value, dict) else None
    if not isinstance(value, dict) or value.get("schema_version") != 1 or value.get("document_type") != "anyps5.external-evidence" or not isinstance(kind, str) or kind not in PROFILE_KINDS or value.get("run_id") != manifest["run_id"]:
        raise SchemaError("normalized profile identity/protocol mismatch")
    observed = read_json(contained_path(run_dir, "observations/process.json", must_exist=True))
    validate_process_observation(observed, manifest)
    raw_path = contained_path(run_dir, "observations/samples.jsonl", must_exist=True)
    raw_identity = identity_file(raw_path)
    primary = reconstruct_samples(raw_path, manifest, max_samples=100000, max_bytes=2 * 1024**3)
    if identity_file(raw_path) != raw_identity or primary["sample_count"] == 0:
        raise SchemaError("profile attachment lacks stable primary process identity evidence")
    identity = primary["process"]
    supplied = value.get("process", {})
    if not isinstance(supplied, dict) or identity.get("identity_verified") is not True or supplied.get("pid") != identity["pid"] or supplied.get("creation_filetime") != identity["creation_filetime"] or supplied.get("executable_sha256") != identity["executable"]["sha256"]:
        raise SchemaError("profile process does not match observed PID/creation/executable identity")
    source = value.get("source", {})
    if not isinstance(source, dict):
        raise SchemaError("normalized profile source descriptor must be an object")
    relative = source.get("artifact")
    if not isinstance(relative, str) or not relative.startswith(("external/", "collectors/")):
        raise SchemaError("raw profile source must use a contained external/ or collectors/ artifact path")
    if raw_source.suffix.casefold() not in (".etl", ".json", ".jsonl", ".csv", ".txt", ".gz", ".qdrep", ".nsys-rep"):
        raise SchemaError("raw profile must have an explicit supported evidence suffix")
    expected = {key: source.get(key) for key in ("size", "sha256")}
    if identity_file(raw_source) != expected:
        raise SchemaError("raw profiling source differs from normalized receipt")
    dependencies = profile_dependency_artifacts(run_dir, manifest, value)
    observation_path(str(Path(run_dir) / relative))
    destination = observation_path(str(contained_path(run_dir, relative)))
    if destination.exists():
        if identity_file(destination) != expected:
            raise SchemaError("existing profiling source has different identity")
    else:
        if copy_attachment(raw_source, destination) != expected:
            raise SchemaError("raw profile changed after receipt validation")
    artifacts = [descriptor(kind + "_source", relative, run_dir)]
    relative = f"external/{kind}-{index:04d}.json"
    if copy_attachment(profile, contained_path(run_dir, relative)) != profile_identity:
        raise SchemaError("normalized profile changed after identity validation")
    artifacts.append(descriptor(kind, relative, run_dir))
    for item in dependencies:
        if identity_file(contained_path(run_dir, item["path"], must_exist=True)) != {key: item[key] for key in ("size", "sha256")}:
            raise SchemaError("profile dependency changed during attachment")
    if identity_file(raw_source) != expected or identity_file(profile) != profile_identity:
        raise SchemaError("profiling inputs changed during attachment")
    merged = {item["path"]: item for item in artifacts}
    for item in dependencies:
        if item["path"] in merged:
            if any(merged[item["path"]][key] != item[key] for key in ("size", "sha256")):
                raise SchemaError("profile attachment dependencies contradict preserved source bytes")
        else:
            merged[item["path"]] = item
    return list(merged.values())


def attach_profile(run_dir, profile, raw_source):
    from capture_manifest import file_identity
    from capture_schema import validate_manifest
    root = observation_path(str(run_dir))
    manifest_path = contained_path(root, "manifest.json", must_exist=True)
    lock = root / ".capture-attach.lock"
    with lock.open("x", encoding="utf-8") as stream:
        stream.write("explicit post-run profiling attachment\n")
    try:
        before = file_identity(manifest_path)
        manifest = validate_manifest(read_json(manifest_path), run_dir=root)
        if manifest["state"] not in ("completed", "failed", "interrupted") or manifest["execution"].get("child_may_be_running"):
            raise SchemaError("profile attachment requires a finalized run with no child reported running")
        index = sum(item["kind"] in PROFILE_KINDS for item in manifest["artifacts"])
        artifacts = attach_profile_files(root, manifest, profile, raw_source, index=index)
        if file_identity(manifest_path) != before:
            raise SchemaError("manifest changed during profile attachment")
        previous = f"external/manifest-before-profile-{index:04d}.json"
        if copy_attachment(manifest_path, contained_path(root, previous)) != before:
            raise SchemaError("prior manifest changed while preserving its exact revision")
        manifest["artifacts"].append(descriptor("manifest_revision", previous, root))
        known = {item["path"] for item in manifest["artifacts"]}
        manifest["artifacts"].extend(item for item in artifacts if item["path"] not in known)
        manifest["execution"].setdefault("external_attachments", []).append({"attached_at": utc_now(), "previous_manifest_sha256": before["sha256"], "artifacts": [item["path"] for item in artifacts], "normalized_identity_rewritten": False})
        validate_manifest(manifest, run_dir=root)
        if file_identity(manifest_path) != before:
            raise SchemaError("manifest changed before profile attachment publication")
        atomic_write_json(manifest_path, manifest)
        return artifacts
    finally:
        lock.unlink()


def create_build_receipt(source_root, settings, outputs, symbols=()):
    from capture_manifest import source_fingerprint
    root = observation_path(str(source_root))
    if not isinstance(settings, dict):
        raise SchemaError("build settings must be an explicit JSON object")
    settings_identity = canonical_sha256(settings)
    binaries = [{"path": str(observation_path(str(path))), **identity_file(path)} for path in outputs]
    if not binaries:
        raise SchemaError("build receipt requires explicit existing output files")
    if len({os.path.normcase(item["path"]) for item in binaries}) != len(binaries):
        raise SchemaError("build receipt outputs must be distinct")
    value = {"schema_version": 1, "document_type": "anyps5.build-receipt", "created_at": utc_now(), "source": source_fingerprint(root),
             "settings": copy.deepcopy(settings), "settings_sha256": settings_identity, "outputs": binaries, "symbols": [], "compiler_invocation_verified": False,
             "source_binary_binding": "unverified", "scope": "explicit_postbuild_source_settings_and_file_identity_observation"}
    for symbol, binary in symbols:
        selected = next((item for item in binaries if item["path"] == str(observation_path(str(binary)))), None)
        if selected is None:
            raise SchemaError("symbol pairing must name a recorded output")
        value["symbols"].append({"path": str(observation_path(str(symbol))), **identity_file(symbol), "image_sha256": selected["sha256"], "binding": "declared_pair_not_verified_debug_id"})
    for output in binaries:
        if identity_file(output["path"]) != {key: output[key] for key in ("size", "sha256")}:
            raise SchemaError("build output changed while observing source/settings/output identities")
    return value


def create_symbol_receipt(image, symbol):
    return {"schema_version": 1, "document_type": "anyps5.symbol-receipt", "created_at": utc_now(),
            "image": {"path": str(observation_path(str(image))), **identity_file(image)},
            "symbol_file": {"path": str(observation_path(str(symbol))), **identity_file(symbol)},
            "binding": "declared_pair_not_verified_debug_id", "symbol_resolution": "unknown", "function_ranges": [],
            "limitations": ["Matching file hashes do not verify that debug information belongs to the image; no symbols or unwind frames are inferred."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Observe explicit build/symbol file identities without building or launching a process.")
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build-receipt")
    build.add_argument("--source-root", required=True, type=Path)
    build.add_argument("--settings", required=True, type=Path)
    build.add_argument("--binary", action="append", required=True, type=Path)
    symbols = commands.add_parser("symbol-receipt")
    symbols.add_argument("--image", required=True, type=Path)
    symbols.add_argument("--symbols", required=True, type=Path)
    for command in (build, symbols):
        command.add_argument("--output", required=True, type=Path)
    attachment = commands.add_parser("attach-profile")
    attachment.add_argument("--run-dir", required=True, type=Path)
    attachment.add_argument("--profile", required=True, type=Path)
    attachment.add_argument("--source", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "attach-profile":
            attach_profile(args.run_dir, args.profile, args.source)
        else:
            value = create_build_receipt(args.source_root, read_json(observation_path(str(args.settings))), args.binary) if args.command == "build-receipt" else create_symbol_receipt(args.image, args.symbols)
            atomic_write_json(observation_path(str(args.output)), value, replace=False)
    except (OSError, ValueError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
