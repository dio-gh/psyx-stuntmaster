#!/usr/bin/env python3
"""Find exact and near-shared polygon edges in a captured GP0 frame."""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from pathlib import Path

from list_textured_gp0 import read_capture, signed_11


@dataclass(frozen=True)
class Domain:
    clip: tuple[int, int, int, int]
    offset: tuple[int, int]


@dataclass(frozen=True)
class Edge:
    packet: int
    a: tuple[int, int]
    b: tuple[int, int]
    domain: Domain


def polygon_positions(packet: list[int], offset: tuple[int, int]) -> list[tuple[int, int]]:
    opcode = packet[0] >> 24
    vertices = 4 if opcode & 0x08 else 3
    textured = bool(opcode & 0x04)
    gouraud = bool(opcode & 0x10)
    result: list[tuple[int, int]] = []
    word = 1
    for vertex in range(vertices):
        if vertex and gouraud:
            word += 1
        xy = packet[word]
        result.append((signed_11(xy) + offset[0], signed_11(xy >> 16) + offset[1]))
        word += 1 + int(textured)
    return result


def edges(path: Path) -> list[Edge]:
    display, packets = read_capture(path)
    clip = (display[0], display[1], display[0] + display[2] - 1, display[1] + display[3] - 1)
    offset = (display[0], display[1])
    result: list[Edge] = []
    for packet_index, packet in enumerate(packets):
        if not packet:
            continue
        opcode = packet[0] >> 24
        if opcode == 0xE3:
            clip = (packet[0] & 0x3FF, (packet[0] >> 10) & 0x1FF, clip[2], clip[3])
        elif opcode == 0xE4:
            clip = (clip[0], clip[1], packet[0] & 0x3FF, (packet[0] >> 10) & 0x1FF)
        elif opcode == 0xE5:
            offset = (signed_11(packet[0]), signed_11(packet[0] >> 11))
        if not 0x20 <= opcode <= 0x3F:
            continue
        points = polygon_positions(packet, offset)
        domain = Domain(clip, offset)
        for index, point in enumerate(points):
            other = points[(index + 1) % len(points)]
            if point != other:
                result.append(Edge(packet_index, point, other, domain))
    return result


def canonical(edge: Edge) -> tuple[Domain, tuple[int, int], tuple[int, int]]:
    return (edge.domain, *sorted((edge.a, edge.b)))


def endpoint_distance(left: Edge, right: Edge) -> int:
    direct = max(
        abs(left.a[0] - right.a[0]), abs(left.a[1] - right.a[1]),
        abs(left.b[0] - right.b[0]), abs(left.b[1] - right.b[1]))
    reverse = max(
        abs(left.a[0] - right.b[0]), abs(left.a[1] - right.b[1]),
        abs(left.b[0] - right.a[0]), abs(left.b[1] - right.a[1]))
    return min(direct, reverse)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--tolerance", type=int, default=2)
    args = parser.parse_args()

    all_edges = edges(args.capture)
    buckets: dict[tuple[Domain, tuple[int, int], tuple[int, int]], list[Edge]] = {}
    for edge in all_edges:
        buckets.setdefault(canonical(edge), []).append(edge)
    boundaries = [items[0] for items in buckets.values() if len(items) == 1]
    near: list[tuple[float, int, Edge, Edge]] = []
    for index, left in enumerate(boundaries):
        for right in boundaries[index + 1:]:
            if left.domain != right.domain or left.packet == right.packet:
                continue
            distance = endpoint_distance(left, right)
            if 0 < distance <= args.tolerance:
                length = math.dist(left.a, left.b)
                if length >= 4:
                    near.append((length, distance, left, right))
    near.sort(key=lambda item: (-item[0], -item[1]))
    print(
        f"edges={len(all_edges)} exact_shared={sum(len(v) > 1 for v in buckets.values())} "
        f"boundaries={len(boundaries)} near_shared={len(near)}")
    for length, distance, left, right in near[:80]:
        print(
            f"length={length:.1f} delta={distance} packets={left.packet},{right.packet} "
            f"edge={left.a}->{left.b} near={right.a}->{right.b} "
            f"offset={left.domain.offset} clip={left.domain.clip}")


if __name__ == "__main__":
    main()
