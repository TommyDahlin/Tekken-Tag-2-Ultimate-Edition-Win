"""Unit tests of a Tekken 8 fighter's automatic evades (convert/moveset.py EVADE: Azucena's Libertador ducks a high
attack and hops a low one): Tekken 8's evade properties become TTT2's parry rows first in the move's cancel list, on
made-up tables (no game files needed).

    python3 -m pytest -q tools/characters/tests/test_tekken8_evade.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_tekken8_convert import Source8, converter, converted  # noqa: E402
from ttt2char.convert import idmaps as M  # noqa: E402
from ttt2char.convert import moveset, tagsystem  # noqa: E402

FRAMES, MOVE, CLASS, DIRECTION, MARK = (moveset.EVADE['t8'][k]
                                        for k in ('frames', 'move', 'class', 'direction', 'mark'))
START = 0x8001                      # a property's frame: from the move's start
HIGH, MID, LOW = 1, 2, 3
DUCK, HOP = 1, 2                    # the evade moves (Tekken 8 indices; a slot's move 0 is none); the moves given
FIRST = 3                           # start at 3
CONVERTED = 100                     # a converted move's TTT2 index: CONVERTED + its Tekken 8 index


def evade_source(*moves):
    """A Tekken 8 moveset: a move, the duck, the hop, then `moves`, each a list of property rows (frame, id, value,
    slot[, requirement row])."""
    n = FIRST + len(moves)
    k = Source8({i: 'Cat_m%d' % i for i in range(n)}, [(i, 'own') for i in range(n)])
    always = k.req((0, 0))
    for i, rows in enumerate([[]] * FIRST + list(moves)):
        start = len(k.tables['extra_move_properties'])
        for r in rows:
            req = k.req(r[4]) if len(r) > 4 else always
            k.tables['extra_move_properties'].append(dict(frame=r[0], id=r[1], value=r[2], value2=r[3], req=req,
                                                          value3=0, value4=0, value5=0))
        k.tables['extra_move_properties'].append(dict(frame=0, id=0, value=0, value2=0, req=None, value3=0,
                                                      value4=0, value5=0))
        k.tables['moves'][i].update(extraprops=start if rows else None)
    return k


def libertador(frame=START, frames=0x7FFF):
    """Azucena's Libertador moves' properties: slot 0 takes highs into the duck, slot 1 lows into the hop."""
    return [(frame, FRAMES, frames, 0), (frame, CLASS, HIGH, 0), (frame, CLASS, LOW, 1), (frame, MARK, 1, 0),
            (frame, DIRECTION, 3, 0), (frame, DIRECTION, 3, 1), (frame, MOVE, DUCK, 0), (frame, MOVE, HOP, 1)]


def evade_converter(k, converted_moves=None):
    """A converter of k past its links: the moves converted (default all), each with a list of its own (an END row
    into the stance); the cancel extradata 0x80 at index 0."""
    c = converter(k, req={0: 0})
    n = len(k.tables['moves'])
    keep = range(n) if converted_moves is None else converted_moves
    c.newidx = {i: CONVERTED + i for i in keep}
    c.delta_move, c.dn, c.react_remap, c.cls = {}, {}, {}, ['K'] * n
    c.alloc_ok = c.linking = 0
    c.T.update(cancels=[], cancel_extras=[{'value': 0x80}], moves=[dict(cancel=None) for _ in range(CONVERTED + n)])
    c.extra_index = {0x80: 0}
    for i in keep:
        c.T['moves'][CONVERTED + i]['cancel'] = len(c.T['cancels'])
        c.T['cancels'].append(dict(direction=M.LIST_END, button=0, req=None, extradata=0, win_start=30,
                                   win_end=32767, start_frame=30, move_id=0x8001, option=0))
    return c


def rows_of(c, move):
    """Tekken 8 move `move`'s converted cancel rows: (input, window, start frame, extradata value, requirement list,
    target)."""
    out = []
    for x in tagsystem.cancels(c.T, c.T['moves'][CONVERTED + move]['cancel']):
        req = converted(c, x['req']) if x['req'] is not None else None
        out.append((x['direction'], x['button'], x['win_start'], x['win_end'], x['start_frame'],
                    c.T['cancel_extras'][x['extradata']]['value'], req, x['move_id']))
    return out


def evade_rows(first, last, tests, target):
    return [(0, 0, first, last, first, 0x80, [(t, limb)], CONVERTED + target) for t in tests for limb in (0, 64)]


END = (M.LIST_END, 0, 30, 32767, 30, 0x80, None, 0x8001)


class Evades(unittest.TestCase):
    def test_libertador(self):
        # Libertador from its start, for the rest of the move: a high (faced or back turned, any limb or a hit
        # location 64 attack) into the duck, then a low into the hop, before the move's own rows
        k = evade_source(libertador())
        c = evade_converter(k)
        c.evade_rows()
        self.assertEqual(rows_of(c, FIRST), evade_rows(1, 32767, (678, 682), DUCK) +
                         evade_rows(1, 32767, (680, 684), HOP)
                         + [END])
        self.assertEqual(c.stats['moves given automatic evades (Tekken 8: cancel rows)'], 1)
        self.assertEqual(c.stats['automatic evade rows'], 8)
        self.assertEqual(rows_of(c, DUCK), [END])                   # (the evade moves have none of their own here)

    def test_window(self):
        # from the property's frame for its frames (here 10: 18..27), cut where the next evade opens (frame 22), a
        # second one for the rest of the move
        k = evade_source(libertador(18, 10) + [(22, FRAMES, 0x7FFF, 0)])
        c = evade_converter(k)
        self.assertEqual(c.evades(FIRST), [(18, 21, [(HIGH, DUCK), (LOW, HOP)]),
                                           (22, 32767, [(HIGH, DUCK), (LOW, HOP)])])

    def test_classes(self):
        # mid and a slot with no class (any attack: all three of TTT2's tests); a slot with no move, a property row
        # under a condition TTT2 cannot test (Heat) and evades of 0 frames give nothing
        k = evade_source([(5, FRAMES, 6, 0), (5, CLASS, MID, 0), (5, MOVE, DUCK, 0), (5, MOVE, HOP, 1)],
                         [(1, FRAMES, 9, 0), (1, CLASS, HIGH, 0)],
                         [(START, FRAMES, 0x7FFF, 0, (1028, 1)), (START, CLASS, HIGH, 0), (START, MOVE, DUCK, 0)],
                         [(START, FRAMES, 0, 0), (START, CLASS, HIGH, 0), (START, MOVE, DUCK, 0)])
        c = evade_converter(k)
        self.assertEqual(c.evades(FIRST), [(5, 10, [(MID, DUCK), (0, HOP)])])
        self.assertEqual([c.evades(FIRST + i) for i in (1, 2, 3)], [[], [], []])
        c.evade_rows()
        self.assertEqual(rows_of(c, FIRST), evade_rows(5, 10, (679, 683), DUCK)
                         + evade_rows(5, 10, (678, 682, 679, 683, 680, 684), HOP) + [END])
        for i in (1, 2, 3):
            self.assertEqual(rows_of(c, FIRST + i), [END])

    def test_evade_move_not_converted(self):
        k = evade_source(libertador())
        c = evade_converter(k, converted_moves=[DUCK, FIRST])
        c.evade_rows()
        self.assertEqual(rows_of(c, FIRST), evade_rows(1, 32767, (678, 682), DUCK) + [END])
        self.assertEqual(c.stats['evades left out: the evade move is not converted'], 1)

    def test_shared_lists(self):
        # a list another move shares is copied, not changed; moves with the same evades and list share the copy
        k = evade_source(libertador(), libertador(), [])
        c = evade_converter(k)
        moves = c.T['moves']
        a, b, other = (CONVERTED + FIRST + i for i in range(3))
        shared = moves[a]['cancel']
        moves[b]['cancel'] = moves[other]['cancel'] = shared
        c.evade_rows()
        self.assertEqual(rows_of(c, FIRST + 2), [END])
        self.assertEqual(moves[other]['cancel'], shared)
        self.assertEqual(moves[a]['cancel'], moves[b]['cancel'])
        self.assertNotEqual(moves[a]['cancel'], shared)

    def test_properties_left_out(self):
        # the evade properties themselves have no TTT2 counterpart: left out of the property lists, not counted as
        # ids without one
        c = evade_converter(evade_source(libertador()))
        for p in (FRAMES, MOVE, CLASS, DIRECTION, MARK):
            self.assertIsNone(c.map_prop(p))
        self.assertEqual(c.stats['evade properties (cancel rows instead)'], 5)
        self.assertFalse(c.unmapped)

    def test_tekken7_unchanged(self):
        self.assertIsNone(moveset.EVADE['t7'])
        c = evade_converter(evade_source(libertador()))
        before = rows_of(c, FIRST)
        c.game = 't7'
        c.evade_rows()
        self.assertEqual(rows_of(c, FIRST), before)
        self.assertEqual(c.evades(FIRST), [])


if __name__ == '__main__':
    unittest.main()
