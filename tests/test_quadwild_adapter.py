"""Deterministic adapter safety tests. No remeshing, Blender, GUI, or models."""
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/smartquad'))
import quadwild_adapter as adapter

TRIANGLE = 'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n'

class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.mesh = self.root / 'mesh.obj'
        self.mesh.write_text(TRIANGLE)

    def tearDown(self):
        self.temp.cleanup()

    def test_triangle_area_and_edge_scale(self):
        area, mean = adapter.triangular_obj_measurements(self.mesh)
        self.assertEqual(area, .5)
        self.assertAlmostEqual(mean, (2 + math.sqrt(2))/3)

    def test_non_triangular_input_is_not_silently_triangulated(self):
        self.mesh.write_text(TRIANGLE.replace('f 1 2 3', 'f 1 2 3 1'))
        with self.assertRaisesRegex(adapter.QuadWildFailure, 'triangular'):
            adapter.triangular_obj_measurements(self.mesh)

    def test_nonfinite_and_bad_indices_are_rejected(self):
        for source in (TRIANGLE.replace('v 0 0 0', 'v nan 0 0'), TRIANGLE.replace('f 1 2 3', 'f 1 2 8'),
                       TRIANGLE.replace('f 1 2 3', 'f 1 2 2')):
            with self.subTest(source=source):
                self.mesh.write_text(source)
                with self.assertRaises(adapter.QuadWildFailure):
                    adapter.triangular_obj_measurements(self.mesh)

    def test_written_output_does_not_excuse_engine_crash(self):
        (self.root / 'mesh_rem_p0_1_quadrangulation_smooth.obj').write_text(TRIANGLE)
        with patch.object(adapter.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0x80000003)):
            with self.assertRaisesRegex(adapter.QuadWildFailure, '80000003'):
                adapter.run_stage(['fake'], self.root, self.root/'failure.log', 1)

    def test_timeout_is_failure(self):
        with patch.object(adapter.subprocess, 'run', side_effect=subprocess.TimeoutExpired('fake', 1)):
            with self.assertRaisesRegex(adapter.QuadWildFailure, 'stage limit'):
                adapter.run_stage(['fake'], self.root, self.root/'timeout.log', 1)

    def test_missing_dependency_does_not_start(self):
        with patch.object(adapter.subprocess, 'run') as launch:
            with self.assertRaises(adapter.QuadWildFailure):
                adapter.remesh(self.root/'quadwild.exe', self.mesh, 10, False)
            launch.assert_not_called()

    def test_non_lemon_configuration_does_not_start(self):
        for name in ('quadwild.exe', 'quad_from_patches.exe'):
            (self.root/name).touch()
        config = self.root/'config/satsuma'
        config.mkdir(parents=True)
        (config/'default.json').write_text(json.dumps({'matching_solver': 'Blossom5'}))
        with patch.object(adapter.subprocess, 'run') as launch:
            with self.assertRaisesRegex(adapter.QuadWildFailure, 'Lemon'):
                adapter.remesh(self.root/'quadwild.exe', self.mesh, 10, False)
            launch.assert_not_called()

    def test_existing_log_is_not_overwritten(self):
        log = self.root/'existing.log'
        log.write_text('preserved')
        with self.assertRaises(FileExistsError):
            adapter.run_stage(['fake'], self.root, log, 1)
        self.assertEqual(log.read_text(), 'preserved')

if __name__ == '__main__':
    unittest.main()
