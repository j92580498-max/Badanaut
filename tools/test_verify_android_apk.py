"""Exercise APK validation using synthetic ELF headers and ZIP entries."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("verify_apk", Path(__file__).with_name("verify-android-apk.py"))
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class ApkVerificationTest(unittest.TestCase):
    def entries(self):
        entries = {name: b"fixture" for name in ("AndroidManifest.xml", "classes.dex", "resources.arsc", "assets/launcher.json", "assets/fonts/LiberationSans-Regular.ttf", "assets/fonts/LiberationSans-Bold.ttf", "assets/fonts/LICENSE.txt")}
        entries["assets/launcher.json"] = (Path(__file__).resolve().parents[1] / "frontends/shared-ui/launcher.json").read_bytes()
        for abi, elf_class, machine in (("arm64-v8a", 2, 183), ("armeabi-v7a", 1, 40)):
            header = bytearray(64)
            header[:6] = b"\x7fELF" + bytes((elf_class, 1))
            struct.pack_into("<HH", header, 16, 3, machine)
            for library in ("libSDL2.so", "libunicorn.so", "libmain.so", "libc++_shared.so"):
                entries[f"lib/{abi}/{library}"] = header
        return entries

    def verify(self, entries):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.apk"
            with zipfile.ZipFile(path, "w") as apk:
                for name, content in entries.items():
                    apk.writestr(name, content)
            with contextlib.redirect_stdout(io.StringIO()):
                verifier.verify_apk(path)

    def test_accepts_bada_without_pocket_pc(self):
        self.verify(self.entries())

    def test_requires_the_shared_interface_in_the_apk(self):
        entries = self.entries()
        del entries["assets/launcher.json"]
        with self.assertRaisesRegex(ValueError, "assets/launcher.json"):
            self.verify(entries)

    def test_rejects_retained_browser_interface(self):
        entries = self.entries()
        entries["assets/index.html"] = b"old interface"
        with self.assertRaisesRegex(ValueError, "browser interface"):
            self.verify(entries)

    def test_rejects_a_truncated_launcher_string(self):
        entries = self.entries()
        entries["assets/launcher.json"] = b'{"settingsHelp": "Changes apply\n    "trigger": [280, 0, 40, 28]}}'
        with self.assertRaisesRegex(ValueError, "launcher.json is invalid"):
            self.verify(entries)

    def test_rejects_a_missing_menu_even_with_valid_json(self):
        entries = self.entries()
        launcher = json.loads(entries["assets/launcher.json"])
        del launcher["menu"]
        entries["assets/launcher.json"] = json.dumps(launcher).encode()
        with self.assertRaisesRegex(ValueError, "missing the menu object"):
            self.verify(entries)

    def test_rejects_retained_pocket_pc_engine(self):
        for abi in ("arm64-v8a", "armeabi-v7a", "x86_64"):
            with self.subTest(abi=abi):
                entries = self.entries()
                entries[f"lib/{abi}/libpockethle_jni.so"] = b"legacy"
                with self.assertRaisesRegex(ValueError, "Pocket PC"):
                    self.verify(entries)

    def test_requires_bada_and_unicorn_for_both_abis(self):
        for abi in ("arm64-v8a", "armeabi-v7a"):
            for library in ("libmain.so", "libunicorn.so"):
                with self.subTest(abi=abi, library=library):
                    entries = self.entries()
                    del entries[f"lib/{abi}/{library}"]
                    with self.assertRaisesRegex(ValueError, "missing"):
                        self.verify(entries)

    def test_rejects_library_for_wrong_cpu(self):
        entries = self.entries()
        entries["lib/arm64-v8a/libmain.so"] = entries["lib/armeabi-v7a/libmain.so"]
        with self.assertRaisesRegex(ValueError, "architecture"):
            self.verify(entries)


if __name__ == "__main__":
    unittest.main()
