"""Native reference-sized app picker. Geometry and fonts are shared with Android."""
from __future__ import annotations

import base64
import json
import math
from pathlib import Path

from PySide6.QtCore import QBuffer, QByteArray, QIODevice, QPointF, QRectF, Qt, QTimer, Signal, Slot
from PySide6.QtGui import QColor, QFont, QFontDatabase, QFontMetricsF, QImageReader, QPainter, QPainterPath
from PySide6.QtWidgets import QWidget


class Launcher(QWidget):
    request = Signal(str)

    def __init__(self, assets: Path, parent=None):
        super().__init__(parent)
        self.layout = json.loads((assets / "launcher.json").read_text(encoding="utf-8"))
        self.fonts = {}
        for weight, key in ((False, "regularFont"), (True, "boldFont")):
            font_id = QFontDatabase.addApplicationFont(str(assets / self.layout[key]))
            families = QFontDatabase.applicationFontFamilies(font_id)
            if not families:
                raise RuntimeError(f"Could not load bundled font: {self.layout[key]}")
            self.fonts[weight] = families[0]
        self.games, self.images, self.settings, self.draft = [], {}, {}, {}
        self.runtime_ready = False
        self.page_index = 0
        self.menu_open = False
        self.wheel_distance = 0
        self.screen = "library"
        self.selected = None
        self.choice = None
        self.message = ""
        self.return_screen = "library"
        self.sequence = 0
        self.pending = None
        self.focus_index = 0
        self.pressed = None
        self.press_point = None
        self.held = False
        self.hold_timer = QTimer(self)
        self.hold_timer.setSingleShot(True)
        self.hold_timer.setInterval(550)
        self.hold_timer.timeout.connect(self.hold)
        self.setFocusPolicy(Qt.StrongFocus)
        self.setAccessibleName("Badanaut game library")
        self.setMouseTracking(True)
        self.resize(self.layout["width"], self.layout["height"])

    def font(self, size, bold=False):
        font = QFont(self.fonts[bold])
        font.setPixelSize(size)
        font.setBold(bold)
        return font

    def transform(self):
        scale = min(self.width() / self.layout["width"], self.height() / self.layout["height"])
        return scale, (self.width() - self.layout["width"] * scale) / 2, (self.height() - self.layout["height"] * scale) / 2

    def point(self, position):
        scale, x, y = self.transform()
        return QPointF((position.x() - x) / scale, (position.y() - y) / scale)

    def tile_rect(self, index):
        g = self.layout["grid"]
        return QRectF(g["x"] + index % g["columns"] * g["stepX"],
                      g["y"] + index // g["columns"] * g["stepY"], g["iconSize"], g["iconSize"])

    def visible_games(self):
        count = self.layout["grid"]["gamesPerPage"]
        return self.games[self.page_index * count:(self.page_index + 1) * count]

    def controls(self):
        # (id, rectangle, label). This same list drives painting and hit testing.
        if self.screen == "library":
            menu = self.layout["menu"]
            if self.menu_open:
                x, y, w, _ = menu["rect"]
                return [("more", QRectF(*menu["trigger"]), "More options")] + [
                    (item["id"], QRectF(x, y + i * menu["itemHeight"], w, menu["itemHeight"]), item["text"])
                    for i, item in enumerate(menu["items"])]
            controls = []
            for i, game in enumerate(self.visible_games()):
                tile = self.tile_rect(i)
                controls.append(("game:" + game["id"], QRectF(tile.x() - 8, tile.y(), 74, 76), game["name"]))
            return controls + [("more", QRectF(*menu["trigger"]), "More options")]
        controls = [("back", QRectF(10, 10, 72, 28), "Back")]
        if self.screen == "settings":
            for i, option in enumerate(self.layout["settings"]):
                value = self.draft.get(option["key"], option["choices"][0])
                label = option["names"][option["choices"].index(value)]
                controls.append(("setting:" + option["key"], QRectF(158, 56 + 42 * i, 152, 30), label))
            controls += [("importRuntime", QRectF(10, 280, 300, 30), "Import runtime override"),
                         ("diagnostics", QRectF(10, 320, 300, 30), "Export launch log"),
                         ("saveSettings", QRectF(10, 428, 300, 30), "Save settings")]
        elif self.screen == "choices":
            controls += [(f"value:{i}", QRectF(10, 64 + i * 44, 300, 34), name)
                         for i, name in enumerate(self.choice["names"])]
        elif self.screen == "details" and self.selected:
            controls += [("launch", QRectF(10, 246, 300, 30), "Open game"),
                         ("setIcon", QRectF(10, 288, 300, 30), "Choose game icon"),
                         ("removeConfirm", QRectF(10, 330, 300, 30), "Remove game")]
        elif self.screen == "remove":
            controls += [("remove", QRectF(10, 246, 300, 30), "Remove game"),
                         ("cancelRemove", QRectF(10, 288, 300, 30), "Cancel")]
        return controls

    def hit(self, position):
        for key, rect, _ in self.controls():
            if rect.contains(position):
                return key
        return None

    def navigate(self, screen):
        self.screen = screen
        self.menu_open = False
        self.focus_index = 0
        self.pressed = None
        self.update()

    def send(self, action, args=None):
        if self.pending:
            return
        self.sequence += 1
        self.pending = (self.sequence, action)
        self.update()
        self.request.emit(json.dumps(dict(id=self.sequence, action=action, args=args or {})))

    @Slot(str)
    def receive(self, raw):
        result = json.loads(raw)
        if not self.pending or result["id"] != self.pending[0]:
            return
        action = self.pending[1]
        self.pending = None
        if not result["ok"]:
            self.show_message(result.get("error") or "The operation failed")
        elif action == "bootstrap":
            state = result["result"]
            self.games = sorted(state["games"], key=lambda game: (game["name"].lower(), game["id"]))
            self.settings = state["settings"]
            self.runtime_ready = state["runtimeReady"]
            self.page_index = min(self.page_index, max(0, math.ceil(len(self.games) / self.layout["grid"]["gamesPerPage"]) - 1))
            self.images = {}
            for game in self.games:
                if game.get("icon"):
                    try:
                        data = base64.b64decode(game["icon"].split(",", 1)[1])
                        buffer = QBuffer()
                        buffer.setData(QByteArray(data))
                        buffer.open(QIODevice.ReadOnly)
                        reader = QImageReader(buffer)
                        size = reader.size()
                        if size.isValid() and size.width() * size.height() <= 16_000_000:
                            reader.setScaledSize(size.scaled(384, 384, Qt.KeepAspectRatio))
                            image = reader.read()
                            if not image.isNull():
                                self.images[game["id"]] = image
                    except (ValueError, IndexError):
                        pass
            if self.selected:
                self.selected = next((g for g in self.games if g["id"] == self.selected["id"]), None)
        elif result.get("result") is False:
            self.send("bootstrap")
        else:
            if action in ("remove", "saveSettings"):
                self.navigate("library")
            if action != "diagnostics":
                self.send("bootstrap")
        self.update()

    @Slot(str)
    def engine_event(self, raw):
        self.show_message(json.loads(raw).get("error", "The game has closed"))

    def show_message(self, text):
        self.return_screen = self.screen if self.screen != "message" else self.return_screen
        self.message = text
        self.navigate("message")

    def activate(self, key):
        if self.pending or not key:
            return
        if key.startswith("game:"):
            self.send("launch", {"id": key[5:]})
        elif key == "more":
            self.menu_open = not self.menu_open
        elif key == "settings":
            self.draft = dict(self.settings)
            self.navigate("settings")
        elif key == "back":
            self.navigate({"choices": "settings", "remove": "details", "message": self.return_screen}.get(self.screen, "library"))
        elif key.startswith("setting:"):
            self.choice = next(o for o in self.layout["settings"] if o["key"] == key[8:])
            self.navigate("choices")
        elif key.startswith("value:"):
            self.draft[self.choice["key"]] = self.choice["choices"][int(key[6:])]
            self.navigate("settings")
        elif key == "saveSettings":
            self.send(key, self.draft)
        elif key == "removeConfirm":
            self.navigate("remove")
        elif key == "cancelRemove":
            self.navigate("details")
        elif key in ("launch", "setIcon", "remove") and self.selected:
            self.send(key, {"id": self.selected["id"]})
        else:
            self.menu_open = False
            self.send(key)
        self.update()

    def details(self, key):
        if key and key.startswith("game:") and not self.pending:
            self.selected = next(g for g in self.games if g["id"] == key[5:])
            self.navigate("details")

    def hold(self):
        self.held = True
        self.details(self.pressed)

    def mousePressEvent(self, event):
        if event.button() == Qt.RightButton:
            self.details(self.hit(self.point(event.position())))
            return
        if event.button() != Qt.LeftButton:
            return
        self.press_point = self.point(event.position())
        self.pressed = self.hit(self.press_point)
        if self.menu_open and self.pressed is None:
            self.menu_open = False
            self.held = True
            self.update()
            return
        self.held = False
        if self.pressed and self.pressed.startswith("game:"):
            self.hold_timer.start()
        self.update()

    def mouseMoveEvent(self, event):
        position = self.point(event.position())
        if self.press_point and (position - self.press_point).manhattanLength() > 12:
            self.hold_timer.stop()
        self.setCursor(Qt.PointingHandCursor if self.hit(position) else Qt.ArrowCursor)

    def mouseReleaseEvent(self, event):
        if event.button() != Qt.LeftButton:
            return
        self.hold_timer.stop()
        position = self.point(event.position())
        key = self.pressed
        if not self.held:
            if self.screen == "library" and not self.menu_open and not self.pending and self.press_point and abs(position.x() - self.press_point.x()) > 60:
                direction = 1 if position.x() < self.press_point.x() else -1
                self.move_page(direction)
            elif self.press_point and (position - self.press_point).manhattanLength() <= 12 and key == self.hit(position):
                self.activate(key)
        self.pressed, self.press_point = None, None
        self.update()

    def keyPressEvent(self, event):
        controls = self.controls()
        if event.key() in (Qt.Key_Escape, Qt.Key_Backspace):
            if self.menu_open:
                self.menu_open = False
            elif self.screen != "library":
                self.activate("back")
        elif event.key() in (Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space):
            self.activate(controls[self.focus_index % len(controls)][0])
        elif event.key() == Qt.Key_Menu:
            self.details(controls[self.focus_index % len(controls)][0])
        elif event.key() in (Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Tab):
            delta = {Qt.Key_Left: -1, Qt.Key_Right: 1, Qt.Key_Up: -4, Qt.Key_Down: 4, Qt.Key_Tab: 1}[event.key()]
            self.focus_index = (self.focus_index + delta) % len(controls)
        elif event.key() in (Qt.Key_PageDown, Qt.Key_PageUp):
            if self.screen == "library":
                self.move_page(1 if event.key() == Qt.Key_PageDown else -1)
        else:
            super().keyPressEvent(event)
        self.update()

    def move_page(self, direction):
        if self.pending or self.menu_open or self.screen != "library":
            return
        pages = max(1, math.ceil(len(self.games) / self.layout["grid"]["gamesPerPage"]))
        self.page_index = max(0, min(pages - 1, self.page_index + direction))
        self.focus_index = 0
        self.update()

    def wheelEvent(self, event):
        delta = event.pixelDelta().x() or event.angleDelta().x()
        if not delta:
            event.ignore()
            return
        self.wheel_distance += delta
        if abs(self.wheel_distance) >= 60:
            self.move_page(1 if self.wheel_distance < 0 else -1)
            self.wheel_distance = 0
        event.accept()

    def text(self, painter, text, rect, size=13, bold=False, color=None, align=Qt.AlignLeft | Qt.AlignVCenter, wrap=False):
        painter.setFont(self.font(size, bold))
        painter.setPen(QColor(color or self.layout["labelColor"]))
        painter.drawText(rect, int(align) | (int(Qt.TextWordWrap) if wrap else 0), text)

    def icon(self, painter, game, rect):
        path = QPainterPath()
        path.addRoundedRect(rect, self.layout["grid"]["radius"], self.layout["grid"]["radius"])
        painter.save()
        painter.setClipPath(path)
        painter.fillRect(rect, QColor(self.layout["tileColor"]))
        image = self.images.get(game["id"])
        if image:
            size = min(image.width(), image.height())
            source = QRectF((image.width() - size) / 2, (image.height() - size) / 2, size, size)
            painter.drawImage(rect, image, source)
        else:
            initials = "".join(word[0] for word in game["name"].split()[:2]).upper() or "?"
            self.text(painter, initials, rect, 22, True, "#ffffff", Qt.AlignCenter)
        painter.restore()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(self.layout["background"]))
        painter.setRenderHint(QPainter.Antialiasing)
        painter.setRenderHint(QPainter.SmoothPixmapTransform)
        scale, x, y = self.transform()
        painter.translate(x, y)
        painter.scale(scale, scale)
        if self.screen == "library":
            g = self.layout["grid"]
            for i, game in enumerate(self.visible_games()):
                rect = self.tile_rect(i)
                self.icon(painter, game, rect)
                painter.save()
                painter.setClipRect(QRectF(rect.x() - 8, rect.y() + 60, g["labelWidth"], 17))
                painter.setFont(self.font(g["labelSize"], True))
                painter.setPen(QColor(self.layout["labelColor"]))
                width = QFontMetricsF(painter.font()).horizontalAdvance(game["name"])
                painter.drawText(QPointF(rect.center().x() - width / 2, rect.y() + g["labelBaseline"]), game["name"])
                painter.restore()
            menu = self.layout["menu"]
            trigger = QRectF(*menu["trigger"])
            painter.setPen(Qt.NoPen)
            painter.setBrush(QColor(self.layout["labelColor"]))
            for dy in (-5, 0, 5):
                painter.drawEllipse(QPointF(trigger.center().x(), trigger.center().y() + dy), 1.5, 1.5)
            pages = max(1, math.ceil(len(self.games) / g["gamesPerPage"]))
            if pages > 1:
                dots = self.layout["pageDots"]
                for i in range(pages):
                    painter.setBrush(QColor(dots["active"] if i == self.page_index else dots["inactive"]))
                    painter.drawEllipse(QPointF(160 + (i - (pages - 1) / 2) * dots["spacing"], dots["y"]), dots["radius"], dots["radius"])
            if not self.games:
                self.text(painter, self.layout["emptyText"], QRectF(24, 136, 272, 90), 15, align=Qt.AlignCenter, wrap=True)
                self.text(painter, self.layout["emptyHint"], QRectF(10, 240, 300, 50), 12, align=Qt.AlignCenter, wrap=True)
        else:
            title = {"settings": "Settings", "details": "Game details", "choices": self.choice["label"] if self.choice else "", "remove": "Remove game?", "message": "Badanaut"}[self.screen]
            self.text(painter, title, QRectF(90, 10, 220, 28), 16, True)
            if self.screen == "settings":
                for i, option in enumerate(self.layout["settings"]):
                    self.text(painter, option["label"], QRectF(10, 56 + 42 * i, 144, 30), 12, True, wrap=True)
                self.text(painter, "Runtime installed" if self.runtime_ready else "Bada runtime needed", QRectF(10, 261, 300, 16), 11, True)
                self.text(painter, self.layout["settingsHelp"], QRectF(10, 360, 300, 62), 11, wrap=True)
            elif self.screen in ("details", "remove") and self.selected:
                self.icon(painter, self.selected, QRectF(131, 62, 58, 58))
                self.text(painter, self.selected["name"], QRectF(10, 129, 300, 38), 16, True, align=Qt.AlignCenter, wrap=True)
                content = ("This removes the installed package and its saved data." if self.screen == "remove" else
                           self.selected["appId"] + "\n" + self.selected.get("source", ""))
                self.text(painter, content, QRectF(10, 174, 300, 60), 12, align=Qt.AlignCenter, wrap=True)
            elif self.screen == "message":
                self.text(painter, self.message[:1500], QRectF(10, 56, 300, 388), 12, align=Qt.AlignLeft | Qt.AlignTop, wrap=True)
        controls = self.controls()
        for key, rect, label in controls:
            if key.startswith("game:") or key == "more" or (self.screen == "library" and self.menu_open):
                continue
            painter.fillRect(rect, QColor(self.layout["pressedColor"] if self.pressed == key else self.layout["buttonColor"]))
            self.text(painter, label, rect, self.layout["buttonSize"] if self.screen == "library" else 15, color=self.layout["buttonTextColor"], align=Qt.AlignCenter)
        if self.screen == "library" and self.menu_open:
            menu = self.layout["menu"]
            painter.fillRect(QRectF(*menu["rect"]), QColor(menu["background"]))
            for key, rect, label in controls:
                if key == "more":
                    continue
                if self.pressed == key:
                    painter.fillRect(rect, QColor("#444444"))
                self.text(painter, label, rect.adjusted(12, 0, -8, 0), 15, color=menu["textColor"])
        if self.hasFocus() and self.focus_index and controls:
            painter.setPen(QColor(self.layout["labelColor"]))
            painter.setBrush(Qt.NoBrush)
            painter.drawRect(controls[self.focus_index % len(controls)][1].adjusted(-2, -2, 2, 2))
        footer = "Please wait…" if self.pending else self.layout["version"]
        painter.setFont(self.font(self.layout["footerSize"], True))
        painter.setPen(QColor(self.layout["labelColor"]))
        width = QFontMetricsF(painter.font()).horizontalAdvance(footer)
        painter.drawText(QPointF(316 - width, self.layout["footerBaseline"]), footer)
        painter.end()
