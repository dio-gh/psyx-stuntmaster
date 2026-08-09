#!/usr/bin/env python3
"""Shared loader for retail code images.

Two kinds of image carry retail R3000A code:

* the boot PS-X EXE, whose header supplies its own text address;
* the `*_REL.BIN` overlays, which are raw code with no header and must be
  told where they load.

Much of the object and gameplay logic lives in the overlays rather than the
boot executable, so RE tools need both. Overlays share the boot executable's
`$gp`, which is why gp-relative global scans work across all of them.

Known overlay slots, from the retail `GAME_REL.SYM` overlay table:

    0x80010000  overlay ids 3, 4   42,840 bytes   OL2_REL.BIN
    0x8001A758  overlay ids 6, 7   47,300 bytes   NBOL_REL.BIN

`OL1_REL.BIN` (39,360) and `BOL_REL.BIN` (32,660) are not in that table. Their
names and sizes pair them with the two slots above, but that is unverified —
pass `--load-address` explicitly and record any confirmation in
`docs/re/README.md`.
"""

from __future__ import annotations

from pathlib import Path

PSX_EXE_HEADER_SIZE = 0x800
PSX_EXE_SIGNATURE = b"PS-X EXE"

# The runtime global pointer for the retail build. Overlays share it.
RETAIL_GP = 0x800DC94C


class CodeImage:
    """A flat block of guest code with a known base address."""

    def __init__(self, path: Path, base_address: int, code: bytes) -> None:
        self.path = path
        self.base_address = base_address
        self.code = code

    @property
    def end_address(self) -> int:
        return self.base_address + len(self.code)

    def words(self):
        """Yield (address, word) for every aligned word in the image."""
        for offset in range(0, len(self.code) - 3, 4):
            yield (
                self.base_address + offset,
                int.from_bytes(self.code[offset : offset + 4], "little"),
            )

    def slice(self, address: int, byte_count: int) -> bytes:
        if address < self.base_address or (
            address + byte_count > self.end_address
        ):
            raise SystemExit(
                f"range 0x{address:08X}..0x{address + byte_count:08X} "
                f"is outside 0x{self.base_address:08X}..0x{self.end_address:08X}"
            )
        offset = address - self.base_address
        return self.code[offset : offset + byte_count]


def load(path: Path, load_address: int | None = None) -> CodeImage:
    """Load a PS-X EXE or a raw overlay.

    `load_address` is required for raw overlays and rejected for a PS-X EXE,
    whose header is authoritative.
    """
    image = path.read_bytes()
    is_psx_exe = (
        len(image) >= PSX_EXE_HEADER_SIZE
        and image[:8] == PSX_EXE_SIGNATURE
    )

    if is_psx_exe:
        if load_address is not None:
            raise SystemExit(
                f"{path}: --load-address does not apply to a PS-X EXE; its "
                "header supplies the text address"
            )
        text_address = int.from_bytes(image[0x18:0x1C], "little")
        text_size = int.from_bytes(image[0x1C:0x20], "little")
        code = image[PSX_EXE_HEADER_SIZE : PSX_EXE_HEADER_SIZE + text_size]
        return CodeImage(path, text_address, code)

    if load_address is None:
        raise SystemExit(
            f"{path}: not a PS-X EXE. Raw overlays need --load-address "
            "(see tools/psx_image.py for the known overlay slots)."
        )
    return CodeImage(path, load_address, image)
