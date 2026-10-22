#!/usr/bin/env python3
"""Offline camera simulator for the QR log channel: renders what the phone draws (the real C encoder through ctypes), views it
through a model of the ESP32-CAM (scale, rotation, tilt, defocus blur, noise, low contrast, JPEG) and decodes with the host decoder.

  tools/qrsim.py modules [--cam 720|1200] [--quality good|typical|poor] [--tiles N] ...   decode rate vs QR version / px per module
  tools/qrsim.py deliver [--p P] [--burst B] ...                                          frames needed / time to deliver N bytes (schedule model)
  tools/qrsim.py --selftest                                                                  one tiny end-to-end check
Needs opencv + numpy (PYTHONPATH=~/pixelios-artifacts/pylib) and gcc."""
import argparse, ctypes, os, random, subprocess, sys, tempfile, threading, time

LOCK = threading.Lock()      # the C encoder keeps static QR buffers

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

W, H = 1080, 2400           # Pixel 6a panel
QUALITY = {                 # blur sigma (camera px), noise sigma (gray levels), JPEG quality, contrast (white-black gray span)
    "good": (0.6, 2.0, 90, 170),
    "typical": (0.9, 4.0, 85, 140),
    "poor": (1.3, 6.0, 75, 110),
}
CAMS = {720: (1280, 720), 1200: (1600, 1200)}


_LIBS = {}


def build_lib(tx_size=None):
    """The phone's encoder as a shared library (qrlog/qrstream.c with qrz, qrlog, qrcodegen); tx_size = a smaller transport ring for tests."""
    if tx_size in _LIBS:
        return _LIBS[tx_size]
    d = os.path.join(tempfile.gettempdir(), "qrsim-lib")
    os.makedirs(d, exist_ok=True)
    srcs = [os.path.join(ROOT, "qrlog", f) for f in ("qrstream.c", "qrz.c", "qrlog.c", "qrcodegen.c", "qrstream.h", "qrz.h", "qrz_tables.h")]
    stamp = int(max(os.path.getmtime(s) for s in srcs))
    so = os.path.join(d, "libqrstream-%d-%s-%d.so" % (stamp, tx_size or 0, os.getuid()))
    if not os.path.exists(so):
        subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-I", os.path.join(ROOT, "qrlog"), "-o", so + ".tmp"]
                              + (["-DQS_TX_SIZE=%d" % tx_size] if tx_size else []) + [s for s in srcs if s.endswith(".c")])
        os.replace(so + ".tmp", so)
    lib = _LIBS[tx_size] = ctypes.CDLL(so)
    lib.qs_init.restype = lib.qs_next_frame.restype = lib.qs_draw.restype = lib.qs_sizeof.restype = ctypes.c_uint
    lib.qs_hi.restype = lib.qs_lo.restype = lib.qs_reduced.restype = lib.qs_stream_len.restype = ctypes.c_uint64
    lib.qs_reduced.argtypes = lib.qs_stream_len.argtypes = [ctypes.c_void_p]
    lib.qs_feed.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_uint]
    lib.qs_next_frame.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.qs_flush.argtypes = [ctypes.c_void_p]
    lib.qs_set_fec.argtypes = lib.qs_set_red.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    lib.qs_hi.argtypes = lib.qs_lo.argtypes = [ctypes.c_void_p]
    lib.qrlog_nframes.restype = lib.qrlog_make_frame.restype = lib.qrlog_draw.restype = ctypes.c_uint
    lib.qrlog_nframes.argtypes = [ctypes.c_uint, ctypes.c_uint]
    lib.qrlog_make_frame.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_char_p]
    lib.qrlog_draw.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_char_p]
    lib.qs_init.argtypes = [ctypes.c_void_p] + [ctypes.c_uint] * 6
    lib.qs_draw.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
                            ctypes.POINTER(ctypes.c_char_p), ctypes.c_uint]
    return lib


class Encoder:
    """The phone's QL2 encoder (qrlog/qrstream.c) on a log."""

    def __init__(self, log=b"", ver=10, ecc=0, tiles=1, unit=8192, klog=6, sid=1, fec=True, tx_size=None, red=None):
        self.lib = build_lib(tx_size)
        self.buf = ctypes.create_string_buffer(self.lib.qs_sizeof())
        self.fed = 0
        with LOCK:
            self.B = self.lib.qs_init(self.buf, ver, ecc, tiles, unit, klog, sid)
            assert self.B, "qs_init rejected the configuration"
            self.tiles = tiles
            self.lib.qs_set_fec(self.buf, int(fec))
            if red is not None:
                self.lib.qs_set_red(self.buf, red)
        if log:
            self.feed(log)
            self.flush()

    def feed(self, data):
        with LOCK:
            self.lib.qs_feed(self.buf, self.fed, data, len(data))
        self.fed += len(data)

    def reduced(self):
        return self.lib.qs_reduced(self.buf)

    def stream_len(self):
        return self.lib.qs_stream_len(self.buf)

    def flush(self):
        with LOCK:
            self.lib.qs_flush(self.buf)

    def frame(self):
        t = ctypes.create_string_buffer(4300)
        with LOCK:
            n = self.lib.qs_next_frame(self.buf, t)
        return t.value.decode() if n else None

    def screen(self, texts):
        """gray H x W array of the panel showing these frames."""
        import numpy as np
        fb = (ctypes.c_uint32 * (W * H))()
        arr = (ctypes.c_char_p * len(texts))(*[t.encode() for t in texts])
        with LOCK:
            sz = self.lib.qs_draw(self.buf, fb, W, H, W * 4, arr, len(texts))
        assert sz, "draw failed"
        return (np.frombuffer(fb, dtype=np.uint32) & 255).astype(np.uint8).reshape(H, W), sz


def camera(cv2, np, screen, cam_w, cam_h, scale, rot=3.0, tilt=0.01, quality="typical", landscape=False, rng=random, noise_rng=None):
    """What the camera records: screen (gray, H x W) -> gray JPEG-decoded image of cam_w x cam_h."""
    blur, noise, jq, span = QUALITY[quality] if isinstance(quality, str) else quality
    s = cv2.resize(screen, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA).astype(np.float32)   # sensor pixel-area integration
    h, w = s.shape
    if landscape:
        s = np.rot90(s)
        h, w = s.shape
    ang = rot + rng.uniform(-0.5, 0.5)
    c = (cam_w / 2 + rng.uniform(-8, 8), cam_h / 2 + rng.uniform(-8, 8))
    src = np.float32([[0, 0], [w, 0], [w, h], [0, h]])
    k = tilt * min(w, h)
    dst = src + np.float32([[rng.uniform(-k, k), rng.uniform(-k, k)] for _ in range(4)])
    M = cv2.getRotationMatrix2D((w / 2, h / 2), ang, 1.0)
    dst = (np.hstack([dst, np.ones((4, 1))]) @ M.T) + np.float32([c[0] - w / 2, c[1] - h / 2])
    P = cv2.getPerspectiveTransform(src, dst.astype(np.float32))
    body = np.full_like(s, 255)
    bg = 20.0
    img = cv2.warpPerspective(s, P, (cam_w, cam_h), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=bg)
    mask = cv2.warpPerspective(body, P, (cam_w, cam_h), flags=cv2.INTER_LINEAR, borderValue=0) / 255.0
    lo = 35 + rng.uniform(-5, 5)
    img = bg + (lo + img / 255.0 * span - bg) * mask               # panel black ~ lo, white ~ lo + span; dark surround outside
    yy, xx = np.mgrid[0:cam_h, 0:cam_w]
    img = img * (1 - 0.18 * (((xx - cam_w / 2) / cam_w) ** 2 + ((yy - cam_h / 2) / cam_h) ** 2) * 2)   # vignette
    if blur > 0:
        img = cv2.GaussianBlur(img, (0, 0), blur)
    img = img + np.random.default_rng((noise_rng or rng).randrange(1 << 30)).normal(0, noise, img.shape)
    img = np.clip(img, 0, 255).astype(np.uint8)
    ok, jpg = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, jq])
    return cv2.imdecode(jpg, cv2.IMREAD_GRAYSCALE), len(jpg)


def module_px(sz, tiles=1):
    """Screen pixels per module the phone's drawing code picks (qs_draw / qrlog_draw: 48 px margin, 4-module quiet zone)."""
    return min(W - 96, (H - 96) // tiles) // (sz + 8)


SAMPLE = b""


def ql1_screen(lib, text, chunk, idx):
    """QL1 baseline: the old encoder and drawing code."""
    import numpy as np
    n = lib.qrlog_nframes(len(text), chunk)
    out = ctypes.create_string_buffer(chunk + 64 + 1)
    fb = (ctypes.c_uint32 * (W * H))()
    with LOCK:
        lib.qrlog_make_frame(text, len(text), chunk, idx % n, out)
        sz = lib.qrlog_draw(fb, W, H, W * 4, out)
    return (np.frombuffer(fb, dtype=np.uint32) & 255).astype(np.uint8).reshape(H, W), sz, out.value.decode("latin-1")


# ---------------------------------------------------------------- measurements

def sample_log():
    out = b""
    for n in ("qr-a.txt", "qr-b.txt", "qr-c.txt"):
        with open(os.path.join(HERE, "testdata", n), "rb") as f:
            out += f.read()
    return out


def try_frames(cv2, np, enc, dets, scale, cam, quality, tiles, trials, seed=0, fast=False, landscape=False, method="roi", average=1, proto="ql2", chunk=200):
    """Share of displayed frames decoded back exactly, over `trials` screens.
    method roi = qrcam.decode_all (find, cut out, magnify, sharpen); whole = the old whole-image decode. average = N noise realisations
    of the same view averaged first (several camera frames of one displayed frame). proto ql1 = the old text frames (chunk bytes)."""
    import qrl2, qrcam
    ok = tot = 0
    sz = 0
    for i in range(trials):
        if proto == "ql1":
            scr, sz, t = ql1_screen(enc, SAMPLE, chunk, i)
            texts = [t]
            accept = lambda x, t=t: x == t
        else:
            texts = [enc.frame() for _ in range(tiles)]
            scr, sz = enc.screen(texts)
            accept = lambda x: qrl2.parse_frame(x) is not None
        imgs = [camera(cv2, np, scr, cam[0], cam[1], scale, quality=quality, rng=random.Random(seed * 1000 + i), landscape=landscape,
                       noise_rng=random.Random(seed * 77000 + i * 10 + k))[0] for k in range(average)]
        img = imgs[0] if average == 1 else np.mean([x.astype(np.float32) for x in imgs], axis=0).astype(np.uint8)
        if method == "whole":
            got = qrcam.decode_whole(cv2, img, dets, accept=accept) if len(texts) == 1 else qrcam.decode_all(cv2, img, dets, want=len(texts), accept=accept, fast=False)
        else:
            got = qrcam.decode_all(cv2, img, dets, want=len(texts), accept=accept, fast=fast)
        ok += sum(t in got for t in texts)
        tot += len(texts)
    return ok / tot, sz


def cmd_modules(a):
    import cv2, numpy as np, qrcam
    from multiprocessing.pool import ThreadPool
    dets = qrcam.detectors(cv2)
    log = sample_log()
    cam = CAMS[a.cam]
    global SAMPLE
    SAMPLE = log[:12000]
    jobs = []
    for q in a.quality:
        for tiles in a.tiles:
            for ecc in a.ecc:
                for ver in a.ver:
                    for s in a.scale:
                        jobs.append((q, tiles, ecc, ver, s))

    def one(j):
        q, tiles, ecc, ver, s = j
        if a.proto == "ql1":
            enc = build_lib()
            rate, sz = try_frames(cv2, np, enc, dets, s, cam, q, 1, a.trials, fast=a.fast, landscape=a.landscape, method=a.decode, average=a.average, proto="ql1", chunk=ver)
            return (q, 1, "M", "chunk%d" % ver, sz, ver, s, module_px(sz, 1) * s, rate)
        enc = Encoder(log, ver=ver, ecc=ecc, tiles=tiles, klog=4)
        rate, sz = try_frames(cv2, np, enc, dets, s, cam, q, tiles, a.trials, fast=a.fast, landscape=a.landscape, method=a.decode, average=a.average)
        mp = module_px(sz, tiles) * s
        return (q, tiles, "LMQH"[ecc], "v%d" % ver, sz, enc.B, s, mp, rate)
    print("quality tiles ecc ver modules B/frame scale cam_px/module decoded%")
    with ThreadPool(a.threads) as pool:
        for r in pool.imap(one, jobs):
            print("%-7s %d %s %-8s %3d modules %4d B/frame  s=%.2f  %.2f px/module  %3.0f%%" % (r[:8] + (r[8] * 100,)), flush=True)


def synth_log(nbytes, seed=1):
    """A log of nbytes: the real sample lines cycled with random digits (same shapes, no exact repeats)."""
    import re
    rng = random.Random(seed)
    lines = sample_log().split(b"\n")
    out, n, i = [], 0, 0
    while n < nbytes:
        ln = lines[i % len(lines)]
        i += 1
        if rng.random() < 0.7:
            ln = re.sub(rb"[0-9a-f]{4,}", lambda m: bytes(rng.choice(b"0123456789abcdef") for _ in m.group()), ln)
        out.append(ln)
        n += len(ln) + 1
    return b"\n".join(out)[:nbytes] + b"\n"


def ql1_time(p, n, ms, loops_cap=100000, rng=random):
    """Static log: seconds until all n frames of one QL1 snapshot were read, frames looping every n*ms, each read with prob p."""
    got, t = set(), 0
    while len(got) < n and t < loops_cap * n:
        for i in range(n):
            t += 1
            if rng.random() < p:
                got.add(i)
        if len(got) == n:
            break
    return t * ms / 1000.0


def deliver_run(log, ver, ecc, tiles, unit, klog, fec, p, burst, ms, targets, max_ticks, seed, growth=0.0):
    """Seconds until the receiver holds >= each target of reduced log bytes ('all' = everything the phone produced); the real encoder +
    receiver, frames lost at random (p = read probability per frame, burst = chance a whole tick is blanked, e.g. by an iOS redraw).
    growth > 0: the log arrives at that many bytes/s instead of being there from the start (flushed every ~3 s like the kernel thread)."""
    import qrl2
    rng = random.Random(seed)
    enc = Encoder(log if not growth else b"", ver=ver, ecc=ecc, tiles=tiles, unit=unit, klog=klog, fec=fec)
    rec = qrl2.Receiver()
    res, blank, fed, ended = {}, False, 0, None
    for tick in range(1, max_ticks + 1):
        if growth and fed < len(log):
            n = min(len(log) - fed, max(1, int(growth * ms / 1000)))
            enc.feed(log[fed:fed + n])
            fed += n
            if fed == len(log) or tick % max(1, 3000 // ms) == 0:
                enc.flush()
            ended = tick * ms / 1000.0 if fed == len(log) else None
        texts = [enc.frame() for _ in range(tiles)]
        blank = (blank and rng.random() < 0.5) or rng.random() < burst          # bursty redraw loss: a blanked tick tends to repeat
        for t in texts:
            if t and not blank and rng.random() < p:
                rec.feed(t)
        if tick % 4 == 0 and rec.streams:
            c = rec.coverage(rec.sids()[0])
            total = enc.reduced()
            for tg in targets:
                need = total if tg == "all" else tg
                if tg not in res and c["bytes"] >= need and (tg != "all" or (c["complete"] and (not growth or fed == len(log)))):
                    res[tg] = tick * ms / 1000.0
            if len(res) == len(targets):
                break
    return res, enc.B, enc.reduced(), enc.stream_len(), ended


def real_log(nbytes):
    """The phone logs we have (tools/testdata plus any in logs/runs or /tmp/w/corpus), cycled up to nbytes: realistic compressibility
    (optimistic: the shipped dictionary was trained on part of it)."""
    import glob
    files = sorted(glob.glob(os.path.join(HERE, "testdata", "qr-*.txt"))) + sorted(glob.glob(os.path.join(ROOT, "logs", "runs", "*", "merged.log"))) \
        + sorted(glob.glob("/tmp/w/corpus/run*.txt"))
    out, n = [], 0
    while n < nbytes:
        for f in files:
            with open(f, "rb") as fh:
                d = fh.read()
            out.append(d)
            n += len(d)
    return b"".join(out)[:nbytes]


def cmd_deliver(a):
    print("p = chance a displayed frame is read, burst = chance a whole tick is lost (iOS redraw), tick %d ms, %d tile(s), v%d ecc %s, unit %d, K %d" %
          (a.ms, a.tiles, a.ver, "LMQH"[a.ecc], a.unit, 1 << a.klog))
    ql1_n = 12000 // 200 + 1
    for name, log in (("synthetic (random hex digits: pessimistic)", synth_log(max(a.sizes))),
                      ("real phone logs (in-sample dictionary: optimistic)", real_log(max(a.sizes)))):
        print("\nlog: %s" % name)
        for size in a.sizes:
            sub = log[:size]
            for p in a.p:
                ts = [ql1_time(p, ql1_n, a.ms, rng=random.Random(i)) for i in range(20)]
                line = "  %8d B  p=%.2f burst=%.2f  QL1 12 kB tail (idle log only): %4.0fs |" % (size, p, a.burst, sum(ts) / len(ts))
                for fec in (True, False):
                    r, B, red, st, _ = deliver_run(sub, a.ver, a.ecc, a.tiles, a.unit, a.klog, fec, p, a.burst, a.ms, ["all"], a.max_ticks, 1)
                    line += " QL2 %s %s |" % ("fountain" if fec else "rotation", ("%5.0fs" % r["all"]) if "all" in r else ">%ds" % (a.max_ticks * a.ms // 1000))
                print(line + "  (reduced %d B -> stream %d B = %.1fx)" % (red, st, red / max(1, st)), flush=True)
    if a.growth:
        print("\ngrowing log: produced at the given rate for 120 s; seconds from the end of the log until the receiver has everything")
        for g in a.growth:
            sub = real_log(int(g * 120))
            for p in a.p:
                r, B, red, st, ended = deliver_run(sub, a.ver, a.ecc, a.tiles, a.unit, a.klog, True, p, a.burst, a.ms, ["all"], a.max_ticks, 1, growth=g)
                print("  %5.0f B/s (%d B, stream %d B)  p=%.2f: complete %s after the log ended" %
                      (g, red, st, p, ("%.0fs" % (r["all"] - (ended or 0))) if "all" in r else "NOT within %ds" % (a.max_ticks * a.ms // 1000)), flush=True)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    m = sub.add_parser("modules")
    m.add_argument("--cam", type=int, default=720, choices=sorted(CAMS))
    m.add_argument("--quality", nargs="+", default=["typical"], choices=sorted(QUALITY))
    m.add_argument("--tiles", nargs="+", type=int, default=[1])
    m.add_argument("--ecc", nargs="+", type=int, default=[0])
    m.add_argument("--ver", nargs="+", type=int, default=[10])
    m.add_argument("--scale", nargs="+", type=float, default=[0.27])
    m.add_argument("--trials", type=int, default=8)
    m.add_argument("--threads", type=int, default=3)
    m.add_argument("--fast", action="store_true", help="ROI route only (skip the whole-image fallback)")
    m.add_argument("--decode", choices=["roi", "whole"], default="roi", help="roi: find, cut out, magnify, sharpen (qrcam.decode_all); whole: the old whole-image decode")
    m.add_argument("--average", type=int, default=1, help="average this many camera frames of the same screen before decoding")
    m.add_argument("--proto", choices=["ql2", "ql1"], default="ql2", help="ql1: the old text frames; --ver then means the chunk size in bytes")
    m.add_argument("--landscape", action="store_true", help="phone lying on its side (long edge along the camera width)")
    d = sub.add_parser("deliver")
    d.add_argument("--p", nargs="+", type=float, default=[0.9, 0.7, 0.5])
    d.add_argument("--burst", type=float, default=0.0)
    d.add_argument("--ms", type=int, default=800)
    d.add_argument("--tiles", type=int, default=2)
    d.add_argument("--ver", type=int, default=10)
    d.add_argument("--ecc", type=int, default=0)
    d.add_argument("--unit", type=int, default=8192)
    d.add_argument("--klog", type=int, default=6)
    d.add_argument("--sizes", nargs="+", type=int, default=[51200, 204800])
    d.add_argument("--growth", nargs="+", type=float, default=[])
    d.add_argument("--max-ticks", type=int, default=20000)
    a = ap.parse_args()
    if a.cmd == "modules":
        cmd_modules(a)
    elif a.cmd == "deliver":
        cmd_deliver(a)
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
