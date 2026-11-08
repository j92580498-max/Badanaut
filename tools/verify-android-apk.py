#!/usr/bin/env python3
"""Verify the bada engine for both Android ABIs and reject Pocket PC libraries."""

import argparse
import struct
import zipfile


def verify_apk(path):
    libraries = ("libSDL2.so", "libunicorn.so", "libmain.so", "libc++_shared.so")
    architectures = {"arm64-v8a": (2, 183), "armeabi-v7a": (1, 40)}
    with zipfile.ZipFile(path) as apk:
        names = apk.namelist()
        if len(names) != len(set(names)):
            raise ValueError("APK contains duplicate entries")
        if any(name.startswith("lib/") and name.endswith("/libpockethle_jni.so") for name in names):
            raise ValueError("APK still contains the Pocket PC native engine")
        for name in ("AndroidManifest.xml", "classes.dex", "resources.arsc", "assets/index.html", "assets/style.css", "assets/app.js"):
            if name not in names or not apk.getinfo(name).file_size:
                raise ValueError(f"APK is missing {name}")
        for abi, (elf_class, machine) in architectures.items():
            for library in libraries:
                name = f"lib/{abi}/{library}"
                if name not in names:
                    raise ValueError(f"APK is missing {name}")
                with apk.open(name) as source:
                    header = source.read(52)
                if len(header) < 52 or header[:4] != b"\x7fELF" or header[4:6] != bytes((elf_class, 1)):
                    raise ValueError(f"Invalid ELF architecture: {name}")
                if struct.unpack_from("<HH", header, 16) != (3, machine):
                    raise ValueError(f"Invalid ELF type/machine: {name}")
    print(f"Verified APK: {path} (ARM64/ARMv7, bada native engine)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk")
    args = parser.parse_args()
    try:
        verify_apk(args.apk)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"APK verification failed: {error}\n")
