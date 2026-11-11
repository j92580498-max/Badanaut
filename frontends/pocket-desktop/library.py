"""Bada package library. No UI or third-party dependencies."""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import re
import shutil
import time
import uuid
import xml.etree.ElementTree as ET
import zipfile

DEFAULTS = dict(scaling="fit", filter="nearest", vsync=True,
                fullscreen=True, iconStyle="glossy")
ENTRY = ".badanaut-game.json"
MAX_BYTES = 512 * 1024 * 1024
MAX_ICON = 2 * 1024 * 1024


def settings(values):
    result = {**DEFAULTS, **{k: v for k, v in values.items() if k in DEFAULTS}}
    for key, options in {"scaling": ("fit", "integer"),
                         "filter": ("nearest", "linear"), "iconStyle": ("glossy", "flat")}.items():
        if result[key] not in options:
            raise ValueError(f"Unsupported {key}")
    if any(type(result[k]) is not bool for k in ("vsync", "fullscreen")):
        raise ValueError("Display switches must be true or false")
    return result


def arm_elf(path):
    try:
        with Path(path).open("rb") as source:
            h = source.read(52)
        return len(h) == 52 and h[:7] == b"\x7fELF\x01\x01\x01" and h[16:20] == b"\x03\x00\x28\x00"
    except OSError:
        return False


def normalize(name):
    known = {"bin": "Bin", "info": "Info", "res": "Res", "appinfo.sys": "Appinfo.sys",
             "manifest.xml": "manifest.xml", "signature.xml": "Signature.xml"}
    return known.get(name.lower(), name[:-4] + ".exe" if name.lower().endswith(".exe") else name)


def extract(archive, destination):
    """Stage a bounded archive, rejecting paths that alias on Windows or Linux."""
    total, paths = 0, set()
    with zipfile.ZipFile(archive) as source:
        if len(source.infolist()) > 20_000:
            raise ValueError("Too many files in the Bada package")
        for entry in source.infolist():
            raw = entry.filename.replace("\\", "/")
            parts = raw.rstrip("/").split("/")
            if raw.startswith("/") or any(p in ("", ".", "..") or ":" in p or
                                           p.endswith((".", " ")) or re.match(r"^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)", p, re.I)
                                           for p in parts):
                raise ValueError("Unsafe path in the Bada package")
            if (entry.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError("Symbolic links are not supported in Bada packages")
            name = "/".join(map(normalize, parts))
            if name.casefold() in paths:
                raise ValueError("Duplicate path in the Bada package")
            paths.add(name.casefold())
            target = destination.joinpath(*name.split("/"))
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(entry) as inp, target.open("wb") as out:
                    while chunk := inp.read(16 * 1024):
                        total += len(chunk)
                        if total > MAX_BYTES:
                            raise ValueError("Bada package exceeds 512 MB")
                        out.write(chunk)


def inspect(extracted):
    candidates = [p for p, dirs, _ in os.walk(extracted)
                  if "Bin" in dirs and "Info" in dirs and len(Path(p).relative_to(extracted).parts) <= 8]
    if len(candidates) != 1:
        raise ValueError("Choose one Bada package containing Bin/*.exe and Info/Appinfo.sys or Info/manifest.xml")
    root = Path(candidates[0])
    executables = list((root / "Bin").glob("*.exe"))
    if len(executables) != 1 or not arm_elf(executables[0]):
        raise ValueError("The Bada package must contain one 32-bit ARM ELF executable in Bin")
    app_id, name, icon = "", "", ""
    for filename in ("Appinfo.sys", "manifest.xml"):
        path = root / "Info" / filename
        if not path.is_file() or path.stat().st_size > 1024 * 1024:
            continue
        data = path.read_bytes()
        declarations = data.replace(b"\0", b"").upper()
        if b"<!DOCTYPE" in declarations or b"<!ENTITY" in declarations:
            raise ValueError("Unsupported XML declarations in Bada metadata")
        try:
            doc = ET.fromstring(data)
        except ET.ParseError:
            continue
        if filename == "Appinfo.sys" and doc.tag == "Application":
            app_id = doc.get("AppId", "").strip()
            name = (doc.get("Name", "") or doc.get("AppName", "")).strip()
            icon = (doc.findtext("Icon") or "").strip()
        elif doc.tag == "Manifest":
            app_id = app_id or (doc.findtext("Id") or "").strip()
            name = name or (doc.findtext("Name") or "").strip()
            icon = icon or (doc.findtext("Icon") or "").strip()
    if not app_id:
        raise ValueError("No AppId found in the Bada package metadata")
    return root, dict(app_id=app_id, display_name=name or executables[0].stem,
                      executable=executables[0].relative_to(root).as_posix(), icon=icon)


def icon_data(root, hint, executable=""):
    candidates = [root / "_badanaut-icon.png"]
    if hint:
        for base in (root, root / "Res", root / "Info"):
            candidate = (base / hint.replace("\\", "/")).resolve()
            if candidate.is_relative_to(root.resolve()):
                candidates.append(candidate)
    stems = ("icon", "appicon", "mainmenuicon", Path(executable.replace("\\", "/")).stem.lower())
    for directory in (root / "Res", root / "Info", root):
        if directory.is_dir():
            candidates.extend(sorted((p for p in directory.iterdir() if p.is_file() and
                                      p.stem.lower() in stems), key=lambda p: p.name))
    for path in candidates:
        if not path.is_file() or path.stat().st_size > MAX_ICON:
            continue
        data = path.read_bytes()
        mime = "png" if data.startswith(b"\x89PNG\r\n\x1a\n") else "jpeg" if data.startswith(b"\xff\xd8\xff") else "webp" if data[:4] == b"RIFF" and data[8:12] == b"WEBP" else None
        if mime:
            return f"data:image/{mime};base64," + base64.b64encode(data).decode("ascii")
    return None


def write_json(path, values):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(values, ensure_ascii=False), encoding="utf-8")
    temporary.replace(path)


class Library:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.games = self.root / "games"
        self.games.mkdir(parents=True, exist_ok=True)

    def directory(self, game_id):
        if not isinstance(game_id, str) or not re.fullmatch(r"[a-z0-9-]{1,80}", game_id):
            raise ValueError("Invalid Bada game ID")
        path = self.games / game_id
        if path.is_symlink():
            raise ValueError("Invalid Bada game directory")
        return path

    def list_games(self):
        entries = []
        for path in self.games.iterdir():
            if not path.is_dir() or path.name.startswith(".") or path.is_symlink():
                continue
            try:
                data = json.loads((path / ENTRY).read_text(encoding="utf-8"))
                entries.append(dict(id=path.name, name=data["display_name"], appId=data["app_id"],
                                    source=data["source"], importedAt=data["imported_at"],
                                    lastPlayed=data.get("last_played", 0), icon=icon_data(path, data.get("icon", ""), data.get("executable", ""))))
            except (OSError, ValueError, KeyError):
                continue  # A damaged installation must not hide the rest of the library.
        return entries

    def get_settings(self):
        try:
            return settings(json.loads((self.root / "display.json").read_text(encoding="utf-8")))
        except (OSError, ValueError, TypeError):
            return dict(DEFAULTS)

    def bootstrap(self):
        return dict(games=self.list_games(), settings=self.get_settings(),
                    runtimeReady=arm_elf(self.root / "libstdc++.so.6"), platform="desktop")

    def import_game(self, archive):
        game_id = str(uuid.uuid4())
        staging = self.games / f".installing-{game_id}"
        staging.mkdir()
        try:
            extract(archive, staging)
            package, metadata = inspect(staging)
            metadata.update(source=Path(archive).name, imported_at=int(time.time()))
            write_json(package / ENTRY, metadata)
            package.rename(self.directory(game_id))
        finally:
            if staging.exists():
                shutil.rmtree(staging)
        return game_id

    def import_runtime(self, source):
        source = Path(source)
        if source.stat().st_size > 32 * 1024 * 1024 or not arm_elf(source):
            raise ValueError("Select a 32-bit ARM Bada libstdc++.so.6, up to 32 MB")
        temporary = self.root / ".runtime-installing"
        try:
            shutil.copyfile(source, temporary)
            temporary.replace(self.root / "libstdc++.so.6")
        finally:
            temporary.unlink(missing_ok=True)

    def prepare(self, game_id):
        directory = self.directory(game_id)
        data = json.loads((directory / ENTRY).read_text(encoding="utf-8"))
        exe = (directory / data["executable"]).resolve()
        if exe.parent != (directory / "Bin").resolve() or not arm_elf(exe) or not (directory / "Info").is_dir() or not data["app_id"].strip():
            raise ValueError("The installed Bada package is incomplete")
        runtime = self.root / "libstdc++.so.6"
        if not arm_elf(runtime):
            raise ValueError("Select a Bada runtime before launching")
        config = self.get_settings()
        args = ["--input", str(directory), "--libstdcxx", str(runtime), "--app-id", data["app_id"],
                "--scaling", config["scaling"], "--filter", config["filter"],
                "--vsync=" + str(config["vsync"]).lower(), "--fullscreen=" + str(config["fullscreen"]).lower()]
        return args

    def played(self, game_id):
        path = self.directory(game_id) / ENTRY
        metadata = json.loads(path.read_text(encoding="utf-8"))
        metadata["last_played"] = int(time.time())
        write_json(path, metadata)

    def remove(self, game_id):
        shutil.rmtree(self.directory(game_id))

    def save_settings(self, values):
        write_json(self.root / "display.json", settings(values))
