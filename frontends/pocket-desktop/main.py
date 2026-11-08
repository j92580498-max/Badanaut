"""Windowed desktop host for the shared Badanaut interface."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import sys

from PySide6.QtCore import QObject, Signal, Slot, QProcess, QStandardPaths, QTimer, QBuffer, QByteArray, QIODevice, Qt
from PySide6.QtGui import QImageReader, QIcon
from PySide6.QtWidgets import QApplication, QFileDialog, QMainWindow
from launcher import Launcher

from library import Library, MAX_ICON


def assets():
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS) / "shared-ui"
    return Path(__file__).resolve().parent.parent / "shared-ui"


def engine_path():
    name = "bada-hle.exe" if os.name == "nt" else "bada-hle"
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS) / "native" / name
    return Path(__file__).resolve().parents[2] / ".build" / "desktop" / "native" / name


class Host(QObject):
    reply = Signal(str)
    event = Signal(str)
    completed = Signal(object)

    def __init__(self, window, library, engine):
        super().__init__(window)
        self.window, self.library, self.engine = window, library, Path(engine).resolve()
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="BadaLibrary")
        self.completed.connect(self.finish)
        self.process = None
        self.log = None

    def respond(self, request_id, result=None, error=None):
        self.reply.emit(json.dumps(dict(id=request_id, ok=error is None, result=result, error=error)))

    @Slot(str)
    def request(self, raw):
        try:
            call = json.loads(raw)
            request_id, action, args = call["id"], call["action"], call.get("args", {})
            if action in ("importGame", "importRuntime", "setIcon"):
                title, filters = {"importGame": ("Add a Bada game", "Bada packages (*.zip)"),
                                  "importRuntime": ("Select Bada libstdc++.so.6", "All files (*)"),
                                  "setIcon": ("Select game artwork", "Images (*.png *.jpg *.jpeg *.webp)")}[action]
                filename, _ = QFileDialog.getOpenFileName(self.window, title, "", filters)
                if not filename:
                    self.respond(request_id, False)
                    return
                args["filename"] = filename
            if action == "launch" and self.process and self.process.state() != QProcess.NotRunning:
                raise ValueError("Close the running game before opening another one")
            if action == "diagnostics":
                source = self.library.root / "launch.log"
                if not source.is_file():
                    raise ValueError("Launch a game first to create a log")
                filename, _ = QFileDialog.getSaveFileName(self.window, "Export launch log", "Badanaut-log.txt", "Text (*.txt)")
                if not filename:
                    self.respond(request_id, False)
                    return
                args["filename"] = filename
            def work():
                try:
                    result = self.dispatch(action, args)
                    self.completed.emit((request_id, action, args, result, None))
                except Exception as error:
                    self.completed.emit((request_id, action, args, None, str(error)))
            self.executor.submit(work)
        except Exception as error:
            self.respond(locals().get("request_id", 0), error=str(error))

    def dispatch(self, action, args):
        if action == "bootstrap":
            return self.library.bootstrap()
        if action == "importGame":
            return self.library.import_game(args["filename"])
        if action == "importRuntime":
            self.library.import_runtime(args["filename"])
        elif action == "remove":
            self.library.remove(args["id"])
        elif action == "saveSettings":
            self.library.save_settings(args)
        elif action == "launch":
            if not self.engine.is_file():
                raise ValueError("The Bada engine is missing. Build the desktop engine first.")
            return self.library.prepare(args["id"])
        elif action == "setIcon":
            path = Path(args["filename"])
            if path.stat().st_size > MAX_ICON:
                raise ValueError("Choose an image under 2 MB")
            reader = QImageReader(str(path))
            size = reader.size()
            if not size.isValid() or size.width() * size.height() > 16_000_000:
                raise ValueError("Choose an image up to 16 megapixels")
            image = reader.read()
            if image.isNull():
                raise ValueError("This image could not be decoded")
            image = image.scaled(192, 192, Qt.KeepAspectRatioByExpanding, Qt.SmoothTransformation)
            image = image.copy((image.width() - 192) // 2, (image.height() - 192) // 2, 192, 192)
            output = QByteArray()
            buffer = QBuffer(output)
            buffer.open(QIODevice.WriteOnly)
            if not image.save(buffer, "PNG"):
                raise ValueError("Could not save the game icon")
            (self.library.directory(args["id"]) / "_badanaut-icon.png").write_bytes(bytes(output))
        elif action == "diagnostics":
            Path(args["filename"]).write_bytes((self.library.root / "launch.log").read_bytes())
        else:
            raise ValueError("Unknown library action")
        return True

    @Slot(object)
    def finish(self, completed):
        request_id, action, args, result, error = completed
        if action == "launch" and error is None:
            try:
                self.start_game(request_id, args["id"], result)
            except Exception as failure:
                self.respond(request_id, error=str(failure))
        else:
            self.respond(request_id, result, error)

    def start_game(self, request_id, game_id, arguments):
        self.log = (self.library.root / "launch.log").open("wb")
        self.log.write(b"Badanaut desktop launch\n")
        process = QProcess(self)
        self.process = process
        process.setProcessChannelMode(QProcess.MergedChannels)
        process.setWorkingDirectory(str(self.engine.parent))
        process.readyReadStandardOutput.connect(self.read_log)
        def started():
            # The engine's optional console reader must see EOF. An unused open
            # stdin pipe can leave it blocked while the game process shuts down.
            process.closeWriteChannel()
            try:
                self.library.played(game_id)
                self.respond(request_id, True)
            except Exception as error:
                self.respond(request_id, error=str(error))
        def failed(error):
            if error == QProcess.FailedToStart:
                self.respond(request_id, error="Could not start the Bada engine: " + process.errorString())
                if self.log:
                    self.log.close()
                    self.log = None
        def finished(code, status):
            self.read_log()
            if self.log:
                self.log.close()
                self.log = None
            if code != 0 or status == QProcess.CrashExit:
                tail = (self.library.root / "launch.log").read_bytes()[-2500:].decode("utf-8", errors="replace")
                self.event.emit(json.dumps(dict(error="The game stopped before completing normally.\n\n" + tail)))
            self.window.raise_()
            self.window.activateWindow()
        process.started.connect(started)
        process.errorOccurred.connect(failed)
        process.finished.connect(finished)
        process.start(str(self.engine), arguments)

    def read_log(self):
        data = bytes(self.process.readAllStandardOutput())
        if self.log:
            remaining = max(0, 2 * 1024 * 1024 - self.log.tell())
            self.log.write(data[:remaining])
            self.log.flush()

    def shutdown(self):
        self.executor.shutdown(wait=True, cancel_futures=True)
        if self.process and self.process.state() != QProcess.NotRunning:
            self.process.terminate()
            if not self.process.waitForFinished(1500):
                self.process.kill()
                self.process.waitForFinished(1000)


def main():
    parser = argparse.ArgumentParser(description="Badanaut Bada game library")
    parser.add_argument("--library", type=Path, help="Override the library directory")
    parser.add_argument("--engine", type=Path, help="Override the native emulator path")
    parser.add_argument("--smoke-test", action="store_true", help="Check packaged native UI and library, then exit")
    args = parser.parse_args()
    app = QApplication(sys.argv[:1])
    app.setApplicationName("Badanaut")
    app.setOrganizationName("Badanaut")
    root = args.library or Path(QStandardPaths.writableLocation(QStandardPaths.AppDataLocation)) / "bada"
    window = QMainWindow()
    view = Launcher(assets(), window)
    window.setWindowTitle(view.layout["version"])
    app.setWindowIcon(QIcon(str(assets() / "mark.svg")))
    available = app.primaryScreen().availableGeometry()
    window.resize(320, 480)
    window.move(available.center() - window.rect().center())
    host = Host(window, Library(root), args.engine or engine_path())
    view.request.connect(host.request)
    host.reply.connect(view.receive)
    host.event.connect(view.engine_event)
    window.setCentralWidget(view)
    app.aboutToQuit.connect(host.shutdown)
    window.show()
    if args.smoke_test:
        def received(raw):
            response = json.loads(raw)
            if response.get("ok") and isinstance(response.get("result"), dict) and "games" in response["result"]:
                def check():
                    image = view.grab().toImage()
                    valid = view.pending is None and view.layout["grid"]["gamesPerPage"] == 15 and not image.isNull()
                    app.exit(0 if valid else 1)
                QTimer.singleShot(100, check)
        host.reply.connect(received)
        QTimer.singleShot(30000, lambda: app.exit(1))
    QTimer.singleShot(0, lambda: view.send("bootstrap"))
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
