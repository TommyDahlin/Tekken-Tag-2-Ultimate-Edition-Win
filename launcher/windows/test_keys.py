"""The Windows launcher's keyboard keys (keys.py) as launcher/core/keymap.cpp has them: its tables,
keyboard.ini as the Linux launcher writes it and the game reads it, swapping, refusing."""
from pathlib import Path
import tempfile
import unittest

import keys
from keys import Keymap


class Event:
    def __init__(self, keysym, keycode): self.keysym, self.keycode = keysym, keycode


class Keys(unittest.TestCase):
    def test_tables_from_keymap_cpp(self):
        table, names, inputs = keys.tables()
        self.assertEqual(len(inputs), 19)
        self.assertEqual(inputs[0], ('dpad_up', 'D-pad', 'Up', 'D-pad Up', 26))
        self.assertEqual(inputs[17][2], '− (Select)')
        self.assertEqual((table[4], table[49][1], table[80][1]), ((30, 'A', True), '\\', 'Left'))
        self.assertEqual(keys.key_of('return'), 40)
        self.assertEqual((keys.key_of('7'), keys.key_of('3'), keys.key_of('600'), keys.key_of('nothing')), (7, 0, 0, 0))
        self.assertEqual(keys.key_name(80), 'Left')

    def test_file_as_the_linux_launcher_writes_it(self):
        text = Keymap().text()
        self.assertTrue(text.startswith('# Tekken Tag Tournament 2: the keyboard\'s keys for the Wii U GamePad\'s buttons.\n'))
        self.assertIn('\n# D-pad\ndpad_up     = 26   # W\n', text)
        self.assertIn('\n# System\nplus        = 40   # Enter\nminus       = 42   # Backspace\nhome        = 74   # Home\n', text)
        self.assertEqual(Keymap.parse(text)[0].keys, Keymap().keys)
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / 'config/keyboard.ini'
            Keymap().save(file)
            self.assertEqual(file.read_bytes(), text.encode('utf-8'))
            self.assertEqual(Keymap.load(file)[0].keys, Keymap().keys)
            missing, notes = Keymap.load(Path(folder) / 'missing.ini')
            self.assertEqual((missing.keys, notes), (Keymap().keys, []))

    def test_swap_and_refusals(self):
        keymap = Keymap()
        self.assertEqual(keymap.bind(5, 26), (True, 0, ''))          # Bottom button takes W from D-pad Up ...
        self.assertEqual(keymap.keys[:6], [14, 22, 4, 7, 12, 26])    # ... which gets K
        self.assertEqual(keymap.bind(5, 68)[2], 'F11 toggles full screen')
        self.assertEqual(keymap.bind(5, 227)[0], False)
        self.assertEqual(keymap.bind(5, 26), (True, -1, ''))

    def test_file_read_as_the_game_reads_it(self):
        keymap, notes = Keymap.parse('﻿dpad_up = K  # by hand\nface_bottom = 14\nbogus = 3\nhome = F11\nnot a line\n')
        self.assertEqual(keymap.keys[0], 14)                         # the earlier line keeps K ...
        self.assertEqual(keymap.keys[5], 26)                         # ... the other gets a free key
        self.assertEqual(keymap.keys[18], 74)                        # (F11 cannot be given: the default)
        self.assertEqual(len(notes), 4)

    def test_keys_pressed(self):
        # (Off Windows a key's code is X's: Linux's event code + 8.)
        self.assertEqual(keys.usage_of(Event('Left', 0)), 80)
        self.assertEqual(keys.usage_of(Event('Shift_R', 0)), 229)
        if keys.sys.platform != 'win32': self.assertEqual(keys.usage_of(Event('a', 30 + 8)), 4)


if __name__ == '__main__': unittest.main()
