import argparse
import json
import math
import sys
from datetime import datetime
from pathlib import Path

from capture_health import assess_capture
from capture_manifest import source_identity
from capture_schema import (SchemaError, canonical_sha256, contained_path, read_json, sha256_file,
                            validate_comparison, validate_health, validate_manifest)


OUTPUT_ENVIRONMENT = frozenset({"APS5_TRACE_DIR"})
LIMITATIONS = [
    "Changes are descriptive candidate-minus-baseline differences between run-level medians; frames are not independent repetitions.",
    "Ranges describe observed repeat variation, not confidence intervals or statistical significance.",
    "Declared machine, scene, cache and configuration metadata are not independent observations of those conditions.",
    "Comparable means descriptive cohort comparison; controlled experiment eligibility requires capture_experiment with reviewed conditions and workload evidence.",
    "Source checkout fingerprints do not prove which source produced the supplied runtime binaries; source-to-binary binding is unverified.",
    "Declared runtime file hashes do not prove which modules the process loaded; loaded modules are unverified.",
    "Collector tool identities are preserved receipt observations; their comparison does not authenticate execution or rehash currently installed tools.",
    "Legacy producer attribution is incomplete: a false dropped flag does not establish per-thread coverage, short slices may be omitted, and late GPU work may be uncollected.",
]


def _environment(manifest, health):
    excluded = set(OUTPUT_ENVIRONMENT)
    detail = health.get("draw_attribution", {})
    lightweight = health.get("measurement_workload", {})
    if detail.get("correlation_valid") is True or (
            lightweight.get("correlation_valid") is True and lightweight.get("run_id") == manifest["run_id"]):
        excluded.add("APS5_CAPTURE_RUN_ID")
    return {key.upper(): value for key, value in manifest["environment"]["effective"].items()
            if key.upper() not in excluded}


def _context(manifest, health):
    from capture_experiment import collector_tool_observations
    collector_status, collector_tools = collector_tool_observations(manifest, health)
    build = {key: value for key, value in manifest["build"].items() if key != "receipt_sha256"}
    observation = manifest.get("observation")
    if observation is not None:
        observation = {key: value for key, value in observation.items()
                       if key not in ("build_receipts", "symbol_receipts", "profiling_evidence")}
    return {"machine": manifest["machine"], "title": manifest["title"], "scene": manifest["scene"],
            "settings": manifest["settings"], "capture": manifest["capture"], "build": build,
            "observation": observation,
            "collectors": manifest.get("collectors"),
            "collector_tools": {"status": collector_status, "identities": collector_tools},
            "environment": _environment(manifest, health), "arguments": manifest["execution"]["command"][1:],
            "cwd": manifest["execution"]["cwd"]}


def _identity(manifest, health):
    source = source_identity(manifest["source"])
    source["untracked"] = sorted(source["untracked"], key=lambda value: value["path"])
    source["submodules"] = sorted(source["submodules"], key=canonical_sha256)
    runtime = sorted(({"role": value["role"], "name": Path(value["path"]).name,
                       "size": value["size"], "sha256": value["sha256"]}
                      for value in manifest["runtime"]["files"]), key=canonical_sha256)
    observed = health.get("observed_context", {})
    producer = {"commit": observed.get("commit"), "dirty": observed.get("dirty")}
    return {"source_sha256": canonical_sha256(source), "runtime_sha256": canonical_sha256(runtime),
            "build_sha256": canonical_sha256(manifest["build"]),
            "producer_revision_sha256": canonical_sha256(producer)}


def _differences(before, after, prefix=""):
    if isinstance(before, dict) and isinstance(after, dict):
        result = {}
        for key in sorted(before.keys() | after.keys()):
            name = f"{prefix}.{key}" if prefix else key
            if key not in before or key not in after:
                result[name] = {"baseline": before.get(key), "candidate": after.get(key)}
            else:
                result.update(_differences(before[key], after[key], name))
        return result
    return {} if before == after else {prefix: {"baseline": before, "candidate": after}}


def _load_run(directory):
    directory = Path(directory).resolve(strict=True)
    if not directory.is_dir():
        raise SchemaError("run input must be a directory containing manifest.json")
    manifest = validate_manifest(read_json(directory / "manifest.json"), run_dir=directory)
    for artifact in manifest["artifacts"]:
        path = contained_path(directory, artifact["path"], must_exist=True)
        if not path.is_file() or path.stat().st_size != artifact["size"] or sha256_file(path) != artifact["sha256"]:
            raise SchemaError(f"artifact integrity mismatch: {artifact['path']}")
    health = validate_health(assess_capture(directory, manifest=manifest))
    if health["run_id"] != manifest["run_id"]:
        raise SchemaError("recomputed health run_id does not match manifest")
    selected = health.get("artifacts_selected", [])
    artifact_keys = {(value["kind"], value["path"], value["sha256"], value["size"])
                     for value in manifest["artifacts"]}
    for artifact in selected:
        if (artifact["kind"], artifact["path"], artifact["sha256"], artifact["size"]) not in artifact_keys:
            raise SchemaError("health selected an artifact absent from the manifest")
    if health["frame_metrics"]["status"] == "valid" and (
            sum(value["kind"] == "trace" for value in selected) != 1 or
            sum(value["kind"] == "summary" for value in selected) != 1):
        raise SchemaError("usable frame metrics require exactly one selected trace and summary")
    context = _context(manifest, health)
    observed = health.get("observed_context", {})
    context["observed_context"] = {key: observed.get(key) for key in ("gpu", "driver", "os", "resolution")}
    return {"directory": directory, "manifest": manifest, "health": health, "identity": _identity(manifest, health),
            "context": context}


def _time(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def _independence(runs):
    reasons = []
    ids, directories, paths, files, traces, captures = {}, {}, {}, {}, {}, {}
    for run in runs:
        manifest = run["manifest"]
        identifier = manifest["run_id"]
        capture_id = run["health"].get("draw_attribution", {}).get("capture_id")
        if capture_id is not None:
            if capture_id in captures:
                reasons.append(f"Runs {captures[capture_id]} and {identifier} reuse a detailed capture UUID.")
            captures[capture_id] = identifier
        for value, previous, description in ((identifier, ids, "run UUID"),
                                              (str(run["directory"]), directories, "run directory")):
            if value in previous:
                reasons.append(f"Reused {description}: {identifier} and {previous[value]}.")
            previous[value] = identifier
        for artifact in run["health"].get("artifacts_selected", []):
            path = contained_path(run["directory"], artifact["path"], must_exist=True)
            key = str(path)
            if key in paths:
                reasons.append(f"Runs {paths[key]} and {identifier} share a selected artifact.")
            paths[key] = identifier
            stat = path.stat()
            file_key = (stat.st_dev, stat.st_ino)
            if stat.st_ino:
                if file_key in files:
                    reasons.append(f"Runs {files[file_key]} and {identifier} share the same selected artifact file.")
                files[file_key] = identifier
            if artifact["kind"] in ("trace", "draw_trace", "measurement_workload"):
                digest = artifact["sha256"]
                if digest in traces:
                    reasons.append(f"Runs {traces[digest]} and {identifier} reuse byte-identical trace evidence.")
                traces[digest] = identifier
    for index, before in enumerate(runs):
        first = before["manifest"]
        for after in runs[index + 1:]:
            second = after["manifest"]
            if first["machine"]["id"] != second["machine"]["id"]:
                continue
            if all(value is not None for value in (first["started_at"], first["ended_at"],
                                                   second["started_at"], second["ended_at"])):
                if max(_time(first["started_at"]), _time(second["started_at"])) < min(
                        _time(first["ended_at"]), _time(second["ended_at"])):
                    reasons.append(f"Runs {first['run_id']} and {second['run_id']} overlap on the same machine.")
    return reasons


def _experiment(baseline, candidate):
    runs = baseline + candidate
    if not any("experiment" in run["manifest"] for run in runs):
        return [], "unpaired", []
    reasons = []
    experiments = [run["manifest"].get("experiment") for run in runs]
    if any(not isinstance(value, dict) or not isinstance(value.get("id"), str) or not value["id"]
           for value in experiments):
        return ["Experiment identity must be present and nonempty for every run when supplied."], "invalid", []
    if len({value["id"] for value in experiments}) != 1:
        reasons.append("Runs have different experiment identities.")
    if not any("pair_id" in value for value in experiments):
        return reasons, "unpaired", []
    mappings = []
    for label, cohort in (("baseline", baseline), ("candidate", candidate)):
        mapping = {}
        for run in cohort:
            pair = run["manifest"]["experiment"].get("pair_id")
            if not isinstance(pair, str) or not pair:
                reasons.append(f"Every {label} run needs a nonempty explicit pair_id.")
            elif pair in mapping:
                reasons.append(f"Duplicate {label} pair identity: {pair}.")
            else:
                mapping[pair] = run
        mappings.append(mapping)
    if mappings[0].keys() != mappings[1].keys():
        reasons.append("Baseline and candidate explicit pair identities do not match.")
    pairs = [(pair, mappings[0][pair], mappings[1][pair])
             for pair in sorted(mappings[0].keys() & mappings[1].keys())]
    return reasons, "explicit_pairs", pairs


def _distribution(values, total):
    ordered = sorted(values)
    result = {"count": len(ordered), "missing": total - len(ordered), "values": ordered,
              "median": None, "min": None, "max": None, "range": None, "stdev": None}
    if not ordered:
        return result
    middle = len(ordered) // 2
    median = ordered[middle] if len(ordered) % 2 else ordered[middle - 1] / 2 + ordered[middle] / 2
    spread = ordered[-1] - ordered[0]
    maximum = max(abs(value) for value in ordered)
    normalized = [value / maximum for value in ordered] if maximum else [0.0] * len(ordered)
    mean = math.fsum(value / len(normalized) for value in normalized)
    stdev = (math.sqrt(math.fsum((value - mean) ** 2 / (len(normalized) - 1)
                                for value in normalized)) * maximum if len(normalized) > 1 else None)
    if any(value is not None and not math.isfinite(value) for value in (median, spread, stdev)):
        raise SchemaError("Run-level variation is outside the finite numeric range")
    result.update(median=median, min=ordered[0], max=ordered[-1], range=spread, stdev=stdev)
    return result


def _cohort(runs):
    observations = []
    for run in runs:
        manifest, health = run["manifest"], run["health"]
        metrics = ({key: value for key, value in health["frame_metrics"].get("metrics", {}).items()
                    if value is not None} if health["frame_metrics"]["status"] == "valid" else {})
        draw_metrics = _draw_metrics(health)
        combined = {**metrics, **draw_metrics}
        observations.append({"run_id": manifest["run_id"], "run_dir": str(run["directory"]),
                             "health_status": health["status"], "frame_metrics": metrics,
                             "draw_metrics": draw_metrics, "metrics": combined,
                             "metric_units": {name: _metric_unit(health, name) for name in combined},
                             "source_commit": manifest["source"]["commit"], "source_dirty": manifest["source"]["dirty"],
                             "presents": health["frame_metrics"].get("presents"),
                             "duration_ms": health["frame_metrics"].get("duration_ms"),
                             "identity": run["identity"], "provenance": health["provenance"],
                             "draw_availability": {key: health.get("draw_attribution", {}).get(key, "unknown")
                                                   for key in ("identity_status", "cpu_status", "gpu_status", "gpu_association_status", "origin_graph_status")},
                             "detailed_attribution": health["detailed_attribution"],
                             "issues": health["issues"]})
    names = sorted({name for observation in observations for name in observation["metrics"]})
    metrics = {name: _distribution([row["metrics"][name] for row in observations
                                   if name in row["metrics"]], len(runs)) for name in names}
    for name, distribution in metrics.items():
        units = [row["metric_units"][name] for row in observations if name in row["metrics"]]
        distribution["units"] = sorted({unit for unit in units if unit is not None})
        distribution["units_complete"] = all(unit is not None for unit in units)
    return {"run_count": len(runs), "runs": observations, "metrics": metrics,
            "context": runs[0]["context"] if runs else {}}


def _draw_metrics(health):
    detail = health.get("draw_attribution", {})
    statuses = {"workload": "identity_status", "functions": "cpu_status", "shaders": "identity_status", "gpu": "gpu_status"}
    result = {}
    if detail.get("correlation_valid") is True:
        for category, status in statuses.items():
            for name, value in detail.get("metrics", {}).get(category, {}).items():
                available = detail.get(status) == "valid"
                if category == "workload":
                    available = detail.get("workload_status", detail.get("identity_status")) == "valid"
                if category == "functions" and "function_availability" in detail:
                    scope = name.rsplit(".", 1)[0]
                    available = detail["function_availability"].get(scope) == "valid"
                if available and value is not None:
                    result[f"draw.{category}.{name}"] = value
    evidence = health.get("evidence", {})
    if evidence.get("run_id") == health.get("run_id") and evidence.get("status") in ("valid", "unknown"):
        for name, value in evidence.get("metrics", {}).items():
            if value is not None and evidence.get("metric_availability", {}).get(name) == "valid":
                result[f"evidence.{name}"] = value
    from capture_experiment import workload_observations
    workload_status, workload = workload_observations(health)
    if workload_status == "invalid":
        result = {name: value for name, value in result.items()
                  if not name.startswith(("draw.workload.", "evidence.workload."))}
    elif workload_status == "valid":
        for name, value in workload.items():
            result["draw." + name] = value
    return result


def _metric_unit(health, name):
    if name.startswith("evidence."):
        unit = health.get("evidence", {}).get("metric_units", {}).get(name[len("evidence."):])
        return unit if isinstance(unit, str) and unit else None
    if name.startswith("frame_ms.") or name == "mean_frame_ms":
        return "ms"
    if name.startswith(("draw.workload.", "draw.shaders.")) or name.endswith(".calls"):
        return "count"
    if name.startswith("draw.functions.") and name.endswith("_ms"):
        return "ms"
    if name == "draw.gpu.available_queries":
        return "count"
    if name == "draw.gpu.sum_of_recorded_intervals_ms_including_shared_and_overlap":
        return "ms"
    return None


def _delta(before, after):
    change = after - before
    percent = change / before * 100 if before else None
    if not math.isfinite(change) or percent is not None and not math.isfinite(percent):
        raise SchemaError("Metric difference is outside the finite numeric range")
    return change, percent


def compare_runs(baseline, candidate):
    report = {"schema_version": 1, "document_type": "anyps5.capture-comparison", "status": "invalid",
              "reasons": [], "limitations": list(LIMITATIONS), "baseline": {}, "candidate": {}, "metrics": {},
              "configuration_differences": {}, "identity_changes": {}, "pairing": "unpaired", "pairs": [],
              "evidence_level": "declared_configuration_and_runtime_files",
              "performance_claim_eligibility": "not_assessed", "causal_status": "unknown"}
    cohorts = []
    invalid, incomparable, insufficient = [], [], []
    for label, directories in (("baseline", baseline), ("candidate", candidate)):
        runs = []
        directories = list(directories)
        if not directories:
            invalid.append(f"{label}: at least one run directory is required.")
        for directory in directories:
            try:
                runs.append(_load_run(directory))
            except (OSError, ValueError, OverflowError) as error:
                invalid.append(f"{label} {directory}: {error}")
        cohorts.append(runs)
        for run in runs:
            health, manifest = run["health"], run["manifest"]
            if health["status"] == "invalid" or health["frame_metrics"]["status"] == "invalid":
                invalid.append(f"{label} {manifest['run_id']}: recomputed capture health is invalid.")
            elif health["frame_metrics"]["status"] != "valid":
                insufficient.append(f"{label} {manifest['run_id']}: frame timing usability is unknown.")
            if health["provenance"].get("runtime_boundaries_verified") is not True:
                insufficient.append(f"{label} {manifest['run_id']}: declared runtime files were not verified before and after execution.")
            if health["provenance"].get("producer_revision_verified") is not True:
                insufficient.append(f"{label} {manifest['run_id']}: producer revision cannot be correlated with the inspected source.")
            if health["provenance"].get("process_identity_verified") is not True:
                insufficient.append(f"{label} {manifest['run_id']}: trace process identity cannot be correlated with the recorded child process.")
            if run["context"]["collector_tools"]["status"] in ("unknown", "invalid"):
                insufficient.append(f"{label} {manifest['run_id']}: requested collector tool identities are unproven.")
            observed = run["context"]["observed_context"]
            if any(not isinstance(value, str) or not value.strip() for value in observed.values()):
                insufficient.append(f"{label} {manifest['run_id']}: observed GPU, driver, OS or resolution is unavailable.")
            for cache_name in ("shader", "pipeline", "driver"):
                cache = manifest["settings"]["cache"].get(cache_name)
                state = cache.get("state") if isinstance(cache, dict) else cache
                if state in (None, "unknown"):
                    insufficient.append(f"{label} {manifest['run_id']}: {cache_name} cache state is unknown.")
            if manifest["state"] != "completed" or manifest["exit_code"] != 0:
                invalid.append(f"{label} {manifest['run_id']}: execution did not complete successfully.")
        if runs:
            for run in runs[1:]:
                if run["identity"] != runs[0]["identity"]:
                    incomparable.append(f"{label}: source, runtime or build identity changes within the cohort.")
                if run["context"] != runs[0]["context"]:
                    incomparable.append(f"{label}: recorded comparison conditions change within the cohort.")
    before, after = cohorts
    try:
        incomparable.extend(_independence(before + after))
        reasons, report["pairing"], pairs = _experiment(before, after)
        incomparable.extend(reasons)
        for label, runs in zip(("baseline", "candidate"), cohorts):
            report[label] = _cohort(runs)
        if before and after:
            report["configuration_differences"] = _differences(before[0]["context"], after[0]["context"])
            report["identity_changes"] = _differences(before[0]["identity"], after[0]["identity"])
            if report["configuration_differences"]:
                incomparable.append("Baseline and candidate scene, title, machine or configuration do not match.")
        if len(before) < 2 or len(after) < 2:
            insufficient.append("At least two independent runs per cohort are needed to describe repeat variation; one pair cannot establish a repeatable gain.")
        if not invalid and not incomparable:
            names = sorted(report["baseline"]["metrics"].keys() | report["candidate"]["metrics"].keys())
            for name in names:
                base = report["baseline"]["metrics"].get(name, _distribution([], len(before)))
                changed = report["candidate"]["metrics"].get(name, _distribution([], len(after)))
                available = bool(base["count"] and changed["count"] and not base["missing"] and not changed["missing"]
                                 and base.get("units_complete") and changed.get("units_complete")
                                 and len(base.get("units", [])) == 1 and base["units"] == changed.get("units"))
                change, percent = _delta(base["median"], changed["median"]) if available else (None, None)
                report["metrics"][name] = {"baseline": base, "candidate": changed, "available": available,
                                            "unit": base["units"][0] if available else None,
                                            "change": change, "change_pct": percent}
            if not any(metric["available"] for metric in report["metrics"].values()):
                insufficient.append("No frame metric is available for every run in both cohorts.")
            for identity, base, changed in pairs:
                base_metrics = {key: value for key, value in base["health"]["frame_metrics"].get("metrics", {}).items()
                                if value is not None and base["health"]["frame_metrics"]["status"] == "valid"}
                changed_metrics = {key: value for key, value in changed["health"]["frame_metrics"].get("metrics", {}).items()
                                   if value is not None and changed["health"]["frame_metrics"]["status"] == "valid"}
                base_metrics.update(_draw_metrics(base["health"]))
                changed_metrics.update(_draw_metrics(changed["health"]))
                deltas = {}
                for name in sorted(base_metrics.keys() | changed_metrics.keys()):
                    unit = _metric_unit(base["health"], name)
                    change, percent = (_delta(base_metrics[name], changed_metrics[name])
                                       if name in base_metrics and name in changed_metrics and unit is not None
                                       and unit == _metric_unit(changed["health"], name) else (None, None))
                    deltas[name] = {"change": change, "change_pct": percent, "unit": unit if change is not None else None}
                report["pairs"].append({"pair_id": identity, "baseline_run_id": base["manifest"]["run_id"],
                                        "candidate_run_id": changed["manifest"]["run_id"], "metrics": deltas})
        if any(row["health"]["frame_metrics"].get("metrics", {}) == {} for row in before + after):
            insufficient.append("One or more runs have no usable frame metrics.")
    except (OSError, ValueError, OverflowError) as error:
        invalid.append(str(error))
        report["metrics"] = {}
        report["pairs"] = []
    report["status"] = "invalid" if invalid else "incomparable" if incomparable else "insufficient" if insufficient else "comparable"
    report["reasons"] = list(dict.fromkeys(invalid + incomparable + insufficient))
    return validate_comparison(report)


def _cell(value):
    return str(value).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("|", "&#124;").replace("\r", " ").replace("\n", " ")


def _number(value):
    return "n/a" if value is None else format(value, ".3f")


def text_report(report):
    lines = [f"Capture comparison: {report['status']}",
             f"Runs: baseline={report['baseline'].get('run_count', 0)}, candidate={report['candidate'].get('run_count', 0)}",
             f"Evidence: {report['evidence_level']}; pairing: {report['pairing']}",
             "Performance claims: not assessed; use the controlled experiment workflow.",
             "Source-to-binary binding: unverified. Loaded modules: unverified.", ""]
    lines.extend(f"- {_cell(reason)}" for reason in report["reasons"])
    lines.extend(["", "| Run-level metric | Baseline median | Baseline min–max | Candidate median | Change | Change (%) | Counts |",
                  "| --- | ---: | --- | ---: | ---: | ---: | --- |"])
    for name, metric in report["metrics"].items():
        before, after = metric["baseline"], metric["candidate"]
        lines.append(f"| {_cell(name)} | {_number(before['median'])} | {_number(before['min'])}–{_number(before['max'])} | "
                     f"{_number(after['median'])} | {_number(metric['change'])} | {_number(metric['change_pct'])} | "
                     f"{before['count']}/{report['baseline']['run_count']}, {after['count']}/{report['candidate']['run_count']} |")
    for title, differences in (("Configuration differences", report["configuration_differences"]),
                                ("Permitted between-cohort identity changes", report["identity_changes"])):
        if differences:
            lines.extend(["", title])
            lines.extend(f"- {_cell(name)}: {_cell(json.dumps(values, ensure_ascii=False, allow_nan=False))}"
                         for name, values in differences.items())
    lines.extend(["", "Evidence limitations", *[f"- {_cell(value)}" for value in report["limitations"]]])
    return "\n".join(lines) + "\n"


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="Compare manifest-backed captures using independent run-level observations and recomputed health.")
    parser.add_argument("--baseline", type=Path, action="append", required=True, help="Baseline run directory; repeat for independent runs")
    parser.add_argument("--candidate", type=Path, action="append", required=True, help="Candidate run directory; repeat for independent runs")
    parser.add_argument("--json", action="store_true", help="Write a JSON report instead of text")
    args = parser.parse_args(argv)
    try:
        report = compare_runs(args.baseline, args.candidate)
        output = json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n" if args.json else text_report(report)
    except (OSError, ValueError, OverflowError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")
    sys.stdout.write(output)
    return 0 if report["status"] == "comparable" else 1 if report["status"] == "insufficient" else 2


if __name__ == "__main__":
    raise SystemExit(main())
