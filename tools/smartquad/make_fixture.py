"""Create an offline, intersection-free textured Smart Quad regression asset."""
import argparse
import json
from pathlib import Path
import sys

import bpy
import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--segments', type=int, default=32)
    parser.add_argument('--rings', type=int, default=20)
    args = parser.parse_args(sys.argv[sys.argv.index('--')+1:])
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    if (root / 'TexturedReference.glb').exists():
        raise RuntimeError('Fixture already exists; refusing to overwrite it.')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    size = 256
    texture = bpy.data.images.new('CheckerAlbedo', width=size, height=size, alpha=True)
    y, x = np.indices((size, size))
    checker = ((x // 32 + y // 32) % 2).astype(bool)
    pixels = np.zeros((size, size, 4), dtype=np.float32)
    pixels[checker] = (.9, .2, .045, 1)
    pixels[~checker] = (.12, .42, .8, 1)
    texture.pixels[:] = pixels.ravel()
    texture.filepath_raw = str(root / 'CheckerAlbedo.png')
    texture.file_format = 'PNG'
    texture.save()
    material = bpy.data.materials.new('Checker PBR')
    material.use_nodes = True
    node = material.node_tree.nodes.new('ShaderNodeTexImage')
    node.image = texture
    node.interpolation = 'Linear'
    bsdf = material.node_tree.nodes.get('Principled BSDF')
    material.node_tree.links.new(node.outputs['Color'], bsdf.inputs['Base Color'])
    bsdf.inputs['Roughness'].default_value = .62
    bsdf.inputs['Metallic'].default_value = .08
    if not 8 <= args.segments <= 128 or not 8 <= args.rings <= 80:
        raise RuntimeError('Fixture resolution is outside its bounded range.')
    bpy.ops.mesh.primitive_uv_sphere_add(segments=args.segments, ring_count=args.rings, radius=1,
                                        location=(0, 0, 0))
    sphere = bpy.context.object
    sphere.name = 'TexturedOrganicReference'
    sphere.scale = (1, .72, 1.3)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    sphere.data.materials.append(material)
    for face in sphere.data.polygons:
        face.use_smooth = True
    bpy.ops.object.select_all(action='DESELECT')
    sphere.select_set(True)
    bpy.context.view_layer.objects.active = sphere
    bpy.ops.export_scene.gltf(filepath=str(root / 'TexturedReference.glb'),
                              export_format='GLB', use_selection=True,
                              export_materials='EXPORT', export_animations=False,
                              export_normals=True, export_texcoords=True,
                              export_tangents=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(root / 'TexturedReference.blend'))
    rna = bpy.ops.object.quadriflow_remesh.get_rna_type()
    evidence = {"fixture": str(root / 'TexturedReference.glb'),
                "source": "Locally generated analytic ellipsoid with original UV checker PBR",
                "license": "CC0-1.0", "connected_components": 1,
                "input_vertices": len(sphere.data.vertices),
                "input_faces": len(sphere.data.polygons),
                "segments": args.segments, "rings": args.rings,
                "quadriflow_properties": list(rna.properties.keys()),
                "blender_version": bpy.app.version_string}
    (root / 'fixture.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
    print(json.dumps(evidence), flush=True)


if __name__ == '__main__':
    main()
