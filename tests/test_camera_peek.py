from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
from peek_camera import analyze, capture_link, render_links, RECORD_SIZE


def store(data, offset, matrix):
    struct.pack_into("<16f", data, offset, *(matrix[r][c] for c in range(4) for r in range(4)))


class CameraPeekTests(unittest.TestCase):
    def test_link_bounds(self):
        links = render_links()
        self.assertEqual(len(links), 35)
        self.assertEqual(links[3], ("view_slot_00", (0x6D1E528, 0)))
        self.assertEqual(links[-1], ("view_slot_31", (0x6D213A8, 0)))

    def test_link_capture(self):
        data = bytearray(RECORD_SIZE)
        identity = [[float(r == c) for c in range(4)] for r in range(4)]
        store(data, 0, identity)
        store(data, 0x40, identity)
        memory = {0x10100: (0x20000).to_bytes(8, "little"),
                  0x20030: (0x30000).to_bytes(8, "little"), 0x30350: bytes(data)}
        def read(address, size):
            result = memory[address]
            self.assertEqual(len(result), size)
            return result
        details, captured = capture_link(read, 0x10000, (0x100, 0x30, 0x350))
        self.assertEqual(details["status"], "inverse_consistent")
        self.assertEqual(details["address"], "0x30350")
        self.assertTrue(details["repeated_read_equal"])
        self.assertEqual(captured, bytes(data))
    def test_changed_and_unavailable_links(self):
        count = 0
        def changing(address, size):
            nonlocal count
            if size == 8:
                count += 1
                return (0x20000 if count == 1 else 0x30000).to_bytes(8, "little")
            return bytes(size)
        details, data = capture_link(changing, 0x10000, (0x100, 0))
        self.assertEqual(details["status"], "pointer_changed")
        self.assertIsNone(data)
        for pointer in (0, 1, 0xFFFFFFFFFFFFFFFF):
            details, data = capture_link(lambda a, n: pointer.to_bytes(8, "little"),
                                         0x10000, (0x100, 0))
            self.assertEqual(details["status"], "unavailable")
            self.assertIsNone(data)
        def inaccessible(address, size):
            raise OSError("unmapped")
        self.assertEqual(capture_link(inaccessible, 0x10000, (0x100, 0))[0]["status"],
                         "unavailable")

    def test_view_label_hint(self):
        def read(address, size):
            if size == 8:
                return (0x20000).to_bytes(8, "little")
            if size == 64:
                self.assertEqual(address, 0x20EFC)
                return b"U::OverlayCamera\0".ljust(64, b"\0")
            return bytes(size)
        details, _ = capture_link(read, 0x10000, (0x100, 0), view_label=True)
        self.assertEqual(details["view_label_hint"], "U::OverlayCamera")
        details, _ = capture_link(read, 0x10000, (0x100, 0))
        self.assertNotIn("view_label_hint", details)
        def missing_label(address, size):
            if size == 64:
                raise OSError("unmapped")
            return read(address, size)
        details, data = capture_link(missing_label, 0x10000, (0x100, 0), view_label=True)
        self.assertEqual(details["view_label_status"], "unavailable")
        self.assertIsNotNone(data)
        def invalid_label(address, size):
            return b"\xff" * 64 if size == 64 else read(address, size)
        details, _ = capture_link(invalid_label, 0x10000, (0x100, 0), view_label=True)
        self.assertNotIn("view_label_hint", details)

    def test_invalid(self):
        self.assertEqual(analyze(b"")["status"], "wrong_size")
        self.assertEqual(analyze(bytes(RECORD_SIZE))["status"], "inverse_inconsistent")
        data = bytearray(RECORD_SIZE)
        struct.pack_into("<f", data, 0, float("nan"))
        self.assertEqual(analyze(data)["status"], "nonfinite")

    def test_large_translation_rounding(self):
        data = bytearray(RECORD_SIZE)
        # Rational orthonormal rotation, rounded independently into float fields.
        inverse = [[0.6, 0, 0.8, 31451.443], [0, 1, 0, 266.8],
                   [-0.8, 0, 0.6, -7225.297], [0, 0, 0, 1]]
        view = [[inverse[c][r] for c in range(3)] + [0] for r in range(3)] + [[0, 0, 0, 1]]
        for r in range(3):
            view[r][3] = -sum(view[r][k]*inverse[k][3] for k in range(3))
        store(data, 0, inverse); store(data, 0x40, view)
        self.assertEqual(analyze(data)["status"], "inverse_consistent")
        view[0][3] += 1
        store(data, 0x40, view)
        self.assertEqual(analyze(data)["status"], "inverse_inconsistent")


if __name__ == "__main__":
    unittest.main()
