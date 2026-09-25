"""Read known camera records from the pinned retail X4 process.

No injection, writes, thread suspension, debugger attach or stack walking. Two
equal reads reduce tearing risk but do not establish atomicity or camera identity.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import math
from pathlib import Path
import time

from analyze_uniforms import matrix, multiply, error

PINNED = "19750a6563889a970f434b5566eb396c6b2dc29ff814bd3e336f838176ad6891"
CAMERAS = (0x6D1C950, 0x6D1D660)
RECORD_SIZE = 0xD10


def render_links():
    """Fixed, disassembly-derived paths; never scan memory or walk containers.

    Each offset is dereferenced, except the final camera-record displacement.
    Retained view slots are NOT proof of membership in the current frame.
    """
    return [
        ("selected_object", (0x6D1BAE0, 0x10)),
        ("selected_camera", (0x6D1B920, 0)),
        ("context_copy", (0x6CF1908, 0x30, 0x350)),
        *[(f"view_slot_{i:02d}", (0x6D1E528 + i * 0x180, 0))
          for i in range(32)],
    ]


def resolve(read, base, offsets):
    address = base
    chain = []
    for offset in offsets[:-1]:
        source = address + offset
        value = int.from_bytes(read(source, 8), "little")
        # Conservative canonical user-mode range; NULL/uninitialized slots skip.
        if not 0x10000 <= value <= 0x7FFFFFFFFFFF - RECORD_SIZE:
            raise ValueError("null_or_invalid_pointer")
        chain.append((source, value))
        address = value
    return address + offsets[-1], chain


def capture_link(read, base, offsets, *, view_label=False):
    """Reject changed links, tolerate inaccessible/freed records. Not atomic."""
    try:
        address, before = resolve(read, base, offsets)
        first = read(address, RECORD_SIZE)
        second = read(address, RECORD_SIZE)
        label = {}
        if view_label:
            # Only camera-at-view+0x10 paths have the 64-byte label at view+0xf0c.
            # Labels are diagnostic hints, never authority to modify a camera.
            try:
                raw = read(address + 0xEFC, 64)
                prefix, separator, _ = raw.partition(b"\0")
                if separator and all(32 <= byte < 127 for byte in prefix):
                    label["view_label_hint"] = prefix.decode("ascii")
                else:
                    label["view_label_status"] = "not_terminated_printable_ascii"
            except OSError:
                label["view_label_status"] = "unavailable"
        after_address, after = resolve(read, base, offsets)
        metadata = dict(address=hex(address),
                        pointer_chain=[(hex(p), hex(v)) for p, v in before])
        if before != after or address != after_address:
            return dict(status="pointer_changed", **metadata), None
        return dict(repeated_read_equal=first == second, **metadata, **label,
                    **analyze(second)), second
    except (OSError, ValueError) as exc:
        return dict(status="unavailable", reason=str(exc)), None


def analyze(data):
    if len(data) != RECORD_SIZE:
        return {"status": "wrong_size"}
    inverse, view, projection, jitter = [matrix(data, o) for o in (0, 0x40, 0x140, 0x1C0)]
    if not all(math.isfinite(v) for m in (inverse, view, projection, jitter) for row in m for v in row):
        return {"status": "nonfinite"}
    identity = [[float(r == c) for c in range(4)] for r in range(4)]
    inverse_error = error(multiply(view, inverse), identity)
    # Float inverse pairs at large world coordinates suffer cancellation in the
    # translation column. Bound each product component by its own term magnitudes.
    consistent = all(
        abs(sum(view[r][k]*inverse[k][c] for k in range(4))-identity[r][c]) <=
        1e-5 + 8*2**-23*sum(abs(view[r][k]*inverse[k][c]) for k in range(4))
        for r in range(4) for c in range(4))
    output = multiply(jitter, projection)
    output[1] = [-v for v in output[1]]
    return dict(status="inverse_consistent" if consistent else "inverse_inconsistent",
                inverse_error=inverse_error, inverse_view=inverse, view=view,
                native_projection=projection, jitter=jitter, uniform_projection=output)


class Remote:
    def __init__(self, pid):
        if C.sizeof(C.c_void_p) != 8:
            raise RuntimeError("Use 64-bit Python")
        self.k = C.WinDLL("kernel32", use_last_error=True)
        self.k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
        self.k.OpenProcess.restype = W.HANDLE
        self.k.CloseHandle.argtypes = [W.HANDLE]
        self.k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p,
                                            C.c_size_t, C.POINTER(C.c_size_t)]
        self.k.ReadProcessMemory.restype = W.BOOL
        self.k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
        self.k.QueryFullProcessImageNameW.restype = W.BOOL
        self.k.K32EnumProcessModules.argtypes = [W.HANDLE, C.POINTER(W.HMODULE), W.DWORD, C.POINTER(W.DWORD)]
        self.k.K32EnumProcessModules.restype = W.BOOL
        self.handle = self.k.OpenProcess(0x0400 | 0x0010, False, pid)  # query + VM_READ only
        if not self.handle:
            raise C.WinError(C.get_last_error())

    def close(self):
        if self.handle:
            self.k.CloseHandle(self.handle)
            self.handle = None

    def read(self, address, size):
        if not 0 < size <= RECORD_SIZE:
            raise ValueError("Read exceeds camera bound")
        data = C.create_string_buffer(size)
        count = C.c_size_t()
        if not self.k.ReadProcessMemory(self.handle, address, data, size, C.byref(count)) or count.value != size:
            raise C.WinError(C.get_last_error())
        return data.raw

    def verify(self):
        filename = C.create_unicode_buffer(32768)
        length = W.DWORD(len(filename))
        if not self.k.QueryFullProcessImageNameW(self.handle, 0, filename, C.byref(length)):
            raise C.WinError(C.get_last_error())
        path = Path(filename.value)
        if path.name.lower() != "x4.exe" or hashlib.sha256(path.read_bytes()).hexdigest() != PINNED:
            raise RuntimeError("Unsupported executable; no camera reads attempted")
        modules = (W.HMODULE * 2048)()
        needed = W.DWORD()
        if not self.k.K32EnumProcessModules(self.handle, modules, C.sizeof(modules), C.byref(needed)):
            raise C.WinError(C.get_last_error())
        if not needed.value or needed.value > C.sizeof(modules):
            raise RuntimeError("Module enumeration incomplete")
        base = modules[0]
        if self.read(base, 2) != b"MZ" or self.read(base + 0x1218468, 9) != bytes.fromhex("41 c6 46 54 00 45 88 6e 55"):
            raise RuntimeError("Loaded-image signature differs")
        return path, base


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New private report directory")
    parser.add_argument("--samples", type=int, default=8)
    parser.add_argument("--render-links", action="store_true",
                        help="Also read selected/context cameras and 32 retained view slots")
    args = parser.parse_args()
    if not 1 <= args.samples <= 40:
        parser.error("samples must be 1..40 (at most ten seconds)")
    remote = Remote(args.pid)
    try:
        path, base = remote.verify()
        args.output.mkdir(parents=True, exist_ok=False)
        samples = []
        for i in range(args.samples):
            for rva in CAMERAS:
                first = remote.read(base + rva, RECORD_SIZE)
                second = remote.read(base + rva, RECORD_SIZE)
                name = f"camera-{rva:x}-{i}.bin"
                (args.output / name).write_bytes(second)
                sample = dict(index=i, rva=hex(rva), timestamp=time.time(),
                              repeated_read_equal=first == second, file=name, **analyze(second))
                samples.append(sample)
                print(f"{rva:x} {i}: {sample['status']}, repeated_read_equal={first == second}")
            if args.render_links:
                for label, offsets in render_links():
                    details, data = capture_link(remote.read, base, offsets,
                                                 view_label=label == "selected_object" or
                                                 label.startswith("view_slot_"))
                    sample = dict(index=i, source=label, timestamp=time.time(), **details)
                    if data is not None:
                        name = f"{label}-{i}.bin"
                        (args.output / name).write_bytes(data)
                        sample["file"] = name
                    samples.append(sample)
                    print(f"{label} {i}: {sample['status']}")
            if i + 1 < args.samples:
                time.sleep(0.25)
        report = dict(pid=args.pid, executable=str(path), sha256=PINNED, base=hex(base),
                      camera_identity="unverified", render_links=args.render_links,
                      retained_slots_may_be_stale=True, samples=samples)
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    finally:
        remote.close()


if __name__ == "__main__":
    main()
