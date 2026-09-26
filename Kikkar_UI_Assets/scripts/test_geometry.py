import ast
import math
from pathlib import Path
import unittest


class MeshWinding(unittest.TestCase):
    def test_anther_surface_faces_outward(self):
        tree = ast.parse(Path(__file__).with_name('build_flower.py').read_text())
        definition = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'Geometry')
        namespace = {'math': math}
        exec(compile(ast.Module(body=[definition], type_ignores=[]), 'build_flower.py', 'exec'), namespace)
        mesh = namespace['Geometry']()
        mesh.bead((0, 0, 0), 1, (1, 1, 1, 1))
        volume = 0
        for face in mesh.faces:
            a = mesh.vertices[face[0]]
            for i in range(1, len(face) - 1):
                b, c = mesh.vertices[face[i]], mesh.vertices[face[i + 1]]
                volume += (a[0] * (b[1] * c[2] - b[2] * c[1])
                           + a[1] * (b[2] * c[0] - b[0] * c[2])
                           + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6
        self.assertGreater(volume, 3, 'bead winding must have outward normals')


if __name__ == '__main__':
    unittest.main()
