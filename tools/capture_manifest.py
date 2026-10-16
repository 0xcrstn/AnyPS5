import argparse
import copy
import hashlib
import os
import re
import stat
import subprocess
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path

import capture_process

from capture_schema import (
    SchemaError,
    atomic_write_json,
    canonical_json_bytes,
    canonical_sha256,
    read_json,
    validate_cache,
    validate_capture,
    validate_manifest,
)


RELEVANT_PREFIXES = ("APS5_", "ANYPS5_", "VK_")
RUNNER_OWNED_ENVIRONMENT = frozenset({"APS5_CAPTURE_RUN_ID"})


def utc_now():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def relevant_environment_name(name):
    return isinstance(name, str) and name.upper().startswith(RELEVANT_PREFIXES)


def build_environment(overrides, inherited=None):
    if not isinstance(overrides, dict):
        raise SchemaError("environment.overrides must be an object")
    child = dict(os.environ if inherited is None else inherited)
    removed = sorted(name for name in child if relevant_environment_name(name))
    for name in removed:
        del child[name]
    normalized_names = set()
    for name in overrides:
        if not relevant_environment_name(name) or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is None:
            raise SchemaError("environment overrides must use APS5_, ANYPS5_ or VK_ names")
        if name.upper() in normalized_names:
            raise SchemaError(f"duplicate environment override: {name.upper()}")
        normalized_names.add(name.upper())
    effective = {}
    for name, value in overrides.items():
        normalized = name.upper()
        if value is not None and (not isinstance(value, str) or "\0" in value):
            raise SchemaError(f"environment override {name} must be a string without NUL or null")
        if value is not None:
            child[normalized] = value
            effective[normalized] = value
    return child, {"effective": dict(sorted(effective.items())), "removed_inherited": removed}


def safe_path(value, name, *, resolve=False):
    if not isinstance(value, str) or not value or "\0" in value:
        raise SchemaError(f"{name} must be an absolute path string")
    path = Path(value)
    if not path.is_absolute():
        raise SchemaError(f"{name} must be absolute")
    if any(part.casefold() == "fakelib" for part in path.parts):
        raise SchemaError(f"{name} must not access fakelib")
    try:
        resolved = path.resolve()
    except (OSError, RuntimeError) as error:
        raise SchemaError(f"cannot resolve {name}: {error}") from error
    if any(part.casefold() == "fakelib" for part in resolved.parts):
        raise SchemaError(f"{name} must not resolve into fakelib")
    return resolved if resolve else Path(os.path.abspath(path))


def required_object(parent, key):
    value = parent.get(key)
    if not isinstance(value, dict):
        raise SchemaError(f"{key} must be an object")
    return value


def required_strings(value, keys, name):
    for key in keys:
        if not isinstance(value.get(key), str) or not value[key].strip() or "\0" in value[key]:
            raise SchemaError(f"{name}.{key} must be a nonempty string without NUL")


def trace_detail(config):
    configured = config["settings"]["diagnostics"]["options"].get("trace_detail")
    environment = {name.upper(): value for name, value in config["environment"]["overrides"].items()}
    explicit = environment.get("APS5_TRACE_DETAIL")
    if configured not in (None, "draw") or explicit not in (None, "draw"):
        raise SchemaError("trace_detail and APS5_TRACE_DETAIL support only the opt-in value draw")
    if configured is not None and "APS5_TRACE_DETAIL" in environment and explicit != configured:
        raise SchemaError("trace detail configuration conflicts with environment override")
    return configured or explicit


def trace_workload(config):
    environment = {name.upper(): value for name, value in config["environment"]["overrides"].items()}
    enabled = environment.get("APS5_TRACE_WORKLOAD")
    if enabled not in (None, "0", "1"):
        raise SchemaError("APS5_TRACE_WORKLOAD must be 0, 1, or null")
    if enabled == "1" and config["capture"]["unit"] != "frames":
        raise SchemaError("measurement workload capture requires a fixed-frame window")
    return enabled == "1"


def validate_config(config):
    if not isinstance(config, dict):
        raise SchemaError("config must be an object")
    canonical_json_bytes(config)
    result = copy.deepcopy(config)
    result["source_root"] = str(safe_path(result.get("source_root"), "source_root", resolve=True))
    runtime = required_object(result, "runtime")
    executable = safe_path(runtime.get("executable"), "runtime.executable", resolve=True)
    libraries = runtime.get("libraries")
    if not isinstance(libraries, list):
        raise SchemaError("runtime.libraries must be a list of explicit file paths")
    normalized = [str(safe_path(value, "runtime.libraries entry", resolve=True)) for value in libraries]
    identities = [value.replace("\\", "/").casefold() for value in (str(executable), *normalized)]
    if len(identities) != len(set(identities)):
        raise SchemaError("runtime files must be distinct")
    result["runtime"] = {"executable": str(executable), "libraries": normalized}
    build = required_object(result, "build")
    if (not isinstance(build.get("compiler"), (str, dict)) or not build["compiler"]
            or isinstance(build["compiler"], str) and not build["compiler"].strip()):
        raise SchemaError("build.compiler must be a nonempty declared string or object")
    required_strings(build, ("build_type",), "build")
    required_object(build, "options")
    if "receipt" in build:
        build["receipt"] = str(safe_path(build["receipt"], "build.receipt", resolve=True))
    machine = required_object(result, "machine")
    required_strings(machine, ("id", "cpu", "gpu", "driver", "os"), "machine")
    required_strings(required_object(result, "title"), ("id", "version"), "title")
    scene = required_object(result, "scene")
    required_strings(scene, ("id",), "scene")
    required_object(scene, "recipe")
    settings = required_object(result, "settings")
    for key in ("resolution", "presentation"):
        if (not isinstance(settings.get(key), (str, dict, list)) or not settings[key]
                or isinstance(settings[key], str) and not settings[key].strip()):
            raise SchemaError(f"settings.{key} must be a nonempty string, object or list")
    validate_cache(required_object(settings, "cache"))
    diagnostics = required_object(settings, "diagnostics")
    if diagnostics.get("mode") not in ("measurement", "attribution", "forensic"):
        raise SchemaError("settings.diagnostics.mode is invalid")
    required_object(diagnostics, "options")
    if "report_detail" in diagnostics["options"] and diagnostics["options"]["report_detail"] not in ("compact", "full"):
        raise SchemaError("settings.diagnostics.options.report_detail must be compact or full")
    relinker = required_object(settings, "relinker")
    flags = relinker.get("flags")
    if not isinstance(flags, list) or any(not isinstance(value, str) or not value.strip() or "\0" in value for value in flags):
        raise SchemaError("settings.relinker.flags must be a list of nonempty strings without NUL")
    validate_capture(result.get("capture"))
    execution = required_object(result, "execution")
    command = execution.get("command")
    if not isinstance(command, list) or not command or any(
            not isinstance(value, str) or "\0" in value for value in command):
        raise SchemaError("execution.command must be a nonempty argv list without NUL")
    command_executable = safe_path(command[0], "execution.command[0]", resolve=True)
    if os.path.normcase(str(command_executable)) != os.path.normcase(str(executable)):
        raise SchemaError("execution.command[0] must identify runtime.executable")
    command[0] = str(command_executable)
    execution["cwd"] = str(safe_path(execution.get("cwd"), "execution.cwd", resolve=True))
    environment = required_object(result, "environment")
    build_environment(environment.get("overrides"), {})
    if any(name.upper() in RUNNER_OWNED_ENVIRONMENT for name in environment["overrides"]):
        raise SchemaError("APS5_CAPTURE_RUN_ID is runner-owned and cannot be overridden")
    if trace_detail(result) == "draw":
        diagnostics["options"]["trace_detail"] = "draw"
    trace_workload(result)
    if "experiment" in result:
        experiment = required_object(result, "experiment")
        required_strings(experiment, ("id",), "experiment")
        if "pair_id" in experiment:
            required_strings(experiment, ("pair_id",), "experiment")
    if "observation" in result:
        result["observation"] = capture_process.validate_observation_config(result["observation"])
    if "collectors" in result:
        from capture_collectors import validate_collectors_config
        result["collectors"] = validate_collectors_config(result["collectors"], check_files=False)
    return result


def file_identity(path):
    path = safe_path(str(path), "identity path")
    initial = path.stat()
    if not stat.S_ISREG(initial.st_mode):
        raise SchemaError(f"not a regular file: {path}")
    digest = hashlib.sha256()
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0)
    with os.fdopen(os.open(path, flags), "rb") as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise SchemaError(f"not a regular file: {path}")
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
        after = os.fstat(source.fileno())
    current = path.stat()
    attributes = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
    changed = any(getattr(initial, key) != getattr(current, key) or getattr(before, key) != getattr(after, key)
                  for key in attributes)
    identity_changed = any(getattr(initial, key) != getattr(before, key) or getattr(after, key) != getattr(current, key)
                           for key in ("st_dev", "st_ino", "st_size"))
    if changed or identity_changed:
        raise SchemaError(f"file changed while hashing: {path}")
    return {"size": after.st_size, "sha256": digest.hexdigest()}


def git_bytes(root, *arguments):
    environment = dict(os.environ)
    for name in list(environment):
        if name.upper().startswith("GIT_"):
            del environment[name]
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    command = ["git", "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false", "-C", str(root)]
    configured = subprocess.run([*command, "config", "--null", "--name-only", "--get-regexp",
                                 r"^filter\..*\.(clean|process|required)$"], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=environment, check=False, shell=False)
    if configured.returncode not in (0, 1):
        raise SchemaError("cannot inspect configured Git content filters safely")
    filters = set()
    for key in configured.stdout.split(b"\0"):
        if key:
            text = os.fsdecode(key)
            if not text.startswith("filter.") or text.rsplit(".", 1)[-1] not in ("clean", "process", "required"):
                raise SchemaError("unexpected configured Git content filter key")
            filters.add(text.rsplit(".", 1)[0])
    for name in sorted(filters):
        command.extend(("-c", f"{name}.clean=", "-c", f"{name}.process=", "-c", f"{name}.required=false"))
    command.extend(arguments)
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment,
                            check=False, shell=False)
    if result.returncode:
        raise SchemaError(f"Git source inspection failed ({arguments[0]}): "
                          f"{result.stderr.decode('utf-8', errors='replace').strip()}")
    return result.stdout


def source_path(root, relative):
    path = root / relative
    if Path(relative).is_absolute() or ".." in Path(relative).parts:
        raise SchemaError("Git source path escapes repository")
    safe_path(str(path), "Git source path")
    if not path.resolve().is_relative_to(root):
        if not path.is_symlink():
            raise SchemaError("Git source path resolves outside repository")
    return path


def diff_bytes(root, *, cached=False):
    arguments = ["diff", "--binary", "--full-index", "--no-ext-diff", "--no-textconv",
                 "--ignore-submodules=none", "--submodule=short", "--no-color", "--no-renames",
                 "--diff-algorithm=myers", "--no-indent-heuristic", "--no-relative",
                 "--src-prefix=a/", "--dst-prefix=b/"]
    if cached:
        arguments.extend(("--cached", "HEAD"))
    arguments.append("--")
    return git_bytes(root, *arguments)


def source_fingerprint(root):
    root = safe_path(str(root), "source_root", resolve=True)
    actual_root = Path(os.fsdecode(git_bytes(root, "rev-parse", "--show-toplevel")).strip()).resolve()
    if os.path.normcase(str(root)) != os.path.normcase(str(actual_root)):
        raise SchemaError("source_root must be the repository worktree root")
    commit = git_bytes(root, "rev-parse", "--verify", "HEAD").decode("ascii").strip()
    entries = git_bytes(root, "ls-files", "--stage", "-z")
    submodules = []
    for entry in entries.split(b"\0"):
        if not entry:
            continue
        metadata, raw_path = entry.split(b"\t", 1)
        mode, index_commit, stage = metadata.split(b" ")
        relative = os.fsdecode(raw_path)
        path = source_path(root, relative)
        if stage != b"0":
            raise SchemaError("cannot fingerprint an unresolved Git index")
        if mode != b"160000":
            continue
        if not path.resolve().is_relative_to(root):
            raise SchemaError("submodule path resolves outside source_root")
        initialized = (path / ".git").exists()
        item = {"path": relative, "index_commit": index_commit.decode("ascii"), "initialized": initialized}
        if initialized:
            item["source"] = source_fingerprint(path)
        submodules.append(item)
    index_diff = diff_bytes(root, cached=True)
    worktree_diff = diff_bytes(root)
    untracked_names = git_bytes(root, "ls-files", "--others", "--exclude-standard", "-z")
    untracked = []
    for raw_path in untracked_names.split(b"\0"):
        if not raw_path:
            continue
        relative = os.fsdecode(raw_path)
        path = source_path(root, relative)
        if path.is_symlink():
            target = os.fsencode(os.readlink(path))
            identity = {"size": len(target), "sha256": hashlib.sha256(target).hexdigest(), "kind": "symlink"}
        else:
            identity = {**file_identity(path), "kind": "file"}
        untracked.append({"path": relative, **identity})
    untracked_after = []
    for item in untracked:
        path = source_path(root, item["path"])
        if item["kind"] == "symlink" and path.is_symlink():
            target = os.fsencode(os.readlink(path))
            identity = {"size": len(target), "sha256": hashlib.sha256(target).hexdigest(), "kind": "symlink"}
        elif item["kind"] == "file" and not path.is_symlink():
            identity = {**file_identity(path), "kind": "file"}
        else:
            raise SchemaError("untracked source changed during inspection")
        untracked_after.append({"path": item["path"], **identity})
    for item in submodules:
        if item["initialized"]:
            if source_fingerprint(root / item["path"]) != item["source"]:
                raise SchemaError("submodule source changed during inspection")
        elif ((root / item["path"]) / ".git").exists():
            raise SchemaError("submodule initialization changed during inspection")
    if (untracked_after != untracked
            or git_bytes(root, "rev-parse", "--verify", "HEAD").decode("ascii").strip() != commit
            or git_bytes(root, "ls-files", "--stage", "-z") != entries
            or git_bytes(root, "ls-files", "--others", "--exclude-standard", "-z") != untracked_names
            or diff_bytes(root, cached=True) != index_diff
            or diff_bytes(root) != worktree_diff):
        raise SchemaError("source checkout changed during inspection")
    diff_hashes = {"index_diff_sha256": hashlib.sha256(index_diff).hexdigest(),
                   "worktree_diff_sha256": hashlib.sha256(worktree_diff).hexdigest()}
    result = {"root": str(root), "commit": commit, "dirty": bool(index_diff or worktree_diff or untracked
              or any(item.get("source", {}).get("dirty", False) for item in submodules)),
              "diff_sha256": canonical_sha256(diff_hashes), **diff_hashes,
              "untracked": sorted(untracked, key=lambda item: item["path"]),
              "submodules": sorted(submodules, key=lambda item: item["path"]),
              "provenance": "inspected_source_checkout", "build_binding": "unverified",
              "untracked_scope": "git_nonignored_files", "ignored_files_included": False,
              "content_filters": "disabled_for_inspection"}
    result["fingerprint_sha256"] = canonical_sha256(source_identity(result))
    return result


def source_identity(source):
    result = {key: copy.deepcopy(value) for key, value in source.items() if key not in ("root", "fingerprint_sha256")}
    for item in result["submodules"]:
        if "source" in item:
            item["source"] = source_identity(item["source"])
    return result


def create_manifest(config, *, source=None, inherited_environment=None, run_id=None, now=None):
    config = validate_config(config)
    timestamp = utc_now() if now is None else now
    inspected_source = source_fingerprint(config["source_root"]) if source is None else copy.deepcopy(source)
    runtime_files = []
    paths = [("executable", config["runtime"]["executable"])]
    paths.extend(("library", path) for path in config["runtime"]["libraries"])
    for role, path in paths:
        runtime_files.append({"role": role, "path": path, **file_identity(path)})
    build = copy.deepcopy(config["build"])
    receipt = build.pop("receipt", None)
    build["receipt_sha256"] = file_identity(receipt)["sha256"] if receipt is not None else None
    build["provenance"] = "declared_configuration"
    build["source_binary_binding"] = "unverified"
    _, environment = build_environment(config["environment"]["overrides"], inherited_environment)
    identifier = str(uuid.uuid4()) if run_id is None else run_id
    if trace_detail(config) == "draw":
        environment["effective"].update(APS5_TRACE_DETAIL="draw", APS5_CAPTURE_RUN_ID=identifier)
    elif trace_workload(config):
        environment["effective"]["APS5_CAPTURE_RUN_ID"] = identifier
    scene = copy.deepcopy(config["scene"])
    scene["recipe_sha256"] = canonical_sha256(scene["recipe"])
    machine = copy.deepcopy(config["machine"])
    machine["provenance"] = "declared_configuration"
    manifest = {"schema_version": 1, "document_type": "anyps5.run-manifest",
                "run_id": identifier,
                "created_at": timestamp, "started_at": None, "ended_at": None,
                "state": "planned", "exit_code": None, "source": inspected_source, "build": build,
                "runtime": {"inventory_kind": "declared_files", "loaded_modules_verified": False,
                            "files": runtime_files}, "machine": machine, "title": config["title"],
                "scene": scene, "settings": config["settings"], "environment": environment,
                "capture": config["capture"], "artifacts": [],
                "execution": {"command": config["execution"]["command"], "cwd": config["execution"]["cwd"],
                              "pid": None, "inventory_verification": {}}}
    if "experiment" in config:
        manifest["experiment"] = config["experiment"]
    if "observation" in config:
        manifest["observation"] = config["observation"]
    if "collectors" in config:
        manifest["collectors"] = config["collectors"]
    validate_manifest(manifest)
    return manifest


def verify_runtime(manifest):
    validate_manifest(manifest)
    findings = []
    for item in manifest["runtime"]["files"]:
        expected = {key: item[key] for key in ("size", "sha256")}
        actual = None
        try:
            actual = file_identity(item["path"])
            reason = "changed" if actual != expected else None
        except FileNotFoundError:
            reason = "missing"
        except SchemaError:
            reason = "unsafe"
        except OSError:
            reason = "unreadable"
        if reason is not None:
            findings.append({"path": item["path"], "reason": reason, "expected": expected, "actual": actual})
    return {"verified": not findings, "findings": findings, "checked_at": utc_now()}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Create local run provenance without building, preparing or launching a game.")
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        manifest = create_manifest(read_json(args.config))
        directory = safe_path(str(args.run_dir.absolute()), "run directory", resolve=True)
        if directory.is_relative_to(Path(manifest["source"]["root"]).resolve()):
            raise SchemaError("run directory must be outside source_root")
        directory.mkdir(parents=False, exist_ok=False)
        atomic_write_json(directory / "manifest.json", manifest, replace=False)
    except (OSError, ValueError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")
    sys.stdout.write(str(directory / "manifest.json") + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
