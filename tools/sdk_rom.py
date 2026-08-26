"""ROM identity helpers for the headless snesrecomp SDK."""

from __future__ import annotations

import hashlib
import pathlib
import zlib
from typing import Optional


class RomVerifyError(ValueError):
    """Raised when a ROM fails size/extension/digest checks."""

    def __init__(self, message: str, *, details: Optional[dict] = None) -> None:
        super().__init__(message)
        self.details = details or {}


def normalize_hex_digest(value: str) -> str:
    text = value.strip().lower()
    if text.startswith("0x"):
        text = text[2:]
    return text


def compute_rom_identity(rom_path: pathlib.Path) -> dict:
    """Return digests and basic shape metadata for a ROM file."""
    path = pathlib.Path(rom_path).expanduser().resolve()
    if not path.is_file():
        raise RomVerifyError(f"ROM not found: {path}")

    suffix = path.suffix.lower()
    if suffix not in (".sfc", ".smc"):
        raise RomVerifyError(
            f"ROM must be a .sfc or .smc file (got {suffix or 'no extension'})",
            details={"path": str(path), "suffix": suffix},
        )

    raw = path.read_bytes()
    size = len(raw)
    if size < 32 * 1024 or size > 16 * 1024 * 1024:
        raise RomVerifyError(
            "ROM size is outside the supported 32 KiB to 16 MiB range",
            details={"path": str(path), "size": size},
        )
    if size % 1024 not in (0, 512):
        raise RomVerifyError(
            "ROM size is not a standard SNES image size",
            details={"path": str(path), "size": size},
        )

    crc32 = f"{zlib.crc32(raw) & 0xFFFFFFFF:08x}"
    sha256 = hashlib.sha256(raw).hexdigest()
    return {
        "path": str(path),
        "name": path.name,
        "size": size,
        "has_copier_header": (size % 1024) == 512,
        "crc32": crc32,
        "sha256": sha256,
    }


def verify_rom(
    rom_path: pathlib.Path,
    *,
    expected_crc32: Optional[str] = None,
    expected_sha256: Optional[str] = None,
) -> dict:
    """Validate ROM shape and optional expected digests.

    Digests are compared case-insensitively; a leading ``0x`` is accepted on
    CRC32 inputs. Returns the identity dict on success.
    """
    identity = compute_rom_identity(rom_path)
    mismatches = []

    if expected_crc32:
        want = normalize_hex_digest(expected_crc32)
        if len(want) != 8 or any(c not in "0123456789abcdef" for c in want):
            raise RomVerifyError(
                f"invalid expected CRC32: {expected_crc32!r}",
                details={"expected_crc32": expected_crc32},
            )
        if identity["crc32"] != want:
            mismatches.append({
                "field": "crc32",
                "expected": want,
                "actual": identity["crc32"],
            })

    if expected_sha256:
        want = normalize_hex_digest(expected_sha256)
        if len(want) != 64 or any(c not in "0123456789abcdef" for c in want):
            raise RomVerifyError(
                f"invalid expected SHA-256: {expected_sha256!r}",
                details={"expected_sha256": expected_sha256},
            )
        if identity["sha256"] != want:
            mismatches.append({
                "field": "sha256",
                "expected": want,
                "actual": identity["sha256"],
            })

    if mismatches:
        raise RomVerifyError(
            "ROM identity mismatch",
            details={
                "path": identity["path"],
                "actual": {
                    "crc32": identity["crc32"],
                    "sha256": identity["sha256"],
                    "size": identity["size"],
                },
                "mismatches": mismatches,
            },
        )

    identity["verified"] = bool(expected_crc32 or expected_sha256)
    return identity
