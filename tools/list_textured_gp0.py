#!/usr/bin/env python3
"""List textured PS1 primitives from a Stuntmaster .GP0 frame capture."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def signed_11(value: int) -> int:
    value &= 0x7FF
    return value - 0x800 if value & 0x400 else value


def read_capture(path: Path) -> tuple[tuple[int, int, int, int], list[list[int]]]:
    data = path.read_bytes()
    words = memoryview(data).cast("I")
    if len(words) < 6 or words[0] != 0x534D4750:
        raise ValueError(f"{path}: invalid GP0 capture")
    display = tuple(words[1:5])
    packet_count = words[5]
    offset = 6
    packets: list[list[int]] = []
    for _ in range(packet_count):
        count = words[offset]
        offset += 1
        packets.append(list(words[offset : offset + count]))
        offset += count
    return display, packets


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--tpage", type=lambda value: int(value, 0))
    parser.add_argument("--clut", type=lambda value: int(value, 0))
    args = parser.parse_args()

    display, packets = read_capture(args.capture)
    offset_x, offset_y = display[0], display[1]
    for packet_index, packet in enumerate(packets):
        if not packet:
            continue
        opcode = packet[0] >> 24
        if opcode == 0xE5:
            offset_x = signed_11(packet[0])
            offset_y = signed_11(packet[0] >> 11)
            continue
        if not 0x20 <= opcode <= 0x3F or opcode & 0x04 == 0:
            continue

        vertex_count = 4 if opcode & 0x08 else 3
        gouraud = bool(opcode & 0x10)
        positions: list[tuple[int, int]] = []
        uvs: list[tuple[int, int]] = []
        colors: list[tuple[int, int, int]] = []
        clut = 0
        tpage = 0
        word_index = 1
        for vertex in range(vertex_count):
            if vertex and gouraud:
                color = packet[word_index]
                colors.append(
                    (color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF)
                )
                word_index += 1
            elif vertex == 0:
                color = packet[0]
                colors.append(
                    (color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF)
                )
            position = packet[word_index]
            uv_word = packet[word_index + 1]
            positions.append(
                (
                    signed_11(position) + offset_x,
                    signed_11(position >> 16) + offset_y,
                )
            )
            uvs.append((uv_word & 0xFF, (uv_word >> 8) & 0xFF))
            if vertex == 0:
                clut = uv_word >> 16
            elif vertex == 1:
                tpage = uv_word >> 16
            word_index += 2

        if args.tpage is not None and tpage != args.tpage:
            continue
        if args.clut is not None and clut != args.clut:
            continue
        print(
            f"packet={packet_index} opcode=0x{opcode:02X} "
            f"tpage=0x{tpage:04X} clut=0x{clut:04X} "
            f"xy={positions} uv={uvs} rgb={colors}"
        )


if __name__ == "__main__":
    main()
