"""Parser tests cover malformed input and shader-specified layouts, not VR output."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('reflect_capture', Path(__file__).resolve().parents[1]/'tools/reflect_capture.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def inst(op, *args):
    return [((len(args)+1) << 16) | op, *args]


def string(text):
    data = text.encode()+b'\0'
    data += b'\0'*(-len(data) % 4)
    return list(struct.unpack('<'+'I'*(len(data)//4), data))


def encode(words):
    return struct.pack('<'+'I'*len(words), *words)


class ReflectionTests(unittest.TestCase):
    def test_rejects_invalid_streams(self):
        for data in (b'', b'\0'*20, encode([0x07230203, 0x10000, 0, 8, 0, 0]),
                     encode([0x07230203, 0x10000, 0, 8, 0, (4 << 16) | 5, 1])):
            with self.subTest(data=data), self.assertRaises(ValueError):
                module.reflect(data)

    def test_reads_declared_set_binding_and_matrix_layout(self):
        # A targeted reflection fixture, not an executable shader. Non-default set
        # numbers and offsets ensure the parser reads metadata rather than guessing.
        words = [0x07230203, 0x10000, 0, 100, 0]
        words += inst(5, 10, *string('camera_block'))
        for i, name in enumerate(('M_view', 'M_projection', 'M_viewprojection')):
            words += inst(6, 10, i, *string(name))
            words += inst(72, 10, i, 35, 128+64*i)
            words += inst(72, 10, i, 7, 16)
            words += inst(72, 10, i, 5)
        words += inst(30, 10, 7, 7, 7)
        words += inst(32, 11, 2, 10)
        words += inst(59, 11, 12, 2)
        words += inst(71, 12, 34, 9)
        words += inst(71, 12, 33, 2)
        block = module.reflect(encode(words))['blocks'][0]
        self.assertEqual((block['set'], block['binding']), (9, 2))
        self.assertEqual(block['identity'], 'camera')
        self.assertEqual([m['offset'] for m in block['members']], [128, 192, 256])
        self.assertTrue(all(m['matrix_order'] == 'column_major' for m in block['members']))

    def test_stripped_names_do_not_claim_camera_identity(self):
        words = [0x07230203, 0x10000, 0, 100, 0]
        words += inst(30, 10, 7, 7, 7) + inst(32, 11, 2, 10) + inst(59, 11, 12, 2)
        words += inst(71, 12, 34, 1) + inst(71, 12, 33, 0)
        self.assertEqual(module.reflect(encode(words))['blocks'][0]['identity'], 'other_or_unknown')


if __name__ == '__main__':
    unittest.main()
