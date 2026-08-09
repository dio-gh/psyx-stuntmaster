#!/usr/bin/env python3
"""List screen-spanning untextured primitives in captured Stuntmaster GP0 frames."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


MAGIC = 0x534D4750


def signed11(value: int) -> int:
    value &= 0x7FF
    return value - 0x800 if value & 0x400 else value


def read_frame(path: Path) -> tuple[int, int, int, int, list[list[int]]]:
    data = path.read_bytes()
    if len(data) % 4:
        raise ValueError("size is not word-aligned")
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    if len(words) < 6 or words[0] != MAGIC:
        raise ValueError("not a captured Stuntmaster GP0 frame")
    display_x, display_y, width, height, count = words[1:6]
    cursor = 6
    packets: list[list[int]] = []
    for _ in range(count):
        if cursor >= len(words):
            raise ValueError("truncated packet table")
        length = words[cursor]
        cursor += 1
        if cursor + length > len(words):
            raise ValueError("truncated packet")
        packets.append(words[cursor : cursor + length])
        cursor += length
    return display_x, display_y, width, height, packets


def rgb(word: int) -> tuple[int, int, int]:
    return word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF


def candidates(path: Path) -> list[str]:
    display_x, display_y, width, height, packets = read_frame(path)
    offset_x = display_x
    offset_y = display_y
    found: list[str] = []
    for packet_index, packet in enumerate(packets):
        if not packet:
            continue
        opcode = packet[0] >> 24
        if opcode == 0xE5:
            offset_x = signed11(packet[0])
            offset_y = signed11(packet[0] >> 11)
            continue

        positions: list[int] = []
        colors: list[tuple[int, int, int]] = []
        kind = ""
        if 0x60 <= opcode <= 0x63 and len(packet) >= 3:
            x = signed11(packet[1]) + offset_x - display_x
            y = signed11(packet[1] >> 16) + offset_y - display_y
            w = packet[2] & 0xFFFF
            h = packet[2] >> 16
            positions = [x, x + w]
            colors = [rgb(packet[0])]
            kind = f"tile y={y}..{y + h}"
        elif 0x20 <= opcode <= 0x3F and not opcode & 0x04:
            vertices = 4 if opcode & 0x08 else 3
            gouraud = bool(opcode & 0x10)
            cursor = 1
            ys: list[int] = []
            for vertex in range(vertices):
                if vertex and gouraud:
                    colors.append(rgb(packet[cursor]))
                    cursor += 1
                elif vertex == 0:
                    colors.append(rgb(packet[0]))
                if cursor >= len(packet):
                    break
                positions.append(
                    signed11(packet[cursor]) + offset_x - display_x
                )
                ys.append(
                    signed11(packet[cursor] >> 16) + offset_y - display_y
                )
                cursor += 1
            if len(positions) != vertices:
                continue
            kind = f"{'quad' if vertices == 4 else 'tri'} y={min(ys)}..{max(ys)}"
        else:
            continue

        minimum = min(positions)
        maximum = max(positions)
        # Include almost-spanning candidates so an off-by-one or split overlay
        # cannot hide from the report.
        if minimum <= 8 and maximum >= width - 8:
            found.append(
                f"#{packet_index} op=0x{opcode:02X} {kind} "
                f"x={minimum}..{maximum} colors={colors}"
            )
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.paths:
        try:
            found = candidates(path)
        except (OSError, ValueError) as error:
            print(f"{path}: {error}")
            continue
        if found:
            print(path)
            for line in found:
                print(f"  {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

