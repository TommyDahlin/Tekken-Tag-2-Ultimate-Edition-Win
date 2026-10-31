#!/usr/bin/env python3
"""Reads the traces TTT2_TRACE_FIGHT_STATE writes (runtime/src/title/fight_trace.h).

  fight_state_report.py summary a.trace
      How many pages change per frame, and which address ranges they form:
      what a rollback snapshot has to cover.
  fight_state_report.py compare a.trace b.trace
      Whether two runs of the same input script changed the same pages to the
      same contents, and where they first differ.
"""

import sys

PAGE = 0x1000


def load(path):
    """-> [(battle frame, {page: hash})], the first scan (the baseline) left out."""
    frames = []
    with open(path) as f:
        current = None
        for line in f:
            kind, *rest = line.split()
            if kind == "F":
                current = (int(rest[0]), {})
                frames.append(current)
            elif kind == "P":
                current[1][int(rest[0], 16)] = int(rest[1], 16)
    return frames[1:]


def ranges(pages, gap=16):
    """Merges sorted page addresses that lie within `gap` pages into (begin, end, pages)."""
    out = []
    for page in sorted(pages):
        if out and page - out[-1][1] <= gap * PAGE:
            out[-1][1] = page + PAGE
            out[-1][2] += 1
        else:
            out.append([page, page + PAGE, 1])
    return out


def show(found, limit=40):
    for begin, end, count in sorted(found, key=lambda r: -r[2])[:limit]:
        print(f"  {begin:08X}-{end:08X}  {(end - begin) // 1024:8d} KiB spanned, {count * 4:8d} KiB changing")
    if len(found) > limit:
        print(f"  ... {len(found) - limit} more ranges")


def summary(path):
    frames = load(path)
    if not frames:
        sys.exit("no frames in the trace (the first only sets the baseline)")
    counts = [len(pages) for _, pages in frames]
    seen = {}
    for _, pages in frames:
        for page in pages:
            seen[page] = seen.get(page, 0) + 1
    print(f"{len(frames)} frames; pages changing per frame: min {min(counts)}, "
          f"mean {sum(counts) // len(counts)}, max {max(counts)} (x 4 KiB)")
    print(f"{len(seen)} different pages changed ({len(seen) * 4 / 1024:.1f} MiB)")
    for share in (0.9, 0.5, 0.1):
        hot = [p for p, n in seen.items() if n >= share * len(frames)]
        print(f"\npages changing in at least {int(share * 100)}% of the frames: {len(hot)} "
              f"({len(hot) * 4 / 1024:.1f} MiB), in {len(ranges(hot))} ranges:")
        show(ranges(hot), 12)
    print("\nall changed pages by range:")
    show(ranges(seen), 30)


def compare(a_path, b_path):
    a, b = load(a_path), load(b_path)
    frames = min(len(a), len(b))
    print(f"{len(a)} and {len(b)} frames; comparing {frames}")
    differing = 0
    first = None
    for i in range(frames):
        pages_a, pages_b = a[i][1], b[i][1]
        # a page is different if only one run changed it or the contents differ
        diff = [p for p in pages_a.keys() | pages_b.keys() if pages_a.get(p) != pages_b.get(p)]
        if diff:
            differing += 1
            if first is None:
                first = (i, a[i][0], diff)
    if first is None:
        print("identical: every frame changed the same pages to the same contents")
        return
    i, frame, diff = first
    print(f"{differing} of {frames} frames differ; the first is scan {i} (battle frame {frame}), "
          f"{len(diff)} pages:")
    show(ranges(diff))
    all_diff = set()
    for i in range(frames):
        pa, pb = a[i][1], b[i][1]
        all_diff.update(p for p in pa.keys() | pb.keys() if pa.get(p) != pb.get(p))
    print(f"\nover the whole trace {len(all_diff)} different pages, in {len(ranges(all_diff))} ranges:")
    show(ranges(all_diff), 20)


def main(argv):
    if len(argv) == 3 and argv[1] == "summary":
        summary(argv[2])
    elif len(argv) == 4 and argv[1] == "compare":
        compare(argv[2], argv[3])
    else:
        sys.exit(__doc__)


main(sys.argv)
