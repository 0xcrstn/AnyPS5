import argparse
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
from uuid import UUID
import zlib

from capture_collectors import run_command, tool_identity, utc_now
from capture_process import identity_file, observation_path
from capture_schema import SchemaError, atomic_write_json, contained_path, read_json, validate_manifest


class BinaryFile:
    def __init__(self, path):
        self.path = observation_path(str(path))
        self.identity = identity_file(self.path)
        if not 0 < self.identity["size"] <= 4 * 1024**3:
            raise SchemaError("symbol input must be a bounded nonempty file")

    def read(self, offset, length):
        if type(offset) is not int or type(length) is not int or min(offset, length) < 0 or length > 32 * 1024**2 or offset + length > self.identity["size"]:
            raise SchemaError("binary metadata range is invalid or exceeds its bound")
        with self.path.open("rb") as stream:
            stream.seek(offset)
            result = stream.read(length)
        if len(result) != length:
            raise SchemaError("binary metadata was truncated")
        return result

    def unchanged(self):
        if identity_file(self.path) != self.identity:
            raise SchemaError("binary changed during symbol inspection")


def pe_metadata(path):
    file = BinaryFile(path)
    dos = file.read(0, 64)
    if dos[:2] != b"MZ":
        raise SchemaError("symbol image is not PE")
    position = struct.unpack_from("<I", dos, 60)[0]
    header = file.read(position, 24)
    if header[:4] != b"PE\0\0":
        raise SchemaError("PE signature is invalid")
    machine, count, unused, symbol_offset, symbol_count, optional_size, flags = struct.unpack_from("<HHIIIHH", header, 4)
    if not 1 <= count <= 1024 or not 96 <= optional_size <= 4096 or symbol_count > 1_000_000:
        raise SchemaError("PE metadata capacity exceeded")
    optional = file.read(position + 24, optional_size)
    magic = struct.unpack_from("<H", optional)[0]
    if magic == 0x20B:
        if len(optional) < 112:
            raise SchemaError("PE64 optional header is truncated")
        image_base, directory_offset = struct.unpack_from("<Q", optional, 24)[0], 112
    elif magic == 0x10B:
        image_base, directory_offset = struct.unpack_from("<I", optional, 28)[0], 96
    else:
        raise SchemaError("unsupported PE optional header")
    image_size = struct.unpack_from("<I", optional, 56)[0]
    header_size = struct.unpack_from("<I", optional, 60)[0]
    if not image_size or image_base + image_size > 2**64:
        raise SchemaError("PE mapped image bounds are invalid")
    if position + 24 + optional_size + count * 40 > header_size or header_size > file.identity["size"]:
        raise SchemaError("PE section headers exceed declared headers")
    directory_count = struct.unpack_from("<I", optional, directory_offset - 4)[0]
    if directory_count > 32 or directory_offset + directory_count * 8 > len(optional):
        raise SchemaError("PE data directory bounds are invalid")
    strings = b""
    if symbol_offset and symbol_count:
        string_offset = symbol_offset + symbol_count * 18
        string_size = struct.unpack("<I", file.read(string_offset, 4))[0]
        if not 4 <= string_size <= 8 * 1024**2:
            raise SchemaError("COFF string table exceeds bound")
        strings = file.read(string_offset, string_size)
    sections = []
    for index in range(count):
        entry = file.read(position + 24 + optional_size + index * 40, 40)
        raw_name = entry[:8].split(b"\0", 1)[0]
        if raw_name.startswith(b"/"):
            try:
                offset = int(raw_name[1:])
                if not 4 <= offset < len(strings) or b"\0" not in strings[offset:]:
                    raise ValueError()
                raw_name = strings[offset:].split(b"\0", 1)[0]
            except ValueError as error:
                raise SchemaError("invalid COFF long section name") from error
        name = raw_name.decode("ascii", errors="strict")
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", entry, 8)
        characteristics = struct.unpack_from("<I", entry, 36)[0]
        if rva + virtual_size > image_size or raw_offset + raw_size > file.identity["size"]:
            raise SchemaError("PE section exceeds file or image")
        sections.append({"name": name, "rva": rva, "virtual_size": virtual_size, "raw_size": raw_size, "raw_offset": raw_offset, "executable": bool(characteristics & 0x20000000)})
    def rva_offset(rva, length):
        candidates = [section["raw_offset"] + rva - section["rva"] for section in sections if section["rva"] <= rva and rva + length <= section["rva"] + section["raw_size"]]
        if rva + length <= header_size:
            candidates.append(rva)
        if len(candidates) != 1:
            raise SchemaError("PE RVA does not map uniquely to file bytes")
        return candidates[0]
    debug = []
    if directory_count > 6:
        rva, size = struct.unpack_from("<II", optional, directory_offset + 6 * 8)
        if size:
            if size % 28 or size > 28 * 1024:
                raise SchemaError("PE debug directory exceeds bound")
            entries = file.read(rva_offset(rva, size), size)
            for offset in range(0, size, 28):
                kind, length, address, pointer = struct.unpack_from("<IIII", entries, offset + 12)
                if kind == 2 and 24 <= length <= 32768:
                    record = file.read(pointer, length)
                    if record[:4] == b"RSDS":
                        debug.append({"kind": "RSDS", "guid": str(UUID(bytes_le=record[4:20])), "age": struct.unpack_from("<I", record, 20)[0]})
    debuglink = None
    for section in sections:
        if section["name"] == ".gnu_debuglink":
            data = file.read(section["raw_offset"], section["raw_size"])
            end = data.find(b"\0")
            checksum = (end + 4) & ~3
            if end <= 0 or checksum + 4 > len(data):
                raise SchemaError("GNU debuglink metadata is truncated")
            name = data[:end].decode("utf-8", errors="strict")
            if Path(name).name != name or "/" in name or "\\" in name:
                raise SchemaError("GNU debuglink requires a basename")
            debuglink = {"filename": name, "crc32": f"{struct.unpack_from('<I', data, checksum)[0]:08x}"}
    ranges = []
    if machine == 0x8664 and symbol_offset and symbol_count:
        names = {}
        symbols = file.read(symbol_offset, symbol_count * 18)
        index = 0
        while index < symbol_count:
            record = symbols[index * 18:(index + 1) * 18]
            name = record[:8]
            value, section_index, symbol_type, storage, auxiliary = struct.unpack_from("<IhHBB", record, 8)
            if index + auxiliary >= symbol_count:
                raise SchemaError("COFF auxiliary symbol records are truncated")
            if 1 <= section_index <= len(sections) and symbol_type & 0x30 == 0x20:
                if name[:4] == b"\0" * 4:
                    string_index = struct.unpack_from("<I", name, 4)[0]
                    if not 4 <= string_index < len(strings) or b"\0" not in strings[string_index:]:
                        raise SchemaError("COFF symbol name is invalid")
                    name = strings[string_index:].split(b"\0", 1)[0]
                else:
                    name = name.split(b"\0", 1)[0]
                decoded = name.decode("utf-8", errors="replace")
                if decoded and len(decoded) <= 4096:
                    names.setdefault(sections[section_index - 1]["rva"] + value, set()).add(decoded)
            index += 1 + auxiliary
        if directory_count > 3:
            rva, size = struct.unpack_from("<II", optional, directory_offset + 3 * 8)
            if size:
                if size % 12 or size > 12 * 65536:
                    raise SchemaError("PE runtime function table exceeds bound")
                data = file.read(rva_offset(rva, size), size)
                for offset in range(0, size, 12):
                    first, last, unwind = struct.unpack_from("<III", data, offset)
                    selected = [section for section in sections if section["executable"] and section["rva"] <= first < last <= section["rva"] + section["virtual_size"]]
                    if first in names and len(names[first]) == 1 and len(selected) == 1:
                        ranges.append({"start_rva": str(first), "end_rva": str(last), "name": next(iter(names[first])), "source": "embedded_coff_function_and_amd64_runtime_function"})
    file.unchanged()
    return {"path": str(file.path), **file.identity, "machine": machine, "image_base": image_base, "image_size": image_size, "sections": sections,
            "codeview": debug, "gnu_debuglink": debuglink, "embedded_dwarf": any(row["name"] in (".debug_info", ".zdebug_info") for row in sections), "function_ranges": ranges}


def pdb_identity(path):
    file = BinaryFile(path)
    header = file.read(0, 56)
    if header[:32] != b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0":
        raise SchemaError("only MSF7 PDB identity is supported")
    block_size, free_map, blocks, directory_bytes, reserved, block_map = struct.unpack_from("<IIIIII", header, 32)
    if block_size not in (512, 1024, 2048, 4096) or blocks * block_size > file.identity["size"] or not 8 <= directory_bytes <= 16 * 1024**2:
        raise SchemaError("PDB container bounds unsupported")
    count = (directory_bytes + block_size - 1) // block_size
    if count * 4 > block_size:
        raise SchemaError("multi-page PDB block maps are unsupported")
    indices = struct.unpack(f"<{count}I", file.read(block_map * block_size, count * 4))
    directory = b"".join(file.read(index * block_size, block_size) for index in indices)[:directory_bytes]
    stream_count = struct.unpack_from("<I", directory)[0]
    if not 2 <= stream_count <= 65536 or 4 + stream_count * 4 > len(directory):
        raise SchemaError("PDB stream directory is invalid")
    sizes = struct.unpack_from(f"<{stream_count}I", directory, 4)
    offset = 4 + stream_count * 4
    stream = None
    for index, size in enumerate(sizes):
        pages = 0 if size == 0xFFFFFFFF else (size + block_size - 1) // block_size
        if offset + pages * 4 > len(directory):
            raise SchemaError("PDB stream page list is truncated")
        if index == 1:
            if not 28 <= size <= 16 * 1024**2:
                raise SchemaError("PDB identity stream is invalid")
            page_ids = struct.unpack_from(f"<{pages}I", directory, offset)
            stream = b"".join(file.read(page * block_size, block_size) for page in page_ids)[:size]
            break
        offset += pages * 4
    version, signature, age = struct.unpack_from("<III", stream)
    if version != 20000404:
        raise SchemaError("unsupported PDB info stream version")
    file.unchanged()
    return {"path": str(file.path), **file.identity, "kind": "MSF7", "guid": str(UUID(bytes_le=stream[12:28])), "age": age}


def debug_binding(image, symbol=None):
    metadata = pe_metadata(image)
    if symbol is None or observation_path(str(symbol)) == observation_path(str(image)):
        return metadata, {"path": metadata["path"], "size": metadata["size"], "sha256": metadata["sha256"]}, "embedded_dwarf_image_sha256" if metadata["embedded_dwarf"] else "embedded_image_metadata"
    path = observation_path(str(symbol))
    if path.suffix.casefold() == ".pdb":
        identity = pdb_identity(path)
        if not any(row["guid"] == identity["guid"] and row["age"] == identity["age"] for row in metadata["codeview"]):
            raise SchemaError("PDB GUID and age do not match PE CodeView identity")
        return metadata, identity, "pe_codeview_pdb_guid_age"
    link = metadata["gnu_debuglink"]
    if not link or path.name != link["filename"]:
        raise SchemaError("separate debug file lacks an exact GNU debuglink filename")
    checksum = 0
    before = identity_file(path)
    with path.open("rb") as stream:
        while data := stream.read(1024 * 1024):
            checksum = zlib.crc32(data, checksum)
    if f"{checksum & 0xffffffff:08x}" != link["crc32"] or identity_file(path) != before:
        raise SchemaError("separate debug file CRC32 differs or bytes changed")
    debug = pe_metadata(path)
    layout = lambda value: [(row["name"], row["rva"], row["virtual_size"]) for row in value["sections"] if row["executable"]]
    if metadata["image_base"] != debug["image_base"] or layout(metadata) != layout(debug) or not debug["embedded_dwarf"]:
        raise SchemaError("separate PE DWARF address layout does not match image")
    return metadata, {"path": str(path), **before, "crc32": link["crc32"]}, "gnu_debuglink_crc32_and_layout"


def parse_addr2line(text, addresses):
    if type(text) is not str or len(text.encode("utf-8")) > 16 * 1024**2 or "\0" in text:
        raise SchemaError("addr2line output is invalid or oversized")
    lines = text.splitlines()
    rows = []
    position = 0
    for rva, address in addresses:
        if position >= len(lines) or re.fullmatch(r"0x[0-9a-fA-F]+", lines[position]) is None or int(lines[position], 16) != address:
            raise SchemaError("addr2line output address sequence does not match request")
        position += 1
        frames = []
        while position < len(lines) and re.fullmatch(r"0x[0-9a-fA-F]+", lines[position]) is None:
            if position + 1 >= len(lines) or len(frames) >= 128:
                raise SchemaError("addr2line function/location pairs are truncated or too deep")
            name, location = lines[position:position + 2]
            position += 2
            if len(name) > 4096 or len(location) > 8192:
                raise SchemaError("addr2line labels exceed limits")
            match = re.fullmatch(r"(.*):([0-9]+|\?)(?: \(discriminator [0-9]+\))?", location)
            if match is None:
                raise SchemaError("unsupported addr2line source location")
            filename, line = match.groups()
            if name not in ("??", "?") and name.strip():
                frames.append({"name": name, "file": None if filename in ("??", "?") else filename, "line": int(line) if line.isdigit() and int(line) else None})
        rows.append({"rva": str(rva), "va": str(address), "status": "resolved" if frames else "unknown", "frames": frames})
    if position != len(lines):
        raise SchemaError("addr2line returned unrequested records")
    return rows


def read_output(value, maximum=16 * 1024**2):
    path = observation_path(value["path"])
    expected = {key: value[key] for key in ("size", "sha256")}
    if expected["size"] > maximum or identity_file(path) != expected:
        raise SchemaError("symbol command output hash/size mismatch")
    with path.open("rb") as stream:
        data = stream.read(maximum + 1)
    if len(data) > maximum or identity_file(path) != expected:
        raise SchemaError("symbol command output changed while reading")
    return data.decode("utf-8", errors="strict")


def create_symbol_receipt(image, symbol=None, *, addresses=(), tool=None, output_dir=None, command_runner=None):
    metadata, debug, binding = debug_binding(image, symbol)
    selected = list(addresses)
    if len(selected) > 256 or any(type(address) is not int or not 0 <= address < metadata["image_size"] for address in selected):
        raise SchemaError("symbol addresses must be at most256 explicit in-image RVAs")
    selected = sorted(set(selected))
    receipt = {"schema_version": 1, "document_type": "anyps5.symbol-receipt", "created_at": utc_now(),
               "image": {key: metadata[key] for key in ("path", "size", "sha256", "image_base", "image_size", "codeview", "gnu_debuglink")},
               "symbol_file": debug, "binding": binding, "symbol_resolution": "unknown", "function_ranges": metadata["function_ranges"],
               "address_resolutions": [], "commands": [], "status": "observed_metadata", "errors": [], "limitations": ["No source-to-binary, unwind or complete symbol coverage claim.", "Function ranges require explicit PE runtime bounds and a matching embedded function symbol; adjacent symbols are never extrapolated.",
               "PDB GUID/age proves pairing only; unsupported PDB procedure extraction remains unknown.", "addr2line labels are recorded tool results on verified debug inputs, not independently authenticated execution."]}
    if selected and binding in ("embedded_dwarf_image_sha256", "gnu_debuglink_crc32_and_layout"):
        executable = observation_path(str(tool)) if tool is not None else None
        if executable is None or executable.name.casefold() not in ("addr2line.exe", "addr2line", "x86_64-w64-mingw32-addr2line.exe") or not executable.is_file() or output_dir is None:
            raise SchemaError("exact DWARF lookups require explicit installed addr2line and new output_dir")
        directory = observation_path(str(output_dir))
        directory.mkdir(exist_ok=False)
        runner = command_runner or run_command
        def invoke(argv, prefix, **options):
            try:
                result = runner(argv, cwd=str(directory), output_prefix=prefix, **options)
            except Exception as error:
                result = {"argv": list(argv), "tool": tool_identity(executable), "status": "failed", "returncode": None,
                          "error": {"type": type(error).__name__, "message": str(error)}}
                for name in ("stdout", "stderr"):
                    path = Path(str(prefix) + f".{name}.txt")
                    if path.is_file():
                        result[name] = {"path": str(path), **identity_file(path)}
                receipt["commands"].append(result)
                raise
            receipt["commands"].append(result)
            return result
        try:
            version = invoke([str(executable), "--version"], directory / "version", timeout_seconds=10)
            if version["status"] != "completed":
                raise SchemaError("addr2line version query failed")
            pairs = [(address, metadata["image_base"] + address) for address in selected]
            command = [str(executable), "--addresses", "--functions", "--demangle", "--inlines", "--exe=" + debug["path"], *[hex(address) for unused, address in pairs]]
            result = invoke(command, directory / "addresses", timeout_seconds=30, max_output_bytes=16 * 1024**2)
            if result["status"] != "completed":
                raise SchemaError("addr2line lookup failed")
            receipt["address_resolutions"] = parse_addr2line(read_output(result["stdout"]), pairs)
            receipt["symbol_resolution"] = "exact_addresses_recorded_tool_output"
            receipt["status"] = "recorded_tool_output"
        except Exception as error:
            receipt["status"] = "failed"
            receipt["errors"].append({"type": type(error).__name__, "message": str(error)})
            atomic_write_json(directory / "failed-symbol-receipt.json", receipt, replace=False)
            raise
    if identity_file(metadata["path"]) != {key: metadata[key] for key in ("size", "sha256")} or identity_file(debug["path"]) != {key: debug[key] for key in ("size", "sha256")}:
        raise SchemaError("symbol input changed during extraction")
    return receipt


def assess_symbol_receipt(receipt, *, observed_image=None):
    result = {"status": "unknown", "binding": "unknown", "reason": None, "image": None, "function_ranges": [], "address_resolutions": [], "issues": []}
    try:
        if type(receipt) is not dict or receipt.get("schema_version") != 1 or receipt.get("document_type") != "anyps5.symbol-receipt":
            raise SchemaError("unsupported symbol receipt")
        if receipt.get("binding") == "declared_pair_not_verified_debug_id":
            result["reason"] = "legacy declared pair does not prove debug identity"
            return result
        metadata, debug, binding = debug_binding(receipt["image"]["path"], receipt["symbol_file"]["path"])
        for value, actual in ((receipt["image"], metadata), (receipt["symbol_file"], debug)):
            if any(value.get(key) != actual.get(key) for key in ("size", "sha256")):
                raise SchemaError("symbol receipt input hash does not match actual file")
        image_fields = ("path", "size", "sha256", "image_base", "image_size", "codeview", "gnu_debuglink")
        if any(receipt["image"].get(key) != metadata[key] for key in image_fields):
            raise SchemaError("symbol receipt image layout differs from independently read metadata")
        if observed_image is not None and any(observed_image.get(key) != metadata[key] for key in ("size", "sha256")):
            raise SchemaError("symbol image differs from observed mapped module file")
        if binding != receipt.get("binding") or receipt.get("function_ranges", []) != metadata["function_ranges"]:
            raise SchemaError("symbol binding or function ranges differ from independently read metadata")
        if receipt.get("status") == "failed" or receipt.get("errors"):
            raise SchemaError("symbol receipt records failed extraction")
        result.update(status="valid", binding=binding, image={key: metadata[key] for key in image_fields}, function_ranges=metadata["function_ranges"], reason="fresh binary/debug linkage and explicit metadata ranges verified")
        resolutions = receipt.get("address_resolutions", [])
        if resolutions:
            commands = receipt.get("commands")
            if type(commands) is not list or len(commands) != 2:
                raise SchemaError("exact-address receipt requires version and lookup command records")
            pairs = []
            for row in resolutions:
                if type(row) is not dict or type(row.get("rva")) is not str or re.fullmatch(r"0|[1-9][0-9]*", row["rva"]) is None or not 0 <= int(row["rva"]) < metadata["image_size"]:
                    raise SchemaError("exact-address receipt RVA is invalid")
                pairs.append((int(row["rva"]), metadata["image_base"] + int(row["rva"])))
            if len(pairs) > 256 or len(set(pairs)) != len(pairs):
                raise SchemaError("exact-address receipt inventory exceeds bounds")
            lookup = commands[1]
            tool = tool_identity(lookup["tool"]["path"])
            if tool != lookup["tool"] or lookup["status"] != "completed" or lookup["returncode"] != 0:
                raise SchemaError("symbol lookup tool bytes/status do not match receipt")
            version = commands[0]
            if version.get("tool") != tool or version.get("argv") != [tool["path"], "--version"] or version.get("status") != "completed" or version.get("returncode") != 0:
                raise SchemaError("symbol version command does not match verified lookup tool")
            expected = [tool["path"], "--addresses", "--functions", "--demangle", "--inlines", "--exe=" + debug["path"], *[hex(address) for unused, address in pairs]]
            if lookup["argv"] != expected:
                raise SchemaError("symbol lookup argv differs from exact verified addresses")
            for command in commands:
                for name in ("stdout", "stderr"):
                    value = command[name]
                    if identity_file(value["path"]) != {key: value[key] for key in ("size", "sha256")} or value["size"] > 16 * 1024**2:
                        raise SchemaError("symbol command output hash/size mismatch")
            actual = parse_addr2line(read_output(lookup["stdout"]), pairs)
            if actual != resolutions:
                raise SchemaError("exact-address labels differ from preserved tool output")
            for command in commands:
                for name in ("stdout", "stderr"):
                    if identity_file(command[name]["path"]) != {key: command[name][key] for key in ("size", "sha256")}:
                        raise SchemaError("symbol command output changed during parsing")
            result["address_resolutions"] = actual
            result["address_resolution_provenance"] = "recorded_tool_output_not_independently_authenticated"
        if identity_file(metadata["path"]) != {key: metadata[key] for key in ("size", "sha256")} or identity_file(debug["path"]) != {key: debug[key] for key in ("size", "sha256")}:
            raise SchemaError("symbol inputs changed during assessment")
    except OSError as error:
        result.update(status="unknown", binding="unknown", function_ranges=[], address_resolutions=[], reason="symbol verification inputs are unavailable: " + str(error))
        result["issues"].append({"severity": "warning", "code": "symbol_inputs_unavailable", "message": str(error)})
    except (ValueError, KeyError, TypeError, AttributeError, UnicodeError, struct.error) as error:
        result.update(status="invalid", binding="unknown", function_ranges=[], address_resolutions=[], reason=str(error))
        result["issues"].append({"severity": "error", "code": "symbol_receipt_invalid", "message": str(error)})
    return result


def resolve_symbol_address(assessment, rva):
    unknown = {"status": "unknown", "name": None, "frames": [], "reason": "verified symbol input or exact address unavailable"}
    if type(assessment) is not dict or assessment.get("status") != "valid" or type(rva) is not int or not 0 <= rva < assessment["image"]["image_size"]:
        return unknown
    exact = [row for row in assessment["address_resolutions"] if int(row["rva"]) == rva and row["status"] == "resolved"]
    if len(exact) == 1:
        return {"status": "resolved", "name": exact[0]["frames"][0]["name"], "frames": exact[0]["frames"], "binding": assessment["binding"],
                "scope": "exact_rva", "provenance": "recorded_addr2line_result_not_independently_authenticated"}
    ranges = [row for row in assessment["function_ranges"] if int(row["start_rva"]) <= rva < int(row["end_rva"])]
    if len(ranges) == 1:
        return {"status": "resolved", "name": ranges[0]["name"], "frames": [], "binding": assessment["binding"], "scope": "explicit_pe_runtime_function_range",
                "provenance": "independently_parsed_embedded_coff_and_runtime_bounds"}
    return {**unknown, "reason": "no unique explicit function range or exact addr2line lookup"}


def attach_symbol_receipt(run_dir, receipt_path):
    from capture_process import assess_process_observation, copy_attachment, descriptor
    run = observation_path(str(run_dir))
    lock_path = run / ".capture-attach.lock"
    lock = lock_path.open("x", encoding="utf-8")
    try:
        with lock:
            lock.write("symbol receipt attachment\n")
        manifest_path = run / "manifest.json"
        before = identity_file(manifest_path)
        manifest = validate_manifest(read_json(manifest_path), run_dir=run)
        if manifest["state"] not in ("completed", "failed", "interrupted") or manifest.get("execution", {}).get("child_may_be_running"):
            raise SchemaError("symbol attachment requires a finalized non-running capture")
        source = observation_path(str(receipt_path))
        source_identity = identity_file(source)
        if source_identity["size"] > 4 * 1024**2:
            raise SchemaError("symbol receipt exceeds attachment bound")
        receipt = read_json(source)
        if type(receipt) is not dict or receipt.get("document_type") != "anyps5.symbol-receipt" or receipt.get("schema_version") != 1:
            raise SchemaError("symbol attachment requires a symbol receipt object")
        usable = {}
        for item in manifest["artifacts"]:
            if item["kind"] in ("process_observation", "process_samples"):
                path = contained_path(run, item["path"], must_exist=True)
                if identity_file(path) != {key: item[key] for key in ("size", "sha256")}:
                    raise SchemaError("process evidence hash mismatch before symbol attachment")
                usable[item["path"]] = path
        process = assess_process_observation(run, manifest, usable)
        candidates = [row for row in process["modules"] if row.get("file_identity") == {key: receipt.get("image", {}).get(key) for key in ("size", "sha256")}]
        if process["status"] == "invalid" or not candidates:
            raise SchemaError("symbol image has no independently verified observed module candidate")
        assessed = assess_symbol_receipt(receipt, observed_image=candidates[0]["file_identity"])
        if assessed["status"] != "valid":
            raise SchemaError("symbol receipt cannot be freshly verified: " + str(assessed["reason"]))
        additions = []
        existing = {item["path"]: item for item in manifest["artifacts"]}
        for command in receipt.get("commands", []):
            for name in ("stdout", "stderr"):
                item = command[name]
                path = observation_path(item["path"])
                if not path.is_relative_to(run):
                    raise SchemaError("symbol command outputs must be preserved inside the capture run")
                relative = path.relative_to(run).as_posix()
                contained_path(run, relative, must_exist=True)
                actual = descriptor("symbol_output", relative, run)
                if {key: actual[key] for key in ("size", "sha256")} != {key: item[key] for key in ("size", "sha256")}:
                    raise SchemaError("symbol output changed before attachment")
                if relative in existing and any(existing[relative][key] != actual[key] for key in ("size", "sha256")):
                    raise SchemaError("existing symbol output descriptor disagrees")
                if relative not in existing:
                    additions.append(actual)
                    existing[relative] = actual
        external = run / "external"
        external.mkdir(exist_ok=True)
        index = sum(item["kind"] == "symbol_receipt" for item in manifest["artifacts"])
        target = external / f"symbol-receipt-{index:04d}.json"
        backup = external / f"manifest-before-symbols-{index:04d}.json"
        copied = copy_attachment(source, target)
        if copied != source_identity or identity_file(source) != source_identity:
            raise SchemaError("symbol receipt changed during attachment")
        copied_manifest = copy_attachment(manifest_path, backup)
        if copied_manifest != before or identity_file(manifest_path) != before:
            raise SchemaError("manifest changed during symbol attachment")
        manifest["artifacts"].extend([*additions, descriptor("symbol_receipt", target.relative_to(run).as_posix(), run), descriptor("manifest_revision", backup.relative_to(run).as_posix(), run)])
        manifest.setdefault("external_attachments", []).append({"kind": "symbol_receipt", "previous_manifest_sha256": before["sha256"], "receipt_source_sha256": source_identity["sha256"], "run_identity_rewritten": False})
        validate_manifest(manifest, run_dir=run)
        if identity_file(manifest_path) != before:
            raise SchemaError("manifest changed before symbol receipt publication")
        atomic_write_json(manifest_path, manifest)
        return {"status": "attached", "artifact": target.relative_to(run).as_posix(), "previous_manifest_sha256": before["sha256"]}
    finally:
        lock_path.unlink()


def main(argv=None):
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == "attach":
        parser = argparse.ArgumentParser()
        parser.add_argument("--run-dir", required=True)
        parser.add_argument("--receipt", required=True)
        args = parser.parse_args(arguments[1:])
        try:
            print(json.dumps(attach_symbol_receipt(args.run_dir, args.receipt)))
            return 0
        except (OSError, ValueError, TypeError, KeyError) as error:
            print(f"symbol attachment failed: {str(error)[:1024]}", file=sys.stderr)
            return 2
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--symbols")
    parser.add_argument("--rva", action="append", default=[])
    parser.add_argument("--addr2line")
    parser.add_argument("--command-output-dir")
    parser.add_argument("--output", required=True)
    args = parser.parse_args(arguments)
    try:
        value = create_symbol_receipt(args.image, args.symbols, addresses=[int(text, 0) for text in args.rva], tool=args.addr2line, output_dir=args.command_output_dir)
        atomic_write_json(observation_path(args.output), value, replace=False)
        return 0
    except (OSError, ValueError, TypeError, KeyError, UnicodeError, struct.error) as error:
        print(f"symbol receipt failed: {str(error)[:1024]}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
