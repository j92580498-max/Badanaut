"""A launched process must be able to exit after reading its unused stdin."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from main import Host, Library
from PySide6.QtCore import QCoreApplication, QObject, QEventLoop, QTimer, QProcess


class Window(QObject):
    def raise_(self):
        pass

    def activateWindow(self):
        pass


class ProcessTests(unittest.TestCase):
    def test_game_process_sees_eof_and_exits_normally(self):
        app = QCoreApplication.instance() or QCoreApplication([])
        with tempfile.TemporaryDirectory() as temporary:
            library = Library(temporary)
            directory = library.directory("fixture")
            directory.mkdir()
            (directory / ".badanaut-game.json").write_text(json.dumps({"display_name": "Fixture"}))
            window = Window()
            host = Host(window, library, Path(sys.executable))
            self.addCleanup(host.shutdown)
            host.start_game(1, "fixture", ["-c", "import sys; print('stdin closed' if sys.stdin.read() == '' else 'unexpected input')"])
            loop = QEventLoop()
            host.process.finished.connect(loop.quit)
            timeout = QTimer()
            timeout.setSingleShot(True)
            timeout.timeout.connect(loop.quit)
            timeout.start(10000)
            loop.exec()
            self.assertEqual(host.process.state(), QProcess.NotRunning, "The game process hung on stdin")
            self.assertEqual(host.process.exitCode(), 0)
            self.assertIn("stdin closed", (library.root / "launch.log").read_text())
