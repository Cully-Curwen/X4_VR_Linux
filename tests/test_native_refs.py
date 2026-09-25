from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
from find_native_refs import instruction_target


class NativeReferenceTests(unittest.TestCase):
    def test_rip_relative(self):
        self.assertEqual(instruction_target(0x1000, 7, "lea", "rcx, [rip + 0x1234]"), 0x223b)
        self.assertEqual(instruction_target(0x1000, 7, "mov", "rax, qword ptr [rip - 0x20]"), 0xfe7)
        self.assertEqual(instruction_target(0x1000, 6, "call", "qword ptr [rip]"), 0x1006)

    def test_direct_and_unresolved(self):
        self.assertEqual(instruction_target(0x1000, 5, "call", "0xf41000"), 0xf41000)
        self.assertEqual(instruction_target(0x1000, 5, "jmp", "0x1200"), 0x1200)
        for mnemonic, operands in [("call", "rax"), ("jmp", "qword ptr [rcx + 0x20]"),
                                   ("mov", "eax, 0xf41000"), ("lea", "rax, [rcx + 0x20]")]:
            self.assertIsNone(instruction_target(0x1000, 5, mnemonic, operands))


if __name__ == "__main__":
    unittest.main()
