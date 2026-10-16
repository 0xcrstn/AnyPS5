import copy
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from uuid import UUID
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import capture_symbols as symbols
from capture_collectors import tool_identity
from capture_process import identity_file
from capture_schema import SchemaError, read_json


GUID = UUID("9f9fe756-1b44-4cd5-969c-940770df1486")


def pe_bytes(*, dwarf=True, codeview=None, debuglink=None):
    sections = [(b".text", 0x1000, 0x100, 0x200, 0x100, 0x60000020), (b".pdata", 0x2000, 12, 0x300, 12, 0x40000040)]
    strings = bytearray(b"\0\0\0\0")
    if dwarf:
        offset = len(strings)
        strings.extend(b".debug_info\0")
        sections.append((f"/{offset}".encode(), 0x3000, 16, 0x310, 16, 0x40000040))
    if codeview is not None:
        sections.append((b".rdata", 0x4000, 256, 0x400, 256, 0x40000040))
    if debuglink is not None:
        offset = len(strings)
        strings.extend(b".gnu_debuglink\0")
        sections.append((f"/{offset}".encode(), 0x4000, 256, 0x400, 256, 0x40000040))
    struct.pack_into("<I", strings, 0, len(strings))
    data = bytearray(0x600)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 0x40)
    data[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x44, 0x8664, len(sections), 0, 0x340, 1, 240, 0)
    optional = 0x58
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    struct.pack_into("<II", data, optional + 56, 0x5000, 0x200)
    struct.pack_into("<I", data, optional + 108, 16)
    struct.pack_into("<II", data, optional + 112 + 24, 0x2000, 12)
    for index, (name, rva, virtual_size, raw_offset, raw_size, flags) in enumerate(sections):
        position = optional + 240 + index * 40
        data[position:position + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", data, position + 8, virtual_size, rva, raw_size, raw_offset)
        struct.pack_into("<I", data, position + 36, flags)
    struct.pack_into("<III", data, 0x300, 0x1000, 0x1020, 0x2030)
    data[0x340:0x348] = b"FuncA\0\0\0"
    struct.pack_into("<IhHBB", data, 0x348, 0, 1, 0x20, 2, 0)
    data[0x352:0x352 + len(strings)] = strings
    if codeview is not None:
        struct.pack_into("<II", data, optional + 112 + 48, 0x4000, 28)
        record = b"RSDS" + codeview.bytes_le + struct.pack("<I", 7) + b"synthetic.pdb\0"
        struct.pack_into("<IIII", data, 0x400 + 12, 2, len(record), 0x4020, 0x420)
        data[0x420:0x420 + len(record)] = record
    if debuglink is not None:
        filename, crc = debuglink
        record = filename.encode() + b"\0"
        record += b"\0" * (-len(record) % 4) + struct.pack("<I", crc)
        data[0x400:0x400 + len(record)] = record
    return bytes(data)


def pdb_bytes(guid=GUID, age=7):
    data = bytearray(512 * 5)
    data[:32] = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"
    struct.pack_into("<IIIIII", data, 32, 512, 1, 5, 16, 0, 2)
    struct.pack_into("<I", data, 1024, 3)
    struct.pack_into("<IIII", data, 1536, 2, 0xFFFFFFFF, 28, 4)
    struct.pack_into("<III", data, 2048, 20000404, 0, age)
    data[2060:2076] = guid.bytes_le
    return bytes(data)


class SymbolTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.image = self.root / "synthetic.prx"
        self.image.write_bytes(pe_bytes())

    def test_embedded_coff_name_requires_explicit_runtime_range(self):
        receipt = symbols.create_symbol_receipt(self.image)
        self.assertEqual(receipt["function_ranges"], [{"start_rva": "4096", "end_rva": "4128", "name": "FuncA", "source": "embedded_coff_function_and_amd64_runtime_function"}])
        assessed = symbols.assess_symbol_receipt(receipt, observed_image=identity_file(self.image))
        self.assertEqual(assessed["status"], "valid")
        self.assertEqual(symbols.resolve_symbol_address(assessed, 0x101F)["name"], "FuncA")
        self.assertEqual(symbols.resolve_symbol_address(assessed, 0x1020)["status"], "unknown")
        self.assertEqual(symbols.resolve_symbol_address(assessed, 0x2000)["status"], "unknown")

    def test_mutated_layout_or_function_range_is_rejected(self):
        receipt = symbols.create_symbol_receipt(self.image)
        for field in ("image_base", "image_size"):
            changed = copy.deepcopy(receipt)
            changed["image"][field] += 1
            self.assertEqual(symbols.assess_symbol_receipt(changed)["status"], "invalid")
        receipt["function_ranges"][0]["end_rva"] = "8192"
        self.assertEqual(symbols.assess_symbol_receipt(receipt)["status"], "invalid")

    def test_missing_historical_image_is_unknown_but_different_bytes_invalid(self):
        receipt = symbols.create_symbol_receipt(self.image)
        self.image.write_bytes(pe_bytes(dwarf=False))
        self.assertEqual(symbols.assess_symbol_receipt(receipt)["status"], "invalid")
        self.image.unlink()
        self.assertEqual(symbols.assess_symbol_receipt(receipt)["status"], "unknown")

    def test_pdb_pairing_requires_exact_codeview_guid_and_age(self):
        self.image.write_bytes(pe_bytes(codeview=GUID))
        debug = self.root / "synthetic.pdb"
        debug.write_bytes(pdb_bytes())
        receipt = symbols.create_symbol_receipt(self.image, debug)
        self.assertEqual(receipt["binding"], "pe_codeview_pdb_guid_age")
        self.assertEqual(symbols.assess_symbol_receipt(receipt)["status"], "valid")
        for guid, age in ((UUID(int=1), 7), (GUID, 8)):
            debug.write_bytes(pdb_bytes(guid, age))
            with self.assertRaises(SchemaError):
                symbols.create_symbol_receipt(self.image, debug)

    def test_gnu_debuglink_crc_and_executable_layout_must_match(self):
        debug = self.root / "synthetic.debug"
        debug.write_bytes(pe_bytes())
        crc = zlib.crc32(debug.read_bytes()) & 0xFFFFFFFF
        self.image.write_bytes(pe_bytes(debuglink=(debug.name, crc)))
        receipt = symbols.create_symbol_receipt(self.image, debug)
        self.assertEqual(receipt["binding"], "gnu_debuglink_crc32_and_layout")
        self.assertEqual(symbols.assess_symbol_receipt(receipt)["status"], "valid")
        debug.write_bytes(debug.read_bytes() + b"changed")
        with self.assertRaises(SchemaError):
            symbols.create_symbol_receipt(self.image, debug)

    def test_exact_address_parser_preserves_inline_chain_and_unknowns(self):
        text = "0x140001008\nInlineName\na.cpp:9\nOuterName\nb.cpp:10\n0x14000100c\n??\n??:0\n"
        rows = symbols.parse_addr2line(text, [(0x1008, 0x140001008), (0x100C, 0x14000100C)])
        self.assertEqual([row["name"] for row in rows[0]["frames"]], ["InlineName", "OuterName"])
        self.assertEqual(rows[1]["status"], "unknown")
        with self.assertRaises(SchemaError):
            symbols.parse_addr2line(text, [(0x1008, 0x140001009)])
        with self.assertRaises(SchemaError):
            symbols.parse_addr2line("0x140001008\nMissingLocation", [(0x1008, 0x140001008)])

    def test_exact_lookup_records_tools_and_reassesses_preserved_output(self):
        tool = self.root / "addr2line.exe"
        tool.write_bytes(b"synthetic tool never executed")
        def command(argv, **kwargs):
            prefix = Path(kwargs["output_prefix"])
            text = "synthetic GNU addr2line version\n" if "--version" in argv else "0x140001008\nResolvedName\nsrc.cpp:13\n"
            outputs = {}
            for name in ("stdout", "stderr"):
                path = Path(str(prefix) + f".{name}.txt")
                path.write_bytes(text.encode() if name == "stdout" else b"")
                outputs[name] = {"path": str(path), **identity_file(path)}
            return {"argv": list(argv), "tool": tool_identity(tool), "status": "completed", "returncode": 0, **outputs}
        receipt = symbols.create_symbol_receipt(self.image, addresses=[0x1008], tool=tool, output_dir=self.root / "commands", command_runner=command)
        assessed = symbols.assess_symbol_receipt(receipt)
        self.assertEqual(assessed["status"], "valid")
        resolved = symbols.resolve_symbol_address(assessed, 0x1008)
        self.assertEqual(resolved["name"], "ResolvedName")
        self.assertEqual(resolved["scope"], "exact_rva")
        self.assertIn("not_independently_authenticated", resolved["provenance"])
        receipt["commands"][0]["argv"] = [str(tool), "not-version"]
        self.assertEqual(symbols.assess_symbol_receipt(receipt)["status"], "invalid")

    def test_failed_command_preserves_failed_receipt_without_success_claim(self):
        tool = self.root / "addr2line.exe"
        tool.write_bytes(b"synthetic tool")
        def failed(argv, **kwargs):
            return {"argv": list(argv), "tool": tool_identity(tool), "status": "failed", "returncode": 1}
        output = self.root / "failed"
        with self.assertRaises(SchemaError):
            symbols.create_symbol_receipt(self.image, addresses=[0x1008], tool=tool, output_dir=output, command_runner=failed)
        receipt = read_json(output / "failed-symbol-receipt.json")
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(receipt["commands"][0]["returncode"], 1)

    def test_malformed_pe_and_pdb_bounds_refuse_metadata_reads(self):
        data = bytearray(pe_bytes())
        struct.pack_into("<H", data, 0x44 + 16, 96)
        self.image.write_bytes(data)
        with self.assertRaises(SchemaError):
            symbols.pe_metadata(self.image)
        debug = self.root / "bad.pdb"
        debug.write_bytes(pdb_bytes()[:100])
        with self.assertRaises(SchemaError):
            symbols.pdb_identity(debug)


if __name__ == "__main__":
    unittest.main()
