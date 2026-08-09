#!/usr/bin/env python3
"""Parse Psy-Q MND/SYM debug records and export compact RE metadata.

The binary record layout was cross-checked against the Unlicense
mefistotelis/psx_mnd_sym project at revision
546480d905779c5d40efb0d515a2b5777d616bd4.
"""

from __future__ import annotations

import argparse
import csv
import json
import struct
import sys
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import BinaryIO, Iterator


KIND_NAMES = {
    0x01: "name1",
    0x02: "name2",
    0x05: "name5",
    0x06: "name6",
    0x80: "line_inc",
    0x82: "line_inc8",
    0x84: "line_inc16",
    0x86: "line_set",
    0x88: "source_set",
    0x8A: "line_end",
    0x8C: "function_start",
    0x8E: "function_end",
    0x90: "block_start",
    0x92: "block_end",
    0x94: "definition",
    0x96: "definition2",
    0x98: "overlay",
    0x9A: "overlay_set",
}

CLASS_NAMES = {
    0x0001: "AUTO",
    0x0002: "EXT",
    0x0003: "STAT",
    0x0004: "REG",
    0x0006: "LABEL",
    0x0008: "MOS",
    0x0009: "ARG",
    0x000A: "STRTAG",
    0x000B: "MOU",
    0x000C: "UNTAG",
    0x000D: "TPDEF",
    0x000F: "ENTAG",
    0x0010: "MOE",
    0x0011: "REGPARM",
    0x0012: "FIELD",
    0x0066: "EOS",
    0x0067: "103",
}


class SymError(RuntimeError):
    pass


@dataclass(frozen=True)
class Record:
    offset: int
    value: int
    kind: int
    body: dict[str, object]


@dataclass
class Function:
    overlay: int
    address: int
    end_address: int | None
    size: int | None
    frame_pointer: int
    frame_size: int
    return_register: int
    register_mask: int
    register_mask_offset: int
    line: int
    source: str
    name: str


def _read_exact(stream: BinaryIO, size: int) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise SymError("truncated MND/SYM record")
    return data


def _u8(stream: BinaryIO) -> int:
    return _read_exact(stream, 1)[0]


def _u16(stream: BinaryIO) -> int:
    return struct.unpack("<H", _read_exact(stream, 2))[0]


def _u32(stream: BinaryIO) -> int:
    return struct.unpack("<I", _read_exact(stream, 4))[0]


def _i32(stream: BinaryIO) -> int:
    return struct.unpack("<i", _read_exact(stream, 4))[0]


def _pascal(stream: BinaryIO) -> str:
    size = _u8(stream)
    return _read_exact(stream, size).decode("ascii", errors="replace")


def iter_records(path: Path) -> Iterator[Record]:
    with path.open("rb") as stream:
        header = _read_exact(stream, 8)
        if header[:3] != b"MND":
            raise SymError(f"invalid MND/SYM signature: {header[:3]!r}")
        if header[3] != 1:
            raise SymError(f"unsupported MND/SYM version: {header[3]}")

        while True:
            offset = stream.tell()
            header = stream.read(5)
            if not header:
                return
            if len(header) != 5:
                raise SymError("truncated MND/SYM record header")
            value, kind = struct.unpack("<IB", header)
            body: dict[str, object] = {}

            if kind in (0x01, 0x02, 0x05, 0x06):
                body["name"] = _pascal(stream)
            elif kind in (0x80, 0x8A, 0x9A):
                pass
            elif kind == 0x82:
                body["increment"] = _u8(stream)
            elif kind == 0x84:
                body["increment"] = _u16(stream)
            elif kind in (0x86, 0x8E, 0x90, 0x92):
                body["line"] = _u32(stream)
            elif kind == 0x88:
                body["line"] = _u32(stream)
                body["source"] = _pascal(stream)
            elif kind == 0x8C:
                body.update(
                    frame_pointer=_u16(stream),
                    frame_size=_u32(stream),
                    return_register=_u16(stream),
                    register_mask=_u32(stream),
                    register_mask_offset=_i32(stream),
                    line=_u32(stream),
                    source=_pascal(stream),
                    name=_pascal(stream),
                )
            elif kind == 0x94:
                body.update(
                    storage_class=_u16(stream),
                    type=_u16(stream),
                    size=_u32(stream),
                    name=_pascal(stream),
                )
            elif kind == 0x96:
                storage_class = _u16(stream)
                type_id = _u16(stream)
                size = _u32(stream)
                dimensions = [_u32(stream) for _ in range(_u16(stream))]
                body.update(
                    storage_class=storage_class,
                    type=type_id,
                    size=size,
                    dimensions=dimensions,
                    tag=_pascal(stream),
                    name=_pascal(stream),
                )
            elif kind == 0x98:
                body["length"] = _u32(stream)
                body["id"] = _u32(stream)
            else:
                raise SymError(f"unsupported tag 0x{kind:02X} at file offset 0x{offset:X}")

            yield Record(offset, value, kind, body)


def _is_guest_address(value: int) -> bool:
    return 0x80000000 <= value < 0xA0000000


def analyze(path: Path) -> dict[str, object]:
    counts: Counter[int] = Counter()
    overlays: list[dict[str, int]] = []
    labels: list[dict[str, object]] = []
    definitions: list[dict[str, object]] = []
    functions: list[Function] = []
    source_files: set[str] = set()
    active_overlay = 0
    open_functions: list[Function] = []

    for record in iter_records(path):
        counts[record.kind] += 1
        if record.kind == 0x98:
            overlays.append(
                {
                    "id": int(record.body["id"]),
                    "address": record.value,
                    "length": int(record.body["length"]),
                }
            )
        elif record.kind == 0x9A:
            active_overlay = record.value
        elif record.kind in (0x01, 0x02, 0x05, 0x06):
            if _is_guest_address(record.value):
                labels.append(
                    {
                        "overlay": active_overlay,
                        "address": record.value,
                        "kind": KIND_NAMES[record.kind],
                        "name": record.body["name"],
                    }
                )
        elif record.kind == 0x88:
            source_files.add(str(record.body["source"]))
        elif record.kind == 0x8C:
            source_files.add(str(record.body["source"]))
            function = Function(
                overlay=active_overlay,
                address=record.value,
                end_address=None,
                size=None,
                frame_pointer=int(record.body["frame_pointer"]),
                frame_size=int(record.body["frame_size"]),
                return_register=int(record.body["return_register"]),
                register_mask=int(record.body["register_mask"]),
                register_mask_offset=int(record.body["register_mask_offset"]),
                line=int(record.body["line"]),
                source=str(record.body["source"]),
                name=str(record.body["name"]),
            )
            functions.append(function)
            open_functions.append(function)
        elif record.kind == 0x8E and open_functions:
            function = open_functions.pop()
            function.end_address = record.value
            if record.value >= function.address:
                function.size = record.value - function.address
        elif record.kind in (0x94, 0x96) and _is_guest_address(record.value):
            definitions.append(
                {
                    "overlay": active_overlay,
                    "address": record.value,
                    "storage_class": CLASS_NAMES.get(
                        int(record.body["storage_class"]),
                        f"0x{int(record.body['storage_class']):04X}",
                    ),
                    "type": int(record.body["type"]),
                    "size": int(record.body["size"]),
                    "tag": record.body.get("tag", ""),
                    "dimensions": record.body.get("dimensions", []),
                    "name": record.body["name"],
                }
            )

    return {
        "input": str(path),
        "input_sha256": _hash_file(path),
        "record_count": sum(counts.values()),
        "record_counts": {
            KIND_NAMES.get(kind, f"0x{kind:02X}"): count
            for kind, count in sorted(counts.items())
        },
        "overlay_count": len(overlays),
        "function_count": len(functions),
        "label_count": len(labels),
        "addressed_definition_count": len(definitions),
        "source_file_count": len(source_files),
        "overlays": overlays,
        "functions": [asdict(function) for function in functions],
        "labels": labels,
        "definitions": definitions,
        "source_files": sorted(source_files, key=str.casefold),
    }


def _hash_file(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(4 * 1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def _write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def export(result: dict[str, object], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    summary_keys = (
        "input_sha256",
        "record_count",
        "record_counts",
        "overlay_count",
        "function_count",
        "label_count",
        "addressed_definition_count",
        "source_file_count",
        "overlays",
    )
    summary = {key: result[key] for key in summary_keys}
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    _write_csv(output_dir / "functions.csv", result["functions"])  # type: ignore[arg-type]
    _write_csv(output_dir / "labels.csv", result["labels"])  # type: ignore[arg-type]
    _write_csv(output_dir / "definitions.csv", result["definitions"])  # type: ignore[arg-type]
    (output_dir / "source-files.txt").write_text(
        "\n".join(result["source_files"]) + "\n", encoding="utf-8"  # type: ignore[arg-type]
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sym", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--full-json", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = analyze(args.sym)
        if args.output_dir:
            export(result, args.output_dir)
        printable = result if args.full_json else {
            key: value
            for key, value in result.items()
            if key not in {"functions", "labels", "definitions", "source_files"}
        }
        print(json.dumps(printable, indent=2))
        return 0
    except (OSError, SymError) as exc:
        print(f"psyq_sym: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

