import os, pathlib, sys, unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qrmerge

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata")


def big():
    return "".join(pathlib.Path(DATA, "qr-%s.txt" % n).read_text(encoding="latin-1") for n in "abc")


def window(lines, end, size):
    """What the phone sends: the last `size` bytes before line `end`, starting at a line start."""
    out, n = [], 0
    for ln in reversed(lines[:end]):
        n += len(ln) + 1
        if n > size and out:
            break
        out.append(ln)
    return "\n".join(reversed(out)) + "\n"


class QrMerge(unittest.TestCase):
    def test_three_overlapping_windows_rebuild_the_region(self):
        lines = big().split("\n")[:-1]
        self.assertGreater(len(lines), 150)
        ends = [90, 140, 190]
        wins = [window(lines, e, 7000) for e in ends]
        merged, snap, gaps = qrmerge.merge(wins)
        first = len(lines[:ends[0]]) - len(wins[0].split("\n")[:-1])
        self.assertEqual(gaps, 0)
        self.assertEqual(merged, lines[first:ends[-1]])
        self.assertEqual(snap[0], 0)
        self.assertEqual(snap[-1], 2)

    def test_gap_when_snapshots_do_not_overlap(self):
        lines = big().split("\n")[:-1]
        merged, _, gaps = qrmerge.merge([window(lines, 40, 2000), window(lines, 200, 2000)])
        self.assertEqual(gaps, 1)
        self.assertEqual(merged.count(qrmerge.GAP), 1)

    def test_dropped_line_in_one_snapshot_still_aligns(self):
        lines = big().split("\n")[:-1]
        a, b = window(lines, 100, 6000), window(lines, 130, 6000)
        b_lines = b.split("\n")[:-1]
        del b_lines[10]                      # the sanitiser dropped a digit-only-different repeat from this one
        merged, _, gaps = qrmerge.merge([a, "\n".join(b_lines) + "\n"])
        self.assertEqual(gaps, 0)
        self.assertEqual(merged[-1], lines[129])

    def test_identical_snapshot_adds_nothing(self):
        lines = big().split("\n")[:-1]
        w = window(lines, 80, 5000)
        self.assertEqual(qrmerge.merge([w, w])[0], w.split("\n")[:-1])


if __name__ == "__main__":
    unittest.main()
