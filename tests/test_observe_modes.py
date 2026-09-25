"""Opt-in GPU integration check; pass the build configuration directory.

Runs the tiny compute fixture, not X4 or OpenVR. Each mode gets a fresh process
and private capture directory. Never modifies the caller's environment.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


BUILD = Path(sys.argv.pop(1)).resolve()


class ObservationModes(unittest.TestCase):
    def test_modes(self):
        for memory, native, stack in [(False, False, False), (True, False, False),
                                      (True, False, True), (True, True, False)]:
            with self.subTest(memory=memory, native=native, stack=stack):
                with tempfile.TemporaryDirectory(prefix="x4vr-modes-") as directory:
                    env = os.environ.copy()
                    env.update(VK_ADD_LAYER_PATH=str(BUILD),
                               VK_INSTANCE_LAYERS="VK_LAYER_X4VR_observe",
                               X4VR_CAPTURE_DIR=directory,
                               X4VR_CAPTURE_MEMORY=str(int(memory)),
                               X4VR_CAPTURE_NATIVE_CAMERA=str(int(native)),
                               X4VR_CAPTURE_STACK=str(int(stack)),
                               X4VR_OPENVR_BOOTSTRAP="0")
                    result = subprocess.run([str(BUILD / "vulkan_smoke.exe")],
                                            env=env, capture_output=True,
                                            text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    paths = list(Path(directory).glob("process-*/events.jsonl"))
                    self.assertEqual(len(paths), 1)
                    events = [json.loads(line) for line in paths[0].read_text().splitlines()]
                    started = events[0]
                    self.assertEqual(started["memory_sampling"], memory)
                    self.assertEqual(started["native_camera"], native)
                    self.assertEqual(started["stack_trace"], stack)
                    self.assertFalse(started["openvr_bootstrap_requested"])
                    snapshots = [e for e in events if e["event"] == "uniform_snapshot"]
                    self.assertEqual(len(snapshots), int(memory))
                    if memory:
                        snapshot = snapshots[0]
                        self.assertEqual(snapshot["status"], "mapped_cpu_snapshot")
                        self.assertEqual(bool(snapshot["exe_return_rvas"]), stack)
                        self.assertEqual("native_camera" in snapshot, native)
                        if native:
                            self.assertEqual(snapshot["native_camera"]["status"],
                                             "unsupported_executable")
                        expected = struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0,
                                               0, 0, 1, 0, 4, 5, 6, 1)
                        self.assertEqual((paths[0].parent / snapshot["file"]).read_bytes(),
                                         expected)
                    self.assertTrue(any(e["event"] == "instance_destroyed" for e in events))


if __name__ == "__main__":
    unittest.main()
