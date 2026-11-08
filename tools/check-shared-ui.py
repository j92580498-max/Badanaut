"""Exercise the native picker and real library host at reference, phone and desktop sizes.

Artwork/packages are generated locally. Run with xvfb-run on headless Linux.
"""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "frontends/pocket-desktop"))
from main import Host, Library, assets, engine_path
from launcher import Launcher
from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QMainWindow


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--screenshots", type=Path)
    args = parser.parse_args()
    app = QApplication([])
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        library = Library(base / "library")
        for i in range(25):
            image = QImage(96, 96, QImage.Format_RGB32)
            image.fill(QColor.fromHsl((i * 29 + 190) % 360, 145, 100))
            painter = QPainter(image)
            painter.setPen(QColor("white")); painter.drawEllipse(18, 18, 60, 60); painter.end()
            image.save(str(base / "icon.png"))
            header = bytearray(52)
            header[:7] = b"\x7fELF\x01\x01\x01"; header[16:20] = b"\x03\x00\x28\x00"
            with zipfile.ZipFile(base / "demo.zip", "w") as archive:
                archive.writestr("Bin/Demo.exe", header)
                archive.writestr("Info/Appinfo.sys", f'<Application AppId="demo-{i}" Name="Bada demo {i:02d}"><Icon>icon.png</Icon></Application>')
                archive.writestr("Res/icon.png", (base / "icon.png").read_bytes())
            library.import_game(base / "demo.zip")
        window = QMainWindow()
        view = Launcher(assets(), window)
        host = Host(window, library, engine_path())
        view.request.connect(host.request); host.reply.connect(view.receive); host.event.connect(view.engine_event)
        window.setCentralWidget(view); window.resize(320, 480); window.show()

        def wait():
            deadline = time.monotonic() + 10
            while view.pending and time.monotonic() < deadline:
                app.processEvents()
                time.sleep(0.02)
            assert view.pending is None, "Library host timed out"
            QTest.qWait(30)

        def click(key):
            rect = next(rect for name, rect, _ in view.controls() if name == key)
            scale, x, y = view.transform()
            QTest.mouseClick(view, Qt.LeftButton, pos=QPoint(round(x + rect.center().x() * scale), round(y + rect.center().y() * scale)))

        def screenshot(name):
            if args.screenshots:
                args.screenshots.mkdir(parents=True, exist_ok=True)
                view.grab().save(str(args.screenshots / name))

        try:
            view.send("bootstrap"); wait()
            assert len(view.games) == 25 and len(view.images) == 25
            assert len(view.visible_games()) == 15
            image = view.grab().toImage()
            assert image.size().width() == 320 and image.size().height() == 480
            assert image.pixelColor(0, 0) == QColor("black")
            assert image.pixelColor(12, 392) == QColor("white"), "Reference file-manager button moved"
            assert image.pixelColor(250, 330) != QColor("black"), "Reference arrow tile is missing"
            screenshot("reference-library.png")
            first = view.visible_games()[0]["id"]
            click("next"); assert view.page_index == 1 and len(view.visible_games()) == 10
            click("next"); assert view.page_index == 0 and view.visible_games()[0]["id"] == first
            QTest.mouseClick(view, Qt.RightButton, pos=QPoint(47, 61))
            assert view.screen == "details" and view.selected["id"] == first
            screenshot("game-details.png")
            click("removeConfirm"); assert view.screen == "remove"
            click("cancelRemove"); click("back"); assert len(library.list_games()) == 25
            click("settings"); click("setting:resolution"); click("value:4")
            click("setting:filter"); click("value:1")
            assert view.draft["resolution"] == "720x1280" and view.draft["filter"] == "linear"
            screenshot("reference-settings.png")
            click("saveSettings"); wait()
            assert library.get_settings()["resolution"] == "720x1280"
            assert library.get_settings()["filter"] == "linear" and not library.get_settings()["fullscreen"]
            assert view.screen == "library"
            for w, h, name in ((390, 844, "phone"), (960, 720, "desktop")):
                window.resize(w, h); QTest.qWait(50)
                assert len(view.visible_games()) == 15
                click("next"); assert view.page_index == 1
                click("next"); assert view.page_index == 0
                screenshot(name + "-library.png")
                click("settings"); assert view.screen == "settings"
                screenshot(name + "-settings.png"); click("back")
            window.resize(320, 480); QTest.qWait(30)
            # Real mouse hold opens details without launching. Cancel/remove then refresh via host.
            QTest.mousePress(view, Qt.LeftButton, pos=QPoint(47, 61)); QTest.qWait(650)
            QTest.mouseRelease(view, Qt.LeftButton, pos=QPoint(47, 61))
            assert view.screen == "details" and view.pending is None
            click("removeConfirm"); click("remove"); wait()
            assert len(view.games) == 24 and len(library.list_games()) == 24
            click("copyright"); assert view.screen == "copyright"
            screenshot("copyright.png"); click("back")
            # A launch failure must become a readable native message, without hanging the picker.
            click("game:" + view.visible_games()[0]["id"]); wait()
            assert view.screen == "message" and "runtime" in view.message.lower()
            click("back"); assert view.screen == "library"
            print("Native UI passed: reference geometry, package artwork, paging, scaled hit targets, hold/details, removal, saved settings and launch errors.", flush=True)
        finally:
            host.shutdown(); window.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
