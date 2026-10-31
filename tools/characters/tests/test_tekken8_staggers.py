"""A Tekken 8 stagger never takes a TTT2 reaction that knocks down or launches (convert/moveset.py REST, Converter.rest,
Converter.fells): synthetic movesets only."""
import os
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from ttt2char.convert import moveset  # noqa: E402


class Moves:
    """A moveset of named moves [(name, transition (index or None), airborne)]; alias 0x8000 is move 0."""

    def __init__(self, rows):
        self.names = [r[0] for r in rows]
        self.tables = {'moves': [{'transition': r[1] if r[1] is not None else 0x8000,
                                  'airborne_start': 3 if r[2] else 0} for r in rows]}

    def resolve_alias(self, t):
        return 0 if t == 0x8000 else None

    def move_name(self, i):
        return self.names[i]


def fells(k, mid, d, j):
    c = SimpleNamespace(k=k, d=d, name=k.move_name, rest=moveset.Converter.rest)
    return moveset.Converter.fells(c, mid, j)


class Staggers(unittest.TestCase):
    def setUp(self):
        # Tekken 8: a stagger back to the stance, one into a crouch, a launch
        self.k = Moves([('Cat_sKAM00_', None, False), ('sDm_KAOLee', 0, False), ('#405', 3, False),
                        ('cKAM__00_', None, False), ('#37', 5, True), ('dwKAM_00U', None, False)])
        # TTT2's donor: a knock-down, a launch, a stagger
        self.d = Moves([('Ju_sKAM00_', None, False), ('sDw_KAO00_', 4, False), ('sDw_UP_11_', 3, True),
                        ('Dw_AIR00_', 4, True), ('dwKAM_00U', None, False), ('sDm_KAO10FLP', 0, False)])

    def test_rest(self):
        self.assertEqual(moveset.Converter.rest(self.k, 1, self.k.move_name), ('stand', False))
        self.assertEqual(moveset.Converter.rest(self.k, 2, self.k.move_name), ('crouch', False))
        self.assertEqual(moveset.Converter.rest(self.d, 2, self.d.move_name), ('down', True))
        self.assertEqual(moveset.Converter.rest(self.d, 1, self.d.move_name), ('down', False))

    def test_a_stagger_is_not_felled(self):
        self.assertTrue(fells(self.k, 1, self.d, 1))           # stagger -> knock-down: refused
        self.assertTrue(fells(self.k, 2, self.d, 2))           # stagger into a crouch -> launch: refused
        self.assertFalse(fells(self.k, 1, self.d, 5))          # stagger -> stagger: kept

    def test_air_and_down_reactions_keep_ttt2s(self):
        self.assertFalse(fells(self.k, 4, self.d, 2))          # Tekken 8's air reaction -> TTT2's launch: kept
        self.assertFalse(fells(self.k, 4, self.d, 1))


if __name__ == '__main__':
    unittest.main()
