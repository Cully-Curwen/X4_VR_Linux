"""Read-only disassembly of explicitly selected X4 RVA ranges (never patch/load)."""
import argparse
from pathlib import Path
import sys
from inspect_x4 import PE

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'external/python'))
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP


def annotations(pe, instruction):
    result = []
    for operand in instruction.operands:
        if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
            continue
        rva = instruction.address+instruction.size+operand.mem.disp
        result.append(f'ref RVA {rva:#x}')
        try:
            offset = pe.offset(rva)
            raw = pe.data[offset:offset+256].split(b'\0', 1)[0]
            if len(raw) >= 4 and all(32 <= b < 127 for b in raw):
                result.append(repr(raw.decode('ascii')))
        except ValueError:
            pass
    return ' ; ' + ', '.join(result) if result else ''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path)
    parser.add_argument('begin', type=lambda s: int(s, 0))
    parser.add_argument('end', type=lambda s: int(s, 0))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--annotate', action='store_true', help='Annotate RIP-relative RVAs and printable strings')
    args = parser.parse_args()
    if not 0 < args.end-args.begin <= 65536:
        parser.error('Select a positive range no larger than 64 KiB')
    pe = PE(args.exe)
    start = pe.offset(args.begin)
    end = pe.offset(args.end-1)+1
    if end-start != args.end-args.begin:
        parser.error('Range crosses non-contiguous file sections')
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = args.annotate
    text = '\n'.join(f'{i.address:08x}  {i.mnemonic:9s} {i.op_str}' +
        (annotations(pe, i) if args.annotate else '') for i in decoder.disasm(pe.data[start:end], args.begin))+'\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
