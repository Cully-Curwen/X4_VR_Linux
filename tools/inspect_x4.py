"""Read-only PE inspection. Does not load, execute or patch X4.exe."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


class PE:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if self.data[:2] != b'MZ':
            raise ValueError('Not a PE image')
        pe = self.u32(0x3c)
        if self.data[pe:pe+4] != b'PE\0\0':
            raise ValueError('Invalid PE signature')
        coff = pe + 4
        self.machine, sections = struct.unpack_from('<HH', self.data, coff)
        optional_size = self.u16(coff + 16)
        optional = coff + 20
        if self.u16(optional) != 0x20b:
            raise ValueError('Expected PE32+ x64')
        self.directories = optional + 112
        self.sections = []
        for i in range(sections):
            section = optional + optional_size + i*40
            vsize, rva, rawsize, raw = struct.unpack_from('<IIII', self.data, section+8)
            self.sections.append((rva, vsize, rawsize, raw))

    def u16(self, offset):
        return struct.unpack_from('<H', self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def offset(self, rva):
        for start, vsize, rawsize, raw in self.sections:
            if start <= rva < start + max(vsize, rawsize):
                if rva-start >= rawsize:
                    raise ValueError('RVA has no file-backed bytes')
                return raw + rva-start
        raise ValueError(f'Unmapped RVA {rva:#x}')

    def cstring(self, rva):
        start = self.offset(rva)
        end = self.data.index(b'\0', start)
        return self.data[start:end].decode('ascii')

    def exports(self):
        rva, size = struct.unpack_from('<II', self.data, self.directories)
        base = self.offset(rva)
        names = self.u32(base+24)
        functions = self.offset(self.u32(base+28))
        name_table = self.offset(self.u32(base+32))
        ordinals = self.offset(self.u32(base+36))
        result = {}
        for i in range(names):
            name = self.cstring(self.u32(name_table+4*i))
            function = self.u32(functions+4*self.u16(ordinals+2*i))
            if not re.search(r'VR|Camera|HeadTrack|Render|Projection|Stereo', name):
                continue
            if rva <= function < rva+size:
                result[name] = {'forwarder': self.cstring(function)}
                continue
            start = self.offset(function)
            code = self.data[start:start+16]
            stub = None
            if code.startswith((b'\x32\xc0\xc3', b'\x33\xc0\xc3', b'\x31\xc0\xc3')):
                stub = 'returns_zero'
            elif code.startswith((b'\xc3', b'\xc2\x00\x00')):
                stub = 'returns_immediately'
            result[name] = {'rva': f'0x{function:08x}', 'first_16_bytes': code.hex(' '), 'stub': stub}
        return result

    def imports(self):
        rva = self.u32(self.directories+8)
        if not rva:
            return []
        base = self.offset(rva)
        result = []
        while any(self.data[base:base+20]):
            result.append(self.cstring(self.u32(base+12)))
            base += 20
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    pe = PE(args.exe)
    report = {
        'executable': str(args.exe.resolve()),
        'sha256': hashlib.sha256(pe.data).hexdigest(),
        'machine': hex(pe.machine),
        'imports': pe.imports(),
        'selected_exports': pe.exports(),
        'openvr_literal_strings': [s.decode('ascii') for s in re.findall(rb'[\x20-\x7e]{5,}', pe.data)
                                   if re.search(rb'openvr|vrclient|IVRSystem|VRCompositor|VR_Init', s, re.I)],
        'limitations': 'Static evidence only. Absence of imports/strings does not rule out every dynamic path. Stub classifications inspect entry bytes, not callers.'
    }
    text = json.dumps(report, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text+'\n', encoding='utf-8')
    else:
        print(text)


if __name__ == '__main__':
    main()
