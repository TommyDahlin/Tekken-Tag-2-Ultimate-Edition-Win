#!/usr/bin/env python3
"""A launcher for the TTT2 port: sets up netplay (lockstep) and starts the game.

    python tools/netplay_launcher.py

One window to choose the game folder, to host or join a match with a pasted
code, to tweak the settings that matter, and to start the game with them
(the TTT2_* environment variables the port reads) while its messages are shown.

Python 3 with tkinter is needed (it ships with Python on Windows; on Linux
`python3-tk`). The logic below does not need tkinter, so it can be tested.

IP addresses are personal data: the launcher keeps the ones you type in memory
only, unless you tick "Remember addresses", and masks them in the log you copy.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import platform
import queue
import re
import secrets
import shutil
import socket
import subprocess
import sys
import threading
from dataclasses import asdict, dataclass, fields
from pathlib import Path

# The Tekken.rpx the port is built for (TEKKEN TAG TOURNAMENT 2 Wii U EDITION, EU, v16).
SUPPORTED_RPX = "fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019"
INVITE_PREFIX = "TTT2-"
DEFAULT_PORT = 30000
PUBLIC_ADDRESS_URL = "https://api.ipify.org"   # asked only when the button is pressed

MODES = ("local", "host", "join")


@dataclass
class Settings:
    mode: str = "local"              # local | host | join
    game_dir: str = ""               # the folder with code/Tekken.rpx
    exe: str = ""                    # ttt2 (ttt2.exe)
    characters: str = ""             # the character packs folder
    peer: str = ""                   # host: the guest's address; join: the host's
    my_address: str = ""             # what the other player connects to / hears from
    port: int = DEFAULT_PORT
    same_pc: bool = False            # both players on this PC (two windows): a test
    delay: int = 3                   # lockstep input delay, in frames
    encrypt: bool = True
    secret: str = ""
    skip_intro: int = 0              # seconds of A/START taps after start-up; 0: off
    sync: str = "start"              # start: both press START; now: at the very first frame
    game_input_delay: int = -1       # TTT2_INPUT_DELAY; -1: leave to display.ini. Must match.
    second_controller_p2: bool = True
    fullscreen: bool = False
    remember_addresses: bool = False


ADDRESS_FIELDS = ("peer", "my_address")

# Variables the settings decide (cleared from the inherited environment first),
# besides the TTT2_NET_* ones and the self-test and trace ones that slow the game.
MANAGED = ("TTT2_SKIP_INTRO", "TTT2_INPUT_DELAY", "TTT2_LOCAL_PLAYERS", "TTT2_FULLSCREEN", "TTT2_CHARACTERS",
           "TTT2_TRACE_FIGHT_STATE")


# ----------------------------------------------------------------------------- files

def portable_root(here: Path | None = None) -> Path | None:
    """The folder of a portable install, when this script sits in its app/ folder:

        <root>/app/ttt2.exe, app/netplay_launcher.py     the program
        <root>/game/                                      the game
        <root>/data/ttt2/characters, data/ttt2/save       character packs and saves
        <root>/home/                                      the game's settings and caches

    Everything the launcher and the game read and write then stays in <root>."""
    here = here or Path(__file__).resolve().parent
    root = here.parent
    if (here / "ttt2.exe").is_file() and (root / "data").is_dir():
        return root
    return None


def config_path() -> Path:
    portable = portable_root()
    if portable is not None:
        return portable / "home" / ".config" / "ttt2" / "netplay_launcher.json"
    if platform.system() == "Windows":
        base = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return base / "ttt2" / "netplay_launcher.json"


def load_settings(path: Path | None = None) -> Settings:
    path = path or config_path()
    settings = Settings()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return settings
    known = {f.name: f for f in fields(Settings)}
    for key, value in data.items():
        if key in known and isinstance(value, type(getattr(settings, key))):
            setattr(settings, key, value)
    if settings.mode not in MODES:
        settings.mode = "local"
    return settings


def save_settings(settings: Settings, path: Path | None = None) -> None:
    """Writes the settings. Addresses (personal data) go in only when asked for."""
    path = path or config_path()
    data = asdict(settings)
    if not settings.remember_addresses:
        for key in ADDRESS_FIELDS:
            data[key] = ""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def default_locations(root: Path, portable: Path | None = None) -> dict[str, str]:
    """Where the program, the character packs and the game probably are.

    `root` is the source folder (this script's parent); `portable` is the root of a
    portable install (portable_root()), which decides when there is one."""
    exe_names = ("ttt2.exe", "ttt2")
    candidates = [root / "portable" / "ttt2-portable", root / "build-ucrt64-tools-ucrt64", root / "build"]
    if portable is not None:
        candidates.insert(0, portable / "app")
    exe = ""
    for folder in candidates:
        for name in exe_names:
            if (folder / name).is_file():
                exe = str(folder / name)
                break
        if exe:
            break
    exe = exe or (shutil.which("ttt2") or "")

    packs = ""
    places = [root / "portable" / "data" / "ttt2" / "characters", Path.home() / ".local" / "share" / "ttt2" / "characters"]
    if os.environ.get("TTT2_CHARACTERS"):
        places.insert(0, Path(os.environ["TTT2_CHARACTERS"]))
    if portable is not None:
        places = [portable / "data" / "ttt2" / "characters"]   # the install carries its own
    for folder in places:
        if folder.is_dir() and any(folder.iterdir()):
            packs = str(folder)
            break
    game = ""
    if portable is not None and (portable / "game" / "code" / "Tekken.rpx").is_file():
        game = str(portable / "game")
    return {"exe": exe, "characters": packs, "game": game}


def find_game_folder(root: Path, search: list[Path] | None = None) -> str:
    """A folder with code/Tekken.rpx: beside the program, or one level inside the usual
    download folders ('' when there is none). Only looks; the window still checks the file."""
    candidates = [root / "portable" / "game", root / "game"]
    home = Path.home()
    for base in search if search is not None else [home / "Downloads", home / "Desktop", home / "Documents"]:
        try:
            candidates += [c for c in sorted(base.iterdir())[:300] if c.is_dir()]
        except OSError:
            continue
    for folder in candidates:
        if (folder / "code" / "Tekken.rpx").is_file():
            return str(folder)
    return ""


# ----------------------------------------------------------------------------- fingerprints

def game_fingerprint(game_dir: str) -> str:
    """The start of the SHA-256 of the game's Tekken.rpx ('' when it is not there)."""
    rpx = Path(game_dir) / "code" / "Tekken.rpx"
    if not rpx.is_file():
        return ""
    digest = hashlib.sha256()
    with rpx.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def game_status(game_dir: str) -> tuple[bool, str]:
    """(usable, what to tell the player) for a game folder."""
    if not game_dir:
        return False, "Choose the game folder (the one that holds code/Tekken.rpx)."
    rpx = Path(game_dir) / "code" / "Tekken.rpx"
    if not rpx.is_file():
        return False, "There is no code/Tekken.rpx in that folder."
    if game_fingerprint(game_dir) != SUPPORTED_RPX:
        return False, "This Tekken.rpx is not the supported one (EU, update v16)."
    return True, "Game found (EU, update v16)."


def packs_fingerprint(characters_dir: str) -> str:
    """A short fingerprint of the character packs present: the folders and their character.ini."""
    folder = Path(characters_dir) if characters_dir else None
    if folder is None or not folder.is_dir():
        return "none"
    lines = []
    for pack in sorted(p for p in folder.iterdir() if p.is_dir()):
        ini = pack / "character.ini"
        if ini.is_file():
            lines.append(f"{pack.name}:{hashlib.sha256(ini.read_bytes()).hexdigest()}")
    if not lines:
        return "none"
    return hashlib.sha256("\n".join(lines).encode()).hexdigest()[:12]


def count_packs(characters_dir: str) -> int:
    folder = Path(characters_dir) if characters_dir else None
    if folder is None or not folder.is_dir():
        return 0
    return sum(1 for p in folder.iterdir() if p.is_dir() and (p / "character.ini").is_file())


# ----------------------------------------------------------------------------- addresses

def lan_address() -> str:
    """This PC's address on its local network ('' when there is none)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 9))   # no packet is sent: it only picks the route
            return s.getsockname()[0]
    except OSError:
        return ""


def find_public_address(timeout: float = 5.0) -> str:
    """This PC's address as the internet sees it. This contacts PUBLIC_ADDRESS_URL."""
    import urllib.request
    with urllib.request.urlopen(PUBLIC_ADDRESS_URL, timeout=timeout) as response:
        text = response.read(64).decode().strip()
    if not re.fullmatch(r"\d{1,3}(\.\d{1,3}){3}", text):
        raise ValueError("unexpected answer")
    return text


_IPV4 = re.compile(r"\b(?!127\.0\.0\.1\b)(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b")


def mask_addresses(text: str) -> str:
    """Replaces IPv4 addresses (but the loopback) by x.x.x.x: for a log that is shared."""
    return _IPV4.sub("x.x.x.x", text)


def make_secret() -> str:
    return secrets.token_urlsafe(9)


# ----------------------------------------------------------------------------- invite codes

def encode_invite(s: Settings, game_fp: str, packs_fp: str) -> str:
    """A code to hand to the other player: where to reach me and the settings that must match."""
    data = {
        "v": 1, "ip": s.my_address, "port": s.port, "delay": s.delay, "dtls": s.encrypt,
        "psk": s.secret if s.encrypt else "", "sync": s.sync, "idelay": s.game_input_delay,
        "game": game_fp[:12], "packs": packs_fp,
    }
    raw = json.dumps(data, separators=(",", ":")).encode()
    return INVITE_PREFIX + base64.urlsafe_b64encode(raw).decode().rstrip("=")


def decode_invite(text: str) -> dict:
    text = text.strip()
    if not text.startswith(INVITE_PREFIX):
        raise ValueError("That is not a TTT2 code (it should start with " + INVITE_PREFIX + ").")
    body = text[len(INVITE_PREFIX):]
    try:
        data = json.loads(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))
    except (ValueError, TypeError):
        raise ValueError("That code is damaged: copy all of it.") from None
    if not isinstance(data, dict) or data.get("v") != 1:
        raise ValueError("That code is from a different version of the launcher.")
    ip = data.get("ip", "")
    if not isinstance(ip, str) or not re.fullmatch(r"[A-Za-z0-9._:-]{0,253}", ip):
        raise ValueError("That code has no usable address.")
    return data


def apply_invite(s: Settings, invite: dict, game_fp: str, packs_fp: str) -> list[str]:
    """Applies the other player's code and returns what to warn about.

    Joining takes the host's address and the settings that must match; hosting
    takes only the guest's address."""
    warnings: list[str] = []
    s.peer = invite.get("ip", "")
    if s.mode == "join":
        s.port = int(invite.get("port", s.port))
        s.delay = int(invite.get("delay", s.delay))
        s.encrypt = bool(invite.get("dtls", s.encrypt))
        s.secret = str(invite.get("psk", s.secret)) if s.encrypt else s.secret
        s.sync = invite.get("sync", s.sync) if invite.get("sync") in ("start", "now") else s.sync
        s.game_input_delay = int(invite.get("idelay", s.game_input_delay))
    their_game, their_packs = invite.get("game", ""), invite.get("packs", "")
    if their_game and game_fp and their_game != game_fp[:12]:
        warnings.append("Your game is not the same as theirs (a different version): the two games will not agree.")
    if their_packs and their_packs != packs_fp:
        warnings.append("Your character packs are not the same as theirs: fighters may not match "
                        "(play without Tekken 7 fighters, or install the same packs).")
    return warnings


# ----------------------------------------------------------------------------- the environment

def validate(s: Settings) -> list[str]:
    """What is wrong with the settings (empty when the game can start)."""
    problems = []
    ok, message = game_status(s.game_dir)
    if not ok:
        problems.append(message)
    if not s.exe or not Path(s.exe).is_file():
        problems.append("Choose ttt2 (ttt2.exe): the program that runs the game.")
    if s.mode in ("host", "join"):
        if not 1 <= s.port <= 65535:
            problems.append("The port must be 1-65535.")
        if not 1 <= s.delay <= 30:
            problems.append("The input delay must be 1-30 frames.")
        if not s.same_pc and not s.peer.strip():
            problems.append("Enter the other player's address (or paste their code)." if s.mode == "host"
                            else "Enter the host's address (or paste their code).")
        if s.encrypt and not s.secret:
            problems.append("Encryption needs a shared secret (the host's code carries it).")
    return problems


def build_env(s: Settings, base: dict[str, str] | None = None, portable: Path | None = None) -> dict[str, str]:
    """The environment to start the game with.

    With `portable` (the root of a portable install) the game's settings, caches and
    saves are kept in it, whatever the PC's own folders are."""
    env = dict(os.environ if base is None else base)
    if portable is not None:
        env["HOME"] = str(portable / "home")
        env["XDG_DATA_HOME"] = str(portable / "data")
        env["XDG_CONFIG_HOME"] = str(portable / "home" / ".config")
        env["TTT2_SAVE_DIR"] = str(portable / "data" / "ttt2" / "save")
    elif platform.system() == "Windows" and not env.get("HOME"):
        env["HOME"] = str(Path.home())   # where the game keeps display.ini and the like
    inherited_packs = env.get("TTT2_CHARACTERS", "")
    for key in [k for k in env if k.startswith(("TTT2_NET_", "TTT2_ROLLBACK_"))] + list(MANAGED):
        env.pop(key, None)   # what is set here decides, not what the shell had left over

    if s.characters or inherited_packs:
        env["TTT2_CHARACTERS"] = s.characters or inherited_packs
    if s.skip_intro > 0:
        env["TTT2_SKIP_INTRO"] = str(s.skip_intro)
    if s.game_input_delay >= 0:
        env["TTT2_INPUT_DELAY"] = str(s.game_input_delay)
    if s.fullscreen:
        env["TTT2_FULLSCREEN"] = "1"
    if not s.second_controller_p2:
        env["TTT2_LOCAL_PLAYERS"] = "1"

    if s.mode in ("host", "join"):
        env["TTT2_NET_ROLLBACK"] = "1"
        env["TTT2_NET_LOCKSTEP"] = "1"
        env["TTT2_NET_DELAY"] = str(s.delay)
        env["TTT2_NET_SYNC"] = "now" if s.sync == "now" else "menu"
        if s.mode == "host":
            env["TTT2_NET_ROLLBACK_SERVER"] = "1"
        if s.same_pc:
            # Two windows on this PC: each listens on one port and talks to the other's.
            env["TTT2_NET_ROLLBACK_PEER"] = "127.0.0.1"
            host = s.mode == "host"
            env["TTT2_NET_ROLLBACK_LOCAL_PORT"] = str(s.port if host else s.port + 1)
            env["TTT2_NET_ROLLBACK_PORT"] = str(s.port + 1 if host else s.port)
        else:
            env["TTT2_NET_ROLLBACK_PEER"] = s.peer.strip()
            env["TTT2_NET_ROLLBACK_PORT"] = str(s.port)
        if s.encrypt and s.secret:
            env["TTT2_NET_ROLLBACK_PSK_SECRET"] = s.secret
        else:
            env["TTT2_NET_ROLLBACK_NO_DTLS"] = "1"
    return env


def build_command(s: Settings) -> list[str]:
    return [s.exe, s.game_dir]


def describe(s: Settings) -> str:
    """The settings as the lines a person could type, with the secret and addresses masked."""
    env = build_env(s, base={})
    parts = [f"{k}={v}" for k, v in sorted(env.items()) if k.startswith("TTT2_")]
    text = " ".join(parts + [shlex_quote(s.exe), shlex_quote(s.game_dir)])
    return mask_addresses(re.sub(r"(PSK_SECRET=)\S+", r"\1***", text))


def shlex_quote(text: str) -> str:
    return '"' + text + '"' if " " in text else text


# ----------------------------------------------------------------------------- the game's messages

# (a fragment of a line the game prints) -> (state, what to show)
STATUS_RULES = [
    ("rollback net: started", ("network", "Network ready.")),
    ("this game is at the sync point", ("sync", "At the sync point: waiting for the other player.")),
    ("at the sync point, waiting", ("sync", "Waiting for the other player to reach the sync point (press START in the game).")),
    ("both players are at the sync point", ("playing", "Playing: both games are in step.")),
    ("is waiting for the other player's input", ("stall", "The game is waiting for the other player's input (slow connection or they paused).")),
    ("silent too long", ("lost", "The other player went silent: playing on alone.")),
    ("failed to start", ("error", "The network could not start (check the address and port).")),
    ("crash: ", ("crash", "The game crashed: see the log.")),
]


def parse_status(line: str) -> tuple[str, str] | None:
    for fragment, status in STATUS_RULES:
        if fragment in line:
            return status
    return None


# What a Windows exit code from the game means, for the codes people meet.
EXIT_EXPLANATIONS = {
    0xC0000135: "Windows could not find a DLL that ttt2.exe needs: copy the runtime DLLs next to ttt2.exe "
                "(build-kit.sh does it; the error window names the missing one).",
    0xC000007B: "A DLL next to ttt2.exe is the wrong kind (32-bit instead of 64-bit) or damaged.",
    0xC0000005: "The game crashed (an access violation): see the log.",
}


def explain_exit(code: int | None) -> str:
    return "" if code is None else EXIT_EXPLANATIONS.get(code & 0xFFFFFFFF, "")


def count_added_fighters(line: str) -> bool:
    return "added the fighter" in line


# ----------------------------------------------------------------------------- running the game

class Runner:
    """Starts the game and hands its output lines over, one by one."""

    def __init__(self) -> None:
        self.process: subprocess.Popen | None = None
        self.lines: "queue.Queue[str | None]" = queue.Queue()

    def start(self, command: list[str], env: dict[str, str]) -> None:
        if self.running():
            raise RuntimeError("The game is already running.")
        lines: "queue.Queue[str | None]" = queue.Queue()
        self.lines = lines   # one queue per run: a run that was stopped must not end the next
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if platform.system() == "Windows" else 0
        self.process = subprocess.Popen(
            command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", creationflags=flags)
        threading.Thread(target=self._read, args=(self.process, lines), daemon=True).start()

    @staticmethod
    def _read(process: subprocess.Popen, lines: "queue.Queue[str | None]") -> None:
        assert process.stdout is not None
        for line in process.stdout:
            lines.put(line.rstrip("\r\n"))
        process.stdout.close()
        process.wait()
        lines.put(None)   # the end

    def running(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def stop(self) -> None:
        if self.running():
            assert self.process is not None
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()

    def exit_code(self) -> int | None:
        return None if self.process is None else self.process.poll()


# ----------------------------------------------------------------------------- the window

def make_app(root_dir: Path):
    """The window (not yet shown by mainloop): for run_gui, and for the tests."""
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    class App(tk.Tk):
        def __init__(self) -> None:
            super().__init__()
            self.title("TTT2 netplay launcher")
            self.minsize(720, 640)
            self.settings = load_settings()
            self.portable = portable_root()
            defaults = default_locations(root_dir, self.portable)
            if self.portable is not None or not self.settings.exe or not Path(self.settings.exe).is_file():
                self.settings.exe = defaults["exe"] or self.settings.exe
            if self.portable is not None or not self.settings.characters:
                self.settings.characters = defaults["characters"] or self.settings.characters
            if not self.settings.game_dir or not Path(self.settings.game_dir).is_dir():
                self.settings.game_dir = defaults["game"] or find_game_folder(root_dir) or self.settings.game_dir
            if not self.settings.my_address:
                self.settings.my_address = lan_address()
            if not self.settings.secret:
                self.settings.secret = make_secret()
            self.runner = Runner()
            self.log_lines: list[str] = []
            self.fighters = 0
            self._build()
            self._refresh_all()
            self.protocol("WM_DELETE_WINDOW", self._close)
            self.after(100, self._poll_runner)

        # --- variables bound to the settings
        def _var(self, kind, name):
            var = kind(value=getattr(self.settings, name))
            var.trace_add("write", lambda *_: self._changed(name, var))
            return var

        def _changed(self, name, var) -> None:
            try:
                value = var.get()
            except tk.TclError:
                return   # half-typed number
            setattr(self.settings, name, value)
            self._refresh_all()

        def _build(self) -> None:
            self.vars = {
                "mode": self._var(tk.StringVar, "mode"),
                "game_dir": self._var(tk.StringVar, "game_dir"),
                "exe": self._var(tk.StringVar, "exe"),
                "characters": self._var(tk.StringVar, "characters"),
                "peer": self._var(tk.StringVar, "peer"),
                "my_address": self._var(tk.StringVar, "my_address"),
                "port": self._var(tk.IntVar, "port"),
                "same_pc": self._var(tk.BooleanVar, "same_pc"),
                "delay": self._var(tk.IntVar, "delay"),
                "encrypt": self._var(tk.BooleanVar, "encrypt"),
                "secret": self._var(tk.StringVar, "secret"),
                "skip_intro": self._var(tk.IntVar, "skip_intro"),
                "sync": self._var(tk.StringVar, "sync"),
                "game_input_delay": self._var(tk.IntVar, "game_input_delay"),
                "second_controller_p2": self._var(tk.BooleanVar, "second_controller_p2"),
                "fullscreen": self._var(tk.BooleanVar, "fullscreen"),
                "remember_addresses": self._var(tk.BooleanVar, "remember_addresses"),
            }
            notebook = ttk.Notebook(self)
            notebook.pack(fill="both", expand=True, padx=8, pady=8)
            play, options, paths = ttk.Frame(notebook, padding=8), ttk.Frame(notebook, padding=8), ttk.Frame(notebook, padding=8)
            notebook.add(play, text="Play")
            notebook.add(options, text="Settings")
            notebook.add(paths, text="Files")
            self._build_play(play)
            self._build_options(options)
            self._build_paths(paths)

        def _row(self, parent, label, widget_factory, row, hint=""):
            ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=3, padx=(0, 8))
            widget = widget_factory(parent)
            widget.grid(row=row, column=1, sticky="ew", pady=3)
            if hint:
                ttk.Label(parent, text=hint, foreground="#666").grid(row=row, column=2, sticky="w", padx=8)
            return widget

        def _build_play(self, f) -> None:
            f.columnconfigure(0, weight=1)
            modes = ttk.LabelFrame(f, text="What do you want to do?", padding=6)
            modes.grid(row=0, column=0, sticky="ew")
            for text, value in (("Play on this PC (no network)", "local"), ("Host a match", "host"), ("Join a match", "join")):
                ttk.Radiobutton(modes, text=text, value=value, variable=self.vars["mode"]).pack(anchor="w")

            self.net = ttk.LabelFrame(f, text="Connection", padding=6)
            self.net.grid(row=1, column=0, sticky="ew", pady=6)
            self.net.columnconfigure(1, weight=1)
            r = 0
            ttk.Checkbutton(self.net, text="Both players are on this PC (two windows, to test)",
                            variable=self.vars["same_pc"]).grid(row=r, column=0, columnspan=3, sticky="w")
            r += 1
            addr = self._row(self.net, "My address:", lambda p: ttk.Entry(p, textvariable=self.vars["my_address"]), r)
            buttons = ttk.Frame(self.net)
            buttons.grid(row=r, column=2, sticky="w", padx=8)
            ttk.Button(buttons, text="This network", command=self._use_lan).pack(side="left")
            ttk.Button(buttons, text="Find my public address…", command=self._find_public).pack(side="left", padx=4)
            r += 1
            self._row(self.net, "Port (UDP):", lambda p: ttk.Spinbox(p, from_=1024, to=65535, width=8, textvariable=self.vars["port"]), r,
                      "Both players use the same one. Over the internet the host forwards it on the router.")
            r += 1
            self.peer_label = ttk.Label(self.net, text="Other player's address:")
            self.peer_label.grid(row=r, column=0, sticky="w", pady=3)
            ttk.Entry(self.net, textvariable=self.vars["peer"]).grid(row=r, column=1, sticky="ew", pady=3)
            r += 1
            ttk.Label(self.net, text="My code:").grid(row=r, column=0, sticky="nw", pady=3)
            self.my_code = tk.Text(self.net, height=3, width=50, wrap="char", state="disabled", background="#f4f4f4")
            self.my_code.grid(row=r, column=1, sticky="ew", pady=3)
            ttk.Button(self.net, text="Copy", command=self._copy_code).grid(row=r, column=2, sticky="nw", padx=8)
            r += 1
            ttk.Label(self.net, text="Their code:").grid(row=r, column=0, sticky="nw", pady=3)
            self.their_code = tk.Text(self.net, height=3, width=50, wrap="char")
            self.their_code.grid(row=r, column=1, sticky="ew", pady=3)
            ttk.Button(self.net, text="Apply", command=self._apply_code).grid(row=r, column=2, sticky="nw", padx=8)
            r += 1
            self.code_note = ttk.Label(self.net, text="", wraplength=620, justify="left")
            self.code_note.grid(row=r, column=0, columnspan=3, sticky="w")

            run = ttk.Frame(f)
            run.grid(row=2, column=0, sticky="ew")
            self.start_button = ttk.Button(run, text="Start the game", command=self._start)
            self.start_button.pack(side="left")
            self.stop_button = ttk.Button(run, text="Stop", command=self._stop, state="disabled")
            self.stop_button.pack(side="left", padx=6)
            ttk.Button(run, text="Copy log", command=self._copy_log).pack(side="right")
            self.status = ttk.Label(f, text="Ready.", font=("TkDefaultFont", 10, "bold"), wraplength=680, justify="left")
            self.status.grid(row=3, column=0, sticky="w", pady=(6, 2))
            self.problems = ttk.Label(f, text="", foreground="#a00", wraplength=680, justify="left")
            self.problems.grid(row=4, column=0, sticky="w")
            logframe = ttk.Frame(f)
            logframe.grid(row=5, column=0, sticky="nsew", pady=4)
            f.rowconfigure(5, weight=1)
            self.log = tk.Text(logframe, height=10, wrap="none", state="disabled")
            scroll = ttk.Scrollbar(logframe, command=self.log.yview)
            self.log.configure(yscrollcommand=scroll.set)
            self.log.pack(side="left", fill="both", expand=True)
            scroll.pack(side="right", fill="y")

        def _build_options(self, f) -> None:
            f.columnconfigure(1, weight=1)
            r = 0
            self._row(f, "Input delay (frames):", lambda p: ttk.Spinbox(p, from_=1, to=30, width=6, textvariable=self.vars["delay"]), r,
                      "About 17 ms each. Both players should use the same.")
            r += 1
            self.ping = tk.StringVar(value="")
            self.ping.trace_add("write", lambda *_: self._suggest_delay())
            self._row(f, "Ping to them (ms):", lambda p: ttk.Entry(p, textvariable=self.ping, width=8), r)
            self.suggest = ttk.Label(f, text="", foreground="#666")
            self.suggest.grid(row=r, column=2, sticky="w", padx=8)
            r += 1
            ttk.Checkbutton(f, text="Encrypt the connection (shared secret)", variable=self.vars["encrypt"]).grid(row=r, column=0, columnspan=2, sticky="w", pady=3)
            r += 1
            secret = self._row(f, "Secret:", lambda p: ttk.Entry(p, textvariable=self.vars["secret"]), r, "The host's code carries it.")
            r += 1
            ttk.Button(f, text="New secret", command=lambda: self.vars["secret"].set(make_secret())).grid(row=r, column=1, sticky="w")
            r += 1
            sync = ttk.LabelFrame(f, text="Start together", padding=6)
            sync.grid(row=r, column=0, columnspan=3, sticky="ew", pady=6)
            ttk.Radiobutton(sync, text="When both players press START at the title screen", value="start", variable=self.vars["sync"]).pack(anchor="w")
            ttk.Radiobutton(sync, text="Right at the first frame", value="now", variable=self.vars["sync"]).pack(anchor="w")
            r += 1
            self._row(f, "Skip the intro for (seconds):", lambda p: ttk.Spinbox(p, from_=0, to=120, width=6, textvariable=self.vars["skip_intro"]), r,
                      "0: off. Taps A and START; the sync point comes when it ends.")
            r += 1
            self._row(f, "The game's own input delay:", lambda p: ttk.Spinbox(p, from_=-1, to=15, width=6, textvariable=self.vars["game_input_delay"]), r,
                      "-1: as display.ini says. Both players must use the same (0, or 2 as on the Wii U).")
            r += 1
            ttk.Checkbutton(f, text="The second controller is player 2", variable=self.vars["second_controller_p2"]).grid(row=r, column=0, columnspan=2, sticky="w", pady=3)
            r += 1
            ttk.Checkbutton(f, text="Full screen", variable=self.vars["fullscreen"]).grid(row=r, column=0, columnspan=2, sticky="w")
            r += 1
            ttk.Checkbutton(f, text="Remember addresses (they are personal data: off keeps them in memory only)",
                            variable=self.vars["remember_addresses"]).grid(row=r, column=0, columnspan=3, sticky="w", pady=(10, 0))

        def _build_paths(self, f) -> None:
            f.columnconfigure(1, weight=1)
            for r, (label, key, folder) in enumerate((("Game folder:", "game_dir", True), ("ttt2 program:", "exe", False), ("Character packs:", "characters", True))):
                ttk.Label(f, text=label).grid(row=r * 2, column=0, sticky="w", pady=3)
                ttk.Entry(f, textvariable=self.vars[key]).grid(row=r * 2, column=1, sticky="ew", pady=3)
                ttk.Button(f, text="Browse…", command=lambda k=key, d=folder: self._browse(k, d)).grid(row=r * 2, column=2, padx=6)
            self.game_note = ttk.Label(f, text="", wraplength=640, justify="left")
            self.game_note.grid(row=1, column=1, sticky="w")
            self.packs_note = ttk.Label(f, text="", wraplength=640, justify="left")
            self.packs_note.grid(row=5, column=1, sticky="w")

        # --- actions
        def _browse(self, key: str, folder: bool) -> None:
            current = self.vars[key].get()
            chosen = filedialog.askdirectory(initialdir=current or None) if folder else filedialog.askopenfilename(initialdir=str(Path(current).parent) if current else None)
            if chosen:
                self.vars[key].set(chosen)

        def _use_lan(self) -> None:
            address = lan_address()
            if address:
                self.vars["my_address"].set(address)
            else:
                messagebox.showinfo("Address", "No network address was found.")

        def _find_public(self) -> None:
            if not messagebox.askyesno(
                    "Find my public address",
                    f"This asks {PUBLIC_ADDRESS_URL} for the address the internet sees you at, so that service "
                    "learns it. Your address is personal data: share it only with the person you play with.\n\nAsk?"):
                return
            try:
                self.vars["my_address"].set(find_public_address())
            except (OSError, ValueError) as error:
                messagebox.showerror("Address", f"Could not find it: {error}")

        def _fingerprints(self) -> tuple[str, str]:
            return game_fingerprint(self.settings.game_dir), packs_fingerprint(self.settings.characters)

        def _refresh_code(self) -> None:
            game_fp, packs_fp = self._fingerprints()
            code = encode_invite(self.settings, game_fp, packs_fp) if self.settings.my_address else "(enter your address first)"
            self.my_code.configure(state="normal")
            self.my_code.delete("1.0", "end")
            self.my_code.insert("1.0", code)
            self.my_code.configure(state="disabled")

        def _copy_code(self) -> None:
            self.clipboard_clear()
            self.clipboard_append(self.my_code.get("1.0", "end").strip())
            self.code_note.configure(text="Copied: send it to the other player.", foreground="#060")

        def _apply_code(self) -> None:
            try:
                invite = decode_invite(self.their_code.get("1.0", "end"))
            except ValueError as error:
                self.code_note.configure(text=str(error), foreground="#a00")
                return
            game_fp, packs_fp = self._fingerprints()
            warnings = apply_invite(self.settings, invite, game_fp, packs_fp)
            for key in ("peer", "port", "delay", "encrypt", "secret", "sync", "game_input_delay"):
                self.vars[key].set(getattr(self.settings, key))
            note = "Applied." if not warnings else "Applied, but: " + " ".join(warnings)
            self.code_note.configure(text=note, foreground="#060" if not warnings else "#a60")

        def _suggest_delay(self) -> None:
            try:
                ping = float(self.ping.get())
            except ValueError:
                self.suggest.configure(text="")
                return
            frames = max(1, round(ping / 2 / (1000 / 60)) + 1)
            self.suggest.configure(text=f"Suggested: {frames} frames")

        def _refresh_all(self) -> None:
            if not hasattr(self, "net"):
                return
            net = self.settings.mode in ("host", "join")
            for child in self.net.winfo_children():
                self._set_state(child, "normal" if net else "disabled")
            self.peer_label.configure(text="Guest's address:" if self.settings.mode == "host" else "Host's address:")
            self._refresh_code()
            ok, message = game_status(self.settings.game_dir)
            self.game_note.configure(text=message, foreground="#060" if ok else "#a00")
            count = count_packs(self.settings.characters)
            self.packs_note.configure(text=f"{count} character pack(s) found." if count else "No character packs found (the game then has only its own fighters).")
            problems = validate(self.settings)
            self.problems.configure(text="\n".join(problems))
            self.start_button.configure(state="disabled" if self.runner.running() else "normal")

        @staticmethod
        def _set_state(widget, state: str) -> None:
            try:
                if isinstance(widget, tk.Text):
                    return   # the code boxes keep their own state
                widget.configure(state=state)
            except tk.TclError:
                pass
            for child in widget.winfo_children():
                App._set_state(child, state)

        def _show(self, line: str) -> None:
            self.log_lines.append(line)
            self.log.configure(state="normal")
            self.log.insert("end", line + "\n")
            self.log.see("end")
            self.log.configure(state="disabled")

        def _start(self) -> None:
            problems = validate(self.settings)
            if problems:
                messagebox.showerror("Cannot start yet", "\n".join(problems))
                return
            save_settings(self.settings)
            self.log_lines.clear()
            self.log.configure(state="normal")
            self.log.delete("1.0", "end")
            self.log.configure(state="disabled")
            self.fighters = 0
            try:
                self.runner.start(build_command(self.settings), build_env(self.settings, portable=self.portable))
            except OSError as error:
                messagebox.showerror("Cannot start", str(error))
                return
            self.status.configure(text="Starting the game…")
            self.start_button.configure(state="disabled")
            self.stop_button.configure(state="normal")

        def _stop(self) -> None:
            self.runner.stop()

        def _copy_log(self) -> None:
            self.clipboard_clear()
            self.clipboard_append(mask_addresses("\n".join(self.log_lines)))
            self.status.configure(text="Log copied (addresses masked).")

        def _poll_runner(self) -> None:
            try:
                while True:
                    line = self.runner.lines.get_nowait()
                    if line is None:
                        code = self.runner.exit_code()
                        why = explain_exit(code)
                        self.status.configure(text=f"The game has ended (exit code {code}). {why}".strip())
                        if why:
                            self._show("ttt2 launcher: " + why)
                        self.start_button.configure(state="normal")
                        self.stop_button.configure(state="disabled")
                        continue
                    self._show(line)
                    if count_added_fighters(line):
                        self.fighters += 1
                    status = parse_status(line)
                    if status:
                        text = status[1]
                        if status[0] == "network" and self.fighters:
                            text += f" {self.fighters} added fighter(s) loaded."
                        self.status.configure(text=text)
            except queue.Empty:
                pass
            self.after(100, self._poll_runner)

        def _close(self) -> None:
            save_settings(self.settings)
            if self.runner.running() and messagebox.askyesno("Quit", "The game is still running. Stop it and quit?"):
                self.runner.stop()
            elif self.runner.running():
                return
            self.destroy()

    return App()


def run_gui(root_dir: Path) -> None:
    make_app(root_dir).mainloop()


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent
    if "--print-env" in argv:   # for scripts and bug reports: what would be set
        s = load_settings()
        print(describe(s))
        return 0
    try:
        import tkinter  # noqa: F401
    except ImportError:
        print("This needs Python's tkinter (Windows: part of Python; Linux: install python3-tk).", file=sys.stderr)
        return 1
    try:
        run_gui(root)
    except Exception:   # pythonw has no console: say what went wrong in a file and a box
        import traceback
        text = traceback.format_exc()
        log = config_path().with_name("launcher-error.log")
        try:
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(text, encoding="utf-8")
        except OSError:
            pass
        try:
            from tkinter import messagebox
            messagebox.showerror("TTT2 launcher", f"The launcher stopped:\n\n{text[-1500:]}\n(Also written to {log})")
        except Exception:
            pass
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
