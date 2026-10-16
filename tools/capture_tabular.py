import argparse
import csv
import hashlib
import io
import math
import re
import sys
from decimal import Decimal, InvalidOperation, localcontext
from pathlib import Path

from capture_schema import SchemaError, atomic_write_json, canonical_sha256, loads_json


MAPPING_TYPE = "anyps5.table-mapping"
ADAPTER = "explicit_table_v1"
MAX_TABLE_BYTES = 64 * 1024 ** 2
MAX_TABLE_ROWS = 262_144
MAX_COLUMNS = 256
MAX_CELL = 65_536
UNITS_PER_SECOND = {"s": 1, "ms": 1_000, "us": 1_000_000, "ns": 1_000_000_000}
FIELDS = {
    "samples": ({"pid", "timestamp", "thread_id", "weight_samples"}, {"weight_ns", "address", "symbol"}),
    "context_switches": ({"pid", "start", "end", "thread_id", "cpu", "state"}, set()),
    "gpu_counters": ({"pid", "timestamp", "name", "unit", "scope", "measurement_kind", "status", "value"},
                     {"debug_label", "shader_sha256", "stage"}),
}
INTEGER_FIELDS = {"pid", "thread_id", "weight_samples", "cpu", "address"}
LIMITATIONS = [
    "The adapter verifies selected exported CSV rows, not original ETL or vendor extraction authenticity.",
    "Exported table aggregation, filtering, sampling coverage, lost records, and symbolization remain unknown.",
    "Declared clock units do not establish clock alignment; this adapter creates no clock anchors.",
    "Exported GPU samples and metrics are not isolated shader invocation times or exclusive stage costs.",
]


def _object(value, name, allowed=None, required=None):
    if type(value) is not dict:
        raise SchemaError(f"{name} must be an object")
    if allowed is not None and set(value) - set(allowed):
        raise SchemaError(f"{name} contains unsupported fields")
    if required is not None and not set(required) <= set(value):
        raise SchemaError(f"{name} is missing required fields")
    return value


def _text(value, name, maximum=MAX_CELL):
    if type(value) is not str or not value.strip() or "\0" in value or len(value) > maximum:
        raise SchemaError(f"{name} must be bounded nonempty text")
    return value


def _uint(value, name, minimum=0):
    if type(value) is not int or not minimum <= value < 2 ** 64:
        raise SchemaError(f"{name} must be a uint64 integer")
    return value


def validate_mapping(mapping):
    _object(mapping, "table mapping", {"schema_version", "document_type", "tool", "table", "csv", "clock", "columns",
                                         "integer_formats", "status_values", "state_values"},
            {"schema_version", "document_type", "tool", "table", "csv", "clock", "columns"})
    if type(mapping["schema_version"]) is not int or mapping["schema_version"] != 1 or mapping["document_type"] != MAPPING_TYPE:
        raise SchemaError("unsupported table mapping protocol")
    tool = _object(mapping["tool"], "mapping tool", {"name", "version"}, {"name", "version"})
    _text(tool["name"], "mapping tool.name", 256)
    _text(tool["version"], "mapping tool.version", 256)
    table = _object(mapping["table"], "mapping table", {"name", "record_kind"}, {"name", "record_kind"})
    _text(table["name"], "mapping table.name", 256)
    if type(table["record_kind"]) is not str or table["record_kind"] not in FIELDS:
        raise SchemaError("unsupported exported table record kind")
    dialect = _object(mapping["csv"], "mapping csv", {"delimiter", "quotechar", "encoding"}, {"delimiter", "quotechar", "encoding"})
    for name in ("delimiter", "quotechar"):
        if type(dialect[name]) is not str or len(dialect[name]) != 1 or dialect[name] in ("\0", "\r", "\n"):
            raise SchemaError(f"CSV {name} must be one safe character")
    if dialect["delimiter"] == dialect["quotechar"] or dialect["encoding"] not in ("utf-8", "utf-8-sig"):
        raise SchemaError("unsupported CSV dialect or encoding")
    clock = _object(mapping["clock"], "mapping clock", {"domain", "input_unit", "ticks_per_second"}, {"domain", "input_unit", "ticks_per_second"})
    _text(clock["domain"], "mapping clock.domain", 256)
    _uint(clock["ticks_per_second"], "mapping clock.ticks_per_second", 1)
    if type(clock["input_unit"]) is not str or clock["input_unit"] not in {"ticks", *UNITS_PER_SECOND}:
        raise SchemaError("mapping clock requires explicit supported units")
    required, optional = FIELDS[table["record_kind"]]
    columns = _object(mapping["columns"], "mapping columns", required | optional, required)
    for field, header in columns.items():
        _text(header, f"mapping column {field}", 256)
    if "symbol" in columns and "address" not in columns:
        raise SchemaError("declared symbols require an actual instruction address column")
    formats = _object(mapping.get("integer_formats", {}), "mapping integer_formats", INTEGER_FIELDS)
    if set(formats) - set(columns) or any(value not in ("decimal", "hex") for value in formats.values()):
        raise SchemaError("integer formats must select mapped integer columns explicitly")
    categories = (("status_values", {"measured", "unavailable", "not_requested", "failed"})
                  if table["record_kind"] == "gpu_counters" else ("state_values", {"running", "excluded"})
                  if table["record_kind"] == "context_switches" else None)
    for name in ("status_values", "state_values"):
        if name in mapping and (categories is None or name != categories[0]):
            raise SchemaError(f"{name} does not belong to the selected record kind")
    if categories is not None:
        name, allowed = categories
        values = _object(mapping.get(name), name)
        if not 1 <= len(values) <= 64:
            raise SchemaError(f"{name} must be a bounded explicit value map")
        for key, value in values.items():
            _text(key, f"{name} input", 256)
            if type(value) is not str or value not in allowed:
                raise SchemaError(f"unsupported normalized {name} value")
    return mapping


def _decimal(value, name):
    value = value.strip()
    if not value or len(value) > 128:
        raise SchemaError(f"{name} must contain a bounded number")
    try:
        result = Decimal(value)
    except InvalidOperation as error:
        raise SchemaError(f"invalid {name}") from error
    if not result.is_finite() or result < 0:
        raise SchemaError(f"{name} must be finite and nonnegative")
    if abs(result.as_tuple().exponent) > 4096:
        raise SchemaError(f"{name} exceeds the supported decimal exponent bound")
    return result


def _integer(value, field, mapping, minimum=0):
    value = value.strip()
    hexadecimal = mapping.get("integer_formats", {}).get(field) == "hex"
    pattern = r"(?:0[xX])?[0-9a-fA-F]{1,16}" if hexadecimal else r"[0-9]{1,20}"
    if re.fullmatch(pattern, value) is None:
        raise SchemaError(f"exported {field} does not match its declared integer format")
    return _uint(int(value, 16 if hexadecimal else 10), f"exported {field}", minimum)


def _timestamp(value, mapping):
    number = _decimal(value, "exported timestamp")
    unit = mapping["clock"]["input_unit"]
    with localcontext() as context:
        context.prec = 256
        scaled = number if unit == "ticks" else number * mapping["clock"]["ticks_per_second"] / UNITS_PER_SECOND[unit]
    if scaled != scaled.to_integral_value() or scaled >= 2 ** 64:
        raise SchemaError("exported timestamp cannot be represented exactly by its declared integer clock")
    return str(int(scaled))


def _number(value, name):
    decimal = _decimal(value, name)
    result = float(decimal)
    if not math.isfinite(result) or result == 0 and decimal != 0:
        raise SchemaError(f"{name} exceeds finite binary64 range")
    return result


def normalize_exported_table(text, mapping, *, run_id, process, source_artifact, source_sha256, source_size):
    from capture_external import validate_external_document

    validate_mapping(mapping)
    if type(text) is not str or "\0" in text:
        raise SchemaError("exported table must be UTF-8 text without NUL")
    try:
        raw = text.encode("utf-8")
    except UnicodeError as error:
        raise SchemaError("exported table is not valid UTF-8 text") from error
    if len(raw) > MAX_TABLE_BYTES or len(raw) != source_size or hashlib.sha256(raw).hexdigest() != source_sha256:
        raise SchemaError("exported table bytes exceed the bound or contradict their preserved source identity")
    _object(process, "declared process")
    kind = mapping["table"]["record_kind"]
    rows = []
    mapping_digest = canonical_sha256(mapping)
    document = {"schema_version": 1, "document_type": "anyps5.external-evidence", "run_id": run_id,
                "kind": "gpu_profile" if kind == "gpu_counters" else "cpu_profile",
                "process": {"pid": process.get("pid"), "creation_filetime": process.get("creation_filetime"),
                            "executable_sha256": process.get("executable_sha256")},
                "producer": {"tool": mapping["tool"]["name"], "version": mapping["tool"]["version"],
                             "adapter": ADAPTER, "adapter_version": "1", "native_format": "exported_csv_table",
                             "normalized_protocol": True, "mapping_sha256": mapping_digest},
                "source": {"artifact": source_artifact, "sha256": source_sha256, "size": source_size},
                "clock": {"domain": mapping["clock"]["domain"], "ticks_per_second": mapping["clock"]["ticks_per_second"], "anchors": []},
                "coverage": {"state": "unknown", "scope": "explicitly_mapped_exported_rows_only",
                             "start_ticks": None, "end_ticks": None, "lost_records": None},
                "records": {kind: rows}, "adapter_metadata": {"mapping_sha256": mapping_digest,
                    "table": mapping["table"]["name"], "aggregation": "unknown", "coverage": "unknown",
                    "clock_input_unit": mapping["clock"]["input_unit"], "source_rows": 0, "selected_rows": 0,
                    "excluded_process_rows": 0, "excluded_state_rows": 0, "limitations": list(LIMITATIONS)}}
    validate_external_document(document)
    dialect = mapping["csv"]
    if text.startswith("\ufeff"):
        if dialect["encoding"] != "utf-8-sig":
            raise SchemaError("CSV BOM requires explicit utf-8-sig encoding")
        text = text[1:]
    reader = csv.reader(io.StringIO(text, newline=""), delimiter=dialect["delimiter"], quotechar=dialect["quotechar"], strict=True)
    metadata = document["adapter_metadata"]
    try:
        headers = next(reader, [])
        if not 1 <= len(headers) <= MAX_COLUMNS or len(set(headers)) != len(headers):
            raise SchemaError("exported table headers are missing, duplicated, or oversized")
        if any(not header.strip() or len(header) > 256 for header in headers):
            raise SchemaError("exported table contains an invalid header")
        if not set(mapping["columns"].values()) <= set(headers):
            raise SchemaError("exported table lacks a required explicitly mapped real column")
        indices = {field: headers.index(header) for field, header in mapping["columns"].items()}
        previous_line = reader.line_num
        for source_row, cells in enumerate(reader, 2):
            metadata["source_rows"] += 1
            if metadata["source_rows"] > MAX_TABLE_ROWS or len(cells) != len(headers) or any(len(cell) > MAX_CELL for cell in cells):
                raise SchemaError("exported table exceeds its row/cell bound or has a malformed row")
            first_line, previous_line = previous_line + 1, reader.line_num
            values = {field: cells[index] for field, index in indices.items()}
            if _integer(values["pid"], "pid", mapping, 1) != process["pid"]:
                metadata["excluded_process_rows"] += 1
                continue
            row = {"id": f"csv:{kind}:{source_row}", "source_record": f"{source_artifact}#csv-record={source_row}",
                   "source_row": source_row, "source_line_start": first_line, "source_line_end": reader.line_num}
            if kind == "context_switches":
                state = mapping["state_values"].get(values["state"])
                if state is None:
                    raise SchemaError("exported context-switch state is not mapped explicitly")
                if state == "excluded":
                    metadata["excluded_state_rows"] += 1
                    continue
                row.update(thread_id=str(_integer(values["thread_id"], "thread_id", mapping)),
                           start_ticks=_timestamp(values["start"], mapping), end_ticks=_timestamp(values["end"], mapping),
                           cpu=_integer(values["cpu"], "cpu", mapping), state="running")
            else:
                row["timestamp_ticks"] = _timestamp(values["timestamp"], mapping)
            if kind == "samples":
                row.update(thread_id=str(_integer(values["thread_id"], "thread_id", mapping)),
                           weight_samples=_integer(values["weight_samples"], "weight_samples", mapping, 1),
                           weight_ns=_number(values["weight_ns"], "exported sample weight_ns") if "weight_ns" in values else None,
                           stack=[])
                if "address" in values:
                    row["stack"].append({"address": str(_integer(values["address"], "address", mapping)),
                                         "symbol": _text(values["symbol"], "exported symbol") if "symbol" in values and values["symbol"].strip() else None})
            if kind == "gpu_counters":
                status = mapping["status_values"].get(values["status"])
                if status is None:
                    raise SchemaError("exported GPU availability is not mapped explicitly")
                if status != "measured" and values["value"].strip():
                    raise SchemaError("unavailable exported GPU measurement has a contradictory value")
                row.update({field: _text(values[field], f"exported GPU {field}") for field in ("name", "unit", "scope", "measurement_kind")})
                row.update(status=status, value=_number(values["value"], "exported GPU value") if status == "measured" else None,
                           debug_label=_text(values["debug_label"], "exported GPU debug label") if values.get("debug_label", "").strip() else None)
                for field in ("shader_sha256", "stage"):
                    if field in values and values[field].strip():
                        row[field] = _text(values[field], f"exported GPU {field}")
                if "shader_sha256" in row and re.fullmatch(r"[0-9a-f]{64}", row["shader_sha256"]) is None:
                    raise SchemaError("exported shader identity must be an actual canonical SHA-256 column")
            rows.append(row)
    except csv.Error as error:
        raise SchemaError("invalid or oversized exported CSV") from error
    metadata["selected_rows"] = len(rows)
    return validate_external_document(document)


def _read_bounded(path, maximum):
    with path.open("rb") as source:
        raw = source.read(maximum + 1)
    if len(raw) > maximum:
        raise SchemaError("table or mapping artifact exceeds its supported size")
    return raw


def main(argv=None):
    parser = argparse.ArgumentParser(description="Normalize explicitly mapped preserved profiler CSV rows; extraction and coverage remain unknown.")
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--source", required=True)
    parser.add_argument("--mapping", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--creation-filetime")
    parser.add_argument("--executable-sha256")
    args = parser.parse_args(argv)
    try:
        from capture_process import observation_path
        from capture_schema import contained_path

        run_dir = observation_path(str(args.run_dir))
        if not args.source.startswith("external/") or not args.mapping.startswith("external/") or not args.output.startswith("external/"):
            raise SchemaError("source, mapping, and output must be explicit contained external/ paths")
        if Path(args.source).suffix.casefold() != ".csv" or Path(args.mapping).suffix.casefold() != ".json" or Path(args.output).suffix.casefold() != ".json":
            raise SchemaError("table source must be CSV and mapping/normalized output must be JSON")
        for relative in (args.source, args.mapping, args.output):
            observation_path(str(run_dir / relative))
        source = observation_path(str(contained_path(run_dir, args.source, must_exist=True)))
        mapping_path = observation_path(str(contained_path(run_dir, args.mapping, must_exist=True)))
        output = observation_path(str(contained_path(run_dir, args.output)))
        if output in (source, mapping_path):
            raise SchemaError("normalized output must not replace preserved source or mapping")
        if source.stat().st_size > MAX_TABLE_BYTES or mapping_path.stat().st_size > 1024 * 1024:
            raise SchemaError("table or mapping artifact exceeds its supported size")
        raw = _read_bounded(source, MAX_TABLE_BYTES)
        mapping_raw = _read_bounded(mapping_path, 1024 * 1024)
        mapping = loads_json(mapping_raw.decode("utf-8-sig"), context="explicit table mapping")
        document = normalize_exported_table(raw.decode("utf-8"), mapping, run_id=args.run_id,
            process={"pid": args.pid, "creation_filetime": args.creation_filetime, "executable_sha256": args.executable_sha256},
            source_artifact=args.source, source_sha256=hashlib.sha256(raw).hexdigest(), source_size=len(raw))
        document["mapping_artifact"] = {"path": args.mapping, "sha256": hashlib.sha256(mapping_raw).hexdigest(), "size": len(mapping_raw)}
        if _read_bounded(source, MAX_TABLE_BYTES) != raw or _read_bounded(mapping_path, 1024 * 1024) != mapping_raw:
            raise SchemaError("preserved inputs changed during normalization")
        atomic_write_json(output, document, replace=False)
        print(f"Normalized {document['adapter_metadata']['selected_rows']} exported rows; original extraction, aggregation, coverage, and clock alignment remain unknown.")
        return 0
    except (OSError, UnicodeError, SchemaError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
