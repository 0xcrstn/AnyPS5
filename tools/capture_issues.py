import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from capture_schema import SchemaError, contained_path, read_json, validate_manifest


MAX_LOG_BYTES = 16 * 1024 ** 3
MAX_LINE_BYTES = 64 * 1024
MAX_SIGNATURES = 10000
ISSUE_WORDS = re.compile(
    r"\b(?:errors?|failed|failure|fatal|assert(?:ion)?|abort(?:ed)?|exceptions?|"
    r"unsupported|invalid|timeout|timed out|warnings?|refused|missing|cannot|could not|"
    r"unknown|unexpected|mismatch|lost|dropped|denied|overflow|corrupt|rejected)\b|"
    r"NotImplemented|not implemented|not supported|^\[handled-fault\]|^\[slow\]",
    re.IGNORECASE)
PAYLOAD = re.compile(r"(?:RecompileRequest|ShaderBinary|ShaderDump|base64)\s*[:=].*", re.IGNORECASE)
ENCODED = re.compile(r"(?<![A-Za-z0-9+/])[A-Za-z0-9+/]{80,}={0,2}")
PATH = re.compile(r"(?:[A-Za-z]:[\\/]|\\\\)[^\r\n\"<>|]*|(?:^|\s)/[^\s\"<>|]+")


def normalize_signature(line):
    line = PAYLOAD.sub("<payload omitted>", line)
    line = ENCODED.sub("<encoded data omitted>", line)
    line = PATH.sub("<path>", line)
    line = re.sub(r"0x[0-9a-fA-F]+", "0x#", line)
    line = re.sub(r"(?<![A-Za-z_])\d+(?:\.\d+)?", "N", line)
    return " ".join(line.split())[:240]


def _bounded_lines(source, digest, maximum_bytes):
    count = 0
    line_number = 0
    prefix = bytearray()
    truncated = False
    pending_line = False
    while True:
        chunk = source.read(1024 * 1024)
        if not chunk:
            break
        count += len(chunk)
        if count > maximum_bytes:
            raise SchemaError("run log exceeds the explicit analysis byte limit")
        digest.update(chunk)
        start = 0
        while start < len(chunk):
            end = chunk.find(b"\n", start)
            stop = len(chunk) if end < 0 else end
            segment = chunk[start:stop]
            remaining = MAX_LINE_BYTES - len(prefix)
            prefix.extend(segment[:remaining])
            truncated |= len(segment) > remaining
            pending_line = True
            if end < 0:
                break
            line_number += 1
            yield line_number, bytes(prefix).decode("utf-8", errors="replace"), truncated
            prefix.clear()
            truncated = False
            pending_line = False
            start = end + 1
    if pending_line:
        line_number += 1
        yield line_number, bytes(prefix).decode("utf-8", errors="replace"), truncated


def summarize_lines(lines, *, source_path, maximum_signatures=MAX_SIGNATURES):
    if type(maximum_signatures) is not int or not 1 <= maximum_signatures <= MAX_SIGNATURES:
        raise SchemaError("invalid issue signature capacity")
    groups = {}
    totals = {"lines": 0, "candidate_lines": 0, "truncated_lines": 0,
              "unindexed_candidate_lines": 0, "payload_lines_suppressed": 0}
    for number, line, truncated in lines:
        totals["lines"] = number
        totals["truncated_lines"] += bool(truncated)
        payload = PAYLOAD.search(line)
        searchable = line[:payload.start()] if payload else line
        totals["payload_lines_suppressed"] += payload is not None
        if not ISSUE_WORDS.search(searchable):
            continue
        totals["candidate_lines"] += 1
        signature = normalize_signature(searchable)
        if not signature:
            continue
        if signature not in groups:
            if len(groups) >= maximum_signatures:
                totals["unindexed_candidate_lines"] += 1
                continue
            groups[signature] = {"signature": signature, "count": 0,
                                 "first_source": {"path": source_path, "line": number},
                                 "last_source": {"path": source_path, "line": number},
                                 "disposition": "unreviewed", "truncated_examples": 0}
        row = groups[signature]
        row["count"] += 1
        row["last_source"]["line"] = number
        row["truncated_examples"] += bool(truncated)
    return {"status": "unknown", "review_required": True, "correctness_status": "unknown",
            "scan_complete": not totals["truncated_lines"] and not totals["unindexed_candidate_lines"],
            "counts": totals, "signatures": sorted(groups.values(),
                key=lambda row: (-row["count"], row["first_source"]["line"])),
            "limitations": ["Text matches are candidates for review, not proven failures or severity classifications.",
                            "Zero matching signatures does not prove correct rendering or error-free execution.",
                            "Payloads are omitted; overlong lines and exhausted signature capacity leave incomplete coverage.",
                            "Normalization can merge distinct causes; source line references remain authoritative."]}


def assess_log(run_dir, descriptor, *, maximum_bytes=MAX_LOG_BYTES, maximum_signatures=MAX_SIGNATURES):
    if type(maximum_bytes) is not int or not 1 <= maximum_bytes <= MAX_LOG_BYTES:
        raise SchemaError("invalid log analysis byte limit")
    if descriptor.get("kind") != "log":
        raise SchemaError("issue assessment requires a manifest log artifact")
    path = contained_path(run_dir, descriptor["path"], must_exist=True)
    before = path.stat()
    if before.st_size != descriptor["size"]:
        raise SchemaError("log size differs from its manifest descriptor")
    digest = hashlib.sha256()
    with path.open("rb") as source:
        report = summarize_lines(_bounded_lines(source, digest, maximum_bytes),
                                 source_path=descriptor["path"], maximum_signatures=maximum_signatures)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise SchemaError("log changed while its issue evidence was read")
    if digest.hexdigest() != descriptor["sha256"]:
        raise SchemaError("log hash differs from its manifest descriptor")
    report["source"] = dict(descriptor)
    return report


def assess_run_issues(run_dir, manifest):
    reports = [assess_log(run_dir, descriptor) for descriptor in manifest["artifacts"]
               if descriptor["kind"] == "log"]
    return {"schema_version": 1, "document_type": "anyps5.issue-report", "run_id": manifest["run_id"],
            "status": "unknown", "correctness_status": "unknown", "review_required": True,
            "logs": reports, "log_artifacts": len(reports),
            "candidate_lines": sum(report["counts"]["candidate_lines"] for report in reports),
            "scan_complete": bool(reports) and all(report["scan_complete"] for report in reports)}


def compact_issue_report(report, maximum_signatures=100):
    if type(maximum_signatures) is not int or not 1 <= maximum_signatures <= MAX_SIGNATURES:
        raise SchemaError("invalid compact issue signature capacity")
    signatures = sorted((row for log in report["logs"] for row in log["signatures"]),
                        key=lambda row: (-row["count"], row["first_source"]["path"], row["first_source"]["line"]))
    return {**{key: value for key, value in report.items() if key != "logs"},
            "source_artifacts": [log["source"] for log in report["logs"]],
            "retained_signatures": signatures[:maximum_signatures],
            "omitted_signature_count": max(0, len(signatures) - maximum_signatures),
            "meaning": "Bounded review candidates with exact source lines; full report is saved separately."}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Read preserved logs and report reviewable issue signatures without launching a process.")
    parser.add_argument("run_dir", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = validate_manifest(read_json(args.run_dir / "manifest.json"), run_dir=args.run_dir)
        report = assess_run_issues(args.run_dir, manifest)
    except (OSError, ValueError, OverflowError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")
    sys.stdout.write(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n")
    return 0 if report["scan_complete"] else 1


if __name__ == "__main__":
    sys.exit(main())
