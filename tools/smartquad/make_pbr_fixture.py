"""CC0 offline PBR fixture: two disjoint closed textured ellipsoids.

Blender --background --factory-startup --python make_pbr_fixture.py -- --output DIR
"""
import argparse
import json
from pathlib import Path
import sys

import bpy
import numpy as np


def image(root, name, pixels, non_color=False):
    size = pixels.shape[0]
    result = bpy.data.images.new(name, width=size, height=size, alpha=True)
    result.colorspace_settings.name = 'Non-Color' if non_color else 'sRGB'
    result.pixels[:] = pixels.astype(np.float32).ravel()
    result.filepath_raw = str(root / (name + '.png'))
    result.file_format = 'PNG'
    result.save()
    return result


def texture(material, value):
    node = material.node_tree.nodes.new('ShaderNodeTexImage')
    node.image = value
    node.interpolation = 'Linear'
    return node


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--segments', type=int, default=64)
    parser.add_argument('--rings', type=int, default=40)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    if not 16 <= args.segments <= 128 or not 12 <= args.rings <= 80:
        raise RuntimeError('Fixture resolution is outside bounded limits.')
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise RuntimeError('Refusing to overwrite existing fixture assets.')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    size = 256
    y, x = np.indices((size, size), dtype=np.float32)
    u, v = (x + .5) / size, (y + .5) / size
    tau = np.float32(2 * np.pi)
    albedo = np.ones((size, size, 4), dtype=np.float32)
    albedo[:, :, 0] = .25 + .3 * np.sin(tau * u) ** 2
    albedo[:, :, 1] = .15 + .3 * np.cos(tau * v) ** 2
    albedo[:, :, 2] = .25 + .2 * np.sin(tau * (u + v)) ** 2
    orm = np.ones_like(albedo)
    orm[:, :, 0] = .5 + .25 * np.sin(tau * u) * np.cos(tau * v)
    orm[:, :, 1] = .35 + .25 * np.sin(tau * v) ** 2
    orm[:, :, 2] = .1 + .2 * np.sin(tau * u) ** 2
    normals = np.ones_like(albedo)
    nx, ny = .08 * np.sin(tau * u), .08 * np.cos(tau * v)
    normals[:, :, 0] = .5 + nx * .5
    normals[:, :, 1] = .5 + ny * .5
    normals[:, :, 2] = .5 + .5 * np.sqrt(1 - nx * nx - ny * ny)
    emissive = np.ones_like(albedo)
    emissive[:, :, :3] = albedo[:, :, :3] * .015
    maps = {'albedo': image(root, 'ReferenceRGBA', albedo),
            'orm': image(root, 'ReferenceORM', orm, True),
            'normal': image(root, 'ReferenceNormal', normals, True),
            'emission': image(root, 'ReferenceEmission', emissive)}
    settings = bpy.data.node_groups.new('glTF Material Output', 'ShaderNodeTree')
    settings.interface.new_socket(name='Occlusion', in_out='INPUT', socket_type='NodeSocketFloat')
    settings.nodes.new('NodeGroupOutput')
    settings.nodes.new('NodeGroupInput')
    objects, records = [], []
    for index, (location, scale) in enumerate((
            ((-1.15, 0, 0), (.75, .6, 1)),
            ((1.1, 0, 0), (.55, .75, .85)))):
        material = bpy.data.materials.new(f'UsedPBR_{index}')
        material.use_nodes = True
        bsdf = material.node_tree.nodes.get('Principled BSDF')
        links = material.node_tree.links
        color = texture(material, maps['albedo'])
        links.new(color.outputs['Color'], bsdf.inputs['Base Color'])
        links.new(color.outputs['Alpha'], bsdf.inputs['Alpha'])
        packed = texture(material, maps['orm'])
        separate = material.node_tree.nodes.new('ShaderNodeSeparateColor')
        separate.mode = 'RGB'
        links.new(packed.outputs['Color'], separate.inputs['Color'])
        links.new(separate.outputs['Green'], bsdf.inputs['Roughness'])
        links.new(separate.outputs['Blue'], bsdf.inputs['Metallic'])
        ao = material.node_tree.nodes.new('ShaderNodeGroup')
        ao.node_tree = settings
        strength = material.node_tree.nodes.new('ShaderNodeMix')
        strength.data_type = 'RGBA'
        strength.blend_type = 'MIX'
        strength.label = 'Occlusion Strength'
        strength.inputs['Factor'].default_value = .6
        strength.inputs[6].default_value = (1, 1, 1, 1)
        links.new(separate.outputs['Red'], strength.inputs[7])
        links.new(strength.outputs[2], ao.inputs['Occlusion'])
        normal_texture = texture(material, maps['normal'])
        normal = material.node_tree.nodes.new('ShaderNodeNormalMap')
        normal.inputs['Strength'].default_value = .65
        links.new(normal_texture.outputs['Color'], normal.inputs['Color'])
        links.new(normal.outputs['Normal'], bsdf.inputs['Normal'])
        emission = texture(material, maps['emission'])
        links.new(emission.outputs['Color'], bsdf.inputs['Emission Color'])
        bsdf.inputs['Emission Strength'].default_value = 1
        bpy.ops.mesh.primitive_uv_sphere_add(segments=args.segments, ring_count=args.rings,
                                            radius=1, location=location)
        obj = bpy.context.object
        obj.name = f'PBRReference_{index}'
        obj.scale = scale
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        unused = bpy.data.materials.new(f'UnusedSlot_{index}')
        unused.use_nodes = True
        unused.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value = (1, 0, 1, 1)
        obj.data.materials.append(unused)
        obj.data.materials.append(material)
        for face in obj.data.polygons:
            face.material_index = 1
            face.use_smooth = True
        obj.data.calc_loop_triangles()
        objects.append(obj)
        records.append({'name': obj.name, 'vertices': len(obj.data.vertices),
                        'triangles': len(obj.data.loop_triangles), 'material_slot': 1,
                        'occlusion_strength': .6, 'normal_strength': .65})
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    output = root / 'PBRReference.glb'
    bpy.ops.export_scene.gltf(filepath=str(output), export_format='GLB', use_selection=True,
                              export_normals=True, export_tangents=True, export_texcoords=True,
                              export_materials='EXPORT', export_animations=False)
    for value in maps.values():
        value.pack()
    bpy.ops.wm.save_as_mainfile(filepath=str(root / 'PBRReference.blend'))
    evidence = {'license': 'CC0-1.0', 'source': 'Locally generated disjoint closed ellipsoids',
                'objects': records, 'input_triangles': sum(r['triangles'] for r in records),
                'input_vertices': sum(r['vertices'] for r in records),
                'texture_size': size, 'fixture': str(output),
                'ao_formula': '0.5 + 0.25*sin(2*pi*u)*cos(2*pi*v)',
                'occlusion_strength': .6, 'normal_strength': .65,
                'blender_version': bpy.app.version_string}
    (root / 'fixture.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
    print(json.dumps(evidence), flush=True)


if __name__ == '__main__':
    main()
