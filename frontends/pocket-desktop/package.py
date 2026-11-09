"""Build a self-contained desktop directory and ZIP/tar.gz artifact."""
from pathlib import Path
import os
import platform
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
native = root / ".build/desktop/native"
engine = native / ("bada-hle.exe" if os.name == "nt" else "bada-hle")
if not engine.is_file():
    raise SystemExit("Build the native engine before packaging")
command = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onedir", "--windowed",
           "--name", "Badanaut", "--distpath", str(root / "dist"), "--workpath", str(root / ".build/pyinstaller"),
           "--specpath", str(root / ".build"), "--add-data", f"{root / 'frontends/shared-ui'}{os.pathsep}shared-ui"]
for package in ("PySide6_Essentials", "shiboken6"):
    command += ["--copy-metadata", package]
for entry in (root / "third_party/notices.txt").read_text(encoding="utf-8").splitlines():
    notice = Path(entry)
    if not (root / notice).is_file():
        raise SystemExit(f"Missing distribution notice: {notice}; initialize Git submodules first")
    destination = (Path("notices") / notice.parent).as_posix()
    command += ["--add-data", f"{root / notice}{os.pathsep}{destination}"]
for binary in native.iterdir():
    if binary.is_file() and (binary.name == engine.name or binary.suffix.lower() == ".dll" or ".so" in binary.name):
        command += ["--add-binary", f"{binary}{os.pathsep}native"]
command += [str(Path(__file__).with_name("main.py"))]
subprocess.run(command, check=True)
arch = {"x86_64": "x64", "AMD64": "x64", "aarch64": "arm64"}.get(platform.machine(), platform.machine())
if os.name == "nt":
    shutil.make_archive(str(root / f"dist/Badanaut-Windows-{arch}"), "zip", root / "dist", "Badanaut")
else:
    shutil.make_archive(str(root / f"dist/Badanaut-Linux-{arch}"), "gztar", root / "dist", "Badanaut")
