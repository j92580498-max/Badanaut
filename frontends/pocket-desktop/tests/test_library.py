import base64
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from library import Library, DEFAULTS, ENTRY, settings


def elf():
    data = bytearray(52)
    data[:7] = b"\x7fELF\x01\x01\x01"
    data[16:20] = b"\x03\x00\x28\x00"
    return data


def png():
    return base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVR4nGOUW3CHgYGBiYGBgYGBAQASpAGenRzoTAAAAABJRU5ErkJggg==")


class LibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.library = Library(self.root / "library")

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
