#!/usr/bin/env python3
"""Find direct code and absolute-data references in retail code images.

Accepts the boot PS-X EXE or a raw `*_REL.BIN` overlay. Overlays share the
boot executable's `$gp`, so gp-relative global scans are valid across all of
them — which matters, because much of the object and gameplay logic lives in
overlays rather than in the boot executable.

This intentionally handles the common statically linked Psy-Q patterns:

* J/JAL with an immediate target;
* LUI followed within a short basic-block window by an I-type instruction
  whose signed low immediate reconstructs the requested address.

It is a focused triage tool, not a replacement for a control-flow-aware
disassembler. A clean "no direct references found" across every image is
evidence a global is unread, but only within those patterns.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

import psx_image


def integer(value: str) -> int:
    return int(value, 0)


def signed16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def register_name(index: int) -> str:
    names = (
        "zero",
        "at",
        "v0",
        "v1",
        "a0",
        "a1",
        "a2",
        "a3",
        "t0",
        "t1",
        "t2",
        "t3",
        "t4",
        "t5",
        "t6",
        "t7",
        "s0",
        "s1",
        "s2",
        "s3",
        "s4",
        "s5",
        "s6",
        "s7",
        "t8",
        "t9",
        "k0",
        "k1",
        "gp",
        "sp",
        "fp",
        "ra",
    )
    return names[index]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "image", type=Path, help="extracted PS-X EXE or *_REL.BIN overlay"
    )
    parser.add_argument("address", type=integer, help="guest target address")
    parser.add_argument(
        "--window",
        type=integer,
        default=8,
        help="instructions searched after LUI (default: 8)",
    )
    parser.add_argument(
        "--gp",
        type=integer,
        default=psx_image.RETAIL_GP,
        help=f"runtime global-pointer value (default: {psx_image.RETAIL_GP:#x})",
    )
    parser.add_argument(
        "--load-address",
        type=integer,
        default=None,
        help="load address for a raw overlay; rejected for a PS-X EXE",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.window <= 0:
        raise SystemExit("--window must be positive")

    image = psx_image.load(arguments.image, arguments.load_address)
    text_address = image.base_address
    code = image.code
    words = [
        int.from_bytes(code[offset : offset + 4], "little")
        for offset in range(0, len(code) - 3, 4)
    ]
    target = arguments.address & 0xFFFFFFFF
    hits: set[tuple[int, str]] = set()

    for index, word in enumerate(words):
        pc = text_address + index * 4
        opcode = word >> 26
        rs = (word >> 21) & 0x1F
        immediate = word & 0xFFFF
        if opcode in {
            0x08,
            0x09,
            0x20,
            0x21,
            0x23,
            0x24,
            0x25,
            0x28,
            0x29,
            0x2B,
        } and rs == 28:
            destination = (
                arguments.gp + signed16(immediate)
            ) & 0xFFFFFFFF
            if destination == target:
                hits.add((pc, f"$gp + {signed16(immediate):#x}"))

        if opcode in (2, 3):
            destination = (
                ((pc + 4) & 0xF0000000) |
                ((word & 0x03FFFFFF) << 2)
            )
            if destination == target:
                hits.add((pc, "jal" if opcode == 3 else "j"))

        if opcode != 0x0F:  # LUI
            continue
        register = (word >> 16) & 0x1F
        upper = (word & 0xFFFF) << 16
        for distance in range(1, arguments.window + 1):
            candidate_index = index + distance
            if candidate_index >= len(words):
                break
            candidate = words[candidate_index]
            candidate_opcode = candidate >> 26
            rs = (candidate >> 21) & 0x1F
            rt = (candidate >> 16) & 0x1F
            immediate = candidate & 0xFFFF

            # Loads/stores and signed-immediate arithmetic use rs as the base.
            if candidate_opcode in {
                0x08,  # addi
                0x09,  # addiu
                0x20,  # lb
                0x21,  # lh
                0x23,  # lw
                0x24,  # lbu
                0x25,  # lhu
                0x28,  # sb
                0x29,  # sh
                0x2B,  # sw
            } and rs == register:
                destination = (upper + signed16(immediate)) & 0xFFFFFFFF
                if destination == target:
                    description = (
                        f"lui ${register_name(register)} + "
                        f"op=0x{candidate_opcode:02X}"
                    )
                    hits.add(
                        (text_address + candidate_index * 4, description)
                    )

            # ORI is the usual unsigned low-half address constructor.
            if candidate_opcode == 0x0D and rs == register and rt == register:
                destination = upper | immediate
                if destination == target:
                    hits.add(
                        (
                            text_address + candidate_index * 4,
                            f"lui/ori ${register_name(register)}",
                        )
                    )

            # A new value in the same register ends this simple def-use chain.
            writes_rt = candidate_opcode not in {
                0x00,
                0x02,
                0x03,
                0x04,
                0x05,
                0x06,
                0x07,
                0x28,
                0x29,
                0x2A,
                0x2B,
            }
            if writes_rt and rt == register:
                break

    for address, description in sorted(hits):
        print(f"0x{address:08X} {description}")
    if not hits:
        print("no direct references found")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
