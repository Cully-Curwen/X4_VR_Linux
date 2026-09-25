import math
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from analyze_uniforms import camera_candidate, multiply, native_relationships


class CameraAnalysisTests(unittest.TestCase):
    def test_native_byte_correspondence(self):
        camera = bytearray(0x200)
        uniform = bytearray(0x700)
        camera[0x40] = 42; uniform[0] = 42
        result = native_relationships(camera, uniform, uniform)
        self.assertTrue(result['temporary_matches_uniform'])
        self.assertTrue(result['native_view_matches_uniform'])
        self.assertTrue(result['native_inverse_matches_uniform'])
        camera[0x40] = 43
        self.assertFalse(native_relationships(camera, None, uniform)['native_view_matches_uniform'])

    def test_invalid_candidates(self):
        self.assertEqual(camera_candidate(b'')['status'], 'too_short')
        self.assertEqual(camera_candidate(bytes(576))['status'], 'all_zero')
        data = bytearray(576)
        struct.pack_into('<f', data, 0, math.nan)
        self.assertEqual(camera_candidate(data)['status'], 'nonfinite')

    def test_native_projection_order_and_y_flip(self):
        camera, uniform = bytearray(0x200), bytearray(0x700)
        projection = [[2,0,0,0], [0,3,0,0], [0,0,0,0.1], [0,0,1,0]]
        jitter = [[1,0,0,0.2], [0,1,0,0.3], [0,0,1,0], [0,0,0,1]]
        actual = multiply(jitter, projection)
        actual[1] = [-v for v in actual[1]]
        for data, offset, value in ((camera,0x140,projection), (camera,0x1c0,jitter), (uniform,64,actual)):
            struct.pack_into('<16f', data, offset, *(value[r][c] for c in range(4) for r in range(4)))
        self.assertLess(native_relationships(camera, None, uniform)['native_projection_error'], 1e-6)

    def test_column_major_relationships(self):
        view = [[1,0,0,4], [0,1,0,5], [0,0,1,6], [0,0,0,1]]
        inverse = [[1,0,0,-4], [0,1,0,-5], [0,0,1,-6], [0,0,0,1]]
        projection = [[2,0,0,0], [0,-3,0,0], [0,0,0,0.1], [0,0,1,0]]
        data = bytearray(576)
        for offset, value in zip((0,64,448,512), (view,projection,multiply(projection,view),inverse)):
            struct.pack_into('<16f', data, offset, *(value[r][c] for c in range(4) for r in range(4)))
        self.assertEqual(camera_candidate(data)['status'], 'relationships_consistent')
        struct.pack_into('<f', data, 512, 2)
        self.assertEqual(camera_candidate(data)['status'], 'relationships_inconsistent')


if __name__ == '__main__':
    unittest.main()
