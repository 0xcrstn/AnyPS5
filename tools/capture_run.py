import argparse
import datetime
import os
import signal
import stat
import subprocess
import sys
from pathlib import Path

import capture_health
import capture_collectors
import capture_manifest
import capture_process
import capture_schema


RESERVED_ENVIRONMENT = {"APS5_TRACE_DIR", "APS5_TRACE_FRAMES", "APS5_TRACE_SECONDS", "APS5_CAPTURE_RUN_ID"}
INTERRUPT_WAIT_SECONDS = 5
COLLECTOR_POLL_SECONDS = 0.25


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def error_details(error):
    return {"type": type(error).__name__, "message": str(error)}


def record_collector_error(manifest, phase, error):
    manifest["execution"].setdefault("collector_errors", []).append({"phase": phase, **error_details(error)})


class CollectorControl:
    def __init__(self, session, manifest):
        self.session = session
        self.manifest = manifest
        self.finished = False

    def start(self):
        try:
            self.session.start()
        except Exception as error:
            record_collector_error(self.manifest, "start", error)
            self.finish()

    def poll(self):
        if self.finished:
            return
        try:
            self.session.poll()
        except Exception as error:
            record_collector_error(self.manifest, "poll", error)
            self.finish()

    def finish(self):
        if self.finished:
            return
        self.finished = True
        try:
            receipt = self.session.finish()
            if not isinstance(receipt, dict):
                raise ValueError("collector finalization did not return an actual receipt")
            self.manifest["execution"]["collector_receipt"] = receipt
            if receipt.get("run_id") != self.manifest["run_id"]:
                raise ValueError("collector receipt differs from the actual run UUID")
            for error in receipt.get("errors", []):
                record_collector_error(self.manifest, "receipt", ValueError(str(error)))
            if receipt.get("status", receipt.get("state")) != "stopped" and not receipt.get("errors"):
                raise ValueError("collector receipt does not establish an owned-session stop")
        except (Exception, KeyboardInterrupt) as error:
            record_collector_error(self.manifest, "finish", error)
        try:
            self.manifest["execution"]["collector_artifacts"] = [dict(item) for item in self.session.artifacts]
        except (Exception, KeyboardInterrupt) as error:
            record_collector_error(self.manifest, "artifacts", error)


def wait_for_child(process, observer, collectors):
    if observer is not None:
        return observer.wait(process) if collectors is None else observer.wait(process, poll_callback=collectors.poll)
    if collectors is None or collectors.finished:
        return process.wait()
    while True:
        collectors.poll()
        if collectors.finished:
            return process.wait()
        try:
            return process.wait(timeout=COLLECTOR_POLL_SECONDS)
        except subprocess.TimeoutExpired:
            pass


def child_environment(config, trace_dir, inherited=None, *, run_id=None):
    overrides = config["environment"]["overrides"]
    reserved = [name for name in overrides if name.upper() in RESERVED_ENVIRONMENT]
    if reserved:
        raise ValueError("capture config owns these environment variables: " + ", ".join(sorted(reserved)))
    child, metadata = capture_manifest.build_environment(overrides, inherited)
    variable = "APS5_TRACE_FRAMES" if config["capture"]["unit"] == "frames" else "APS5_TRACE_SECONDS"
    child["APS5_TRACE_DIR"] = str(trace_dir)
    child[variable] = capture_schema.capture_spec(config["capture"])
    metadata["effective"]["APS5_TRACE_DIR"] = str(trace_dir)
    metadata["effective"][variable] = child[variable]
    detail = capture_manifest.trace_detail(config) == "draw"
    if detail or capture_manifest.trace_workload(config):
        if run_id is None:
            raise ValueError("draw detail or measurement workload requires the manifest run UUID")
        child["APS5_CAPTURE_RUN_ID"] = run_id
        metadata["effective"]["APS5_CAPTURE_RUN_ID"] = run_id
        if detail:
            child["APS5_TRACE_DETAIL"] = "draw"
            metadata["effective"]["APS5_TRACE_DETAIL"] = "draw"
    return child, metadata


def stop_child(process):
    termination = {"requested_by": "runner_interrupt", "attempts": [], "escalated": False}
    actions = [("interrupt", lambda: interrupt_child(process)),
        ("terminate", process.terminate), ("kill", process.kill)]
    for name, action in actions:
        try:
            code = process.poll()
            if code is not None:
                termination["exit_code"] = code
                return termination
        except (Exception, KeyboardInterrupt) as error:
            termination["attempts"].append({"action": "poll", "result": "failed", **error_details(error)})
        attempt = {"action": name}
        if name != "interrupt":
            termination["escalated"] = True
        try:
            action()
            attempt["result"] = "sent"
        except (Exception, KeyboardInterrupt) as error:
            attempt.update(result="failed", **error_details(error))
        termination["attempts"].append(attempt)
        try:
            termination["exit_code"] = process.wait(timeout=INTERRUPT_WAIT_SECONDS)
            return termination
        except subprocess.TimeoutExpired:
            termination["attempts"].append({"action": "wait", "result": "timeout"})
        except (Exception, KeyboardInterrupt) as error:
            termination["attempts"].append({"action": "wait", "result": "failed", **error_details(error)})
    termination["exit_code"] = None
    termination["child_may_be_running"] = True
    return termination


def interrupt_child(process):
    if os.name == "nt":
        process.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        os.killpg(process.pid, signal.SIGINT)


def inventory_artifacts(run_dir, *, declared_artifacts=()):
    artifacts = []
    findings = []
    paths = [run_dir / "run.log", *sorted((run_dir / "traces").rglob("*")),
             *sorted((run_dir / "observations").rglob("*")), *sorted((run_dir / "external").rglob("*")),
             *sorted((run_dir / "collectors").rglob("*"))]
    declarations = {}
    for item in declared_artifacts:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str) or not item["path"].startswith("collectors/"):
            raise ValueError("collector artifact declaration must remain inside collectors/")
        capture_schema.contained_path(run_dir, item["path"])
        if item["path"] in declarations:
            raise ValueError("collector artifact is declared more than once")
        declarations[item["path"]] = item
    for path in paths:
        relative = path.relative_to(run_dir).as_posix()
        try:
            if path.is_symlink():
                raise ValueError("artifact is a symbolic link")
            path = capture_schema.contained_path(run_dir, relative, must_exist=True)
            mode = path.stat().st_mode
            if stat.S_ISDIR(mode):
                continue
            if not stat.S_ISREG(mode):
                raise ValueError("artifact is not a regular file")
            kind = "log" if relative == "run.log" else "other"
            if relative.startswith("traces/"):
                if path.name.startswith("aps5-workload-") and path.name.endswith(".json.gz"):
                    kind = "measurement_workload"
                elif path.name.startswith("aps5-draw-") and path.name.endswith(".journal.jsonl"):
                    kind = "draw_journal"
                elif path.name.startswith("aps5-draw-") and path.name.endswith(".json"):
                    kind = "draw_trace"
                elif path.name.endswith(".jsonl"):
                    kind = "summary"
                elif path.name.endswith((".json", ".json.gz")):
                    kind = "trace"
            elif relative == "observations/process.json":
                kind = "process_observation"
            elif relative == "observations/samples.jsonl":
                kind = "process_samples"
            elif relative.startswith("observations/cache-") and path.suffix == ".json":
                kind = "cache_snapshot"
            elif relative.startswith("external/"):
                for prefix, selected in (("build_receipt-", "build_receipt"), ("symbol_receipt-", "symbol_receipt"),
                                         ("cpu_profile-", "cpu_profile"), ("gpu_profile-", "gpu_profile"), ("presentation_profile-", "presentation_profile"),
                                         ("cpu-source-", "cpu_profile_source"), ("gpu-source-", "gpu_profile_source"), ("presentation-source-", "presentation_profile_source")):
                    if path.name.startswith(prefix):
                        kind = selected
                        break
            identity = capture_manifest.file_identity(path)
            if relative in declarations:
                expected = declarations.pop(relative)
                kind = expected["kind"]
                if identity != {key: expected[key] for key in ("size", "sha256")}:
                    findings.append({"path": relative, "reason": "collector_artifact_changed"})
            elif relative == "collectors/receipt.json":
                kind = "collector_receipt"
            artifacts.append({"kind": kind, "path": relative, **identity})
        except (OSError, ValueError) as error:
            findings.append({"path": relative, "reason": "unreadable_or_unsafe", **error_details(error)})
    findings.extend({"path": relative, "reason": "collector_artifact_missing"} for relative in declarations)
    return artifacts, findings


def select_capture(run_dir, manifest):
    traces = [item for item in manifest["artifacts"] if item["kind"] == "trace"]
    summaries = [item for item in manifest["artifacts"] if item["kind"] == "summary"]
    findings = []
    pid = manifest["execution"].get("pid")
    if len(traces) != 1 or len(summaries) != 1:
        return [{"reason": "ambiguous_or_missing_capture", "trace_count": len(traces),
                 "summary_count": len(summaries)}]
    trace_name = Path(traces[0]["path"]).name
    if pid is None or not trace_name.startswith("aps5-trace-") or not trace_name.endswith(
            (f"-{pid}.json.gz", f"-{pid}.json")):
        findings.append({"reason": "trace_child_pid_mismatch", "path": traces[0]["path"]})
        return findings
    summary = summaries[0]
    path = capture_schema.contained_path(run_dir, summary["path"], must_exist=True)
    records = []
    try:
        with path.open(encoding="utf-8") as stream:
            for number, line in enumerate(stream, 1):
                if line.strip():
                    record = capture_schema.loads_json(line, context=f"{summary['path']}:{number}")
                    if not isinstance(record, dict):
                        raise ValueError("summary record must be an object")
                    records.append(record)
    except (OSError, ValueError) as error:
        return [{"reason": "invalid_summary", "path": summary["path"], **error_details(error)}]
    spec = f"{manifest['capture']['unit']} {capture_schema.capture_spec(manifest['capture'])}"
    matches = [record for record in records if record.get("trace") == trace_name
               and isinstance(record.get("window"), dict) and record["window"].get("spec") == spec]
    if len(records) != 1 or len(matches) != 1:
        findings.append({"reason": "mixed_or_mismatched_summary", "path": summary["path"],
                         "record_count": len(records), "matching_count": len(matches)})
    else:
        summary["trace"] = trace_name
    detail = manifest["environment"]["effective"].get("APS5_TRACE_DETAIL")
    sidecars = [item for item in manifest["artifacts"] if item["kind"] == "draw_trace"]
    if detail == "draw":
        expected = f"aps5-draw-{manifest['run_id']}-{pid}.json"
        if len(sidecars) != 1 or Path(sidecars[0]["path"]).name != expected:
            findings.append({"reason": "ambiguous_or_missing_draw_capture", "expected": expected,
                             "draw_trace_count": len(sidecars)})
        else:
            sidecars[0]["trace"] = trace_name
    elif sidecars:
        findings.append({"reason": "unexpected_draw_capture", "draw_trace_count": len(sidecars)})
    workloads = [item for item in manifest["artifacts"] if item["kind"] == "measurement_workload"]
    if manifest["environment"]["effective"].get("APS5_TRACE_WORKLOAD") == "1":
        expected = f"aps5-workload-{manifest['run_id']}-{pid}.json.gz"
        if len(workloads) != 1 or Path(workloads[0]["path"]).name != expected:
            findings.append({"reason": "ambiguous_or_missing_measurement_workload", "expected": expected,
                             "workload_count": len(workloads)})
        else:
            workloads[0]["trace"] = trace_name
    elif workloads:
        findings.append({"reason": "unexpected_measurement_workload", "workload_count": len(workloads)})
    return findings


def save_manifest(run_dir, manifest, *, replace=True):
    capture_schema.validate_manifest(manifest, run_dir=run_dir)
    capture_schema.atomic_write_json(run_dir / "manifest.json", manifest, replace=replace)


def run_capture(config, run_dir, *, inherited_environment=None, popen=None):
    config = capture_manifest.validate_config(config)
    collector_config = capture_collectors.validate_collectors_config(config.get("collectors"))
    report_detail = config["settings"]["diagnostics"]["options"].get("report_detail", "compact")
    if type(report_detail) is not str or report_detail not in ("compact", "full"):
        raise ValueError("diagnostics.options.report_detail must be compact or full")
    if os.name == "nt" and Path(config["runtime"]["executable"]).suffix.casefold() in (".bat", ".cmd"):
        raise ValueError("Windows batch script launch is unsupported")
    requested_dir = Path(run_dir)
    if not requested_dir.is_absolute():
        raise ValueError("run directory must be an absolute path")
    run_dir = capture_manifest.safe_path(str(requested_dir), "run directory", resolve=True)
    if run_dir.is_relative_to(Path(config["source_root"])):
        raise ValueError("run directory must be outside source_root")
    manifest = capture_manifest.create_manifest(config, inherited_environment=inherited_environment)
    child_env, environment = child_environment(config, run_dir / "traces", inherited_environment,
                                               run_id=manifest["run_id"])
    manifest["environment"] = environment
    manifest["execution"].update(pid=None, outcome=None)
    process = None
    observer = None
    collectors = None
    observation = config.get("observation")
    observing = observation is not None and observation["enabled"]
    health = None
    manifest_owned = False
    execution_phase = "setup"
    run_dir.mkdir()
    try:
        save_manifest(run_dir, manifest, replace=False)
        manifest_owned = True
        (run_dir / "traces").mkdir()
        if observing:
            try:
                capture_process.snapshot_artifacts(run_dir, manifest, observation, "before")
            except (OSError, ValueError) as error:
                manifest["execution"].setdefault("observation_errors", []).append({"phase": "cache_before", **error_details(error)})
        before = capture_manifest.verify_runtime(manifest)
        manifest["execution"]["inventory_verification"]["before"] = before
        save_manifest(run_dir, manifest)
        if not before["verified"]:
            manifest["state"] = "failed"
            manifest["execution"]["outcome"] = "runtime_verification_failed_before_spawn"
        else:
            launch = subprocess.Popen if popen is None else popen
            options = {"cwd": manifest["execution"]["cwd"], "env": child_env, "shell": False,
                       "executable": manifest["execution"]["command"][0],
                       "stdin": subprocess.DEVNULL}
            if os.name == "nt":
                options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
            else:
                options["start_new_session"] = True
            with (run_dir / "run.log").open("xb") as log:
                options.update(stdout=log, stderr=subprocess.STDOUT)
                execution_phase = "spawn"
                process = launch(manifest["execution"]["command"], **options)
                execution_phase = "wait"
                manifest["started_at"] = utc_now()
                manifest["state"] = "running"
                manifest["execution"]["pid"] = process.pid
                manifest["execution"]["outcome"] = "running"
                save_manifest(run_dir, manifest)
                if observing:
                    try:
                        observer = capture_process.ProcessObserver(run_dir, manifest, observation, process=process)
                    except (OSError, ValueError) as error:
                        manifest["execution"].setdefault("observation_errors", []).append({"phase": "observer_open", **error_details(error)})
                if collector_config is not None and collector_config["enabled"]:
                    try:
                        collectors = CollectorControl(capture_collectors.CollectorSession(
                            run_dir, manifest, collector_config, process=process), manifest)
                        collectors.start()
                    except Exception as error:
                        record_collector_error(manifest, "open", error)
                try:
                    manifest["exit_code"] = wait_for_child(process, observer, collectors)
                except KeyboardInterrupt:
                    manifest["state"] = "interrupted"
                    manifest["execution"]["outcome"] = "runner_interrupt"
                    termination = stop_child(process)
                    manifest["execution"]["termination"] = termination
                    manifest["exit_code"] = termination["exit_code"]
                else:
                    manifest["state"] = "completed" if manifest["exit_code"] == 0 else "failed"
                    manifest["execution"]["outcome"] = "exited"
                    if manifest["exit_code"] < 0 and os.name != "nt":
                        manifest["execution"]["termination"] = {
                            "kind": "external", "signal": -manifest["exit_code"], "requested_by_runner": False}
    except KeyboardInterrupt as error:
        manifest["state"] = "interrupted"
        manifest["execution"]["outcome"] = "runner_interrupt"
        manifest["execution"]["error"] = error_details(error)
        if process is not None:
            termination = stop_child(process)
            manifest["execution"]["termination"] = termination
            manifest["exit_code"] = termination["exit_code"]
    except Exception as error:
        manifest["state"] = "failed"
        manifest["execution"]["outcome"] = "spawn_failed" if execution_phase == "spawn" else "runner_failed"
        manifest["execution"]["error_phase"] = execution_phase
        manifest["execution"]["error"] = error_details(error)
        if process is not None:
            try:
                manifest["exit_code"] = process.poll()
                manifest["execution"]["child_may_be_running"] = manifest["exit_code"] is None
            except Exception as poll_error:
                manifest["execution"]["poll_error"] = error_details(poll_error)
                manifest["execution"]["child_may_be_running"] = True
    finally:
        manifest["ended_at"] = utc_now()
        if manifest_owned:
            if collectors is not None:
                collectors.finish()
            if observing:
                operations = [("cache_after", lambda: capture_process.snapshot_artifacts(run_dir, manifest, observation, "after")),
                              ("evidence_attachment", lambda: capture_process.attach_observation_evidence(run_dir, manifest, observation))]
                if observer is not None:
                    operations.insert(0, ("observer_finish", observer.finish))
                for phase, operation in operations:
                    try:
                        operation()
                    except (Exception, KeyboardInterrupt) as error:
                        manifest["execution"].setdefault("observation_errors", []).append({"phase": phase, **error_details(error)})
            for phase, operation in (
                    ("runtime_verification", lambda: manifest["execution"]["inventory_verification"].update(
                        after=capture_manifest.verify_runtime(manifest))),
                    ("artifact_inventory", lambda: finalize_artifacts(run_dir, manifest))):
                try:
                    operation()
                except (Exception, KeyboardInterrupt) as error:
                    record_finalization_error(manifest, phase, error)
            after = manifest["execution"]["inventory_verification"].get("after")
            if manifest["state"] == "completed" and (after is None or not after["verified"]):
                manifest["state"] = "failed"
                manifest["execution"]["outcome"] = "runtime_verification_failed_after_exit"
            try:
                save_manifest(run_dir, manifest)
            except (Exception, KeyboardInterrupt) as error:
                record_finalization_error(manifest, "manifest_write", error)
            try:
                draw_detail = capture_manifest.trace_detail(config) == "draw"
                detailed_health = capture_health.assess_capture(run_dir, manifest=manifest,
                    include_draw_index=draw_detail and report_detail == "full")
                health = dict(detailed_health)
                if "draw_attribution" in detailed_health:
                    health["draw_attribution"] = {key: value for key, value in detailed_health["draw_attribution"].items()
                                                  if key != "index"}
                if "evidence" in detailed_health:
                    import capture_evidence
                    evidence = detailed_health["evidence"]
                    health["evidence"] = capture_evidence.compact_evidence(evidence)
                    capture_evidence.report_from_assessment(evidence, json_path=run_dir / "evidence.json")
                    record_report_artifacts(run_dir, manifest, (("evidence_report", "evidence.json"),))
                    capture_evidence.report_from_assessment(evidence, html_path=run_dir / "evidence.html", source_directory=run_dir)
                    record_report_artifacts(run_dir, manifest, (("evidence_html", "evidence.html"),))
                if draw_detail:
                    import capture_draws
                    report = capture_draws.report_from_health(run_dir, detailed_health)
                    capture_schema.validate_draw_report(report)
                    capture_schema.atomic_write_json(run_dir / "draws.json", report, replace=False)
                    record_report_artifacts(run_dir, manifest, (("draw_index", "draws.json"),))
                    with (run_dir / "draws.txt").open("x", encoding="utf-8") as output:
                        output.writelines(capture_draws.iter_text_report(report))
                    record_report_artifacts(run_dir, manifest, (("draw_report", "draws.txt"),))
                if any(item["kind"] == "log" for item in manifest["artifacts"]):
                    from capture_issues import assess_run_issues
                    issues = assess_run_issues(run_dir, manifest)
                    capture_schema.atomic_write_json(run_dir / "issues.json", issues, replace=False)
                    record_report_artifacts(run_dir, manifest, (("issue_report", "issues.json"),))
                capture_schema.validate_health(health)
                save_manifest(run_dir, manifest)
                capture_schema.atomic_write_json(run_dir / "health.json", health, replace=False)
            except (Exception, KeyboardInterrupt) as error:
                record_finalization_error(manifest, "health_report", error)
                health = None
            if manifest["execution"].get("finalization_errors"):
                try:
                    save_manifest(run_dir, manifest)
                except (Exception, KeyboardInterrupt):
                    pass
    return {"run_dir": str(run_dir), "manifest": manifest, "health": health}


def record_report_artifacts(run_dir, manifest, reports):
    for kind, name in reports:
        path = capture_schema.contained_path(run_dir, name, must_exist=True)
        manifest["artifacts"].append({"kind": kind, "path": name, **capture_manifest.file_identity(path)})


def finalize_artifacts(run_dir, manifest):
    artifacts, findings = inventory_artifacts(run_dir,
        declared_artifacts=manifest["execution"].get("collector_artifacts", []))
    manifest["artifacts"] = artifacts
    findings.extend(select_capture(run_dir, manifest))
    manifest["execution"]["artifact_findings"] = findings


def record_finalization_error(manifest, phase, error):
    manifest["execution"].setdefault("finalization_errors", []).append({"phase": phase, **error_details(error)})
    if manifest["state"] == "completed":
        manifest["state"] = "failed"
        manifest["execution"]["outcome"] = "finalization_failed"


def main(argv=None):
    parser = argparse.ArgumentParser(description="Explicitly launch one configured runtime and preserve local capture evidence.")
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        result = run_capture(capture_schema.read_json(args.config), args.run_dir)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    manifest = result["manifest"]
    health = result["health"]
    health_status = health["status"] if health is not None else "unavailable"
    frames = health["frame_metrics"]["status"] if health is not None else "unavailable"
    print(f"{manifest['state']}; capture health: {health_status}; frame metrics: {frames}: {result['run_dir']}")
    for error in manifest["execution"].get("finalization_errors", []):
        print(f"{error['phase']}: {error['type']}: {error['message']}", file=sys.stderr)
    for error in manifest["execution"].get("observation_errors", []):
        print(f"observation {error['phase']}: {error['type']}: {error['message']}", file=sys.stderr)
    for error in manifest["execution"].get("collector_errors", []):
        print(f"collector {error['phase']}: {error['type']}: {error['message']}", file=sys.stderr)
    if manifest["state"] == "interrupted":
        return 130
    usable = health is not None and health["status"] != "invalid" and frames == "valid"
    return 0 if manifest["state"] == "completed" and usable and not manifest["execution"].get("collector_errors") else 1


if __name__ == "__main__":
    sys.exit(main())
