"""Prepare local ARM runtimes from a user-supplied bada 2.0 SDK ZIP."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "frontends/pocket-desktop"))
from arm_elf import float_abi, runtime_filename, SUPCXX


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk_zip", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / ".build/bada-sdk-runtimes")
    parser.add_argument("--linker", type=Path, help="Host ARM linker; Windows uses the SDK's bundled linker by default")
    options = parser.parse_args()
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(options.sdk_zip) as sdk:
        def copy(name, destination, limit=32 * 1024 * 1024):
            entry = sdk.getinfo(name)
            if entry.file_size > limit:
                raise ValueError(f"Unexpected SDK input size: {name}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.resolve().is_relative_to(output):
                raise ValueError("SDK output path escapes the output directory")
            with sdk.open(entry) as source, destination.open("wb") as target:
                shutil.copyfileobj(source, target)

        linker = options.linker
        if linker is None:
            if sys.platform == "win32":
                linker = output / ".sdk-tools/arm-bada-eabi-ld.exe"
                copy("Tools/Toolchains/ARM/bin/arm-bada-eabi-ld.exe", linker)
            else:
                located = shutil.which("arm-bada-eabi-ld")
                if not located:
                    raise ValueError("Supply --linker with a host ARM GNU linker to build SDK C++ exception support")
                linker = Path(located)
        linker = linker.resolve()
        for abi, multilib in (("soft", ""), ("hard", "hard/armv6/vfp/")):
            directory = output / abi
            base = "Tools/Toolchains/ARM/arm-bada-eabi/lib/" + multilib
            for name in ("libstdc++.so.6", "libgcc_s.so.1", "libsupc++-xnew.a"):
                copy(base + name, directory / name)
            # SDK 2.0 libstdc++ imports its C++ exception ABI from the OS. Build
            # that companion from the supplied SDK archive without shipping it.
            subprocess.run([str(linker), "-shared", "-soname", SUPCXX, "--whole-archive",
                            str(directory / "libsupc++-xnew.a"), "--no-whole-archive",
                            "-o", str(directory / SUPCXX)], check=True)
            for name in ("libstdc++.so.6", "libgcc_s.so.1", SUPCXX):
                path = directory / name
                if float_abi(path) != abi or runtime_filename(path) != name:
                    raise ValueError(f"SDK runtime validation failed: {path}")
                print(path)
    print("Prepared SDK runtimes for inspection or manual overrides. Normal launches use the bundled bada_libs defaults.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
