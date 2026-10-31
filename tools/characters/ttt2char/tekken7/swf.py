"""Tekken 7's HUD movies (UE4 SwfMovie: a Scaleform "GFX" file, the SWF format with GFx's own tags): their pictures,
sprites and timelines, flattened into placed pictures frame by frame.

The SWF facts used (Adobe's SWF file format specification): a header (signature, version, length, the stage RECT,
frame rate, frame count), then tags (u16 code << 6 | length, length 0x3f: a u32 follows). DefineSprite (39: id, frame
count, its own tags), ShowFrame (1), FrameLabel (43), PlaceObject2/3 (26/70: flags, depth, character, MATRIX,
CXFORMWITHALPHA, ratio, name, clip depth, PlaceObject3's filters and blend mode), RemoveObject2 (28), DefineShape 1-4
(2, 22, 32, 83: id, bounds). GFx's DefineExternalImage2 (1009): u32 id, u16 format, u16 width, u16 height, then the
export name and the file name (u8 length + bytes): a picture the movie names, in the package beside it.
A MATRIX maps (x, y) to (x * sx + y * r1 + tx, x * r0 + y * sy + ty); a colour transform (CXFORM) is c * m / 256 + a.
Positions are twips (1/20 pixel) in the file, pixels here.
"""
import struct

from .. import InputError

BLEND_ADD = 8           # PlaceObject3's blend mode "add"


class _Bits:
    def __init__(self, d, o):
        self.d, self.o, self.b = d, o, 0

    def ub(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.o] >> (7 - self.b)) & 1)
            self.b += 1
            if self.b == 8:
                self.b, self.o = 0, self.o + 1
        return v

    def sb(self, n):
        v = self.ub(n)
        return v - (1 << n) if n and v & (1 << (n - 1)) else v

    def end(self):
        return self.o + (1 if self.b else 0)


def _rect(d, o):
    b = _Bits(d, o)
    n = b.ub(5)
    x0, x1, y0, y1 = (b.sb(n) / 20 for _ in range(4))
    return (x0, y0, x1, y1), b.end()


def _matrix(d, o):
    b = _Bits(d, o)
    sx = sy = 1.0
    r0 = r1 = 0.0
    if b.ub(1):
        n = b.ub(5)
        sx, sy = b.sb(n) / 65536, b.sb(n) / 65536
    if b.ub(1):
        n = b.ub(5)
        r0, r1 = b.sb(n) / 65536, b.sb(n) / 65536
    n = b.ub(5)
    tx, ty = b.sb(n) / 20, b.sb(n) / 20
    return (sx, r0, r1, sy, tx, ty), b.end()


def _cxform(d, o):
    b = _Bits(d, o)
    has_add, has_mult, n = b.ub(1), b.ub(1), b.ub(4)
    mult = tuple(b.sb(n) / 256 for _ in range(4)) if has_mult else (1.0,) * 4
    add = tuple(b.sb(n) / 255 for _ in range(4)) if has_add else (0.0,) * 4
    return (mult, add), b.end()


def _cstr(d, o):
    e = d.index(b'\0', o)
    return d[o:e].decode('latin1'), e + 1


def _tags(d, o, end):
    while o + 2 <= end:
        h, = struct.unpack_from('<H', d, o)
        o += 2
        code, n = h >> 6, h & 0x3f
        if n == 0x3f:
            n, = struct.unpack_from('<I', d, o)
            o += 4
        yield code, d[o:o + n]
        o += n
        if code == 0:
            return


def _place(code, t):
    if code == 26:
        flags, flags2, o = t[0], 0, 1
    else:
        flags, flags2, o = t[0], t[1], 2
    depth, = struct.unpack_from('<H', t, o)
    o += 2
    if code == 70 and flags2 & 0x08:               # a class name (GFx sets "has image" without one)
        _, o = _cstr(t, o)
    p = dict(depth=depth, move=bool(flags & 1))
    if flags & 0x02:
        p['char'], = struct.unpack_from('<H', t, o)
        o += 2
    if flags & 0x04:
        p['matrix'], o = _matrix(t, o)
    if flags & 0x08:
        p['cx'], o = _cxform(t, o)
    if flags & 0x10:
        p['ratio'], = struct.unpack_from('<H', t, o)
        o += 2
    if flags & 0x20:
        p['name'], o = _cstr(t, o)
    if flags & 0x40:
        p['clip'], = struct.unpack_from('<H', t, o)
        o += 2
    if code == 70 and flags2 & 0x02:
        if flags2 & 0x01:                           # filters come first: not read (none in these movies)
            raise InputError('a HUD movie uses filters, which the importer does not read')
        p['blend'] = t[o]
    return p


class Picture:
    def __init__(self, cid, name, width, height):
        self.id, self.name, self.width, self.height = cid, name, width, height


class Shape:
    def __init__(self, cid, bounds):
        self.id, self.bounds = cid, bounds


class Sprite:
    """frames: per frame, its PlaceObject / RemoveObject entries in order; labels {name: frame index}."""

    def __init__(self, cid):
        self.id, self.frames, self.labels = cid, [[]], {}


def _read_timeline(d, o, end, sprite, characters):
    for code, t in _tags(d, o, end):
        if code in (26, 70):
            sprite.frames[-1].append(('place', _place(code, t)))
        elif code == 28:
            sprite.frames[-1].append(('remove', struct.unpack_from('<H', t)[0]))
        elif code == 1:
            sprite.frames.append([])
        elif code == 43:
            sprite.labels[_cstr(t, 0)[0]] = len(sprite.frames) - 1
        elif code == 39:
            cid, _ = struct.unpack_from('<HH', t)
            s = Sprite(cid)
            _read_timeline(t, 4, len(t), s, characters)
            characters[cid] = s
        elif code in (2, 22, 32, 83):
            cid, = struct.unpack_from('<H', t)
            characters[cid] = Shape(cid, _rect(t, 2)[0])
        elif code == 1009:
            cid, _, w, h = struct.unpack_from('<IHHH', t)
            n = t[10]
            characters[cid] = Picture(cid, t[11:11 + n].decode('latin1'), w, h)
    if not sprite.frames[-1]:
        sprite.frames.pop()


class Movie:
    def __init__(self, data):
        if data[:3] not in (b'GFX', b'FWS'):
            raise InputError('not an uncompressed SWF/GFX movie')
        length, = struct.unpack_from('<I', data, 4)
        data = data[:length]
        self.stage, o = _rect(data, 8)
        o += 4                                      # frame rate, frame count
        self.characters = {}
        self.root = Sprite(0)
        _read_timeline(data, o, len(data), self.root, self.characters)

    def sprite(self, cid):
        return self.root if cid == 0 else self.characters[cid]

    def display(self, sprite, frame):
        """The sprite's display list at a frame (0-based), as playing it from its first frame makes it: {depth: entry
        (char, matrix, cx, name, clip, blend, since: the frame its character was placed)}."""
        shown = {}
        for f in range(min(frame, len(sprite.frames) - 1) + 1):
            for kind, p in sprite.frames[f]:
                if kind == 'remove':
                    shown.pop(p, None)
                    continue
                e = shown.get(p['depth'])
                if 'char' in p and (not p['move'] or e is None or e['char'] != p['char']):
                    e = dict(char=p['char'], matrix=(1, 0, 0, 1, 0, 0), cx=((1,) * 4, (0,) * 4), name=None, clip=0,
                             blend=0, since=f)
                    shown[p['depth']] = e
                elif e is None:
                    continue
                for key in ('matrix', 'cx', 'name', 'clip', 'blend'):
                    if key in p:
                        e[key] = p[key]
        return shown

    def find(self, path, sprite=None):
        """The (sprite id, matrix) a dotted instance path names from the root's first frame (or `sprite`'s)."""
        sprite = sprite or self.root
        matrix = (1, 0, 0, 1, 0, 0)
        for name in path.split('.'):
            e = next((e for e in self.display(sprite, 0).values() if e['name'] == name), None)
            if e is None:
                raise InputError('the HUD movie has no %s' % path)
            matrix = concat(matrix, e['matrix'])
            sprite = self.characters[e['char']]
        return sprite, matrix

    def flatten(self, sprite, frame, matrix=(1, 0, 0, 1, 0, 0), cx=((1,) * 4, (0,) * 4), blend=0, skip=(), fixed=None):
        """The pictures a sprite shows at a frame, bottom first: [(picture, matrix, cx, blend, clip rect or None)].
        Nested sprites play their own timelines from where they were placed (looping), those named in `fixed` ({name:
        frame}) stay at that frame (a clip the game's code sets); masks (clip layers) are taken as their bounding
        rectangles; instances named in `skip` are left out."""
        out = []
        masks = []                                  # (clip depth, rect)
        shown = self.display(sprite, frame)
        for depth in sorted(shown):
            e = shown[depth]
            masks = [m for m in masks if depth <= m[0]]
            if e['name'] in skip:
                continue
            m = concat(matrix, e['matrix'])
            c = concat_cx(cx, e['cx'])
            b = e['blend'] or blend
            ch = self.characters.get(e['char'])
            if e['clip']:
                rect = self.bounds(ch, m)
                if rect is not None:
                    masks.append((e['clip'], rect))
                continue
            clip = None
            for _, r in masks:
                clip = r if clip is None else (max(clip[0], r[0]), max(clip[1], r[1]),
                                               min(clip[2], r[2]), min(clip[3], r[3]))
            if isinstance(ch, Picture):
                out.append((ch, m, c, b, clip))
            elif isinstance(ch, Sprite):
                n = len(ch.frames)
                at = fixed[e['name']] if fixed and e['name'] in fixed else (frame - e['since']) % n if n else 0
                for pic, mm, cc, bb, cl in self.flatten(ch, at, m, c, b, skip, fixed):
                    if clip is not None:
                        cl = clip if cl is None else (max(clip[0], cl[0]), max(clip[1], cl[1]),
                                                      min(clip[2], cl[2]), min(clip[3], cl[3]))
                    out.append((pic, mm, cc, bb, cl))
        return out

    def bounds(self, ch, matrix):
        """A shape's (or a sprite's shapes') bounds through a matrix, as (x0, y0, x1, y1)."""
        if isinstance(ch, Shape):
            x0, y0, x1, y1 = ch.bounds
            pts = [apply(matrix, x, y) for x in (x0, x1) for y in (y0, y1)]
            return (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))
        if isinstance(ch, Sprite):
            rects = [self.bounds(self.characters.get(e['char']), concat(matrix, e['matrix']))
                     for e in self.display(ch, 0).values()]
            rects = [r for r in rects if r is not None]
            if rects:
                return (min(r[0] for r in rects), min(r[1] for r in rects), max(r[2] for r in rects),
                        max(r[3] for r in rects))
        return None


def concat(outer, inner):
    """The matrix applying `inner`, then `outer`."""
    a, b, c, d, tx, ty = outer                      # x' = a x + c y + tx, y' = b x + d y + ty
    e, f, g, h, ux, uy = inner
    return (a * e + c * f, b * e + d * f, a * g + c * h, b * g + d * h, a * ux + c * uy + tx, b * ux + d * uy + ty)


def apply(m, x, y):
    return m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]


def concat_cx(outer, inner):
    """The colour transform applying `inner`, then `outer`."""
    (m1, a1), (m2, a2) = inner, outer
    return tuple(x * y for x, y in zip(m1, m2)), tuple(x * y + z for x, y, z in zip(a1, m2, a2))
