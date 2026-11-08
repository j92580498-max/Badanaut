"""Exercise the real shared picker/Qt bridge at desktop and phone dimensions.

All packages and artwork are generated; no game or firmware files are needed.
Run through xvfb-run on headless Linux. --screenshots optionally saves previews.
"""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "frontends/pocket-desktop"))
from main import Host, Library, LocalPage, LocalRequests, assets, engine_path
from PySide6.QtCore import QTimer, QUrl
from PySide6.QtGui import QColor, QImage, QPainter, QLinearGradient, QFont
from PySide6.QtWidgets import QApplication, QMainWindow
from PySide6.QtWebChannel import QWebChannel
from PySide6.QtWebEngineWidgets import QWebEngineView


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--screenshots", type=Path)
    args = parser.parse_args()
    app = QApplication([])
    app.setApplicationName("Badanaut UI test")
    temporary = tempfile.TemporaryDirectory()
    base = Path(temporary.name)
    library = Library(base / "library")
    names = ["Wave Rider", "Orbit Run", "Garden Quest", "Pocket Rally", "Crystal Cavern", "Moon Base",
             "Sky Patrol", "Ocean Puzzle", "Star Hopper", "Tiny Tennis", "Pixel Painter", "Wild Trails"]
    for i in range(25):
        name = names[i] if i < len(names) else f"Bada demo {i:02d}"
        image = QImage(192, 192, QImage.Format_RGB32)
        painter = QPainter(image)
        gradient = QLinearGradient(0, 0, 192, 192)
        gradient.setColorAt(0, QColor.fromHsl((i * 29 + 190) % 360, 145, 155))
        gradient.setColorAt(1, QColor.fromHsl((i * 29 + 215) % 360, 150, 65))
        painter.fillRect(image.rect(), gradient)
        painter.setPen(QColor(255, 255, 255, 180))
        painter.setFont(QFont("Sans Serif", 62))
        painter.drawText(image.rect(), 0x84, str(i + 1))
        painter.end()
        image.save(str(base / "icon.png"))
        header = bytearray(52)
        header[:7] = b"\x7fELF\x01\x01\x01"
        header[16:20] = b"\x03\x00\x28\x00"
        with zipfile.ZipFile(base / "demo.zip", "w") as archive:
            archive.writestr("Bin/Demo.exe", header)
            archive.writestr("Info/Appinfo.sys", f'<Application AppId="demo-{i}" Name="{name}"><Icon>icon.png</Icon></Application>')
            archive.writestr("Res/icon.png", (base / "icon.png").read_bytes())
        library.import_game(base / "demo.zip")

    window = QMainWindow()
    window.resize(1060, 820)
    view = QWebEngineView(window)
    page = LocalPage(view)
    view.setPage(page)
    requests = LocalRequests(page.profile())
    page.profile().setUrlRequestInterceptor(requests)
    channel = QWebChannel(page)
    host = Host(window, library, engine_path())
    channel.registerObject("host", host)
    page.setWebChannel(channel)
    window.setCentralWidget(view)
    window.show()
    failed = False
    def fail(text):
        nonlocal failed
        failed = True
        print("UI check failed:", text, flush=True)
        app.exit(1)
    def js(script, callback):
        page.runJavaScript(script, callback)
    def snapshot(name):
        if args.screenshots:
            args.screenshots.mkdir(parents=True, exist_ok=True)
            view.grab().save(str(args.screenshots / name))
    def desktop(result):
        if result != 25:
            return fail("The real bridge did not load the full game library")
        js("document.getElementById('grid').getBoundingClientRect().bottom < document.querySelector('.dock').getBoundingClientRect().top", lambda result: None if result else fail("The desktop dock obscures game icons"))
        snapshot("desktop-library.png")
        js("document.getElementById('search').value='Wave Rider';document.getElementById('search').dispatchEvent(new Event('input'));document.querySelectorAll('.game-tile').length", search)
    def search(result):
        if result != 1:
            return fail("Search did not find the installed game")
        js("document.querySelector('.game-more').click();document.querySelector('#game-details h3').textContent", details)
    def details(result):
        if result != "Wave Rider":
            return fail("Game details lost the selected package")
        js("document.getElementById('game-dialog').close();document.getElementById('search').value='';document.getElementById('search').dispatchEvent(new Event('input'));document.getElementById('open-settings').click();document.querySelector('#settings-form [name=resolution]').value='720x1280';document.querySelector('#settings-form [name=filter]').value='linear';document.querySelector('#settings-form [name=fullscreen]').checked=false;document.getElementById('settings-form').requestSubmit();true", lambda _: QTimer.singleShot(1500, saved))
    def saved():
        if library.get_settings()["resolution"] != "720x1280" or library.get_settings()["filter"] != "linear" or library.get_settings()["fullscreen"]:
            return fail("Display choices were not persisted by the host")
        window.resize(390, 844)
        QTimer.singleShot(600, lambda: js("JSON.stringify({tiles:document.querySelectorAll('.game-tile').length,pages:document.querySelectorAll('.page-dot').length,overflow:document.documentElement.scrollWidth>innerWidth})", phone))
    def phone(raw):
        result = json.loads(raw)
        if result != dict(tiles=12, pages=3, overflow=False):
            return fail("Phone layout/paging failed: " + raw)
        snapshot("phone-library.png")
        js("document.getElementById('open-settings').click();true", lambda _: QTimer.singleShot(400, settings_view))
    def settings_view():
        snapshot("phone-settings.png")
        js("document.getElementById('settings-dialog').open && document.getElementById('settings-dialog').getBoundingClientRect().width <= innerWidth", finished)
    def finished(result):
        if not result:
            return fail("Phone settings overflow the viewport")
        print("Shared UI passed: library, search, details, saved display choices, phone paging and settings.", flush=True)
        app.quit()
    ready = False
    def reply(raw):
        nonlocal ready
        response = json.loads(raw)
        if not ready and response.get("ok") and isinstance(response.get("result"), dict) and "games" in response["result"]:
            ready = True
            QTimer.singleShot(500, lambda: js("Number(document.getElementById('game-count').textContent)", desktop))
    host.reply.connect(reply)
    app.aboutToQuit.connect(host.shutdown)
    QTimer.singleShot(45000, lambda: fail("Timed out waiting for the shared UI"))
    view.load(QUrl.fromLocalFile(str(assets() / "index.html")))
    code = app.exec()
    temporary.cleanup()
    return 1 if failed else code


if __name__ == "__main__":
    sys.exit(main())
