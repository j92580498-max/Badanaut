"""Check the bundled ARM libraries, ABI, hashes and distribution notices."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "frontends/pocket-desktop"))
from arm_elf import CPP, GCC, SUPCXX, float_abi, runtime_filename


def verify_bundle(directory):
    directory = Path(directory)
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    required = {f"{abi}-float/{name}" for abi in ("soft", "hard") for name in (CPP, GCC, SUPCXX)}
    if not isinstance(manifest, dict) or manifest.get("schema") != 1 or not isinstance(manifest.get("files"), dict) or set(manifest["files"]) != required:
        raise ValueError("Bada runtime manifest must contain both complete float ABI sets")
    checksums = {}
    for line in (directory / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) != 2 or fields[1] not in required or fields[1] in checksums:
            raise ValueError("Invalid Bada runtime checksum list")
        checksums[fields[1]] = fields[0]
    if set(checksums) != required:
        raise ValueError("Bada runtime checksum list is incomplete")
    for name in sorted(required):
        path = directory / name
        abi = name.split("-float/")[0]
        metadata = manifest["files"][name]
        if not isinstance(metadata, dict):
            raise ValueError(f"Invalid runtime manifest entry: {name}")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if metadata.get("float_abi") != abi or float_abi(path) != abi:
            raise ValueError(f"Wrong ARM calling convention: {name}")
        if runtime_filename(path) != path.name:
            raise ValueError(f"Wrong runtime component: {name}")
        if actual != metadata.get("sha256") or actual != checksums[name]:
            raise ValueError(f"Runtime checksum mismatch: {name}")
    for name in ("COPYING.libgcc", "COPYING.libstdcxx"):
        notice = (directory / name).read_text(encoding="utf-8")
        if "GNU GENERAL PUBLIC LICENSE" not in notice or "GCC RUNTIME LIBRARY EXCEPTION" not in notice:
            raise ValueError(f"Missing GNU runtime license texts: {name}")
    for name in ("README.md", "SOURCE.md", "COPYING.RUNTIME"):
        if not (directory / name).is_file():
            raise ValueError(f"Missing runtime distribution notice: {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=ROOT / "bada_libs")
    args = parser.parse_args()
    try:
        verify_bundle(args.directory)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Bada runtime bundle verification failed: {error}\n")
    print(f"Verified Bada runtime bundle: {args.directory} (soft/hard, all three components)")
