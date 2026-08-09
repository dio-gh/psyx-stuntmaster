#!/usr/bin/env python3
"""Compare polygon attributes in one scanout-space region of two GP0 captures."""

from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path

from list_textured_gp0 import read_capture, signed_11


def polygon(packet: list[int], offset: tuple[int, int], display: tuple[int, ...]):
    opcode = packet[0] >> 24
    count = 4 if opcode & 0x08 else 3
    textured = bool(opcode & 0x04)
    gouraud = bool(opcode & 0x10)
    positions: list[tuple[int, int]] = []
    position_words: set[int] = set()
    word = 1
    for vertex in range(count):
        if vertex and gouraud:
            word += 1
        if word >= len(packet):
            return None
        position_words.add(word)
        xy = packet[word]
        positions.append((
            signed_11(xy),
            signed_11(xy >> 16),
        ))
        word += 1 + int(textured)
    attributes_list: list[int] = []
    word = 0
    uv_vertex = 0
    while word < len(packet):
        if word in position_words:
            word += 1
            continue
        value = packet[word]
        if textured and word > 1 and (word - 2) % (3 if gouraud else 2) == 0:
            if uv_vertex >= 2:
                value &= 0xFFFF
            uv_vertex += 1
        attributes_list.append(value)
        word += 1
    attributes = tuple(attributes_list)
    return tuple(positions), attributes


def regional(path: Path, bounds: tuple[int, int, int, int]):
    display, packets = read_capture(path)
    offset = (display[0], display[1])
    result: dict[tuple[tuple[int, int], ...], list[tuple[int, tuple[int, ...]]]] = (
        defaultdict(list)
    )
    for packet_index, packet in enumerate(packets):
        if not packet:
            continue
        opcode = packet[0] >> 24
        if opcode == 0xE5:
            offset = (signed_11(packet[0]), signed_11(packet[0] >> 11))
        if not 0x20 <= opcode <= 0x3F:
            continue
        described = polygon(packet, offset, display)
        if described is None:
            continue
        positions, attributes = described
        x0, y0, x1, y1 = bounds
        if (max(point[0] for point in positions) < x0 or
                min(point[0] for point in positions) >= x1 or
                max(point[1] for point in positions) < y0 or
                min(point[1] for point in positions) >= y1):
            continue
        result[positions].append((packet_index, attributes))
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("x", type=int)
    parser.add_argument("y", type=int)
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    args = parser.parse_args()
    bounds = (args.x, args.y, args.x + args.width, args.y + args.height)
    left = regional(args.left, bounds)
    right = regional(args.right, bounds)
    print(f"left_polygons={sum(map(len, left.values()))} "
          f"right_polygons={sum(map(len, right.values()))}")
    for positions in sorted(left.keys() | right.keys()):
        left_items = left.get(positions, [])
        right_items = right.get(positions, [])
        left_attributes = [item[1] for item in left_items]
        right_attributes = [item[1] for item in right_items]
        if left_attributes == right_attributes:
            continue
        print(f"positions={positions}")
        print(f"  left={left_items}")
        print(f"  right={right_items}")


if __name__ == "__main__":
    main()
