#!/usr/bin/env python3
"""tools/acpm-verify on a SYNTHETIC usb.txt (built from the doc 23 layout and the shim/touchprobe.c print format; NOT an observed dump)."""
import os, subprocess, sys, tempfile, unittest

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "acpm-verify")
P = "SWEET TOUCH: ACPM "


def fixture(mut=None):
    """SYNTHETIC: descriptor of channel 2: ap_max 15, table +0x9210, stride 0x48, q_len 5, 16-byte elements."""
    d = [2, 4, 8, 1, 0x382e8, 0x382ec, 0x382f0, 0, 0, 0, 0x38288, 0x3828c, 0x38290, 5, 16, 2, 0xa7600000, 1]
    init = [0x8db8, 8, 0x9210, 0x26, 0, 8, 15, 0]
    slots = [[0x1cb, 0, 0, 0], [0x1cb, 1, 0, 0], [0x1c9, 0xb01, 0, 0], [0x1ca, 0, 0, 0], [0x1ca, 0x1401, 0, 0]]
    q = {"rr": 3, "rf": 3}
    if mut:
        mut(d, init, slots, q)
    h = lambda ws: " ".join("%08x" % w for w in ws)
    L = [P + "cfg mbox=0x17610000 sram=0x2039000 init=+0xa000", P + "cfg ch=2 speedy=0 pmic=1 stride=0x48",
         P + "init[0..3]=" + h(init[:4]), P + "init[4..7]=" + h(init[4:]), P + "table n=15 tab=+0x9210 idx=2",
         P + "desc[0..5]=" + h(d[:6]), P + "desc[6..11]=" + h(d[6:12]), P + "desc[12..17]=" + h(d[12:]),
         P + "q len=%d elem=%d words=4 bytes=16 ap_poll=1 INTSR0=0" % (d[13], d[14]),
         P + "req(AP->APM) rear@0x20712e8=%d front@0x20712ec=%d" % (q["rr"], q["rf"]), P + "req base@0x20712f0",
         P + "rsp(APM->AP) rear@0x2071288=3 front@0x207128c=3", P + "rsp base@0x2071290"]
    L += [P + "req buf+%03x %s" % (16 * k, h(s)) for k, s in enumerate(slots)]
    return "\n".join(L) + "\n"


def setd(i, v):
    return lambda d, *_: d.__setitem__(i, v)


class T(unittest.TestCase):
    def run_tool(self, text, *args):
        with tempfile.TemporaryDirectory() as d:
            if text is not None:
                with open(d + "/usb.txt", "w") as f:
                    f.write(text)
            return subprocess.run([sys.executable, TOOL, "--from-dir", d, *args], capture_output=True, text=True)

    def test_pass(self):
        r = self.run_tool(fixture())
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("elem_unit_verified=1", r.stdout)
        self.assertIn("internal consistency", r.stdout)

    def test_fail_cases(self):
        for name, mut, why in [
                ("elem words", setd(14, 4), "BYTES"), ("type", setd(3, 2), "TYPE_QUEUE"), ("id", setd(0, 3), "desc id"),
                ("qlen", setd(13, 2), "q_len"), ("unaligned", setd(4, 0x382e9), "unaligned"),
                ("overlap", setd(4, 0x382f4), "overlaps"), ("outside", setd(6, 0x3ffff0), "outside SRAM"),
                ("busy", lambda d, i, s, q: q.__setitem__("rf", 4), "not empty"),
                ("index", lambda d, i, s, q: q.update(rr=9, rf=9), ">= q_len"),
                ("apmax", lambda d, i, s, q: i.__setitem__(6, 33), "ipc_ap_max")]:
            r = self.run_tool(fixture(mut))
            self.assertEqual(r.returncode, 1, name + r.stdout)
            self.assertIn(why, r.stdout, name)

    def test_slots_not_16b_commands_keeps_unit_unverified(self):
        r = self.run_tool(fixture(lambda d, i, s, q: [w.__setitem__(0, 0x1cb ^ 0x300) for w in s]))
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("elem_unit_verified=1", r.stdout)

    def test_error_cases(self):
        self.assertEqual(self.run_tool(None).returncode, 2)
        self.assertEqual(self.run_tool("nothing here\n").returncode, 2)


if __name__ == "__main__":
    unittest.main()
