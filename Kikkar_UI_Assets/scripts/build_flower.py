import argparse
from array import array
import json
import math
from pathlib import Path
import random
import struct
import sys
import time

import bpy
from mathutils import Vector
from bpy_extras.object_utils import world_to_camera_view

TARGET_WIDTH = 800
TARGET_HEIGHT = 480
PETAL_COUNT = 16
TWIG_SEGMENTS = 1
RNG_SEED = 657
FLOWER_RADIUS = 1.70
FLOWER_FRAME_HEIGHT = 0.68
PETAL_LENGTH = 1.16
PETAL_WIDTH = 0.20
PETAL_PITCH = 0.12
PETAL_CURL = 0.24
PETAL_TWIST = 0.22
PETAL_TAPER = 0.72
PETAL_BASE_RADIUS = 0.18
PETAL_COLOR_ROOT = (0.38, 0.085, 0.008, 1.0)
PETAL_COLOR_TIP = (1.0, 0.58, 0.10, 1.0)
PETAL_EDGE_DARKENING = 0.26
PETAL_ROUGHNESS = 0.38
PETAL_SUBSURFACE = 0.08
PETAL_TRANSLUCENCY = 0.08
FLORET_COUNT = 30
FLORET_STAMENS = 24
FLORET_SIZE = 0.37
FILAMENT_RADIUS = 0.0075
ANTHER_RADIUS = 0.021
LEAF_CURL = 0.42
LEAF_LENGTH = 2.85
LEAF_BRANCH_PAIRS = 4
LEAFLET_PAIRS = 12
LEAF_COLOR_ROOT = (0.025, 0.055, 0.018, 1.0)
LEAF_COLOR_TIP = (0.20, 0.28, 0.075, 1.0)
GLOW_RADIUS = 2.75
GLOW_ALPHA = 0.26
GLOW_COLOR = (1.0, 0.33, 0.018, 1.0)
FAST_SAMPLES = 12
CYCLES_SAMPLES = 48
SUPERSAMPLE = 4

ROOT = Path(__file__).resolve().parents[1]


def parse_args():
    parser = argparse.ArgumentParser(description="Render the flower's layers and poses headlessly in Blender.")
    parser.add_argument('--mode', choices=('composite', 'layers', 'poses'), default='composite')
    parser.add_argument('--stage', choices=('camera', 'petals', 'leaves', 'materials'), default='materials')
    parser.add_argument('--fast', action='store_true')
    parser.add_argument('--diagnostic', action='store_true', help='160x88 camera-stage validation only')
    parser.add_argument('--width', type=int, default=TARGET_WIDTH)
    parser.add_argument('--height', type=int, default=TARGET_HEIGHT)
    parser.add_argument('--petals', type=int, default=PETAL_COUNT)
    parser.add_argument('--twig-segments', type=int, choices=(1,), default=TWIG_SEGMENTS,
                        help='one sprite per twig, as requested')
    parser.add_argument('--steps', type=int, default=5)
    parser.add_argument('--bloom', type=float, default=1.0)
    parser.add_argument('--samples', type=int)
    parser.add_argument('--out', type=Path, default=ROOT / 'out')
    parser.add_argument('--save-blend', action='store_true')
    parser.add_argument('--only', nargs='+', help='Restrict layer smoke checks to named collections')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    if args.diagnostic:
        if args.stage != 'camera' or args.mode != 'composite':
            parser.error('--diagnostic requires --stage camera --mode composite')
        args.width, args.height = 160, 88
    elif any(v is None for v in (args.width, args.height, args.petals, args.twig_segments)):
        parser.error('supply --width, --height, --petals and --twig-segments (production choices are unset)')
    if min(args.width, args.height) < 16:
        parser.error('frame dimensions must be at least 16')
    if not 0 <= args.bloom <= 1 or not 2 <= args.steps <= 101:
        parser.error('--bloom must be 0..1 and --steps must be 2..101')
    if args.petals is not None and not 3 <= args.petals <= 80:
        parser.error('--petals must be 3..80')
    if args.twig_segments is not None and not 1 <= args.twig_segments <= 8:
        parser.error('--twig-segments must be 1..8')
    if args.samples is not None and args.samples < 1:
        parser.error('--samples must be positive')
    args.out = args.out.resolve()
    return args


def reference_info():
    result = {}
    for name, candidates in [('composition', ['composition.png']), ('leaf_twig', ['leaf_twig.png', 'leaf_sprig.png']), ('floret_detail', ['floret_detail.png'])]:
        path = next((ROOT / 'refs' / candidate for candidate in candidates if (ROOT / 'refs' / candidate).is_file()), None)
        if path is None:
            raise FileNotFoundError(f'Missing reference: {candidates}')
        with path.open('rb') as stream:
            header = stream.read(24)
        if header[:8] != b'\x89PNG\r\n\x1a\n':
            raise ValueError(f'{path}: expected PNG')
        result[name] = {'filename': str(path.relative_to(ROOT)), 'dimensions': list(struct.unpack('>II', header[16:24]))}
    return result


def socket(node, *names):
    """Finds a node socket under whichever name this Blender version uses."""
    for name in names:
        found = node.inputs.get(name)
        if found is not None:
            return found
    raise RuntimeError(f'{node.bl_idname}: no input matching {names}')


def configure(args):
    if bpy.app.version < (3, 6, 0):
        raise RuntimeError('Blender 3.6 or newer is required')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    for col in list(bpy.data.collections):
        bpy.data.collections.remove(col)
    for blocks in (bpy.data.meshes, bpy.data.materials, bpy.data.lights, bpy.data.cameras):
        for block in list(blocks):
            if block.users == 0:
                blocks.remove(block)
    scene = bpy.context.scene
    scene.render.engine = ('BLENDER_EEVEE_NEXT' if bpy.app.version >= (4, 2, 0) else 'BLENDER_EEVEE') if args.fast else 'CYCLES'
    samples = args.samples or (FAST_SAMPLES if args.fast else CYCLES_SAMPLES)
    if args.fast:
        scene.eevee.taa_render_samples = samples
    else:
        scene.cycles.device = 'CPU'
        scene.cycles.samples = samples
        scene.cycles.seed = RNG_SEED
        scene.cycles.use_animated_seed = False
        scene.cycles.use_adaptive_sampling = False
        scene.cycles.use_denoising = True
        scene.cycles.max_bounces = 6
        scene.cycles.transparent_max_bounces = 8
    scene.render.resolution_x = args.width * SUPERSAMPLE
    scene.render.resolution_y = args.height * SUPERSAMPLE
    scene.render.resolution_percentage = 100
    scene.render.pixel_aspect_x = scene.render.pixel_aspect_y = 1
    scene.render.film_transparent = True
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '8'
    scene.render.image_settings.compression = 30
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'None'
    scene.view_settings.exposure = 0
    scene.view_settings.gamma = 1
    scene.render.dither_intensity = 0
    scene.render.use_compositing = False
    scene.render.use_sequencer = False
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.07, 0.065, 0.055, 1)
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.22
    rig = bpy.data.collections.new('_rig')
    scene.collection.children.link(rig)
    camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('OrthographicCamera'))
    rig.objects.link(camera)
    camera.location = (0, 0, 14)
    camera.rotation_euler = (0, 0, 0)
    camera.data.type = 'ORTHO'
    vertical_span = 2 * (FLOWER_RADIUS + FLORET_SIZE) / FLOWER_FRAME_HEIGHT
    camera.data.ortho_scale = vertical_span * max(1, args.width / args.height)
    camera.data.clip_start = 0.1
    camera.data.clip_end = 100
    scene.camera = camera
    for name, location, power, color, size in [
            ('Key', (-3.5, 3.8, 6), 720, (1.0, 0.83, 0.56), 4.0),
            ('Fill', (4, 0.5, 5), 330, (0.72, 0.85, 1.0), 5.0),
            ('Rim', (0, -3.5, 3.8), 520, (1.0, 0.55, 0.19), 3.0)]:
        data = bpy.data.lights.new(name, 'AREA')
        data.energy, data.color, data.shape, data.size = power, color, 'DISK', size
        lamp = bpy.data.objects.new(name, data)
        rig.objects.link(lamp)
        lamp.location = location
        lamp.rotation_euler = (-lamp.location).to_track_quat('-Z', 'Y').to_euler()
    return scene


def simple_material():
    material = bpy.data.materials.new('Diagnostic gold')
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get('Principled BSDF')
    socket(bsdf, 'Base Color').default_value = (0.65, 0.28, 0.035, 1)
    socket(bsdf, 'Roughness').default_value = 0.42
    socket(bsdf, 'Specular IOR Level', 'Specular').default_value = 0.3
    return material


def surface_material(name, leaf=False):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    bsdf = nodes.get('Principled BSDF')
    color = nodes.new('ShaderNodeVertexColor')
    color.layer_name = 'SurfaceColor'
    links.new(color.outputs['Color'], socket(bsdf, 'Base Color'))
    socket(bsdf, 'Roughness').default_value = 0.47 if leaf else PETAL_ROUGHNESS
    socket(bsdf, 'Specular IOR Level', 'Specular').default_value = 0.32
    socket(bsdf, 'Subsurface Weight', 'Subsurface').default_value = 0.035 if leaf else PETAL_SUBSURFACE
    socket(bsdf, 'Subsurface Radius').default_value = (1.0, 0.55, 0.18)
    if bsdf.inputs.get('Subsurface Scale'):
        socket(bsdf, 'Subsurface Scale').default_value = 0.045
    links.new(color.outputs['Color'], socket(bsdf, 'Emission Color', 'Emission'))
    socket(bsdf, 'Emission Strength').default_value = 0.015 if leaf else 0.13
    noise = nodes.new('ShaderNodeTexNoise')
    noise.inputs['Scale'].default_value = 125
    noise.inputs['Detail'].default_value = 2
    bump = nodes.new('ShaderNodeBump')
    bump.inputs['Strength'].default_value = 0.09
    bump.inputs['Distance'].default_value = 0.012
    links.new(noise.outputs['Fac'], bump.inputs['Height'])
    links.new(bump.outputs['Normal'], socket(bsdf, 'Normal'))
    translucent = nodes.new('ShaderNodeBsdfTranslucent')
    links.new(color.outputs['Color'], translucent.inputs['Color'])
    mix = nodes.new('ShaderNodeMixShader')
    mix.inputs[0].default_value = PETAL_TRANSLUCENCY
    links.new(bsdf.outputs['BSDF'], mix.inputs[1])
    links.new(translucent.outputs[0], mix.inputs[2])
    links.new(mix.outputs[0], nodes.get('Material Output').inputs['Surface'])
    return material


def add_glow():
    material = bpy.data.materials.new('Independent radial emission')
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    uv = nodes.new('ShaderNodeTexCoord')
    distance = nodes.new('ShaderNodeVectorMath')
    distance.operation = 'DISTANCE'
    distance.inputs[1].default_value = (0.5, 0.5, 0)
    links.new(uv.outputs['UV'], distance.inputs[0])
    falloff = nodes.new('ShaderNodeMapRange')
    falloff.interpolation_type = 'SMOOTHERSTEP'
    falloff.inputs['From Min'].default_value = 0
    falloff.inputs['From Max'].default_value = 0.5
    falloff.inputs['To Min'].default_value = GLOW_ALPHA
    falloff.inputs['To Max'].default_value = 0
    links.new(distance.outputs['Value'], falloff.inputs['Value'])
    transparent = nodes.new('ShaderNodeBsdfTransparent')
    emission = nodes.new('ShaderNodeEmission')
    emission.inputs['Color'].default_value = GLOW_COLOR
    emission.inputs['Strength'].default_value = 1
    mix = nodes.new('ShaderNodeMixShader')
    links.new(falloff.outputs['Result'], mix.inputs[0])
    links.new(transparent.outputs[0], mix.inputs[1])
    links.new(emission.outputs[0], mix.inputs[2])
    output = nodes.new('ShaderNodeOutputMaterial')
    links.new(mix.outputs[0], output.inputs['Surface'])
    if hasattr(material, 'surface_render_method'):
        material.surface_render_method = 'BLENDED'
    elif hasattr(material, 'blend_method'):
        material.blend_method = 'BLEND'
        material.use_screen_refraction = False
    bpy.ops.mesh.primitive_plane_add(size=2 * GLOW_RADIUS, location=(0, 0, -0.8))
    obj = bpy.context.object
    for flag in ('visible_shadow', 'visible_diffuse', 'visible_glossy', 'visible_transmission', 'visible_volume_scatter'):
        if hasattr(obj, flag):
            setattr(obj, flag, False)
    return element('glow', obj, (0, 0, -0.8), 0, material)


def element(name, obj, pivot, order, material):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    for old in list(obj.users_collection):
        old.objects.unlink(obj)
    collection.objects.link(obj)
    obj.name = name
    if material:
        obj.data.materials.append(material)
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    empty = bpy.data.objects.new(name + '_pivot', None)
    collection.objects.link(empty)
    empty.parent = obj
    empty.location = obj.matrix_world.inverted() @ Vector(pivot)
    empty.empty_display_type = 'PLAIN_AXES'
    empty.empty_display_size = 0.08
    obj['draw_order'] = order
    return {'object': obj, 'collection': collection, 'pivot': empty, 'draw_order': order}


class Geometry:
    """Collects disconnected parts into one mesh that moves as a unit."""
    def __init__(self):
        self.vertices, self.faces, self.colors = [], [], []

    def vertex(self, point, color):
        self.vertices.append(tuple(point))
        self.colors.append(tuple(color))
        return len(self.vertices) - 1

    def tube(self, points, radius, color, sides=6):
        points = [Vector(p) for p in points]
        start = len(self.vertices)
        for i, point in enumerate(points):
            tangent = (points[min(i + 1, len(points) - 1)] - points[max(0, i - 1)]).normalized()
            across = tangent.cross(Vector((0, 0, 1)))
            if across.length < 0.01:
                across = tangent.cross(Vector((0, 1, 0)))
            across.normalize()
            up = tangent.cross(across).normalized()
            r = radius * (1 - 0.25 * i / (len(points) - 1))
            for j in range(sides):
                a = 2 * math.pi * j / sides
                self.vertex(point + r * (math.cos(a) * across + math.sin(a) * up), color)
        self.faces.append(tuple(start + j for j in reversed(range(sides))))
        for i in range(len(points) - 1):
            for j in range(sides):
                a, b = start + i * sides + j, start + i * sides + (j + 1) % sides
                self.faces.append((a, b, b + sides, a + sides))
        end = start + (len(points) - 1) * sides
        self.faces.append(tuple(end + j for j in range(sides)))

    def bead(self, center, radius, color, elongation=1.0):
        start = len(self.vertices)
        segments, rings = 8, 6
        for i in range(rings + 1):
            latitude = math.pi * i / rings
            for j in range(segments):
                longitude = 2 * math.pi * j / segments
                self.vertex((center[0] + radius * math.sin(latitude) * math.cos(longitude),
                             center[1] + radius * math.sin(latitude) * math.sin(longitude) * elongation,
                             center[2] + radius * math.cos(latitude)), color)
        for i in range(rings):
            for j in range(segments):
                a = start + i * segments + j
                b = start + i * segments + (j + 1) % segments
                self.faces.append((a, a + segments, b + segments, b))

    def blade(self, base, length, width, angle, curl, twist, root_color, tip_color,
              pitch=0.0, longitudinal=18, transverse=8):
        start = len(self.vertices)
        ca, sa = math.cos(angle), math.sin(angle)
        for layer in (-1, 1):
            for i in range(longitudinal + 1):
                t = i / longitudinal
                profile = max(0.002, math.sin(math.pi * t) ** PETAL_TAPER)
                for j in range(transverse + 1):
                    u = 2 * j / transverse - 1
                    x = width * profile * u
                    y = length * t
                    z = (pitch * y + curl * t * t * (0.3 + 0.7 * t)
                         + 0.13 * width * (1 - u * u) * math.sin(math.pi * t)
                         + twist * x * t + layer * 0.005)
                    point = (base[0] + ca * x - sa * y, base[1] + sa * x + ca * y, base[2] + z)
                    darken = 1 - PETAL_EDGE_DARKENING * abs(u) ** 3
                    color = tuple((root_color[k] * (1 - t) + tip_color[k] * t) * darken for k in range(3)) + (1,)
                    self.vertex(point, color)
        stride = transverse + 1
        count = (longitudinal + 1) * stride
        for layer in range(2):
            offset = start + layer * count
            for i in range(longitudinal):
                for j in range(transverse):
                    a = offset + i * stride + j
                    face = (a, a + 1, a + stride + 1, a + stride)
                    self.faces.append(face if layer else tuple(reversed(face)))
        boundary = ([start + j for j in range(stride)]
                    + [start + i * stride + transverse for i in range(1, longitudinal + 1)]
                    + [start + longitudinal * stride + j for j in reversed(range(transverse))]
                    + [start + i * stride for i in reversed(range(1, longitudinal))])
        for a, b in zip(boundary, boundary[1:] + boundary[:1]):
            self.faces.append((a, b, b + count, a + count))

    def object(self, name, pivot, order, material):
        mesh = bpy.data.meshes.new(name + '_mesh')
        mesh.from_pydata(self.vertices, [], self.faces)
        mesh.update()
        colors = mesh.color_attributes.new(name='SurfaceColor', type='FLOAT_COLOR', domain='POINT')
        colors.data.foreach_set('color', [value for color in self.colors for value in color])
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        for vertex in mesh.vertices:
            vertex.co -= Vector(pivot)
        obj.location = pivot
        bpy.context.view_layer.update()
        return element(name, obj, pivot, order, material)


def add_petals(args, bloom, material):
    elements = []
    rng = random.Random(RNG_SEED)
    for i in range(args.petals):
        angle = 2 * math.pi * i / args.petals + rng.uniform(-0.045, 0.045)
        base = (PETAL_BASE_RADIUS * math.cos(angle), PETAL_BASE_RADIUS * math.sin(angle), 0.05 + 0.012 * i)
        mesh = Geometry()
        mesh.blade(base, PETAL_LENGTH * (0.43 + 0.57 * bloom), PETAL_WIDTH * (0.45 + 0.55 * bloom),
                   angle - math.pi / 2, PETAL_CURL + 0.7 * (1 - bloom), PETAL_TWIST * (-1 if i % 2 else 1),
                   PETAL_COLOR_ROOT, PETAL_COLOR_TIP, pitch=PETAL_PITCH)
        elements.append(mesh.object(f'petal_{i:02d}', base, 60 + i, material))
    return elements


def add_leaves(material):
    elements = []
    for name, flip, order in [('leaf_L', -1, 10), ('leaf_R', 1, 11)]:
        mesh = Geometry()
        base = Vector((flip * 0.85, flip * 0.42, -0.40))
        direction = Vector((flip * math.cos(0.46), flip * math.sin(0.46), 0))
        across = Vector((-direction.y, direction.x, 0))

        def stem(t):
            return base + LEAF_LENGTH * t * direction + LEAF_CURL * t * t * across + Vector((0, 0, 0.10 * math.sin(math.pi * t)))

        stem_color = (0.20, 0.14, 0.035, 1)
        mesh.tube([stem(i / 24) for i in range(25)], 0.027, stem_color)
        for branch in range(LEAF_BRANCH_PAIRS):
            t = 0.20 + branch * 0.19
            start = stem(t)
            length = 0.99 - 0.12 * branch
            for side in (-1, 1):
                branch_dir = (0.48 * direction + 0.88 * side * across).normalized()
                normal = Vector((-branch_dir.y, branch_dir.x, 0))

                def branch_point(s):
                    return start + branch_dir * (length * s) + direction * (0.10 * s * s) + Vector((0, 0, 0.05 * math.sin(math.pi * s)))

                mesh.tube([branch_point(i / 12) for i in range(13)], 0.012, stem_color)
                for leaflet in range(LEAFLET_PAIRS):
                    u = (leaflet + 0.75) / (LEAFLET_PAIRS + 0.5)
                    origin = branch_point(u)
                    leaf_length = 0.20 * (math.sin(math.pi * u) ** 0.45) + 0.035
                    for flank in (-1, 1):
                        vector = (0.46 * branch_dir + 0.89 * flank * normal).normalized()
                        angle = math.atan2(vector.y, vector.x) - math.pi / 2
                        mesh.blade(origin, leaf_length, 0.032, angle, 0.023, flank * 0.12,
                                   LEAF_COLOR_ROOT, LEAF_COLOR_TIP, longitudinal=7, transverse=4)
                        mesh.tube([origin, origin + vector * leaf_length * 0.8 + Vector((0, 0, 0.012))],
                                  0.0028, (0.24, 0.26, 0.09, 1), sides=4)
        elements.append(mesh.object(name, base, order, material))
    return elements


def add_florets(bloom, material):
    result = []
    rng = random.Random(RNG_SEED + 10)
    outer_count = round(FLORET_COUNT * 2 / 3)
    inner_count = FLORET_COUNT - outer_count
    for i in range(FLORET_COUNT):
        outer = i < outer_count
        index, count = (i, outer_count) if outer else (i - outer_count, inner_count)
        angle = 2 * math.pi * index / count + (0 if outer else 0.21) + rng.uniform(-0.07, 0.07)
        radius = FLOWER_RADIUS * (0.88 if outer else 0.46) * (0.54 + 0.46 * bloom)
        radial = Vector((math.cos(angle), math.sin(angle), 0))
        tangent = Vector((-math.sin(angle), math.cos(angle), 0))
        base = radial * radius + Vector((0, 0, 0.32 + 0.008 * i))
        hinge = radial * 0.14 + Vector((0, 0, base.z))
        mesh = Geometry()
        stem = [hinge.lerp(base, j / 8) + Vector((0, 0, 0.045 * math.sin(math.pi * j / 8))) for j in range(9)]
        mesh.tube(stem, 0.012, (0.62, 0.27, 0.036, 1))
        mesh.bead(base, 0.050, (0.90, 0.40, 0.024, 1))
        for j in range(5):
            theta = angle + (j - 2) * 0.55
            mesh.blade(base, 0.13 + bloom * 0.04, 0.045, theta - math.pi / 2, 0.07,
                       (j - 2) * 0.1, PETAL_COLOR_ROOT, PETAL_COLOR_TIP,
                       longitudinal=10, transverse=6)
        for j in range(FLORET_STAMENS):
            a = rng.uniform(-1.23, 1.23) * (0.28 + 0.72 * bloom)
            length = FLORET_SIZE * rng.uniform(0.65, 1.25)
            spread = radial * math.cos(a) + tangent * math.sin(a)
            end = base + spread * length * (0.52 + 0.48 * bloom)
            end.z += rng.uniform(0.03, 0.12) + 0.28 * (1 - bloom)
            points = [base.lerp(end, k / 6) + tangent * (0.023 * math.sin(math.pi * k / 6)) for k in range(7)]
            mesh.tube(points, FILAMENT_RADIUS, (0.98, 0.73, 0.28, 1), sides=5)
            mesh.bead(end, ANTHER_RADIUS * rng.uniform(0.75, 1.15), (1.0, 0.55, 0.042, 1), elongation=1.4)
        result.append(mesh.object(f'floret_{i:02d}', hinge, 100 + i, material))
    return result


def build_scene(args, bloom):
    scene = configure(args)
    mat = surface_material('Golden botanical tissue') if args.stage == 'materials' else simple_material()
    center = Geometry()
    center.bead((0, 0, 0.1), 0.19, (0.72, 0.25, 0.014, 1))
    for i in range(42):
        angle = i * 2.39996323
        r = 0.18 * math.sqrt((i + 0.5) / 42)
        center.bead((r * math.cos(angle), r * math.sin(angle), 0.14 + 0.12 * (1 - r / 0.20)),
                    0.022, (1.0, 0.49, 0.045, 1))
    elements = [center.object('center', (0, 0, 0.1), 50, mat)]
    if args.stage != 'camera':
        elements += add_petals(args, bloom, mat)
    if args.stage in ('leaves', 'materials'):
        elements += add_leaves(surface_material('Olive leaf tissue', leaf=True) if args.stage == 'materials' else mat)
    if args.stage == 'materials':
        elements += add_florets(bloom, mat)
        elements.append(add_glow())
    return scene, elements


def project_pivot(scene, pivot, width, height):
    point = world_to_camera_view(scene, scene.camera, pivot.matrix_world.translation)
    return [point.x * width, (1 - point.y) * height]


def alpha_report(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    width, height = image.size
    pixels = array('f', [0]) * (width * height * 4)
    image.pixels.foreach_get(pixels)
    left, bottom, right, top = width, height, -1, -1
    for y in range(height):
        offset = y * width * 4 + 3
        for x in range(width):
            if pixels[offset + x * 4] >= 0.5 / 255:
                left, right = min(left, x), max(right, x)
                bottom, top = min(bottom, y), max(top, y)
    bpy.data.images.remove(image)
    bbox = [left, height - 1 - top, right + 1, height - bottom] if right >= 0 else None
    plausible = bbox is not None and bbox != [0, 0, width, height]
    result = {'path': str(path), 'dimensions': [width, height], 'bytes': path.stat().st_size,
              'alpha_bbox': bbox, 'alpha_plausible': plausible}
    print('IMAGE_REPORT ' + json.dumps(result), flush=True)
    if not plausible:
        raise RuntimeError(f'Empty or full-frame alpha: {path}')
    return result


def render(scene, args, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    scene.render.filepath = str(path)
    started = time.monotonic()
    bpy.ops.render.render(write_still=True)
    elapsed = time.monotonic() - started
    if args.fast and elapsed > 60:
        old = scene.eevee.taa_render_samples
        scene.eevee.taa_render_samples = max(1, old // 2)
        print(f'FAST_RENDER_SLOW {elapsed:.1f}s: samples {old} -> {scene.eevee.taa_render_samples}; rerendering', flush=True)
        bpy.ops.render.render(write_still=True)
    return alpha_report(path)


def element_metadata(scene, entry, args, bloom):
    obj = entry['object']
    pivot = project_pivot(scene, entry['pivot'], args.width, args.height)
    rotation = list(obj.rotation_euler)
    return {'element': obj.name, 'collection': entry['collection'].name,
            'pivot_empty': entry['pivot'].name, 'pivot_composite_px': pivot,
            'pivot_source_px': [coordinate * SUPERSAMPLE for coordinate in pivot],
            'rest_rotation_euler_rad': rotation, 'rest_scale': list(obj.scale),
            'runtime_rotation_deg': 0, 'runtime_scale': [1.0, 1.0],
            'draw_order': entry['draw_order'], 'bloom': bloom}


def layer_border(scene, obj):
    corners = [world_to_camera_view(scene, scene.camera, obj.matrix_world @ Vector(c)) for c in obj.bound_box]
    pad_x, pad_y = 12 / scene.render.resolution_x, 12 / scene.render.resolution_y
    scene.render.use_border = True
    scene.render.use_crop_to_border = False
    scene.render.border_min_x = max(0, min(p.x for p in corners) - pad_x)
    scene.render.border_max_x = min(1, max(p.x for p in corners) + pad_x)
    scene.render.border_min_y = max(0, min(p.y for p in corners) - pad_y)
    scene.render.border_max_y = min(1, max(p.y for p in corners) + pad_y)


def main():
    args = parse_args()
    refs = reference_info()
    args.out.mkdir(parents=True, exist_ok=True)
    records = []
    poses = [i / (args.steps - 1) for i in range(args.steps)] if args.mode == 'poses' else [args.bloom]
    all_elements = []
    for bloom in poses:
        scene, elements = build_scene(args, bloom)
        bpy.context.view_layer.update()
        metadata = [element_metadata(scene, entry, args, bloom) for entry in elements]
        all_elements.extend(metadata)
        if args.mode == 'composite':
            path = args.out / 'raw' / 'composite.png'
            report = render(scene, args, path)
            records.append({'element': 'composite', 'bloom': bloom, 'source': str(path.relative_to(args.out)),
                            **{k: v for k, v in report.items() if k != 'path'}})
        else:
            if args.only:
                missing = set(args.only) - {entry['object'].name for entry in elements}
                if missing:
                    raise ValueError(f'Unknown element names: {sorted(missing)}')
            for entry in elements:
                entry['collection'].hide_render = True
            for entry, item in zip(elements, metadata):
                if args.only and item['element'] not in args.only:
                    continue
                entry['collection'].hide_render = False
                bpy.context.view_layer.update()
                layer_border(scene, entry['object'])
                path = args.out / 'layers' / f'{item["element"]}_bloom_{bloom:.2f}.png'
                report = render(scene, args, path)
                records.append({**item, 'source': str(path.relative_to(args.out)),
                                **{k: v for k, v in report.items() if k != 'path'}})
                entry['collection'].hide_render = True
            for entry in elements:
                entry['collection'].hide_render = False
            scene.render.use_border = False
    manifest = {'schema_version': 1, 'status': 'rendered', 'blender_version': bpy.app.version_string,
                'engine': scene.render.engine, 'seed': RNG_SEED, 'references': refs,
                'mode': args.mode, 'stage': args.stage, 'bloom_steps': poses, 'selection': args.only,
                'parameters': {name: value for name, value in globals().items() if name.isupper() and isinstance(value, (int, float, tuple))},
                'configuration': {'width': args.width, 'height': args.height, 'petals': args.petals, 'twig_segments': args.twig_segments,
                                  'samples': scene.eevee.taa_render_samples if args.fast else scene.cycles.samples},
                'frame': {'target_dimensions': [args.width, args.height], 'source_dimensions': [args.width * SUPERSAMPLE, args.height * SUPERSAMPLE], 'supersample': SUPERSAMPLE},
                'renders': records,
                'elements': all_elements}
    for name in ('source_manifest.json', 'manifest.json'):
        (args.out / name).write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    if args.save_blend:
        bpy.ops.wm.save_as_mainfile(filepath=str(args.out / 'flower.blend'))


if __name__ == '__main__':
    try:
        main()
    except SystemExit as error:
        if error.code:
            import os
            os._exit(error.code if isinstance(error.code, int) else 1)
    except Exception:
        import traceback
        traceback.print_exc()
        sys.stdout.flush()
        sys.stderr.flush()
        import os
        os._exit(1)
