import argparse
import copy
import math
import re
import statistics
import sys
from datetime import datetime
from pathlib import Path

from capture_schema import (
    SchemaError, atomic_write_json, canonical_sha256, contained_path,
    read_json, validate_manifest,
)


MAX_PAIRS = 1000
MAX_ATTEMPTS = 10000
MAX_RECEIPT_BYTES = 4 * 1024 * 1024
CONDITIONS = ("scene", "readiness", "cache.shader", "cache.pipeline", "cache.driver", "correctness")
WORKLOAD_COUNTS = frozenset({"requested_draw_operations", "dispatch_operations", "host_draw_commands",
                             "host_dispatch_commands", "indirect_record_operations"})
DIAGNOSTIC_ENVIRONMENT = frozenset({
    "APS5_TRACE_DETAIL", "APS5_PROFILE_DRAW", "APS5_PROFILE_GPU", "APS5_TRACE_WAIT",
    "APS5_TRACE_SHADER_PREP", "APS5_TRACE_HANDLED_FAULTS", "APS5_TRACE_WRITE_TRACKING",
    "APS5_DRAW_TRACE_EVENTS", "APS5_DRAW_TRACE_MEMORY_MB", "APS5_DRAW_TRACE_METADATA",
    "APS5_DRAW_TRACE_QUERIES",
    "APS5_DRAW_TRACE_THREAD_CPU", "APS5_DRAW_TRACE_JOURNAL", "APS5_TRACE_GPU_LABELS", "APS5_TRACE_GPU_COUNTERS",
    "APS5_DRAW_TRACE_THREAD_CYCLES", "APS5_DRAW_TRACE_PERFETTO", "APS5_TRACE_WORKLOAD",
})
COLLECTOR_COLLECTION_FIELDS = ("enabled", "kind", "wpr", "profile", "max_duration_seconds",
                               "max_trace_bytes", "command_timeout_seconds", "export")
OBSERVER_COLLECTION_FIELDS = ("enabled", "interval_ms", "max_samples", "max_output_bytes", "hash_roots",
                              "cache_locations", "effective_settings_files", "max_module_hash_bytes", "max_total_hash_bytes")
ATTACHMENT_FIELDS = frozenset({"build_receipts", "symbol_receipts", "profiling_evidence"})


def _require(condition, message):
    if not condition:
        raise SchemaError(message)


def _text(value, name):
    _require(isinstance(value, str) and bool(value.strip()) and len(value) <= 4096 and "\0" not in value,
             f"{name} must be a nonempty bounded string")
    return value


def _utc(value, name):
    _text(value, name)
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise SchemaError(f"{name} must be a UTC timestamp") from error
    _require("T" in value and parsed.utcoffset() is not None and parsed.utcoffset().total_seconds() == 0,
             f"{name} must be a UTC timestamp")
    return parsed


def _sealed(document, field):
    value = copy.deepcopy(document)
    value.pop(field, None)
    return canonical_sha256(value)


def _differences(before, after, prefix=""):
    if isinstance(before, dict) and isinstance(after, dict):
        result = []
        for key in sorted(before.keys() | after.keys()):
            child = prefix + "/" + key.replace("~", "~0").replace("/", "~1")
            if key not in before or key not in after:
                result.append(child)
            else:
                result.extend(_differences(before[key], after[key], child))
        return result
    return [] if before == after else [prefix]


def _allowed_difference(path, allowed):
    return any(path == prefix or path.startswith(prefix + "/") for prefix in allowed)


def plan_experiment(spec, configurations):
    from capture_manifest import validate_config
    _require(isinstance(spec, dict), "experiment specification must be an object")
    identifier = _text(spec.get("id"), "id")
    _require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", identifier) is not None, "unsafe experiment id")
    mode = spec.get("mode")
    _require(mode in ("aa", "overhead", "ab"), "mode must be aa, overhead or ab")
    pairs = spec.get("pairs", 5)
    _require(type(pairs) is int and 1 <= pairs <= MAX_PAIRS, "pairs must be in 1..1000")
    ordering = spec.get("ordering", "alternating")
    _require(ordering in ("alternating", "randomized"), "unsupported ordering")
    seed = spec.get("seed", "0")
    _text(seed, "seed")
    _require(isinstance(configurations, dict) and set(configurations) == {"A", "B"}, "configurations require A and B")
    configs = {key: validate_config(copy.deepcopy(value)) for key, value in configurations.items()}
    for value in configs.values():
        value.pop("experiment", None)
    differences = _differences(configs["A"], configs["B"])
    treatment = spec.get("treatment_paths", [])
    _require(isinstance(treatment, list) and len(treatment) <= 64 and all(isinstance(path, str) for path in treatment)
             and len(set(treatment)) == len(treatment),
             "treatment_paths must be a unique bounded list")
    for path in treatment:
        _require(isinstance(path, str) and path.startswith("/") and path != "/"
                 and not _allowed_difference(path, ("/scene", "/capture", "/machine", "/title")),
                 "treatment paths must be specific JSON pointers")
    if mode == "aa":
        _require(not differences and not treatment, "AA requires identical effective configurations")
    elif mode == "overhead":
        allowed = (["/settings/diagnostics"] + ["/environment/overrides/" + key for key in DIAGNOSTIC_ENVIRONMENT]
                   + ["/observation/" + key for key in OBSERVER_COLLECTION_FIELDS]
                   + ["/collectors/" + key for key in COLLECTOR_COLLECTION_FIELDS])
        _require(bool(differences) and all(_allowed_difference(path, allowed) for path in differences),
                 "overhead allows diagnostic treatment only, with the same build and runtime")
        _require(all(_allowed_difference(path, treatment) for path in differences), "declare every diagnostic treatment difference")
    else:
        _require(bool(differences) and all(_allowed_difference(path, treatment) for path in differences),
                 "AB requires explicit treatment paths covering every difference")
    metric = spec.get("metric", {"name": "mean_frame_ms", "unit": "ms", "direction": "lower"})
    _require(isinstance(metric, dict) and metric.get("direction") in ("lower", "higher"), "metric direction required")
    _text(metric.get("name"), "metric.name")
    _require(metric.get("unit") in ("ms", "ns", "bytes", "count", "percent", "cycles"), "explicit supported metric unit required")
    tools = copy.deepcopy(spec.get("external_tools", []))
    _require(isinstance(tools, list) and len(tools) <= 16, "external_tools must be bounded")
    for packet in tools:
        _require(isinstance(packet, dict) and packet.get("measurement") in ("live", "replay"), "tool measurement must be live or replay")
        _text(packet.get("id"), "tool.id")
        _require(type(packet.get("maximum_seconds")) is int and 1 <= packet["maximum_seconds"] <= 3600,
                 "external tool requires a bounded collection duration")
        for phase in ("start", "stop", "export"):
            argv = packet.get(phase)
            _require(isinstance(argv, list) and 1 <= len(argv) <= 64, f"tool {phase} must be an explicit argv packet")
            for argument in argv:
                _text(argument, f"tool {phase} argument")
        packet["lifecycle"] = {"start": "planned", "stop": "planned", "export": "planned", "observation": "unavailable"}
        packet["requires_explicit_user_approval"] = True
    _require(len({packet["id"] for packet in tools}) == len(tools), "external tool ids must be unique")
    schedule = []
    for index in range(pairs):
        reverse = (index % 2 == 1) if ordering == "alternating" else (
            int(canonical_sha256({"seed": seed, "pair": index})[:2], 16) % 2 == 1)
        for variant in ("BA" if reverse else "AB"):
            pair_id = f"pair-{index + 1:04d}"
            config = copy.deepcopy(configs[variant])
            config["experiment"] = {"id": identifier, "pair_id": pair_id}
            schedule.append({"slot_id": f"{pair_id}-{variant}", "pair_id": pair_id, "variant": variant,
                             "config_sha256": canonical_sha256(config), "config": config})
    result = {"schema_version": 1, "document_type": "anyps5.experiment-plan", "id": identifier,
              "mode": mode, "pairs": pairs, "ordering": ordering, "seed": seed,
              "ordering_algorithm": "alternating-balanced-pairs" if ordering == "alternating" else "sha256-seed-pair-low-bit-v1",
              "treatment_paths": copy.deepcopy(treatment), "configuration_differences": differences, "metric": copy.deepcopy(metric),
              "external_tools": copy.deepcopy(tools), "schedule": schedule,
              "execution": {"state": "planned", "requires_explicit_user_approval": True,
                            "launch_supported": False, "cache_deletion_supported": False},
              "limitations": ["A schedule is planned data, not evidence of collector or game execution.",
                              "Five matched pairs and verified conditions are required for an eligible experiment.",
                              "Low-overhead evidence may leave workload equivalence unproven."]}
    result["plan_sha256"] = _sealed(result, "plan_sha256")
    return result


def validate_plan(plan):
    _require(isinstance(plan, dict) and plan.get("document_type") == "anyps5.experiment-plan" and plan.get("schema_version") == 1,
             "expected experiment plan version 1")
    _require(plan.get("plan_sha256") == _sealed(plan, "plan_sha256"), "plan hash mismatch")
    schedule = plan.get("schedule")
    _require(isinstance(schedule, list) and len(schedule) == 2 * plan.get("pairs", 0), "invalid schedule size")
    configurations = {}
    for slot in schedule:
        _require(isinstance(slot, dict), "schedule slot must be an object")
        _require(slot.get("variant") in ("A", "B"), "invalid slot variant")
        _require(slot.get("config_sha256") == canonical_sha256(slot.get("config")), "slot configuration hash mismatch")
        configurations.setdefault(slot["variant"], slot["config"])
    rebuilt = plan_experiment(plan, configurations)
    _require(rebuilt == plan, "plan is not the deterministic schedule of its configurations")
    return plan


def _artifact_reference(reference, manifest, run_dir):
    from capture_manifest import file_identity
    _require(isinstance(reference, dict), "artifact reference must be an object")
    path = reference.get("path")
    _text(path, "artifact path")
    key = (path, reference.get("sha256"), reference.get("size"))
    _require(any((item["path"], item["sha256"], item["size"]) == key for item in manifest.get("artifacts", [])),
             "condition reference is not a bound manifest artifact")
    resolved = contained_path(run_dir, path, must_exist=True)
    _require(resolved.is_file() and file_identity(resolved) == {"size": key[2], "sha256": key[1]},
             "condition artifact integrity mismatch")
    return reference


def _external_reference(reference):
    from capture_process import observation_path, identity_file
    _require(isinstance(reference, dict), "external artifact reference must be an object")
    path = observation_path(reference.get("path"))
    _require(type(reference.get("size")) is int and reference["size"] >= 0
             and re.fullmatch(r"[0-9a-f]{64}", reference.get("sha256", "")) is not None, "external artifact hash and size required")
    _require(path.is_file() and identity_file(path) == {"size": reference["size"], "sha256": reference["sha256"]},
             "external artifact integrity mismatch")
    return reference


def _coverage_complete(references, capture, role):
    intervals = []
    for reference in references:
        if reference.get("role") != role:
            continue
        coverage = reference.get("coverage")
        _require(isinstance(coverage, dict) and coverage.get("unit") == capture["unit"], "artifact coverage unit mismatch")
        start, length = coverage.get("start"), coverage.get("length")
        _require(type(start) is int and type(length) is int and start >= 0 and length > 0, "invalid artifact coverage")
        if reference.get("media") == "image":
            _require(capture["unit"] == "frames" and length == 1, "one image can cover only one sampled frame")
        _require(reference.get("media") in ("image", "sequence", "video", "measurement", "snapshot"), "unsupported evidence media")
        intervals.append((start, start + length))
    cursor = capture["start"]
    for start, end in sorted(intervals):
        if start > cursor:
            break
        cursor = max(cursor, end)
    return cursor >= capture["start"] + capture["length"]


def assess_condition_receipt(document, manifest, run_dir, *, capture_id=None):
    _require(isinstance(document, dict) and document.get("document_type") == "anyps5.capture-conditions"
             and document.get("schema_version") == 1, "expected capture conditions version 1")
    _require(document.get("run_id") == manifest["run_id"], "condition run binding mismatch")
    capture_binding_unknown = document.get("capture_id") is not None and capture_id is None
    if capture_id is not None and document.get("capture_id") is not None:
        _require(document.get("capture_id") == capture_id, "condition capture binding mismatch")
    _require(document.get("capture") == manifest["capture"], "condition window binding mismatch")
    claims = document.get("claims")
    _require(isinstance(claims, dict) and set(claims).issubset(CONDITIONS), "unknown condition claims")
    dimensions, verified = {}, set()
    for name in CONDITIONS:
        claim = claims.get(name)
        dimension = {"status": "unknown", "basis": "unavailable", "reasons": ["No reviewed condition evidence supplied."]}
        dimensions[name] = dimension
        if claim is None:
            continue
        _require(isinstance(claim, dict), f"{name} claim must be an object")
        if claim.get("state") == "unknown":
            continue
        _require("state" in claim, "condition state required")
        _require(claim.get("method") == "artifact_review", "condition claims require artifact_review")
        _text(claim.get("reviewer"), "reviewer")
        reviewed = _utc(claim.get("reviewed_at"), "reviewed_at")
        if manifest.get("ended_at"):
            _require(reviewed >= _utc(manifest["ended_at"], "ended_at"), "review predates finalized capture")
        _text(claim.get("observations"), "observations")
        criteria = claim.get("criteria")
        _require(isinstance(criteria, list) and 1 <= len(criteria) <= 64, "explicit review criteria required")
        for criterion in criteria:
            _require(isinstance(criterion, dict) and criterion.get("result") in ("passed", "failed", "unavailable"), "invalid review criterion")
            _text(criterion.get("id"), "criterion.id")
            _text(criterion.get("observation"), "criterion.observation")
        _require(len({criterion["id"] for criterion in criteria}) == len(criteria), "duplicate criterion id")
        references = claim.get("artifacts")
        _require(isinstance(references, list) and 1 <= len(references) <= 256, "review requires bounded artifact references")
        for reference in references:
            _require(isinstance(reference, dict), "artifact reference must be an object")
            key = (reference.get("path"), reference.get("sha256"), reference.get("size"))
            _require(all(type(value) in (str, int) for value in key), "artifact reference identity fields required")
            if key not in verified:
                _artifact_reference(reference, manifest, run_dir)
                verified.add(key)
        dimension.update(basis="reviewed_claim", reasons=[], state=claim["state"], reviewer=claim["reviewer"],
                         reviewed_at=claim["reviewed_at"], criteria=copy.deepcopy(criteria), artifacts=copy.deepcopy(references))
        if any(criterion["result"] == "failed" for criterion in criteria):
            dimension.update(status="invalid", reasons=["Review explicitly reports a failed criterion."])
            continue
        if any(criterion["result"] != "passed" for criterion in criteria):
            dimension["reasons"].append("Review criteria remain unavailable.")
        if claim.get("scope") != "whole_window":
            dimension["reasons"].append("Review coverage is sampled, not the whole window.")
        if name == "scene":
            _require(claim.get("state") == "matched" and claim.get("scene_id") == manifest["scene"]["id"]
                     and claim.get("recipe_sha256") == canonical_sha256(manifest["scene"]["recipe"]), "scene recipe binding mismatch")
            dimension.update(scene_id=claim["scene_id"], recipe_sha256=claim["recipe_sha256"])
        elif name == "readiness":
            _require(claim.get("state") == "ready", "readiness state must be ready or unknown")
            _text(claim.get("checkpoint"), "readiness checkpoint")
            dimension["checkpoint"] = claim["checkpoint"]
        elif name == "correctness":
            _require(claim.get("state") == "preserved", "correctness state must be preserved or unknown")
            _text(claim.get("reference_identity"), "correctness reference identity")
            dimension["reference_identity"] = claim["reference_identity"]
            actual_paths = {contained_path(run_dir, reference["path"], must_exist=True) for reference in references if reference.get("role") == "actual"}
            reference_paths = {contained_path(run_dir, reference["path"], must_exist=True) for reference in references if reference.get("role") == "reference"}
            _require(not actual_paths.intersection(reference_paths), "correctness cannot compare an artifact path with itself")
            _require(not any(actual.samefile(reference) for actual in actual_paths for reference in reference_paths),
                     "correctness cannot compare hardlink aliases of the same file")
            if not _coverage_complete(references, manifest["capture"], "reference"):
                dimension["reasons"].append("Reference evidence does not cover the declared window.")
        else:
            _require(claim.get("state") in ("cold", "warm", "disabled"), "unsupported cache state")
            _text(claim.get("policy_id"), "cache policy id")
            dimension["policy_id"] = claim["policy_id"]
            dimension["observation_scope"] = "declared_application_files"
            if name == "cache.driver":
                dimension["observation_scope"] = claim.get("observation_scope", "unverified_global_driver_state")
                if claim.get("observation_scope") != "vendor_measured" or not claim.get("measurement_method"):
                    dimension["reasons"].append("Application file snapshots do not verify global driver cache state.")
                else:
                    _text(claim["measurement_method"], "driver measurement method")
                    measurements = [reference for reference in references if reference.get("role") == "actual"
                                    and reference.get("media") == "measurement" and reference.get("evidence_type") == "vendor_cache_state"]
                    if not measurements:
                        dimension["reasons"].append("Concrete vendor cache-state measurement artifacts are unavailable.")
                    elif not _coverage_complete(measurements, manifest["capture"], "actual"):
                        dimension["reasons"].append("Vendor cache-state measurements do not cover the declared window.")
                    for reference in measurements:
                        tool = reference.get("tool")
                        _require(isinstance(tool, dict), "vendor cache measurement requires tool identity")
                        _text(tool.get("name"), "vendor cache tool name")
                        _text(tool.get("version"), "vendor cache tool version")
        if not _coverage_complete(references, manifest["capture"], "actual"):
            dimension["reasons"].append("Actual evidence does not cover the declared window.")
        if not dimension["reasons"]:
            dimension["status"] = "valid"
        if capture_binding_unknown and dimension["status"] != "invalid":
            dimension["status"] = "unknown"
            dimension["reasons"].append("Declared capture UUID has not been checked against a freshly decoded native sidecar.")
    status = "invalid" if any(item["status"] == "invalid" for item in dimensions.values()) else (
        "valid" if all(item["status"] == "valid" for item in dimensions.values()) else "unknown")
    return {"schema_version": 1, "document_type": "anyps5.condition-report", "run_id": manifest["run_id"],
            "status": status, "dimensions": dimensions,
            "limitations": ["Hashes verify evidence bytes and run bindings, not the semantic truth of a review.",
                            "Accepted condition claims remain reviewer claims, distinct from automatically observed instrumentation.",
                            "Application cache snapshots do not establish global driver cache warmth."]}


def assess_run_conditions(run_dir, manifest=None, *, capture_id=None):
    directory = Path(run_dir).resolve(strict=True)
    manifest = validate_manifest(read_json(directory / "manifest.json"), run_dir=directory) if manifest is None else manifest
    selected = [item for item in manifest["artifacts"] if item["kind"] == "condition_receipt"]
    if not selected:
        return {"schema_version": 1, "document_type": "anyps5.condition-report", "run_id": manifest["run_id"],
                "status": "unknown", "dimensions": {name: {"status": "unknown", "basis": "unavailable",
                "reasons": ["No selected condition receipt."]} for name in CONDITIONS}, "limitations": []}
    _require(len(selected) == 1, "exactly one condition receipt may be selected")
    reference = _artifact_reference(selected[0], manifest, directory)
    _require(reference["size"] <= MAX_RECEIPT_BYTES, "condition receipt exceeds 4 MiB")
    document = read_json(contained_path(directory, reference["path"], must_exist=True))
    _artifact_reference(reference, manifest, directory)
    return assess_condition_receipt(document, manifest, directory, capture_id=capture_id)


def attach_conditions(run_dir, receipt, evidence=()):
    from capture_process import observation_path, copy_attachment, descriptor, identity_file
    root = observation_path(str(run_dir))
    receipt = observation_path(str(receipt))
    _require(receipt.stat().st_size <= MAX_RECEIPT_BYTES, "condition receipt exceeds 4 MiB")
    identity = identity_file(receipt)
    document = read_json(receipt)
    _require(identity_file(receipt) == identity, "condition receipt changed while reading")
    _require(isinstance(evidence, (list, tuple)) and len(evidence) <= 256, "attachment evidence must be bounded")
    manifest_path = contained_path(root, "manifest.json", must_exist=True)
    lock = root / ".capture-attach.lock"
    with lock.open("x", encoding="utf-8") as stream:
        stream.write("explicit post-run condition attachment\n")
    try:
        before = identity_file(manifest_path)
        manifest = validate_manifest(read_json(manifest_path), run_dir=root)
        _require(manifest["state"] in ("completed", "failed", "interrupted")
                 and not manifest["execution"].get("child_may_be_running"), "conditions require a finalized run with no child reported running")
        _require(not any(item["kind"] == "condition_receipt" for item in manifest["artifacts"]),
                 "a selected condition receipt already exists; preserve it rather than silently replacing review")
        additions = []
        paths = {item["path"] for item in manifest["artifacts"]}
        sources = {}
        for item in evidence:
            _require(isinstance(item, dict) and set(item) == {"path", "source"}, "evidence requires contained path and explicit source")
            _require(isinstance(item["path"], str) and item["path"] not in sources, "duplicate attachment destination")
            sources[item["path"]] = observation_path(item["source"])
        _require(isinstance(document, dict) and isinstance(document.get("claims"), dict), "condition receipt claims must be an object")
        correctness = document["claims"].get("correctness", {})
        references = correctness.get("artifacts", []) if isinstance(correctness, dict) else []
        _require(isinstance(references, list) and len(references) <= 256
                 and all(isinstance(item, dict) and isinstance(item.get("path"), str) for item in references),
                 "correctness references must be a bounded artifact list")
        actual_origins = [sources.get(item["path"], contained_path(root, item["path"])) for item in references if item.get("role") == "actual"]
        reference_origins = [sources.get(item["path"], contained_path(root, item["path"])) for item in references if item.get("role") == "reference"]
        _require(not any(actual.samefile(reference) for actual in actual_origins for reference in reference_origins),
                 "condition attachment cannot copy one original file into both correctness roles")
        for item in evidence:
            _require(isinstance(item, dict) and set(item) == {"path", "source"}, "evidence requires contained path and explicit source")
            relative = item["path"]
            _require(isinstance(relative, str) and relative.startswith("external/conditions/"), "condition copies must use external/conditions/ paths")
            _require(relative not in paths, "condition evidence path already selected")
            destination = contained_path(root, relative)
            copy_attachment(observation_path(item["source"]), destination)
            additions.append(descriptor("condition_evidence", relative, root))
            paths.add(relative)
        manifest["artifacts"].extend(additions)
        assessment = assess_condition_receipt(document, manifest, root)
        _require(identity_file(receipt) == identity, "receipt changed after condition validation")
        relative = "external/conditions/receipt.json"
        _require(copy_attachment(receipt, contained_path(root, relative)) == identity,
                 "copied condition receipt differs from the assessed bytes")
        _require(identity_file(manifest_path) == before, "manifest changed during condition attachment")
        revision = sum(item["kind"] == "manifest_revision" for item in manifest["artifacts"])
        previous = f"external/manifest-before-conditions-{revision:04d}.json"
        _require(copy_attachment(manifest_path, contained_path(root, previous)) == before,
                 "previous manifest backup differs from the assessed bytes")
        manifest["artifacts"].extend([descriptor("manifest_revision", previous, root), descriptor("condition_receipt", relative, root)])
        validate_manifest(manifest, run_dir=root)
        _require(identity_file(manifest_path) == before, "manifest changed before condition publication")
        atomic_write_json(manifest_path, manifest)
        return {"run_id": manifest["run_id"], "receipt": relative, "previous_manifest": previous, "assessment": assessment}
    finally:
        lock.unlink()


def _fresh_assessment(directory):
    from capture_health import assess_capture
    from capture_process import observation_path, identity_file
    directory = observation_path(str(directory))
    manifest_path = directory / "manifest.json"
    before = identity_file(manifest_path)
    manifest = validate_manifest(read_json(manifest_path), run_dir=directory)
    for artifact in manifest["artifacts"]:
        _artifact_reference(artifact, manifest, directory)
    health = assess_capture(directory, manifest=manifest)
    _require(identity_file(manifest_path) == before, "manifest changed during fresh experiment assessment")
    return manifest, health


def _metric(health, specification):
    name, unit = specification["name"], specification["unit"]
    frames = health.get("frame_metrics", {})
    if name == "mean_frame_ms" and unit == "ms" and frames.get("status") == "valid":
        value = frames.get("metrics", {}).get(name)
    else:
        evidence = health.get("evidence", {})
        availability = evidence.get("metric_availability", {}).get(name)
        valid = availability == "valid" or isinstance(availability, dict) and availability.get("status") == "valid"
        value = evidence.get("metrics", {}).get(name) if valid and evidence.get("metric_units", {}).get(name) == unit else None
    return value if type(value) in (int, float) and math.isfinite(value) and value >= 0 else None


def _bound_config(manifest, config):
    for key in ("title", "settings", "capture", "experiment"):
        _require(manifest.get(key) == config.get(key), f"run does not match planned {key}")
    _require(manifest["scene"]["id"] == config["scene"]["id"] and manifest["scene"]["recipe"] == config["scene"]["recipe"], "planned scene mismatch")
    _require({key: manifest["machine"].get(key) for key in config["machine"]} == config["machine"], "planned machine mismatch")
    _require(manifest["execution"]["command"] == config["execution"]["command"] and manifest["execution"]["cwd"] == config["execution"]["cwd"], "planned execution mismatch")
    collection = lambda value: {key: item for key, item in (value or {}).items() if key not in ATTACHMENT_FIELDS}
    _require(collection(manifest.get("observation")) == collection(config.get("observation")), "planned observer collection policy mismatch")
    _require(manifest.get("collectors") == config.get("collectors"), "planned collector collection policy mismatch")
    _require(Path(manifest["source"]["root"]).resolve() == Path(config["source_root"]).resolve(), "planned source root mismatch")
    _require({key: manifest["build"].get(key) for key in config["build"] if key != "receipt"}
             == {key: value for key, value in config["build"].items() if key != "receipt"}, "planned build mismatch")
    expected = [("executable", config["runtime"]["executable"])] + [("library", path) for path in config["runtime"]["libraries"]]
    _require(sorted((item["role"], item["path"]) for item in manifest["runtime"]["files"]) == sorted(expected), "planned runtime inventory mismatch")
    from capture_manifest import build_environment, trace_detail
    effective = build_environment(config["environment"]["overrides"], {})[1]["effective"]
    actual = dict(manifest["environment"]["effective"])
    for key in ("APS5_TRACE_DIR", "APS5_TRACE_FRAMES", "APS5_TRACE_SECONDS", "APS5_CAPTURE_RUN_ID"):
        actual.pop(key, None)
    if trace_detail(config) == "draw":
        effective["APS5_TRACE_DETAIL"] = "draw"
    _require(actual == effective, "planned effective environment mismatch")


def workload_observations(health):
    evidence = health.get("evidence", {})
    lightweight = health.get("measurement_workload", {})
    if lightweight.get("status") == "invalid" or evidence.get("dimensions", {}).get("workload", {}).get("status") == "invalid":
        return "invalid", {}
    sources = []
    if evidence.get("dimensions", {}).get("workload", {}).get("status") == "valid":
        if evidence.get("run_id") not in (None, health.get("run_id")):
            return "invalid", {}
        values = {name[len("workload."):]: value for name, value in evidence.get("metrics", {}).items()
                  if name.startswith("workload.") and (name[len("workload."):] in WORKLOAD_COUNTS
                  or name.startswith("workload.operation_status."))
                  and evidence.get("metric_units", {}).get(name) == "count"
                  and evidence.get("metric_availability", {}).get(name) == "valid"}
        sources.append(values)
    if lightweight.get("status") == "valid":
        if lightweight.get("run_id") != health.get("run_id") or not health.get("run_id"):
            return "invalid", {}
        values = {name: value for name, value in lightweight.get("metrics", {}).items()
                  if name in WORKLOAD_COUNTS and lightweight.get("metric_units", {}).get(name) == "count"}
        values.update({name: value for name, value in lightweight.get("observed_counts", {}).items()
                       if name in {"operation_status." + status for status in ("complete", "skipped", "rejected", "failed", "cancelled")}})
        sources.append(values)
    combined = {}
    complete = False
    for values in sources:
        if any(type(value) is not int or not 0 <= value <= (1 << 64) - 1 for value in values.values()):
            return "invalid", {}
        complete = complete or WORKLOAD_COUNTS.issubset(values)
        for name, value in values.items():
            if name in combined and combined[name] != value:
                return "invalid", {}
            combined[name] = value
    if not complete:
        return "unknown", {}
    return "valid", {"workload." + name: value for name, value in combined.items()
                     if name in WORKLOAD_COUNTS or value != 0}


def collector_tool_observations(manifest, health):
    if not (manifest.get("collectors") or {}).get("enabled"):
        return "not_requested", []
    report = health.get("collector_observation", {})
    status = report.get("tool_identity_status", "unknown")
    if status != "valid":
        return "invalid" if status == "invalid" else "unknown", []
    if report.get("run_id") != manifest["run_id"]:
        return "invalid", []
    identities = report.get("tool_identities")
    if not isinstance(identities, list) or not 1 <= len(identities) <= 64:
        return "unknown", []
    result = []
    for identity in identities:
        if not isinstance(identity, dict) or any(key not in identity for key in ("role", "path", "size", "sha256", "file_version", "version_source")):
            return "unknown", []
        if (not isinstance(identity["role"], str) or not identity["role"] or not isinstance(identity["path"], str)
                or not identity["path"] or type(identity["size"]) is not int or identity["size"] < 0
                or not isinstance(identity["sha256"], str) or re.fullmatch(r"[0-9a-f]{64}", identity["sha256"]) is None):
            return "invalid", []
        result.append({key: identity[key] for key in ("role", "path", "size", "sha256", "file_version", "version_source")})
    return "valid", result


def assess_experiment(plan, attempts, *, assessor=None):
    validate_plan(plan)
    _require(isinstance(attempts, list) and len(attempts) <= MAX_ATTEMPTS, "attempt history exceeds bound")
    slots = {item["slot_id"]: item for item in plan["schedule"]}
    selected, history, used_ids, used_paths = {}, [], set(), set()
    cohort_identities, intervals, trace_hashes, capture_ids = {}, [], set(), set()
    cohort_collector_tools = {}
    observed_context = None
    reasons, rows = [], []
    assessor = _fresh_assessment if assessor is None else assessor
    for attempt in attempts:
        _require(isinstance(attempt, dict) and attempt.get("slot_id") in slots, "attempt refers to unknown slot")
        _require(attempt.get("selection") in ("selected", "rejected"), "attempt selection required")
        if attempt["selection"] == "rejected":
            _text(attempt.get("reason"), "rejection reason")
            _external_reference(attempt.get("evidence"))
        history.append(copy.deepcopy(attempt))
        if attempt["selection"] != "selected":
            reasons.append(f"{attempt['slot_id']}: rejected attempt requires interpretation; selection bias remains possible")
            continue
        _require(attempt["slot_id"] not in selected, "multiple selected attempts for one slot")
        directory = Path(_text(attempt.get("run_dir"), "run_dir"))
        _require(directory.is_absolute(), "run directory must be absolute")
        directory = directory.resolve(strict=True)
        _require(directory not in used_paths, "run directory reused")
        used_paths.add(directory)
        manifest, health = assessor(directory)
        _require(manifest["run_id"] not in used_ids and health.get("run_id") == manifest["run_id"], "run identity reused or health binding mismatch")
        used_ids.add(manifest["run_id"])
        slot = slots[attempt["slot_id"]]
        _bound_config(manifest, slot["config"])
        _require(manifest.get("state") == "completed" and manifest.get("exit_code") == 0, "selected run did not complete successfully")
        _require(health.get("status") != "invalid", "selected capture has invalid evidence")
        if health.get("frame_metrics", {}).get("status") != "valid":
            reasons.append(f"{slot['slot_id']}: frame window unavailable")
        for gate in ("runtime_boundaries_verified", "producer_revision_verified", "process_identity_verified"):
            if health.get("provenance", {}).get(gate) is not True:
                reasons.append(f"{slot['slot_id']}: {gate} unproven")
        if manifest.get("observation", {}).get("enabled") is True and health.get("provenance", {}).get("process_creation_identity_verified") is not True:
            reasons.append(f"{slot['slot_id']}: process creation identity unproven")
        context = {key: health.get("observed_context", {}).get(key) for key in ("gpu", "driver", "os", "resolution")}
        if not all(value is not None and value != "" for value in context.values()):
            reasons.append(f"{slot['slot_id']}: observed hardware or resolution unavailable")
        if observed_context is not None:
            _require(context == observed_context, "observed machine/context differs between runs")
        observed_context = context
        start = _utc(manifest.get("started_at"), "run started_at")
        end = _utc(manifest.get("ended_at"), "run ended_at")
        _require(end >= start, "run interval reversed")
        for machine, prior_start, prior_end in intervals:
            _require(machine != manifest["machine"]["id"] or end <= prior_start or start >= prior_end,
                     "selected run process intervals overlap on the same machine")
        intervals.append((manifest["machine"]["id"], start, end))
        for artifact in manifest["artifacts"]:
            if artifact["kind"] in ("trace", "draw_trace", "measurement_workload"):
                _require(artifact["sha256"] not in trace_hashes, "trace artifact content reused across runs")
                trace_hashes.add(artifact["sha256"])
        capture_id = health.get("draw_attribution", {}).get("capture_id")
        if capture_id is not None:
            _require(capture_id not in capture_ids, "native capture identity reused")
            capture_ids.add(capture_id)
        identity = {"source": {key: value for key, value in manifest["source"].items() if key != "root"},
                    "build": manifest["build"], "runtime": sorted(
                        ({"role": item["role"], "name": Path(item["path"]).name, "size": item["size"], "sha256": item["sha256"]}
                         for item in manifest["runtime"]["files"]), key=canonical_sha256)}
        prior_identity = cohort_identities.setdefault(slot["variant"], identity)
        _require(identity == prior_identity, "source/build/runtime identity drift within a variant")
        collector_status, collector_tools = collector_tool_observations(manifest, health)
        if collector_status in ("unknown", "invalid"):
            reasons.append(f"{slot['slot_id']}: requested collector tool identity unproven")
        if collector_status == "valid":
            prior_tools = cohort_collector_tools.setdefault(slot["variant"], collector_tools)
            _require(prior_tools == collector_tools, "collector tool identity drift within a variant")
        conditions = health.get("conditions", {})
        for name in CONDITIONS:
            if conditions.get("dimensions", {}).get(name, {}).get("status") != "valid":
                reasons.append(f"{slot['slot_id']}: {name} unproven")
        workload_status, workload = workload_observations(health)
        if workload_status != "valid":
            reasons.append(f"{slot['slot_id']}: workload equivalence unproven")
        value = _metric(health, plan["metric"])
        if value is None:
            reasons.append(f"{slot['slot_id']}: metric unavailable or units unverified")
        row = {"slot_id": slot["slot_id"], "variant": slot["variant"], "pair_id": slot["pair_id"],
               "run_id": manifest["run_id"], "run_dir": str(directory), "metric": value,
               "manifest_content_sha256": canonical_sha256(manifest),
               "artifacts": copy.deepcopy(manifest["artifacts"]),
               "identity": identity,
               "conditions": conditions, "workload": workload, "workload_status": workload_status,
               "collector_tools_status": collector_status, "collector_tools": collector_tools}
        if not {"workload." + key for key in WORKLOAD_COUNTS}.issubset(row["workload"]):
            reasons.append(f"{slot['slot_id']}: comparable workload counts unavailable")
        rows.append(row)
        selected[slot["slot_id"]] = row
    if len(selected) != len(slots):
        reasons.append("Not every scheduled slot has a selected run.")
    if plan["pairs"] < 5:
        reasons.append("Fewer than five independent pairs; descriptive diagnostics only.")
    deltas = []
    for index in range(plan["pairs"]):
        pair = f"pair-{index + 1:04d}"
        a, b = selected.get(pair + "-A"), selected.get(pair + "-B")
        if a is None or b is None:
            continue
        for name in CONDITIONS:
            da = a["conditions"].get("dimensions", {}).get(name, {})
            db = b["conditions"].get("dimensions", {}).get(name, {})
            if name.startswith("cache.") and (da.get("state"), da.get("policy_id")) != (db.get("state"), db.get("policy_id")):
                reasons.append(f"{pair}: cache policies or observed states differ")
            if name == "correctness" and da.get("reference_identity") != db.get("reference_identity"):
                reasons.append(f"{pair}: correctness reference identities differ")
            if name == "readiness" and da.get("checkpoint") != db.get("checkpoint"):
                reasons.append(f"{pair}: readiness checkpoints differ")
        if a["workload"] != b["workload"]:
            reasons.append(f"{pair}: recorded workload counts differ")
        if (slots[pair + "-A"]["config"].get("collectors") == slots[pair + "-B"]["config"].get("collectors")
                and a["collector_tools_status"] == b["collector_tools_status"] == "valid"
                and a["collector_tools"] != b["collector_tools"]):
            reasons.append(f"{pair}: collector tool identities differ under the same collection policy")
        if plan["mode"] in ("aa", "overhead") and a["identity"] != b["identity"]:
            reasons.append(f"{pair}: AA/overhead build or runtime identity differs")
        if a["metric"] is not None and b["metric"] is not None:
            delta = b["metric"] - a["metric"]
            deltas.append({"pair_id": pair, "A": a["metric"], "B": b["metric"], "delta": delta,
                           "delta_percent": 100 * delta / a["metric"] if a["metric"] else None})
    values = [row["delta"] for row in deltas]
    summary = {"count": len(values), "median_delta": statistics.median(values) if values else None,
               "min_delta": min(values) if values else None, "max_delta": max(values) if values else None,
               "stdev_delta": statistics.stdev(values) if len(values) > 1 else None, "unit": plan["metric"]["unit"]}
    return {"schema_version": 1, "document_type": "anyps5.experiment-report", "experiment_id": plan["id"],
            "measurement_role": {"aa": "same_mode_variation", "overhead": "instrumentation_overhead", "ab": "treatment_observations"}[plan["mode"]],
            "plan_sha256": plan["plan_sha256"], "status": "insufficient" if reasons else "eligible",
            "reasons": sorted(set(reasons)), "runs": rows, "attempt_history": history, "pairs": deltas, "summary": summary,
            "conclusion": "inconclusive" if reasons else "descriptive_matched_observations",
            "limitations": ["Eligibility does not establish statistical significance or a performance gain.",
                            "Reviewed correctness and scene claims are not automatic semantic proofs.",
                            "Recorded workload counts do not cover uninstrumented work or every guest function.",
                            "Collector tool identities are bound preserved receipt observations, not current binary rehashes or proof of execution authenticity.",
                            "Instrumentation and replay measurements cannot substitute for live low-overhead timing."]}


def append_hypothesis_event(ledger, event):
    _require(isinstance(ledger, dict) and ledger.get("document_type") == "anyps5.hypothesis-ledger"
             and ledger.get("schema_version") == 1, "expected hypothesis ledger version 1")
    entries = ledger.get("events")
    _require(isinstance(entries, list) and len(entries) < 10000, "hypothesis ledger event bound")
    previous = None
    by_id = {}
    for index, item in enumerate(entries):
        _require(isinstance(item, dict), "hypothesis history event must be an object")
        _require(item.get("sequence") == index and item.get("previous_sha256") == previous
                 and item.get("event_sha256") == _sealed(item, "event_sha256"), "hypothesis history hash chain mismatch")
        _require(item.get("id") not in by_id, "duplicate hypothesis event id")
        by_id[item["id"]] = item
        previous = item["event_sha256"]
    _require(isinstance(event, dict) and event.get("kind") in ("observation", "hypothesis", "prediction", "experiment", "decision", "rejection"), "invalid hypothesis event kind")
    _text(event.get("id"), "hypothesis event id")
    _require(event["id"] not in by_id, "hypothesis event id reused")
    _text(event.get("text"), "hypothesis event text")
    _utc(event.get("at"), "hypothesis event time")
    links = event.get("links", [])
    _require(isinstance(links, list) and len(links) <= 64 and all(link in by_id for link in links), "hypothesis links must reference prior events")
    required = {"hypothesis": "observation", "prediction": "hypothesis", "experiment": "prediction", "decision": "experiment"}
    if event["kind"] in required:
        _require(any(by_id[link]["kind"] == required[event["kind"]] for link in links), "hypothesis lifecycle link missing")
    references = event.get("artifacts", [])
    _require(isinstance(references, list) and 1 <= len(references) <= 64, "hypothesis events require evidence references")
    for reference in references:
        _external_reference(reference)
        if event["kind"] == "observation":
            _require((isinstance(reference.get("source_event"), str) and reference["source_event"].startswith("/events/"))
                     or type(reference.get("line")) is int and reference["line"] > 0,
                     "observation requires a source event or line reference")
    if event["kind"] == "prediction":
        prediction = event.get("prediction")
        _require(isinstance(prediction, dict) and prediction.get("direction") in ("lower", "higher", "unchanged"), "falsifiable prediction required")
        _text(prediction.get("metric"), "prediction metric")
        _text(prediction.get("unit"), "prediction unit")
        threshold = prediction.get("threshold")
        _require(type(threshold) in (int, float) and math.isfinite(threshold) and threshold >= 0, "finite prediction threshold required")
    if event["kind"] == "experiment":
        _require(re.fullmatch(r"[0-9a-f]{64}", event.get("plan_sha256", "")) is not None, "experiment event requires sealed plan hash")
    if event["kind"] == "decision":
        _require(event.get("outcome") == "inconclusive", "supported/contradicted decisions require conclude_hypothesis with a freshly assessed report")
    result = copy.deepcopy(ledger)
    item = copy.deepcopy(event)
    if event["kind"] == "decision":
        item.update(evidence_scope="descriptive_matched_observations", causal_status="unknown",
                    statistical_significance="not_established")
    item.update(sequence=len(entries), previous_sha256=previous)
    item["event_sha256"] = _sealed(item, "event_sha256")
    result["events"].append(item)
    return result


def conclude_hypothesis(ledger, event, plan, attempts, *, report_path):
    return _conclude_hypothesis(ledger, event, plan, attempts, report_path=report_path)


def _conclude_hypothesis(ledger, event, plan, attempts, *, report_path, assessor=None):
    from capture_process import observation_path, identity_file
    report = assess_experiment(plan, attempts, assessor=assessor)
    links = event.get("links", [])
    experiments = [item for item in ledger["events"] if item["id"] in links and item["kind"] == "experiment"]
    _require(len(experiments) == 1 and experiments[0].get("plan_sha256") == plan["plan_sha256"], "decision experiment plan binding mismatch")
    predictions = [item for item in ledger["events"] if item["id"] in experiments[0].get("links", []) and item["kind"] == "prediction"]
    _require(len(predictions) == 1, "decision requires exactly one prediction")
    prediction = predictions[0]["prediction"]
    _require(prediction["metric"] == plan["metric"]["name"] and prediction["unit"] == plan["metric"]["unit"], "prediction metric mismatch")
    value = report["summary"]["median_delta"]
    outcome = "inconclusive"
    if report["status"] == "eligible" and value is not None:
        threshold = prediction["threshold"]
        matched = (value < 0 and value <= -threshold if prediction["direction"] == "lower" else
                   value > 0 and value >= threshold if prediction["direction"] == "higher" else abs(value) <= threshold)
        outcome = "supported" if matched else "contradicted"
    prepared = copy.deepcopy(event)
    prepared["reviewer_note"] = prepared.get("text")
    prepared["text"] = f"Descriptive prediction outcome: {outcome}; causal explanation, statistical significance and performance gain are not established."
    prepared.update(kind="decision", outcome="inconclusive", assessment_sha256=canonical_sha256(report))
    destination = observation_path(str(report_path))
    atomic_write_json(destination, report, replace=False)
    reference = {"path": str(destination), **identity_file(destination)}
    prepared["assessment_artifact"] = reference
    prepared["artifacts"] = list(prepared.get("artifacts", [])) + [reference]
    result = append_hypothesis_event(ledger, prepared)
    result["events"][-1]["outcome"] = outcome
    result["events"][-1]["event_sha256"] = _sealed(result["events"][-1], "event_sha256")
    return result, report


def main(argv=None):
    parser = argparse.ArgumentParser(description="Plan local capture experiments or freshly assess existing runs; never launch anything.")
    commands = parser.add_subparsers(dest="command", required=True)
    planning = commands.add_parser("plan")
    planning.add_argument("--spec", type=Path, required=True)
    planning.add_argument("--config-a", type=Path, required=True)
    planning.add_argument("--config-b", type=Path, required=True)
    planning.add_argument("--output-dir", type=Path, required=True)
    assessing = commands.add_parser("assess")
    assessing.add_argument("--plan", type=Path, required=True)
    assessing.add_argument("--attempts", type=Path, required=True)
    assessing.add_argument("--output", type=Path, required=True)
    attaching = commands.add_parser("attach-conditions")
    attaching.add_argument("--run-dir", type=Path, required=True)
    attaching.add_argument("--receipt", type=Path, required=True)
    attaching.add_argument("--evidence", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "plan":
            plan = plan_experiment(read_json(args.spec), {"A": read_json(args.config_a), "B": read_json(args.config_b)})
            directory = args.output_dir.absolute()
            for slot in plan["schedule"]:
                _require(not directory.resolve().is_relative_to(Path(slot["config"]["source_root"]).resolve()), "plan output must be outside source_root")
            directory.mkdir(parents=False, exist_ok=False)
            atomic_write_json(directory / "plan.json", plan, replace=False)
            for slot in plan["schedule"]:
                atomic_write_json(directory / (slot["slot_id"] + ".json"), slot["config"], replace=False)
            sys.stdout.write(str(directory / "plan.json") + "\n")
            return 0
        if args.command == "attach-conditions":
            report = attach_conditions(args.run_dir, args.receipt, read_json(args.evidence) if args.evidence else [])
            sys.stdout.write(report["receipt"] + "\n")
            return 0
        report = assess_experiment(read_json(args.plan), read_json(args.attempts))
        atomic_write_json(args.output, report, replace=False)
        sys.stdout.write(report["status"] + "\n")
        return 0 if report["status"] == "eligible" else 1
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
