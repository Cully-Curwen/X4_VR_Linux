"""Read-only RIP-relative/direct-call reference search within PE unwind ranges.

Unwind ranges can be function fragments. Results are static references, not a
validated call graph or hook ABI. No executable code is loaded or run.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"external/python"))
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from inspect_x4 import PE
from analyze_uniforms import runtime_functions

RIP = re.compile(r"\brip ([+-]) (0x[0-9a-f]+)\b")


def instruction_target(address, size, mnemonic, operands):
    if "[rip]" in operands:
        return address + size
    if match := RIP.search(operands):
        displacement = int(match[2], 16) * (1 if match[1] == "+" else -1)
        return address + size + displacement
    if mnemonic in ("call", "jmp") and re.fullmatch(r"0x[0-9a-f]+", operands):
        return int(operands, 16)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("--range", nargs=2, type=lambda s: int(s, 0), action="append", required=True,
                        help="Half-open target RVA range; repeat for multiple ranges")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if any(not 0 <= low < high <= 2**32 for low, high in args.range):
        parser.error("Invalid RVA range")
    pe = PE(args.exe)
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    refs, skipped = [], []
    functions = sorted(set((begin, end) for begin, end, _ in runtime_functions(pe)))
    for begin, end in functions:
        try:
            start, stop = pe.offset(begin), pe.offset(end-1)+1
            if stop-start != end-begin:
                skipped.append(hex(begin)); continue
        except ValueError:
            skipped.append(hex(begin)); continue
        decoded_end = begin
        for address, size, mnemonic, operands in decoder.disasm_lite(pe.data[start:stop], begin):
            decoded_end = address+size
            target = instruction_target(address, size, mnemonic, operands)
            if target is not None and any(low <= target < high for low, high in args.range):
                refs.append(dict(rva=hex(address), target=hex(target), instruction=f"{mnemonic} {operands}",
                                 unwind_begin=hex(begin), unwind_end=hex(end)))
        if decoded_end < end:
            skipped.append(hex(decoded_end))
    report = dict(sha256=hashlib.sha256(pe.data).hexdigest(), ranges=args.range,
                  references=refs, incompletely_decoded=skipped,
                  limitations="Only unwind-described code ranges; indirect calls and computed addresses are not resolved.")
    args.output.write_text(json.dumps(report, indent=2)+"\n")
    print(f"{len(refs)} references; {len(skipped)} incomplete ranges; {args.output}")


if __name__ == "__main__":
    main()
