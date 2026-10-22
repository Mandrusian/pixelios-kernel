"""Unit tests for tools/pstore-pull.py against synthetic data only.

Never touches a phone: the `run` subcommand is exercised only through
its pure helpers (product parsing, adb-device-state parsing) and its
no-phone refusal path (missing image).
Run: python3 -m unittest discover -s tools -p "test_*.py"  (or from tools/:
python3 -m unittest test_pstore_pull)
"""
import importlib.util
import pathlib
import struct
import sys
import unittest
import zipfile

# The tool's filename is hyphenated (pstore-pull.py), so import it by path.
_MODULE_PATH = pathlib.Path(__file__).resolve().parent / "pstore-pull.py"
_spec = importlib.util.spec_from_file_location("pstore_pull", _MODULE_PATH)
assert _spec is not None and _spec.loader is not None
pstore_pull = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pstore_pull)

SIG = 0x43474244
HDR = struct.Struct("<III")

MARKER_LINE = b"PIXELIOS PSTORE v1 build=abcd1234\n"
LOG_BODY = (
    b"PSTORE ramoops node in live FDT\n"
    b"PSTORE zone/cap 00000000FD3FF000 0001FFF4\n"
    b"SECTION boot CPU\n"
    b"PROBE complete; fast reset for pstore\n"
    b"PSTORE saved bytes=33123 zone=00000000FD3FF000\n"
)


def make_buffer_blob(payload: bytes, start: int, size: int, cap: int) -> bytes:
    """A raw persistent_ram_buffer zone image: header + `cap` data bytes
    holding `payload` stored in wrapped order at ring position `start`."""
    assert len(payload) == size and start <= size <= cap
    data = bytearray(cap)
    first = size - start
    data[start:start + first] = payload[:first]
    data[:start] = payload[first:]
    return HDR.pack(SIG, start, size) + bytes(data)


class TestDecode(unittest.TestCase):
    def test_plain_text_passthrough(self):
        raw = b"[    0.000000] Booting Linux...\n" + MARKER_LINE + LOG_BODY
        self.assertEqual(pstore_pull.extract_log(raw), MARKER_LINE + LOG_BODY)

    def test_buffer_blob_nonwrapped(self):
        payload = MARKER_LINE + LOG_BODY
        raw = make_buffer_blob(payload, start=0, size=len(payload), cap=len(payload) + 64)
        self.assertEqual(pstore_pull.decode_console_ramoops(raw), payload)
        self.assertEqual(pstore_pull.extract_log(raw), payload)

    def test_buffer_blob_wrapped_ring(self):
        payload = MARKER_LINE + LOG_BODY
        # Kernel old-log order is data[start..size) then data[0..start).
        raw = make_buffer_blob(payload, start=40, size=len(payload), cap=len(payload) + 16)
        self.assertEqual(pstore_pull.decode_console_ramoops(raw), payload)
        self.assertEqual(pstore_pull.extract_log(raw), payload)

    def test_kernel_console_noise_before_marker(self):
        payload = MARKER_LINE + LOG_BODY
        raw = b"[   12.1] some android console line\n" * 5 + payload
        self.assertEqual(pstore_pull.extract_log(raw), payload)

    def test_no_marker_returns_empty(self):
        self.assertEqual(pstore_pull.extract_log(b"plain android console log\n"), b"")

    def test_bad_size_field_falls_back_to_plain(self):
        garbage = HDR.pack(SIG, 0, 10_000_000) + b"PIXELIOS PSTORE v1 x\nbody\n"
        # size > available data -> header rejected -> treated as text; the
        # marker sits behind the 12 garbage bytes but find() still locates it.
        self.assertEqual(pstore_pull.extract_log(garbage), b"PIXELIOS PSTORE v1 x\nbody\n")

    def test_wrong_sig_is_plain_text(self):
        raw = b"not a buffer at all\n" + MARKER_LINE
        self.assertEqual(pstore_pull.extract_log(raw), MARKER_LINE)


class TestTailKeep(unittest.TestCase):
    """D8: when the log outgrew the zone, the shim stores exactly `cap`
    bytes - the marker as the fixed first line plus the newest tail
    (size == capacity, start == 0, tail may begin mid-line)."""

    def test_truncated_zone_marker_survives(self):
        tail = b"ddle of an evicted line\nCPU MIDR id=0 value=414fd0b1\n" + LOG_BODY
        retained = MARKER_LINE + tail
        cap = len(retained)
        raw = HDR.pack(SIG, 0, cap) + retained
        self.assertEqual(pstore_pull.decode_console_ramoops(raw), retained)
        self.assertEqual(pstore_pull.extract_log(raw), retained)

    def test_truncated_zone_with_padding_after_data(self):
        retained = MARKER_LINE + b"artial line\n" + LOG_BODY
        raw = HDR.pack(SIG, 0, len(retained)) + retained + b"\xee" * 64
        self.assertEqual(pstore_pull.extract_log(raw), retained)

    def test_kernel_noise_before_truncated_zone(self):
        retained = MARKER_LINE + b"artial tail line\n" + LOG_BODY
        raw = b"[   12.1] android console text\n" * 3 + retained
        self.assertEqual(pstore_pull.extract_log(raw), retained)


class TestBugreport(unittest.TestCase):
    def test_finds_and_prefers_console_ramoops_0(self):
        import io
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            zpath = pathlib.Path(td) / "bugreport.zip"
            payload = MARKER_LINE + LOG_BODY
            with zipfile.ZipFile(zpath, "w") as zf:
                zf.writestr("bugreport-bluejay-2026.txt", "main report\n")
                zf.writestr("FS/sys/fs/pstore/console-ramoops", b"other console\n")
                zf.writestr("FS/sys/fs/pstore/console-ramoops-0", payload)
                zf.writestr("FS/sys/fs/pstore/dmesg-ramoops-0", b"====...")
            with zipfile.ZipFile(zpath) as zf:
                self.assertEqual(
                    pstore_pull.find_bugreport_pstore(zf), "FS/sys/fs/pstore/console-ramoops-0"
                )
                self.assertEqual(pstore_pull.extract_log(zf.read("FS/sys/fs/pstore/console-ramoops-0")), payload)

    def test_no_pstore_entry(self):
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            zpath = pathlib.Path(td) / "bugreport.zip"
            with zipfile.ZipFile(zpath, "w") as zf:
                zf.writestr("bugreport.txt", "nothing here\n")
            with zipfile.ZipFile(zpath) as zf:
                self.assertIsNone(pstore_pull.find_bugreport_pstore(zf))


class TestParsers(unittest.TestCase):
    def test_parse_product(self):
        out = "(bootloader) product: bluejay\n(bootloader) serial: X\nOKAY [0.05s]"
        self.assertEqual(pstore_pull.parse_product(out), "bluejay")
        self.assertIsNone(pstore_pull.parse_product("no vars here"))

    def test_adb_state(self):
        good = "List of devices attached\nABCD123\tdevice\n\n"
        unauth = "List of devices attached\nABCD123\tunauthorized\n\n"
        empty = "List of devices attached\n\n"
        self.assertEqual(pstore_pull.adb_state(good), "device")
        self.assertEqual(pstore_pull.adb_state(unauth), "unauthorized")
        self.assertIsNone(pstore_pull.adb_state(empty))


class TestRunGuards(unittest.TestCase):
    def test_run_refuses_missing_image_without_fastboot(self):
        from types import SimpleNamespace

        rc = pstore_pull.cmd_run(SimpleNamespace(image=pathlib.Path("/nonexistent/img.img")))
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
