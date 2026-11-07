#!/usr/bin/env python3
"""Extract ordinal -> name maps from Microsoft Windows Mobile import libraries.

The Windows Mobile 6 Professional SDK installs a `Lib/ARMV4I/` directory
that contains (among others) `coredll.lib`, `aygshell.lib`, `gx.lib` and
`hss.lib`. Each `.lib` is a Microsoft COFF archive whose members are
"short import" objects. Each short import member encodes the DLL name,
the symbol name, and the ordinal in a compact 20-byte header.

This script parses those archives and writes
`crates/pocket-winceapi/data/<dll>-ordinals.json`, the same format that
PocketHLE consumes at runtime.

Usage:
    python3 tools/extract-ordinals.py \\
        --sdk "C:/Program Files (x86)/Windows Mobile 6 SDK/PocketPC/Lib/ARMV4I" \\
        --out crates/pocket-winceapi/data

It is also safe to point `--sdk` at a directory with just a few `.lib`
files copied out of the SDK install — the script just walks the
directory and processes anything ending in `.lib`.

This script is the only part of the build that needs the Microsoft SDK.
The generated JSON files are clean-room data (just integer -> string
maps) and are safe to commit.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path


# Microsoft COFF Archive: starts with the 8-byte magic "!<arch>\n",
# followed by a series of 60-byte member headers each terminated by 0x60 0x0A.
ARCHIVE_MAGIC = b"!<arch>\n"

# Short import object: machine == IMPORT_OBJECT_HDR_SIG2 == 0x0000.
# layout: u16 sig1, u16 sig2, u16 version, u16 machine, u32 timedatestamp,
# u32 size_of_data, u16 ordinal_or_hint, u16 type_field
SHORT_IMPORT_HEADER = struct.Struct("<HHHHIIHH")

IMPORT_OBJECT_HDR_SIG1 = 0x0000
IMPORT_OBJECT_HDR_SIG2 = 0xFFFF


def parse_archive(path: Path) -> list[tuple[int, str]]:
    """Return a sorted list of (ordinal, symbol_name) entries from `path`."""
    data = path.read_bytes()
    if not data.startswith(ARCHIVE_MAGIC):
        raise ValueError(f"{path}: not a COFF archive (missing magic)")

    out: list[tuple[int, str]] = []
    pos = len(ARCHIVE_MAGIC)
    while pos + 60 <= len(data):
        header = data[pos : pos + 60]
        # Member size is bytes 48..58 as ASCII decimal.
        try:
            member_size = int(header[48:58].rstrip().decode("ascii"))
        except ValueError:
            break
        member_start = pos + 60
        member_end = member_start + member_size
        if member_end > len(data):
            break
        member = data[member_start:member_end]

        if len(member) >= SHORT_IMPORT_HEADER.size:
            sig1, sig2, _ver, _mach, _ts, size_of_data, ord_or_hint, _type = (
                SHORT_IMPORT_HEADER.unpack_from(member)
            )
            if (
                sig1 == IMPORT_OBJECT_HDR_SIG1
                and sig2 == IMPORT_OBJECT_HDR_SIG2
                and size_of_data > 0
                and ord_or_hint > 0
            ):
                blob = member[SHORT_IMPORT_HEADER.size : SHORT_IMPORT_HEADER.size + size_of_data]
                # blob = symbol_name '\0' dll_name '\0'
                if b"\x00" in blob:
                    sym = blob.split(b"\x00", 1)[0].decode("ascii", errors="replace")
                    if sym:
                        out.append((ord_or_hint, sym))

        # Members are 2-byte aligned.
        if member_size % 2 == 1:
            member_size += 1
        pos = member_start + member_size

    # Deduplicate ordinals; keep the first symbol seen.
    seen: dict[int, str] = {}
    for ord_, sym in out:
        seen.setdefault(ord_, sym)
    return sorted(seen.items())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--sdk",
        required=True,
        help="Path to the SDK Lib/ARMV4I directory (or any directory of .lib files).",
    )
    ap.add_argument(
        "--out",
        default="crates/pocket-winceapi/data",
        help="Where to write the JSON files.",
    )
    args = ap.parse_args()

    sdk_dir = Path(args.sdk)
    out_dir = Path(args.out)
    if not sdk_dir.is_dir():
        ap.error(f"{sdk_dir} does not exist")
    out_dir.mkdir(parents=True, exist_ok=True)

    libs = sorted(sdk_dir.glob("*.lib"))
    if not libs:
        ap.error(f"no .lib files found under {sdk_dir}")

    for lib in libs:
        dll_stem = lib.stem.lower()
        try:
            entries = parse_archive(lib)
        except Exception as e:
            print(f"skip {lib.name}: {e}", file=sys.stderr)
            continue
        if not entries:
            print(f"skip {lib.name}: no short-import entries", file=sys.stderr)
            continue

        out_path = out_dir / f"{dll_stem}-ordinals.json"
        payload = {
            "dll": f"{dll_stem}.dll",
            "_comment": (
                f"Auto-generated from {lib.name} by tools/extract-ordinals.py."
                " Do not edit by hand; rerun the script to refresh."
            ),
            "ordinals": {str(o): s for o, s in entries},
        }
        out_path.write_text(json.dumps(payload, indent=2, sort_keys=False) + "\n")
        print(f"{lib.name}: wrote {len(entries)} entries -> {out_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
