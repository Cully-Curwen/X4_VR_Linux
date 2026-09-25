"""Read-only x64 minidump exception/module summary; no symbol downloads."""
import argparse
import json
from pathlib import Path
import struct


def inspect(path):
    data = Path(path).read_bytes()
    if data[:4] != b'MDMP':
        raise ValueError('Not a minidump')
    count, directory = struct.unpack_from('<II', data, 8)
    streams = {kind: (size, rva) for kind, size, rva in
               (struct.unpack_from('<III', data, directory+i*12) for i in range(count))}
    modules = []
    memory = []
    if 5 in streams:
        _, rva = streams[5]
        for i in range(struct.unpack_from('<I', data, rva)[0]):
            memory.append(struct.unpack_from('<QII', data, rva+4+i*16))
    def read(address, size):
        for start, length, offset in memory:
            if start <= address and address+size <= start+length:
                return data[offset+address-start:offset+address-start+size]
        return None
    if 4 in streams:
        _, rva = streams[4]
        for i in range(struct.unpack_from('<I', data, rva)[0]):
            offset = rva+4+i*108
            base, size, checksum, timestamp, name = struct.unpack_from('<QIIII', data, offset)
            length = struct.unpack_from('<I', data, name)[0]
            text = data[name+4:name+4+length].decode('utf-16le')
            modules.append(dict(base=base, size=size, path=text, timestamp=timestamp))
    def identify(address):
        for module in modules:
            if module['base'] <= address < module['base']+module['size']:
                return dict(module=module['path'], rva=hex(address-module['base']))
        return {}
    report = dict(dump=str(Path(path).resolve()), modules=modules)
    if 6 in streams:
        _, rva = streams[6]
        thread = struct.unpack_from('<I', data, rva)[0]
        code, flags, record, address, count = struct.unpack_from('<IIQQI', data, rva+8)
        parameters = list(struct.unpack_from('<'+str(min(count, 15))+'Q', data, rva+40))
        context_size, context_rva = struct.unpack_from('<II', data, rva+160)
        registers = {}
        if context_size >= 256:
            for i, name in enumerate(['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi','r8','r9','r10','r11','r12','r13','r14','r15','rip']):
                registers[name] = hex(struct.unpack_from('<Q', data, context_rva+120+i*8)[0])
        report['exception'] = dict(thread=thread, code=hex(code), flags=flags,
            address=hex(address), parameters=[hex(p) for p in parameters], registers=registers, **identify(address))
        # Expose only small local bytes for manual validation against disassembly.
        report['register_memory'] = {}
        for register, value in registers.items():
            if raw := read(int(value, 16), 32):
                report['register_memory'][register] = raw.hex(' ')
        if 'rsi' in registers and (raw := read(int(registers['rsi'], 16), 32)):
            length, capacity = struct.unpack_from('<QQ', raw, 16)
            if length <= capacity and length <= 4096:
                pointer = int(registers['rsi'], 16) if capacity < 16 else struct.unpack_from('<Q', raw)[0]
                value = read(pointer, length)
                report['rsi_string_candidate'] = dict(length=length, capacity=capacity,
                    address=hex(pointer), contents=value.decode('utf-8', errors='replace') if value else None)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = inspect(args.dump)
    if args.output:
        args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report.get('exception', {}), indent=2))


if __name__ == '__main__':
    main()
