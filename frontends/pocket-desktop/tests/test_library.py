import base64
import json
from pathlib import Path
import sys
import struct
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from library import Library, DEFAULTS, ENTRY, settings
from arm_elf import CPP, GCC, SUPCXX, bundle_roots, float_abi, game_float_abi


def elf(abi=None, payload=b"", exports=()):
    data = bytearray(52)
    data[:7] = b"\x7fELF\x01\x01\x01"
    data[16:20] = b"\x03\x00\x28\x00"
    if abi is not None:
        attributes = b"\x05bada ARM\0\x06\x05" + (b"\x1c\x01" if abi == "hard" else b"")
        scope = b"\x01" + struct.pack("<I", 5 + len(attributes)) + attributes
        vendor = b"aeabi\0" + scope
        section = b"A" + struct.pack("<I", 4 + len(vendor)) + vendor
        struct.pack_into("<I", data, 32, 52)
        struct.pack_into("<HH", data, 46, 40, 1)
        data.extend(struct.pack("<10I", 0, 0x70000003, 0, 0, 92, len(section), 0, 0, 1, 0))
        data.extend(section)
    if exports:
        # Replace the small attribute-only section table with attributes,
        # dynamic strings and dynamic symbols. No game/SDK binaries in tests.
        section = bytes(data[92:])
        data = data[:52]
        count = 4
        struct.pack_into("<I", data, 32, 52)
        struct.pack_into("<HH", data, 46, 40, count)
        start = 52 + 40 * count
        strings, entries = bytearray(b"\0"), bytearray(16)
        for name in exports:
            entries.extend(struct.pack("<IIIBBH", len(strings), 0x1001, 4, 0x12, 0, 1))
            strings.extend(name.encode() + b"\0")
        data.extend(bytes(40))
        data.extend(struct.pack("<10I", 0, 0x70000003, 0, 0, start, len(section), 0, 0, 1, 0))
        data.extend(struct.pack("<10I", 0, 3, 0, 0, start + len(section), len(strings), 0, 0, 1, 0))
        data.extend(struct.pack("<10I", 0, 11, 0, 0, start + len(section) + len(strings), len(entries), 2, 0, 4, 16))
        data.extend(section + strings + entries)
    return data + payload


def sectionless_elf():
    data = bytearray(1024)
    data[:7] = b"\x7fELF\x01\x01\x01"
    data[16:20] = b"\x03\x00\x28\x00"
    struct.pack_into("<I", data, 28, 52)
    struct.pack_into("<I", data, 36, 0x5000002)
    struct.pack_into("<HHH", data, 40, 52, 32, 2)
    struct.pack_into("<8I", data, 52, 1, 0, 0x10000, 0, 1024, 1024, 5, 4096)
    struct.pack_into("<8I", data, 84, 2, 0x180, 0x10180, 0, 48, 48, 6, 4)
    struct.pack_into("<II", data, 0x100, 1, 2)
    struct.pack_into("<IIIBBH", data, 0x130, 1, 0, 0, 0x12, 0, 0)
    strings = b"\0__aeabi_fmul\0"
    data[0x200:0x200 + len(strings)] = strings
    tags = ((5, 0x10200), (10, len(strings)), (6, 0x10120), (11, 16), (4, 0x10100), (0, 0))
    for index, tag in enumerate(tags):
        struct.pack_into("<II", data, 0x180 + index * 8, *tag)
    return data


def png():
    return base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVR4nGOUW3CHgYGBiYGBgYGBAQASpAGenRzoTAAAAABJRU5ErkJggg==")


class LibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.library = Library(self.root / "library", bundle_dirs=[])

    def archive(self, extra=None, prefix=""):
        path = self.root / "game.zip"
        files = {"bin/Demo.EXE": elf(), "info/appinfo.sys": b'<Application AppId="demo-bada" Name="Demo &amp; Game"><Icon>icon.png</Icon></Application>',
                 "res/icon.png": b"\x89PNG\r\n\x1a\n" + b"test", "Res/save.dat": b"saved progress"}
        files.update(extra or {})
        with zipfile.ZipFile(path, "w") as out:
            for name, data in files.items():
                out.writestr(prefix + name, data)
        return path

    def test_nested_package_retains_metadata_resources_and_icon(self):
        game_id = self.library.import_game(self.archive(prefix="outer/package/"))
        game = self.library.list_games()[0]
        self.assertEqual(game["name"], "Demo & Game")
        self.assertEqual(game["appId"], "demo-bada")
        self.assertTrue(game["icon"].startswith("data:image/png;base64,"))
        self.assertEqual((self.library.directory(game_id) / "Res/save.dat").read_bytes(), b"saved progress")
        self.assertEqual(len(list(self.library.games.iterdir())), 1)

    def test_empty_icon_metadata_uses_executable_named_artwork(self):
        game_id = self.library.import_game(self.archive({"info/appinfo.sys": b'<Application AppId="demo" Name="Demo"><Icon/></Application>',
                                                         "Res/DEMO.PNG": b"\x89PNG\r\n\x1a\n" + b"game artwork"}))
        root = self.library.directory(game_id)
        (root / "Res/icon.png").unlink()
        self.assertEqual(base64.b64decode(self.library.list_games()[0]["icon"].split(",")[1]), b"\x89PNG\r\n\x1a\n" + b"game artwork")
        (root / "_badanaut-icon.png").write_bytes(b"\x89PNG\r\n\x1a\n" + b"custom artwork")
        self.assertEqual(base64.b64decode(self.library.list_games()[0]["icon"].split(",")[1]), b"\x89PNG\r\n\x1a\n" + b"custom artwork")

    def test_icon_metadata_resolves_nested_paths_without_case_sensitivity(self):
        game_id = self.library.import_game(self.archive({
            "info/appinfo.sys": b'<Application AppId="demo"><Icon>RES\\ICONS\\MENU.PNG</Icon></Application>',
            "Res/Icons/Menu.png": png(),
        }))
        (self.library.directory(game_id) / "Res/icon.png").unlink()
        self.assertEqual(base64.b64decode(self.library.list_games()[0]["icon"].split(",")[1]), png())

    def test_missing_icon_metadata_finds_nested_artwork_in_existing_packages(self):
        game_id = self.library.import_game(self.archive({
            "info/appinfo.sys": b'<Application AppId="demo"/>', "Res/Icons/MainMenu.png": png(),
        }))
        root = self.library.directory(game_id)
        (root / "Res/icon.png").unlink()
        self.assertEqual(base64.b64decode(self.library.list_games()[0]["icon"].split(",")[1]), png())
        (root / "_badanaut-icon.png").write_text("not an image")
        self.assertEqual(base64.b64decode(self.library.list_games()[0]["icon"].split(",")[1]), png())

    def test_icon_metadata_cannot_escape_the_package(self):
        game_id = self.library.import_game(self.archive({"res/icon.png": png()}))
        root = self.library.directory(game_id)
        (root.parent / "outside.png").write_bytes(b"\x89PNG\r\n\x1a\n" + b"outside artwork")
        metadata = json.loads((root / ENTRY).read_text())
        metadata["icon"] = "../outside.png"
        (root / ENTRY).write_text(json.dumps(metadata))
        self.assertEqual(base64.b64decode(self.library.list_games()[0]["icon"].split(",")[1]), png())

    def test_icon_discovery_cannot_follow_symlinks_outside_the_package(self):
        game_id = self.library.import_game(self.archive({"res/icon.png": png()}))
        root = self.library.directory(game_id)
        outside = self.root / "outside-icons"
        outside.mkdir()
        (outside / "MainMenu.png").write_bytes(b"\x89PNG\r\n\x1a\n" + b"outside artwork")
        try:
            (root / "_badanaut-icon.png").symlink_to(outside / "MainMenu.png")
            (root / "Res/Icons").symlink_to(outside, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Symlinks are unavailable: {error}")
        metadata = json.loads((root / ENTRY).read_text())
        metadata["icon"] = "Res/Icons/MainMenu.png"
        (root / ENTRY).write_text(json.dumps(metadata))
        self.assertEqual(base64.b64decode(self.library.list_games()[0]["icon"].split(",")[1]), png())
        (root / "Res/icon.png").unlink()
        self.assertIsNone(self.library.list_games()[0]["icon"])

    def test_failed_import_does_not_leave_partial_installation(self):
        for name in ("../escape", "C:/escape", "Res/CON.txt", "Res/trailing.", "Res/save.dat ", "RES/SAVE.DAT"):
            with self.subTest(name=name), self.assertRaises((ValueError, OSError)):
                self.library.import_game(self.archive({name: b"unsafe"}))
            self.assertEqual(list(self.library.games.iterdir()), [])

    def test_windows_pe_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "ARM ELF"):
            self.library.import_game(self.archive({"bin/Demo.EXE": b"MZ" + bytes(50)}))

    def test_external_entities_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "XML declarations"):
            self.library.import_game(self.archive({"info/appinfo.sys": b'<!DOCTYPE Application [<!ENTITY x SYSTEM "file:///etc/passwd">]><Application AppId="demo"/>'}))

    def test_missing_runtime_blocks_launch_and_wrong_runtime_is_rejected(self):
        game_id = self.library.import_game(self.archive())
        with self.assertRaisesRegex(ValueError, "runtime"):
            self.library.prepare(game_id)
        runtime = self.root / "libstdc++.so.6"
        runtime.write_bytes(b"MZ" + bytes(50))
        with self.assertRaisesRegex(ValueError, "ARM"):
            self.library.import_runtime(runtime)
        self.assertFalse(self.library.bootstrap()["runtimeReady"])

    def test_launch_uses_saved_settings_and_original_package(self):
        game_id = self.library.import_game(self.archive())
        runtime = self.root / "runtime"
        runtime.write_bytes(elf())
        self.library.import_runtime(runtime)
        self.assertIn("--fullscreen=true", self.library.prepare(game_id))
        self.library.save_settings(dict(scaling="integer", filter="linear", vsync=False, fullscreen=False))
        args = Library(self.library.root).prepare(game_id)
        self.assertNotIn("--resolution", args)
        self.assertEqual(args[args.index("--scaling") + 1], "integer")
        self.assertEqual(args[args.index("--filter") + 1], "linear")
        self.assertEqual(args[args.index("--input") + 1], str(self.library.directory(game_id)))
        self.assertIn("--vsync=false", args)
        self.assertIn("--fullscreen=false", args)
        self.library.played(game_id)
        self.assertGreater(self.library.list_games()[0]["lastPlayed"], 0)

    def test_api_version_is_independent_of_manifest_schema_and_float_abi(self):
        for api, abi in (("2.0", "hard"), ("2.0", "soft"), ("1.2", "hard")):
            with self.subTest(api=api, abi=abi):
                game_id = self.library.import_game(self.archive({
                    "bin/Demo.EXE": elf(abi),
                    "info/manifest.xml": f"<Manifest><ManifestVersion>1.2</ManifestVersion><APIVersion>{api}</APIVersion></Manifest>".encode(),
                }))
                data = json.loads((self.library.directory(game_id) / ENTRY).read_text())
                self.assertEqual(data["api_version"], api)
                self.assertEqual(data["float_abi"], abi)
        game_id = self.library.import_game(self.archive({"info/manifest.xml": b"<Manifest><ManifestVersion>2.0</ManifestVersion></Manifest>"}))
        self.assertEqual(json.loads((self.library.directory(game_id) / ENTRY).read_text())["api_version"], "")

    def test_runtime_import_preserves_versions_and_selects_matching_abi(self):
        hard = self.root / "hard.so"
        soft = self.root / "soft.so"
        hard.write_bytes(elf("hard", b"older runtime"))
        soft.write_bytes(elf("soft", b"SDK 2 runtime"))
        self.library.import_runtime(hard)
        self.library.import_runtime(soft)
        for abi, source in (("hard", hard), ("soft", soft)):
            game_id = self.library.import_game(self.archive({"bin/Demo.EXE": elf(abi)}))
            args = self.library.prepare(game_id)
            runtime = Path(args[args.index("--libstdcxx") + 1])
            self.assertEqual(runtime.read_bytes(), source.read_bytes())
        self.assertEqual(len(self.library.runtimes()), 2)
        hard.write_bytes(elf("hard", b"newer runtime"))
        self.library.import_runtime(hard)
        self.assertEqual(len(self.library.runtimes()), 3)
        self.assertTrue(self.library.bootstrap()["runtimeReady"])

    def test_import_keeps_legacy_runtime_and_launches_old_metadata(self):
        legacy = self.library.root / "libstdc++.so.6"
        legacy.write_bytes(elf("hard"))
        soft = self.root / "soft.so"
        soft.write_bytes(elf("soft"))
        self.library.import_runtime(soft)
        self.assertEqual(legacy.read_bytes(), elf("hard"))
        game_id = self.library.import_game(self.archive({"bin/Demo.EXE": elf("hard")}))
        metadata = self.library.directory(game_id) / ENTRY
        data = json.loads(metadata.read_text())
        del data["float_abi"], data["api_version"]
        data["last_played"] = 123
        metadata.write_text(json.dumps(data))
        self.assertIn(str(legacy), self.library.prepare(game_id))
        self.assertEqual(json.loads(metadata.read_text())["last_played"], 123)

    def test_wrong_float_runtime_does_not_launch(self):
        game_id = self.library.import_game(self.archive({"bin/Demo.EXE": elf("hard")}))
        soft = self.root / "soft.so"
        soft.write_bytes(elf("soft"))
        self.library.import_runtime(soft)
        with self.assertRaisesRegex(ValueError, "hard-float"):
            self.library.prepare(game_id)

    def test_malformed_runtime_attributes_are_rejected_without_replacing_library(self):
        runtime = self.root / "runtime"
        runtime.write_bytes(elf("hard"))
        self.library.import_runtime(runtime)
        malformed = elf("soft")
        struct.pack_into("<I", malformed, 72, 65536)
        runtime.write_bytes(malformed)
        with self.assertRaisesRegex(ValueError, "attributes"):
            self.library.import_runtime(runtime)
        self.assertEqual(len(self.library.runtimes()), 1)
        self.assertEqual(self.library.runtimes()[0][1].read_bytes(), elf("hard"))

    def test_support_libraries_are_identified_by_exports_and_match_the_game_abi(self):
        game_id = self.library.import_game(self.archive({"bin/Demo.EXE": elf("hard")}))
        for filename, names in (("cpp", ("_ZNSsC1Ev", "__cxa_throw")),
                                ("unwinder", ("_Unwind_RaiseException", "__gnu_unwind_frame")),
                                ("exceptions", ("__cxa_throw", "__gxx_personality_v0"))):
            source = self.root / filename  # Selection must not depend on its name.
            source.write_bytes(elf("hard", exports=names))
            self.library.import_runtime(source)
        args = self.library.prepare(game_id)
        self.assertEqual(Path(args[args.index("--libgcc") + 1]).name, GCC)
        self.assertEqual(Path(args[args.index("--libsupcxx") + 1]).name, SUPCXX)
        self.assertTrue(self.library.bootstrap()["runtimeReady"])
        self.assertEqual(len(self.library.runtimes()), 1)
        soft = self.library.import_game(self.archive({"bin/Demo.EXE": elf("soft")}))
        source = self.root / "soft"
        source.write_bytes(elf("soft"))
        self.library.import_runtime(source)
        soft_args = self.library.prepare(soft)
        self.assertNotIn("--libgcc", soft_args)
        self.assertNotIn("--libsupcxx", soft_args)

    def bundled_library(self, engine=None):
        bundle = (self.root / "bada_libs").resolve()
        for abi in ("hard", "soft"):
            directory = bundle / f"{abi}-float"
            directory.mkdir(parents=True, exist_ok=True)
            for filename in (CPP, GCC, SUPCXX):
                (directory / filename).write_bytes(elf(abi, f"bundled {abi} {filename}".encode()))
        roots = bundle_roots(engine) if engine else [bundle]
        return Library(self.root / "automatic-library", bundle_dirs=roots), bundle

    def test_fresh_install_automatically_selects_all_bundled_components(self):
        library, bundle = self.bundled_library()
        self.assertTrue(library.bootstrap()["runtimeReady"])
        for abi, flag in (("hard", 0x400), ("soft", 0x200)):
            executable = elf()  # Only header flags, with no ARM attributes.
            struct.pack_into("<I", executable, 36, flag)
            game = library.import_game(self.archive({"bin/Demo.EXE": executable}))
            arguments = library.prepare(game)
            for option, filename in (("--libstdcxx", CPP), ("--libgcc", GCC), ("--libsupcxx", SUPCXX)):
                self.assertEqual(Path(arguments[arguments.index(option) + 1]), bundle / f"{abi}-float" / filename)
        self.assertFalse((library.root / CPP).exists())
        self.assertFalse((library.root / "runtimes").exists())

    def test_runtime_import_overrides_bundle_without_replacing_it(self):
        library, bundle = self.bundled_library()
        game = library.import_game(self.archive({"bin/Demo.EXE": elf("hard")}))
        bundled_bytes = (bundle / "hard-float" / CPP).read_bytes()
        override = self.root / "override"
        override.write_bytes(elf("hard", b"manual runtime"))
        library.import_runtime(override)
        arguments = library.prepare(game)
        selected = Path(arguments[arguments.index("--libstdcxx") + 1])
        self.assertTrue(selected.is_relative_to(library.root / "runtimes"))
        self.assertEqual(selected.read_bytes(), override.read_bytes())
        self.assertEqual((bundle / "hard-float" / CPP).read_bytes(), bundled_bytes)

    def test_repository_fallback_works_for_engine_under_build_directory(self):
        engine = self.root / ".build" / "desktop" / "native"
        library, bundle = self.bundled_library(engine)
        game = library.import_game(self.archive({"bin/Demo.EXE": elf("soft")}))
        arguments = library.prepare(game)
        self.assertEqual(Path(arguments[arguments.index("--libstdcxx") + 1]), bundle / "soft-float" / CPP)

    def test_bundle_folder_name_cannot_hide_the_wrong_float_abi(self):
        library, bundle = self.bundled_library()
        (bundle / "hard-float" / CPP).write_bytes(elf("soft"))
        game = library.import_game(self.archive({"bin/Demo.EXE": elf("hard")}))
        with self.assertRaisesRegex(ValueError, "hard-float"):
            library.prepare(game)

    def test_stripped_guest_uses_native_loader_import_fallback(self):
        library, bundle = self.bundled_library()
        executable = elf("soft", exports=("__aeabi_fmul",))
        struct.pack_into("<I", executable, 52 + 40 + 4, 0)  # Remove attributes.
        symbols_offset = struct.unpack_from("<I", executable, 52 + 3 * 40 + 16)[0]
        struct.pack_into("<H", executable, symbols_offset + 16 + 14, 0)  # Undefined/imported symbol.
        game = library.import_game(self.archive({"bin/Demo.EXE": executable}))
        directory = library.directory(game)
        path = directory / json.loads((directory / ENTRY).read_text())["executable"]
        self.assertEqual(float_abi(path), "unknown")
        self.assertEqual(game_float_abi(path), "soft")
        arguments = library.prepare(game)
        self.assertEqual(Path(arguments[arguments.index("--libstdcxx") + 1]), bundle / "soft-float" / CPP)

    def test_sectionless_guest_uses_dynamic_imports_and_rejects_invalid_symbol_count(self):
        library, bundle = self.bundled_library()
        executable = sectionless_elf()
        game_id = library.import_game(self.archive({"bin/Demo.EXE": executable}))
        arguments = library.prepare(game_id)
        self.assertEqual(Path(arguments[arguments.index("--libstdcxx") + 1]), bundle / "soft-float" / CPP)
        path = library.directory(game_id) / "Bin/Demo.exe"
        self.assertEqual(float_abi(path), "unknown")
        self.assertEqual(game_float_abi(path), "soft")
        struct.pack_into("<I", executable, 0x104, 0xffffffff)
        path.write_bytes(executable)
        with self.assertRaises(ValueError):
            library.prepare(game_id)

    def test_damaged_entry_does_not_hide_other_games(self):
        self.library.import_game(self.archive())
        damaged = self.library.directory("broken")
        damaged.mkdir()
        (damaged / ENTRY).write_text("not json")
        self.assertEqual(len(self.library.list_games()), 1)

    def test_display_settings_reject_unsupported_values(self):
        self.assertTrue(DEFAULTS["fullscreen"])
        for values in ({"vsync": "true"}, {"filter": "cubic"}, {"fullscreen": "false"}, {"scaling": "stretch"}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                settings(values)

    def test_legacy_resolution_is_discarded_without_resetting_other_settings(self):
        legacy = dict(resolution="720x1280", scaling="integer", filter="linear", vsync=False,
                      fullscreen=False, iconStyle="flat")
        path = self.library.root / "display.json"
        path.write_text(json.dumps(legacy))
        expected = {key: value for key, value in legacy.items() if key != "resolution"}
        self.assertEqual(self.library.get_settings(), expected)
        self.library.save_settings(self.library.get_settings())
        self.assertEqual(json.loads(path.read_text()), expected)

    def test_removal_is_limited_to_selected_game(self):
        first = self.library.import_game(self.archive())
        second = self.library.import_game(self.archive())
        with self.assertRaises(ValueError):
            self.library.remove("../library")
        self.library.remove(first)
        self.assertEqual([g["id"] for g in self.library.list_games()], [second])
        self.assertTrue((self.root / "game.zip").is_file())


if __name__ == "__main__":
    unittest.main()
