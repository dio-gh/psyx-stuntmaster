#!/usr/bin/env python3
"""Disassemble a virtual-address range from a PS-X EXE or a retail overlay.

Capstone is intentionally optional so the rest of the repository remains
dependency-free. Install it only in a reverse-engineering environment.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

import psx_image


def integer(value: str) -> int:
    return int(value, 0)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "image", type=Path, help="extracted PS-X EXE or *_REL.BIN overlay"
    )
    parser.add_argument("address", type=integer, help="guest virtual address")
    parser.add_argument(
        "--bytes",
        type=integer,
        default=0x100,
        dest="byte_count",
        help="number of bytes to decode (default: 0x100)",
    )
    parser.add_argument(
        "--load-address",
        type=integer,
        default=None,
        help="load address for a raw overlay; rejected for a PS-X EXE",
    )
    return parser.parse_args()


# GTE commands are COP2 instructions Capstone does not decode. Without these
# the decoder stops at the first one, which silently truncates every render
# function in this game.
GTE_COMMANDS = {
    0x01: "rtps",
    0x06: "nclip",
    0x0C: "op",
    0x10: "dpcs",
    0x11: "intpl",
    0x12: "mvmva",
    0x13: "ncds",
    0x14: "cdp",
    0x16: "ncdt",
    0x1B: "nccs",
    0x1C: "cc",
    0x1E: "ncs",
    0x20: "nct",
    0x28: "sqr",
    0x29: "dcpl",
    0x2A: "dpct",
    0x2D: "avsz3",
    0x2E: "avsz4",
    0x30: "rtpt",
    0x3D: "gpf",
    0x3E: "gpl",
    0x3F: "ncct",
}


def decode_unknown(word: int) -> str:
    if (word >> 25) == 0x25:  # COP2 command: 0100101 ...
        name = GTE_COMMANDS.get(word & 0x3F)
        if name is not None:
            return f"{name:<8} ; cop2 {word & 0x1FFFFFF:07x}"
        return f"cop2     0x{word & 0x1FFFFFF:07x}"
    return f".word    0x{word:08x}"


def main() -> int:
    arguments = parse_arguments()
    if arguments.byte_count <= 0:
        raise SystemExit("--bytes must be positive")

    image = psx_image.load(arguments.image, arguments.load_address)
    code = image.slice(arguments.address, arguments.byte_count)

    try:
        import capstone
    except ImportError as error:
        raise SystemExit(
            "Capstone is required for disassembly: python -m pip install capstone"
        ) from error

    decoder = capstone.Cs(
        capstone.CS_ARCH_MIPS,
        capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN,
    )
    offset = 0
    while offset < len(code):
        decoded = False
        for instruction in decoder.disasm(
            code[offset:], arguments.address + offset
        ):
            print(
                f"{instruction.address:08X}: "
                f"{instruction.mnemonic:<8} {instruction.op_str}"
            )
            offset += instruction.size
            decoded = True
        if decoded:
            continue
        # Step over exactly one word and keep going; MIPS is fixed-width, so
        # resynchronizing never needs a search.
        word = int.from_bytes(code[offset:offset + 4], "little")
        print(f"{arguments.address + offset:08X}: {decode_unknown(word)}")
        offset += 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
