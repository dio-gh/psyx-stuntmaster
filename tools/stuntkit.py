#!/usr/bin/env python3
"""Dependency-free inspection tools for PS1 single-track BIN/CUE images."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Iterable


LOGICAL_SECTOR_SIZE = 2048
SUPPORTED_BIN_SHA256 = (
    "0dfc8fcb055e2ebf22380f5ff7568706376588fdcf8c4086dcfca67dc8295e14"
)


class DiscError(RuntimeError):
    pass


@dataclass(frozen=True)
class Track:
    bin_path: Path
    mode: str
    index_lba: int

    @property
    def sector_size(self) -> int:
        return 2048 if self.mode == "MODE1/2048" else 2352

    @property
    def user_data_offset(self) -> int:
        return {"MODE1/2048": 0, "MODE1/2352": 16, "MODE2/2352": 24}[self.mode]


@dataclass(frozen=True)
class DirEntry:
    name: str
    extent_lba: int
    size: int
    is_directory: bool


@dataclass(frozen=True)
class PsxExeHeader:
    initial_pc: int
    initial_gp: int
    text_address: int
    text_size: int
    data_address: int
    data_size: int
    bss_address: int
    bss_size: int
    stack_address: int
    stack_size: int


def _timestamp_lba(value: str) -> int:
    match = re.fullmatch(r"(\d+):(\d+):(\d+)", value)
    if not match:
        raise DiscError(f"invalid CUE timestamp: {value}")
    minute, second, frame = map(int, match.groups())
    if second >= 60 or frame >= 75:
        raise DiscError(f"invalid CUE timestamp: {value}")
    return minute * 60 * 75 + second * 75 + frame


def parse_cue(cue_path: Path) -> Track:
    text = cue_path.read_text(encoding="utf-8-sig", errors="strict")
    file_matches = re.findall(
        r'^\s*FILE\s+"([^"]+)"\s+BINARY\s*$', text, re.IGNORECASE | re.MULTILINE
    )
    track_matches = re.findall(
        r"^\s*TRACK\s+\d+\s+(\S+)\s*$", text, re.IGNORECASE | re.MULTILINE
    )
    index_matches = re.findall(
        r"^\s*INDEX\s+01\s+(\d+:\d+:\d+)\s*$",
        text,
        re.IGNORECASE | re.MULTILINE,
    )
    if len(file_matches) != 1 or len(track_matches) != 1 or len(index_matches) != 1:
        raise DiscError("expected one BINARY file with one indexed data track")
    mode = track_matches[0].upper()
    if mode not in {"MODE1/2048", "MODE1/2352", "MODE2/2352"}:
        raise DiscError(f"unsupported track mode: {mode}")
    bin_path = cue_path.parent / file_matches[0]
    if not bin_path.is_file():
        raise DiscError(f"track binary not found: {bin_path}")
    return Track(bin_path.resolve(), mode, _timestamp_lba(index_matches[0]))


def _normalize_iso_name(name: str) -> str:
    return name.split(";", 1)[0].upper()


class Iso9660:
    def __init__(self, cue_path: Path):
        self.cue_path = cue_path.resolve()
        self.track = parse_cue(self.cue_path)
        self._stream: BinaryIO = self.track.bin_path.open("rb")
        descriptor = self.read_sector(16)
        if descriptor[0] != 1 or descriptor[1:6] != b"CD001" or descriptor[6] != 1:
            self.close()
            raise DiscError("ISO9660 primary volume descriptor not found")
        self.volume_id = descriptor[40:72].decode("ascii", errors="replace").rstrip(" ")
        record_length = descriptor[156]
        if record_length < 34 or 156 + record_length > len(descriptor):
            self.close()
            raise DiscError("invalid ISO9660 root record")
        root = self._parse_record(descriptor[156 : 156 + record_length])
        self.root = DirEntry("/", root.extent_lba, root.size, True)

    def __enter__(self) -> "Iso9660":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def close(self) -> None:
        self._stream.close()

    def read_sector(self, lba: int) -> bytes:
        physical_lba = self.track.index_lba + lba
        offset = (
            physical_lba * self.track.sector_size + self.track.user_data_offset
        )
        self._stream.seek(offset)
        data = self._stream.read(LOGICAL_SECTOR_SIZE)
        if len(data) != LOGICAL_SECTOR_SIZE:
            raise DiscError(f"unexpected end of track at LBA {lba}")
        return data

    def read_extent(self, lba: int, size: int) -> bytes:
        output = bytearray()
        while len(output) < size:
            output.extend(self.read_sector(lba))
            lba += 1
        return bytes(output[:size])

    @staticmethod
    def _parse_record(record: bytes) -> DirEntry:
        if len(record) < 34:
            raise DiscError("truncated ISO9660 directory record")
        name_length = record[32]
        if 33 + name_length > len(record):
            raise DiscError("invalid ISO9660 file identifier")
        name = _normalize_iso_name(
            record[33 : 33 + name_length].decode("ascii", errors="replace")
        )
        return DirEntry(
            name=name,
            extent_lba=struct.unpack_from("<I", record, 2)[0],
            size=struct.unpack_from("<I", record, 10)[0],
            is_directory=bool(record[25] & 0x02),
        )

    def list_dir(self, entry: DirEntry | None = None) -> list[DirEntry]:
        directory = entry or self.root
        if not directory.is_directory:
            raise DiscError(f"{directory.name} is not a directory")
        data = self.read_extent(directory.extent_lba, directory.size)
        entries: list[DirEntry] = []
        offset = 0
        while offset < len(data):
            length = data[offset]
            if length == 0:
                offset = ((offset // LOGICAL_SECTOR_SIZE) + 1) * LOGICAL_SECTOR_SIZE
                continue
            if offset + length > len(data):
                raise DiscError("directory record extends beyond its extent")
            record = data[offset : offset + length]
            name_length = record[32]
            is_dot = name_length == 1 and record[33] in (0, 1)
            if not is_dot:
                entries.append(self._parse_record(record))
            offset += length
        return entries

    def find(self, path: str) -> DirEntry:
        current = self.root
        for component in PurePosixPath(path.replace("\\", "/")).parts:
            if component in ("/", "."):
                continue
            wanted = _normalize_iso_name(component)
            try:
                current = next(e for e in self.list_dir(current) if e.name == wanted)
            except StopIteration as exc:
                raise DiscError(f"file not found in disc image: {path}") from exc
        return current

    def read_file(self, path: str) -> bytes:
        entry = self.find(path)
        if entry.is_directory:
            raise DiscError(f"{path} is a directory")
        return self.read_extent(entry.extent_lba, entry.size)

    def walk(self, entry: DirEntry | None = None, prefix: str = "") -> Iterable[tuple[str, DirEntry]]:
        directory = entry or self.root
        for child in self.list_dir(directory):
            path = f"{prefix}/{child.name}" if prefix else child.name
            yield path, child
            if child.is_directory:
                yield from self.walk(child, path)


def parse_system_cnf(data: bytes) -> str:
    text = data.decode("ascii", errors="replace")
    match = re.search(
        r"^\s*BOOT\s*=\s*cdrom:\\+([^;\r\n]+)(?:;\d+)?",
        text,
        re.IGNORECASE | re.MULTILINE,
    )
    if not match:
        raise DiscError("BOOT entry not found in SYSTEM.CNF")
    return match.group(1).replace("\\", "/")


def parse_psx_exe(data: bytes) -> PsxExeHeader:
    if len(data) < 2048 or data[:8] != b"PS-X EXE":
        raise DiscError("invalid PS-X EXE header")
    values = struct.unpack_from("<10I", data, 0x10)
    result = PsxExeHeader(*values)
    if not 0x80000000 <= result.initial_pc < 0xA0000000:
        raise DiscError("PS-X EXE entry point is outside KSEG0")
    if not 0x80000000 <= result.text_address < 0xA0000000:
        raise DiscError("PS-X EXE text address is outside KSEG0")
    if result.text_size > len(data) - 2048:
        raise DiscError("PS-X EXE text is truncated")
    if not result.text_address <= result.initial_pc < result.text_address + result.text_size:
        raise DiscError("PS-X EXE entry point is outside its text segment")
    return result


def hash_file(path: Path, algorithm: str = "sha256") -> str:
    digest = hashlib.new(algorithm)
    with path.open("rb") as stream:
        while block := stream.read(4 * 1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def inspect(cue_path: Path, include_bin_hash: bool) -> dict[str, object]:
    with Iso9660(cue_path) as image:
        system_cnf = image.read_file("SYSTEM.CNF")
        boot_path = parse_system_cnf(system_cnf)
        executable = image.read_file(boot_path)
        header = parse_psx_exe(executable)
        result: dict[str, object] = {
            "cue": str(image.cue_path),
            "bin": str(image.track.bin_path),
            "bin_size": image.track.bin_path.stat().st_size,
            "track_mode": image.track.mode,
            "track_index_lba": image.track.index_lba,
            "volume_id": image.volume_id,
            "boot_path": boot_path,
            "boot_size": len(executable),
            "boot_sha256": hashlib.sha256(executable).hexdigest(),
            "psx_exe": asdict(header),
            "root_entries": [asdict(entry) for entry in image.list_dir()],
        }
        if include_bin_hash:
            bin_hash = hash_file(image.track.bin_path)
            result["bin_sha256"] = bin_hash
            result["supported_image"] = bin_hash == SUPPORTED_BIN_SHA256
        return result


def _hexify_exe_fields(result: dict[str, object]) -> dict[str, object]:
    display = dict(result)
    header = dict(display["psx_exe"])  # type: ignore[arg-type]
    for key in header:
        header[key] = f"0x{header[key]:08X}"
    display["psx_exe"] = header
    return display


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="emit JSON")
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect", help="inspect boot metadata")
    inspect_parser.add_argument("cue", type=Path)
    inspect_parser.add_argument(
        "--skip-bin-hash", action="store_true", help="skip full BIN fingerprint"
    )

    tree_parser = subparsers.add_parser("tree", help="list the ISO9660 tree")
    tree_parser.add_argument("cue", type=Path)

    extract_parser = subparsers.add_parser(
        "extract", help="extract one file for local RE work"
    )
    extract_parser.add_argument("cue", type=Path)
    extract_parser.add_argument("disc_path")
    extract_parser.add_argument("output", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "inspect":
            result = inspect(args.cue, not args.skip_bin_hash)
            if args.json:
                print(json.dumps(result, indent=2))
            else:
                print(json.dumps(_hexify_exe_fields(result), indent=2))
        elif args.command == "tree":
            with Iso9660(args.cue) as image:
                rows = [
                    {"path": path, **asdict(entry)}
                    for path, entry in image.walk()
                ]
            if args.json:
                print(json.dumps(rows, indent=2))
            else:
                for row in rows:
                    kind = "d" if row["is_directory"] else "f"
                    print(
                        f"{kind} {row['extent_lba']:7d} {row['size']:10d} {row['path']}"
                    )
        elif args.command == "extract":
            with Iso9660(args.cue) as image:
                data = image.read_file(args.disc_path)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(data)
            print(f"wrote {len(data)} bytes to {args.output}")
        return 0
    except (DiscError, OSError, UnicodeError) as exc:
        print(f"stuntkit: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

