"""The game's keyboard keys (keyboard.ini), as the Linux launcher's Settings change them
(launcher/core/keymap.*): its inputs, their default keys and the keys' names are read from
launcher/core/keymap.cpp, so that both launchers and the game agree. Keys are positions on the
keyboard (USB HID usages: SDL's scancodes); on Windows a key pressed is found from its scan code.
"""
from pathlib import Path
from functools import lru_cache
import os
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'launcher/core/keymap.cpp'
FIRST_KEY, KEY_COUNT, ESCAPE = 4, 512, 41
MAX_FILE = 64 * 1024


def _unescaped(text):
    return text.encode('ascii', 'backslashreplace').decode('unicode_escape')


@lru_cache(maxsize=None)
def tables(source=SOURCE):
    """(keys, sdl_names, inputs) from keymap.cpp: keys {usage: (evdev, name, typed)}, sdl_names
    [(usage, name)], inputs [(id, group, name, full, default key)]."""
    text = Path(source).read_text(encoding='utf-8')
    def block(start):
        at = text.index(start)
        return text[at:text.index('\n}', at)]
    string = r'"((?:[^"\\]|\\.)*)"'
    keys = {int(u): (int(e), _unescaped(n), t == 'true')
            for u, e, n, t in re.findall(r'\{(\d+), (\d+), %s, (true|false)\}' % string, block('constexpr Key kKeys'))}
    names = [(int(u), _unescaped(n)) for u, n in re.findall(r'\{(\d+), %s\}' % string, block('constexpr Named kSdlNames'))]
    inputs = [(i, g, _unescaped(n), _unescaped(f), int(k)) for i, g, n, f, k in
              re.findall(r'\{"(\w+)", %s, %s, %s, (\d+)\}' % (string, string, string), block('kInputList = '))]
    if len(keys) < 100 or len(names) < 100 or len(inputs) != 19:
        raise ValueError('launcher/core/keymap.cpp no longer has the tables the Windows launcher reads')
    return keys, names, inputs


def inputs():
    return tables()[2]


def key_name(usage):
    """What a key is called where the layout does not say: the launcher's name, else SDL's."""
    keys, names, _ = tables()
    if usage in keys: return keys[usage][1]
    return next((n for u, n in names if u == usage), 'Key %d' % usage)


def reserved(usage):
    """Why a key cannot be given to an input; None when it can (keymap.cpp's reserved_key)."""
    return {68: 'F11 toggles full screen', ESCAPE: 'Esc stops choosing a key', 227: 'Super is the desktop\'s key',
            231: 'Super is the desktop\'s key'}.get(usage)


def modifier(usage):
    return 224 <= usage <= 230 and usage != 227


def key_of(value):
    """A file's value: a usage, or SDL's name of one in any case; 0 when it is neither."""
    value = value.strip()
    if value.isdigit():
        n = int(value)
        return n if FIRST_KEY <= n < KEY_COUNT else 0
    return next((u for u, n in tables()[1] if n.lower() == value.lower()), 0)


class Keymap:
    """One key per input; a key given to one input is taken from the input that had it, which
    gets the first one's old key instead (they swap)."""

    def __init__(self, keys=None):
        self.keys = list(keys or [k for *_, k in inputs()])

    def input_with(self, usage):
        return self.keys.index(usage) if usage in self.keys else -1

    def bind(self, index, usage):
        """(ok, swapped input or -1, why not)."""
        if why := reserved(usage): return False, -1, why
        other = self.input_with(usage)
        if other == index: return True, -1, ''
        old, self.keys[index] = self.keys[index], usage
        if other >= 0: self.keys[other] = old
        return True, other, ''

    def is_default(self):
        return self.keys == Keymap().keys

    def text(self):
        out = ('# Tekken Tag Tournament 2: the keyboard\'s keys for the Wii U GamePad\'s buttons.\n'
               '# Written by Ultimate Launcher (Settings); the game reads it when it starts and\n'
               '# again whenever it changes. A line is "input = key": the key\'s position on the keyboard as a USB HID\n'
               '# usage (an SDL scancode) or an SDL scancode name; the comment names that key on a US keyboard.\n')
        group = None
        for (id_, g, *_), key in zip(inputs(), self.keys):
            if g != group:
                group = g
                out += '\n# %s\n' % g
            out += '%-11s = %-3d  # %s\n' % (id_, key, key_name(key))
        return out

    @classmethod
    def parse(cls, text):
        """The keys as the game reads them, and notes on what it left out or shares."""
        ids = [i[0] for i in inputs()]
        keymap, given, notes = cls(), [0] * len(ids), []
        if text.startswith('﻿'): text = text[1:]
        for number, raw in enumerate(text.split('\n'), 1):
            line = raw.split('#', 1)[0].strip(' \t\r\v\f')
            if not line: continue
            name, equals, value = line.partition('=')
            if not equals:
                notes.append('line %d: not "input = key"' % number); continue
            name = name.strip(' \t\r\v\f')
            if name not in ids:
                notes.append('line %d: no input %s' % (number, name)); continue
            index, key = ids.index(name), key_of(value)
            if not key or reserved(key):
                notes.append('line %d: %s keeps its default' % (number, name)); continue
            keymap.keys[index], given[index] = key, number
        # A key given to two inputs: the input of the earlier line keeps it, the other gets a free key.
        for i in range(len(ids)):
            winner = next((j for j in range(len(ids)) if j != i and keymap.keys[j] == keymap.keys[i] and given[j]
                           and (not given[i] or given[j] < given[i])), -1)
            if winner < 0: continue
            notes.append('%s: %s and %s' % (key_name(keymap.keys[i]), inputs()[winner][3], inputs()[i][3]))
            key = inputs()[i][4]
            if keymap.input_with(key) >= 0: key = inputs()[winner][4]
            if keymap.input_with(key) >= 0:
                key = next(u for u in sorted(tables()[0]) if not reserved(u) and keymap.input_with(u) < 0)
            keymap.keys[i] = key
        return keymap, notes

    @classmethod
    def load(cls, file):
        try:
            if Path(file).stat().st_size > MAX_FILE: return cls(), ['the file is larger than the game reads']
            return cls.parse(Path(file).read_text(encoding='utf-8', errors='replace'))
        except FileNotFoundError:
            return cls(), []
        except OSError as error:
            return cls(), [str(error)]

    def save(self, file):
        file = Path(file)
        file.parent.mkdir(parents=True, exist_ok=True)
        part = file.with_name(file.name + '.part')
        part.write_text(self.text(), encoding='utf-8', newline='\n')
        os.replace(part, file)


# ---------------------------------------------------------------- keys pressed

# Scan codes with the 0xE0 prefix (set 1), as Linux's input event codes, which keymap.cpp's
# table names; without it a scan code is its event code.
EXTENDED = {0x1C: 96, 0x1D: 97, 0x35: 98, 0x37: 99, 0x38: 100, 0x47: 102, 0x48: 103, 0x49: 104, 0x4B: 105,
            0x4D: 106, 0x4F: 107, 0x50: 108, 0x51: 109, 0x52: 110, 0x53: 111, 0x5B: 125, 0x5C: 126, 0x5D: 127}
# Keys found by Tk's names for them: those Windows names alike on both sides, and the ones whose
# scan code is the keypad's but for its 0xE0 prefix (not every system reports that prefix).
NAMED = {'Shift_L': 225, 'Shift_R': 229, 'Control_L': 224, 'Control_R': 228, 'Alt_L': 226, 'Alt_R': 230,
         'Win_L': 227, 'Win_R': 231, 'Super_L': 227, 'Super_R': 231, 'KP_Enter': 88, 'Return': 40,
         'Left': 80, 'Right': 79, 'Up': 82, 'Down': 81, 'Home': 74, 'End': 77, 'Prior': 75, 'Next': 78,
         'Insert': 73, 'Delete': 76, 'KP_Divide': 84, 'App': 101, 'Menu': 101}


def _user32():
    import ctypes
    return ctypes.windll.user32


def usage_of(event):
    """The position of the key pressed (a USB HID usage), 0 when it is none the game takes."""
    if event.keysym in NAMED: return NAMED[event.keysym]
    by_evdev = {e: u for u, (e, _, _) in tables()[0].items()}
    if sys.platform != 'win32':
        return by_evdev.get(event.keycode - 8, 0)        # (X keycodes: the event code + 8)
    code = _user32().MapVirtualKeyW(event.keycode, 4)    # MAPVK_VK_TO_VSC_EX: the scan code, 0xE0.. extended
    if not code: return 0
    evdev = EXTENDED.get(code & 0xFF, 0) if code >> 8 in (0xE0, 0xE1) else code & 0xFF
    return by_evdev.get(evdev, 0)


def label(usage):
    """A key as the keyboard's layout names it (A, ;, ß ...), else its name."""
    keys = tables()[0]
    if usage in keys and keys[usage][2] and sys.platform == 'win32':
        evdev = keys[usage][0]
        scan = next((0xE000 | s for s, e in EXTENDED.items() if e == evdev), evdev)
        try:
            user32 = _user32()
            virtual = user32.MapVirtualKeyW(scan, 3)             # MAPVK_VSC_TO_VK_EX
            character = user32.MapVirtualKeyW(virtual, 2) & 0x7FFF if virtual else 0   # MAPVK_VK_TO_CHAR
            if character > 32: return chr(character).upper()
        except Exception:
            pass
    return key_name(usage)
