"""Offline, fail-closed Smart Quad processing for Pixel3D (Blender 4.2+).

Run Blender --factory-startup --background --threads 2 --python-exit-code 1
--python pipeline.py -- --request request.json --audit-exe pixel3d-smartquad-audit.exe

All geometry is processed in a new output directory. The source is never saved.
An export is accepted only after independent native geometry inspection, sampled
bidirectional surface/feature checks, six camera comparisons, and a GLB reload.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import traceback
from types import SimpleNamespace

import bpy
import bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_simplify import simplify_game
from quadwild_adapter import remesh as quadwild_remesh


START = time.monotonic()
REQUEST = {}
OUT = None
STAGE = None
AUDIT = None
DIAGNOSTICS = []


class Rejected(RuntimeError):
    pass


def progress(stage, percent, message):
    print(json.dumps({"event": "progress", "stage": stage,
                      "progress": percent, "message": message}), flush=True)
    if time.monotonic() - START > float(REQUEST.get("max_seconds", 600)):
        raise Rejected("The bounded processing time was exceeded.")
    if OUT and (OUT / "cancel.request").exists():
        raise Rejected("Cancelled by the user.")


def deviation_limit():
    limit = float(REQUEST.get('max_deviation',
                  .005 if REQUEST.get('high_fidelity', True) else .01))
    if not REQUEST.get('preserve_shape', True):
        limit = min(.03, limit * 2)
    if not 0 < limit <= .05:
        raise Rejected('Maximum relative deviation must be in (0, 0.05].')
    return limit


def select(objects, active=None):
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects:
        obj.hide_set(False)
        obj.select_set(True)
    if active or objects:
        bpy.context.view_layer.objects.active = active or objects[-1]


def mesh_triangles(obj):
    mesh = obj.data
    mesh.calc_loop_triangles()
    vertices = np.array([tuple(v.co) for v in mesh.vertices], dtype=np.float64)
    faces = np.array([tuple(t.vertices) for t in mesh.loop_triangles], dtype=np.int32)
    return vertices.reshape((-1, 3)), faces.reshape((-1, 3))


def component_faces(mesh):
    incident = collections.defaultdict(list)
    for face in mesh.polygons:
        for key in face.edge_keys:
            incident[tuple(sorted(key))].append(face.index)
    adjacency = [[] for _ in mesh.polygons]
    for faces in incident.values():
        for face in faces:
            adjacency[face].extend(f for f in faces if f != face)
    unseen = set(range(len(mesh.polygons)))
    result = []
    while unseen:
        todo = [unseen.pop()]
        group = []
        while todo:
            face = todo.pop()
            group.append(face)
            for neighbor in adjacency[face]:
                if neighbor in unseen:
                    unseen.remove(neighbor)
                    todo.append(neighbor)
        result.append(group)
    return result


def local_topology(obj):
    vertices, faces = mesh_triangles(obj)
    edge_faces = collections.defaultdict(list)
    edge_directions = collections.Counter()
    incident = collections.defaultdict(list)
    duplicate = 0
    seen = set()
    degenerate = 0
    for index, face in enumerate(faces):
        key = tuple(sorted(int(x) for x in face))
        duplicate += key in seen
        seen.add(key)
        if len(set(face)) != 3 or np.linalg.norm(np.cross(
                vertices[face[1]] - vertices[face[0]],
                vertices[face[2]] - vertices[face[0]])) <= 1e-20:
            degenerate += 1
        for vertex in face:
            incident[int(vertex)].append(index)
        for a, b in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            edge_faces[tuple(sorted((int(a), int(b))))].append(index)
            edge_directions[tuple(sorted((int(a), int(b))))] += 1 if a < b else -1
    invalid_links = 0
    for vertex, face_ids in incident.items():
        link = collections.defaultdict(list)
        for index in face_ids:
            others = [int(v) for v in faces[index] if v != vertex]
            if len(others) == 2:
                a, b = others
                link[a].append(b)
                link[b].append(a)
        if not link:
            invalid_links += 1
            continue
        pending = {next(iter(link))}
        visited = set()
        while pending:
            a = pending.pop()
            if a not in visited:
                visited.add(a)
                pending.update(link[a])
        degree_one = sum(len(neighbors) == 1 for neighbors in link.values())
        if (len(visited) != len(link) or any(len(n) > 2 for n in link.values())
                or degree_one not in (0, 2)):
            invalid_links += 1
    return {"vertices": len(vertices), "triangles": len(faces),
            "faces": len(obj.data.polygons),
            "quad_faces": sum(len(p.vertices) == 4 for p in obj.data.polygons),
            "duplicate_faces": int(duplicate), "degenerate_faces": degenerate,
            "overfull_edges": sum(len(f) > 2 for f in edge_faces.values()),
            "inconsistent_winding_edges": sum(len(f) == 2 and edge_directions[e] != 0
                                              for e, f in edge_faces.items()),
            "boundary_edges": sum(len(f) == 1 for f in edge_faces.values()),
            "invalid_vertex_links": invalid_links,
            "euler_characteristic": len(incident) - len(edge_faces) + len(faces),
            "signed_volume": float(np.einsum('ij,ij->i', vertices[faces[:, 0]],
                                    np.cross(vertices[faces[:, 1]], vertices[faces[:, 2]])).sum() / 6),
            "components": len(component_faces(obj.data))}


def write_binary(objects, path):
    arrays, face_arrays = [], []
    offset = 0
    for obj in objects:
        vertices, faces = mesh_triangles(obj)
        arrays.append(vertices.astype('<f4'))
        face_arrays.append((faces + offset).astype('<i4'))
        offset += len(vertices)
    vertices = np.concatenate(arrays)
    faces = np.concatenate(face_arrays)
    with open(path, 'wb') as stream:
        stream.write(struct.pack('<II', len(vertices), len(faces)))
        stream.write(vertices.tobytes())
        stream.write(faces.tobytes())


def audit(objects, label):
    progress('audit', 8, 'Checking topology and intersections: ' + label)
    per_part = [local_topology(o) for o in objects]
    for part in per_part:
        for key in ('duplicate_faces', 'degenerate_faces', 'overfull_edges', 'inconsistent_winding_edges',
                    'invalid_vertex_links'):
            if part[key]:
                raise Rejected(f'{label}: {key}={part[key]}; no destructive repair attempted.')
    binary = STAGE / (label + '.bin')
    result_file = STAGE / (label + '-audit.json')
    write_binary(objects, binary)
    if not AUDIT or not Path(AUDIT).is_file():
        raise Rejected('The independent native intersection checker is required.')
    run = subprocess.run([AUDIT, str(binary), str(result_file)],
                         capture_output=True, text=True, timeout=65)
    if run.returncode != 0:
        raise Rejected(f'{label}: native safety checker failed (exit {run.returncode}): {run.stderr[-1000:]}')
    result = json.loads(result_file.read_text(encoding='utf-8-sig'))
    # The native checker is fail-closed. Unknown schemas are rejected.
    if result.get('complete') is not True:
        raise Rejected(f'{label}: native checker did not complete; inspect the saved audit.')
    pairs = result.get('pair_classification', {})
    stars = result.get('edge_star_classification', {})
    if (not isinstance(pairs, dict) or not isinstance(stars, dict) or
            result.get('faces') != sum(p['triangles'] for p in per_part) or
            result.get('vertices') != sum(p['vertices'] for p in per_part)):
        raise Rejected(f'{label}: native audit schema/counts do not match the input.')
    invalid = {k: pairs.get(k, 0) for k in ('transverse_crossing', 'coplanar_area_overlap',
                                            'edge_face_contact', 'tolerance_sensitive')}
    invalid['crossing_sheet_stars'] = stars.get('crossing_sheet_stars', 0)
    invalid['angular_tie_or_degenerate'] = stars.get('angular_tie_or_degenerate', 0)
    if any(invalid.values()):
        raise Rejected(f'{label}: native safety gate rejected geometry: {invalid}; inspect the saved audit.')
    return {"parts": per_part, "native": result,
            "triangles": sum(p['triangles'] for p in per_part),
            "vertices": sum(p['vertices'] for p in per_part),
            "components": sum(p['components'] for p in per_part),
            "boundary_edges": sum(p['boundary_edges'] for p in per_part)}


def gltf_json(path):
    if path.suffix.lower() == '.glb':
        with path.open('rb') as stream:
            magic, version, length = struct.unpack('<III', stream.read(12))
            if magic != 0x46546c67 or version != 2 or length != path.stat().st_size:
                raise Rejected('Invalid GLB container.')
            chunk_length, chunk_type = struct.unpack('<II', stream.read(8))
            if chunk_type != 0x4e4f534a or chunk_length > 16 * 1024 * 1024:
                raise Rejected('Unsupported GLB JSON chunk.')
            return json.loads(stream.read(chunk_length))
    if path.suffix.lower() == '.gltf':
        return json.loads(path.read_text(encoding='utf-8'))
    return None


def validate_input_document(path):
    document = gltf_json(path)
    if not document:
        return
    if document.get('animations') or document.get('skins'):
        raise Rejected('Animated or skinned input requires a separate rig-preserving workflow.')
    supported = {'KHR_texture_transform', 'KHR_materials_unlit'}
    unsupported = set(document.get('extensionsUsed', [])) - supported
    if unsupported:
        raise Rejected('Material/compression extensions requiring unsupported transfer: '
                       + ', '.join(sorted(unsupported)))
    if any(p.get('targets') for m in document.get('meshes', [])
           for p in m.get('primitives', [])):
        raise Rejected('Morph targets are not supported by this offline geometry operation.')
    for mesh in document.get('meshes', []):
        for primitive in mesh.get('primitives', []):
            if primitive.get('mode', 4) != 4:
                raise Rejected('Only triangle mesh primitives are supported.')


def import_asset(path):
    validate_input_document(path)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    suffix = path.suffix.lower()
    if suffix in ('.glb', '.gltf'):
        bpy.ops.import_scene.gltf(filepath=str(path), merge_vertices=False)
    elif suffix == '.obj':
        bpy.ops.wm.obj_import(filepath=str(path))
    elif suffix == '.ply':
        bpy.ops.wm.ply_import(filepath=str(path))
    else:
        raise Rejected('Supported inputs are GLB, glTF, OBJ and PLY.')
    objects = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if not objects:
        raise Rejected('The input contains no mesh.')
    for obj in objects:
        if obj.data.shape_keys or obj.find_armature():
            raise Rejected('Rigged or shape-key geometry is unsupported.')
        select([obj], obj)
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    total = sum(len(o.data.vertices) for o in objects)
    triangles = sum(len(mesh_triangles(o)[1]) for o in objects)
    if total > 100000 or triangles > 200000:
        raise Rejected('The bounded offline profile supports at most 100,000 input vertices '
                       'and 200,000 triangles. Split larger assets explicitly.')
    if not all(np.isfinite(mesh_triangles(o)[0]).all() for o in objects):
        raise Rejected('Non-finite coordinates in input.')
    return objects


def split_components(objects):
    result = []
    for obj in objects:
        groups = component_faces(obj.data)
        if len(groups) == 1:
            result.append(obj)
            continue
        for number, group in enumerate(groups):
            copy = obj.copy()
            copy.data = obj.data.copy()
            bpy.context.collection.objects.link(copy)
            copy.name = f'{obj.name}_component_{number:03}'
            keep = set(group)
            bm = bmesh.new()
            bm.from_mesh(copy.data)
            bm.faces.ensure_lookup_table()
            bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.index not in keep],
                             context='FACES')
            bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces],
                             context='VERTS')
            bm.to_mesh(copy.data)
            bm.free()
            copy.data.update()
            result.append(copy)
        bpy.data.objects.remove(obj, do_unlink=True)
    return result


def source_bvh(obj):
    vertices, faces = mesh_triangles(obj)
    return BVHTree.FromPolygons([Vector(v) for v in vertices], faces.tolist(),
                                all_triangles=True, epsilon=0.0)


def sample_surface(obj, cap=24000):
    vertices, faces = mesh_triangles(obj)
    # Every vertex and deterministic face/edge probes for small assets. Large
    # assets use a fixed bounded sample; this is explicitly a sampled gate.
    points = [Vector(v) for v in vertices[::max(1, len(vertices) // (cap // 4))]]
    stride = max(1, len(faces) // (cap // 4))
    for face in faces[::stride]:
        a, b, c = [Vector(vertices[i]) for i in face]
        points.extend(((a + b + c) / 3, (a + b) / 2, (b + c) / 2, (c + a) / 2))
    return points[:cap]


def sharp_segments(obj, threshold=math.radians(35)):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    segments = []
    for edge in bm.edges:
        if (edge.is_boundary or not edge.smooth or
                (len(edge.link_faces) == 2 and edge.calc_face_angle(0) >= threshold)):
            segments.append((edge.verts[0].co.copy(), edge.verts[1].co.copy()))
    bm.free()
    return segments


def feature_distance(source, candidate, diagonal):
    a = sharp_segments(source)
    if not a:
        return 0.0
    b = sharp_segments(candidate)
    if not b:
        return float('inf')
    samples = [u.lerp(v, t) for u, v in b for t in (0, .25, .5, .75, 1)]
    tree = KDTree(len(samples))
    for index, point in enumerate(samples):
        tree.insert(point, index)
    tree.balance()
    return max(tree.find(u.lerp(v, t))[2] for u, v in a
               for t in (0, .25, .5, .75, 1)) / diagonal


def surface_quality(source, candidate, diagonal):
    distances = []
    for obj, other in ((source, candidate), (candidate, source)):
        tree = source_bvh(other)
        for point in sample_surface(obj):
            hit = tree.find_nearest(point)
            if hit[0] is None:
                raise Rejected('Surface projection could not find its original component.')
            distances.append(hit[3] / diagonal)
    before = local_topology(source)
    after = local_topology(candidate)
    return {"maximum_surface_deviation": max(distances, default=0),
            "rms_surface_deviation": math.sqrt(sum(x*x for x in distances) /
                                                max(1, len(distances))),
            "sample_count": len(distances),
            "feature_deviation": feature_distance(source, candidate, diagonal),
            "source_boundary": before['boundary_edges'] > 0,
            "output_boundary": after['boundary_edges'] > 0,
            "source_euler_characteristic": before['euler_characteristic'],
            "output_euler_characteristic": after['euler_characteristic'],
            "orientation_preserved": (before['boundary_edges'] > 0 or
                                      before['signed_volume'] * after['signed_volume'] > 0),
            "components_preserved": before['components'] == after['components']}


def make_obj(obj, path, triangulate=False):
    with path.open('w', encoding='ascii') as stream:
        for vertex in obj.data.vertices:
            stream.write('v %.9g %.9g %.9g\n' % tuple(vertex.co))
        if triangulate:
            obj.data.calc_loop_triangles()
        for polygon in (obj.data.loop_triangles if triangulate else obj.data.polygons):
            stream.write('f ' + ' '.join(str(i+1) for i in polygon.vertices) + '\n')


def read_obj(path, name):
    vertices, faces = [], []
    for line in path.read_text(encoding='utf-8').splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == 'v':
            vertices.append(tuple(float(x) for x in words[1:4]))
        elif words[0] == 'f':
            faces.append(tuple(int(w.split('/')[0])-1 for w in words[1:]))
    if not vertices or not faces or len(vertices) > 150000 or len(faces) > 150000:
        raise Rejected('The remesher returned an empty or excessive mesh.')
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def copy_object(source, name):
    obj = source.copy()
    obj.data = source.data.copy()
    obj.name = name
    bpy.context.collection.objects.link(obj)
    return obj


def resolve_exact_seams(objects):
    """Rejoin exporter seam vertices only when a closed manifold is established.

    Exact coincident positions alone cannot establish the identity of separate
    open sheets. Those ambiguous inputs fail closed instead of losing an
    intentional boundary or component. UV/color values remain per corner.
    """
    resolved, notices = [], []
    for source in objects:
        positions = [tuple(vertex.co) for vertex in source.data.vertices]
        if len(set(positions)) == len(positions):
            resolved.append(source)
            continue
        mesh = source.data
        corner_positions = [[tuple(mesh.vertices[mesh.loops[i].vertex_index].co)
                             for i in face.loop_indices] for face in mesh.polygons]
        custom_normals = ([tuple(normal.vector) for normal in mesh.corner_normals]
                          if mesh.has_custom_normals else None)
        uv_values = {layer.name: [tuple(value.uv) for value in layer.data]
                     for layer in mesh.uv_layers}
        colors = {layer.name: (layer.data_type, layer.domain,
                  [tuple(layer.data[loop.vertex_index if layer.domain == 'POINT'
                                         else loop.index].color) for loop in mesh.loops])
                  for layer in mesh.color_attributes}
        color_names = [layer.name for layer in mesh.color_attributes]
        active_index, render_index = (mesh.color_attributes.active_color_index,
                                      mesh.color_attributes.render_color_index)
        active_color = color_names[active_index] if 0 <= active_index < len(color_names) else None
        render_color = color_names[render_index] if 0 <= render_index < len(color_names) else None
        candidate = copy_object(source, source.name + '_ExactSeams')
        bm = bmesh.new()
        try:
            bm.from_mesh(candidate.data)
            first, targetmap = {}, {}
            for vertex in bm.verts:
                position = tuple(vertex.co)
                if position in first:
                    targetmap[vertex] = first[position]
                else:
                    first[position] = vertex
            bmesh.ops.weld_verts(bm, targetmap=targetmap)
            bm.to_mesh(candidate.data)
        finally:
            bm.free()
        candidate.data.update()
        # BMesh can discard split normals or average POINT colors while welding.
        # Restore corner values only when their exact geometric identity survives.
        rebuilt = candidate.data
        rebuilt_positions = [[tuple(rebuilt.vertices[rebuilt.loops[i].vertex_index].co)
                              for i in face.loop_indices] for face in rebuilt.polygons]
        if rebuilt_positions != corner_positions or any(
                name not in rebuilt.uv_layers or
                [tuple(value.uv) for value in rebuilt.uv_layers[name].data] != values
                for name, values in uv_values.items()):
            bpy.data.objects.remove(candidate, do_unlink=True)
            raise Rejected('Exact seam reconstruction changed face-corner or UV identity; source retained.')
        for name, (data_type, _domain, values) in colors.items():
            layer = rebuilt.color_attributes.get(name)
            if layer is None or layer.domain != 'CORNER':
                if layer is not None:
                    rebuilt.color_attributes.remove(layer)
                layer = rebuilt.color_attributes.new(name=name, type=data_type, domain='CORNER')
            layer.data.foreach_set('color', [component for value in values for component in value])
        if colors:
            rebuilt_names = [layer.name for layer in rebuilt.color_attributes]
            if active_color is not None:
                rebuilt.color_attributes.active_color_index = rebuilt_names.index(active_color)
            if render_color is not None:
                rebuilt.color_attributes.render_color_index = rebuilt_names.index(render_color)
        if custom_normals is not None:
            rebuilt.normals_split_custom_set(custom_normals)
        rebuilt.update()
        topology = local_topology(candidate)
        if (topology['boundary_edges'] or any(topology[key] for key in
                ('duplicate_faces', 'degenerate_faces', 'overfull_edges',
                 'invalid_vertex_links', 'inconsistent_winding_edges'))):
            bpy.data.objects.remove(candidate, do_unlink=True)
            raise Rejected('Coincident input vertices are ambiguous open/contact geometry; '
                           'exact seam reconstruction did not establish closed manifold components.')
        notices.append({"object": source.name, "method": "Exact seam reconstruction on a copy",
                        "vertices_before": len(source.data.vertices),
                        "vertices_after": len(candidate.data.vertices),
                        "custom_corner_normals_preserved": custom_normals is not None,
                        "corner_color_layers_preserved": len(colors),
                        "components_after": topology['components']})
        bpy.data.objects.remove(source, do_unlink=True)
        resolved.append(candidate)
    return resolved, notices


def geometry_preserving_quads(source, name):
    obj = copy_object(source, name)
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    triangles = [f for f in bm.faces if len(f.verts) == 3]
    if triangles:
        bmesh.ops.join_triangles(bm, faces=triangles, cmp_seam=True, cmp_sharp=True,
                                cmp_uvs=True, cmp_vcols=True, cmp_materials=True,
                                angle_face_threshold=math.radians(1),
                                angle_shape_threshold=math.radians(40))
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return obj


def remesh(source, target_quads, name, attempt):
    material_ids = set(p.material_index for p in source.data.polygons)
    # Conservatively retain topology and UVs around multi-material boundaries,
    # open surfaces and hard edges. No welding, capping or component union.
    features = sharp_segments(source)
    quadwild = REQUEST.get('backend') == 'quadwild-bimdf'
    if quadwild and (len(material_ids) > 1 or local_topology(source)['boundary_edges']):
        raise Rejected('QuadWild material-boundary/open-surface preservation is not yet validated; '
                       'the original is retained without substituting a different remesher.')
    if len(material_ids) > 1 or local_topology(source)['boundary_edges']:
        obj = geometry_preserving_quads(source, name)
        return obj, 'geometry-preserving Blender triangle pairing', True
    executable = REQUEST.get('remesher_exe')
    if quadwild:
        in_path = STAGE / (name + '-input.obj')
        make_obj(source, in_path, triangulate=True)
        out_path, engine_metrics = quadwild_remesh(executable or '', in_path, target_quads,
            mechanical=bool(features) and REQUEST.get('preserve_sharp_edges', True),
            seconds=min(180, float(REQUEST.get('max_seconds', 600))))
        DIAGNOSTICS.append({'stage': 'quadwild', 'component': name, **engine_metrics})
        obj = read_obj(out_path, name)
        backend = 'QuadWild-BiMDF / Lemon'
    elif executable:
        executable = Path(executable)
        if not executable.is_file():
            raise Rejected('The configured AutoRemesher executable is missing.')
        in_path, out_path = STAGE / (name + '-input.obj'), STAGE / (name + '-output.obj')
        make_obj(source, in_path)
        command = [str(executable), '--input', str(in_path), '--output', str(out_path),
                   '--report', str(STAGE / (name + '-remesher.json')),
                   '--target-quads', str(max(8, target_quads)), '--edge-scaling', '1',
                   '--sharp-edge', '35', '--smooth-normal', '35',
                   '--adaptivity', '1', '--anisotropy', '0.5']
        run = subprocess.run(command, capture_output=True, text=True,
                             timeout=min(180, float(REQUEST.get('max_seconds', 600))))
        (STAGE / (name + '-remesher.log')).write_text(run.stdout + run.stderr,
                                                    encoding='utf-8')
        if run.returncode != 0 or not out_path.is_file():
            raise Rejected('AutoRemesher did not produce a valid result; inspect its log.')
        obj = read_obj(out_path, name)
        backend = 'AutoRemesher 1.2'
    else:
        obj = copy_object(source, name)
        select([obj], obj)
        bpy.ops.object.quadriflow_remesh(use_mesh_symmetry=False,
                                        use_preserve_sharp=bool(REQUEST.get('preserve_sharp_edges', True)),
                                        use_preserve_boundary=True,
                                        preserve_attributes=False,
                                        mode='FACES', target_faces=max(8, target_quads),
                                        seed=0)
        backend = 'Blender QuadriFlow'
    tree = source_bvh(source)
    limit = deviation_limit()
    vertices, _ = mesh_triangles(source)
    diagonal = float(np.linalg.norm(vertices.max(axis=0) - vertices.min(axis=0)))
    for vertex in obj.data.vertices:
        hit = tree.find_nearest(vertex.co)
        if hit[0] is None or hit[3] > max(limit * diagonal * 4, 1e-7):
            raise Rejected('The remesher moved a vertex outside its constrained source surface.')
        vertex.co = hit[0]
    obj.data.update()
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    return obj, backend, False


def principle(material):
    if not material or not material.use_nodes:
        return None
    return next((n for n in material.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)


def bake_channel(source, target, source_material, target_material, socket_name,
                 image_name, size, normal=False, source_socket_path=None):
    image = bpy.data.images.new(image_name, width=size, height=size, alpha=True,
                                float_buffer=False)
    image.colorspace_settings.name = 'sRGB' if socket_name in ('Base Color', 'Emission Color') else 'Non-Color'
    nodes = target_material.node_tree.nodes
    texture = nodes.new('ShaderNodeTexImage')
    texture.image = image
    texture.interpolation = 'Closest' if REQUEST.get('pixel_art', False) else 'Linear'
    nodes.active = texture
    original = list(source.data.materials)
    original_indices = [polygon.material_index for polygon in source.data.polygons]
    visibility = [(obj, obj.hide_render) for obj in bpy.context.scene.objects if obj.type == 'MESH']
    temporary = None
    try:
        if not normal:
            temporary = source_material.copy()
            temporary.name = image_name + '_source'
            src_bsdf = principle(temporary)
            if source_socket_path is not None:
                node_name, socket_index = source_socket_path
                source_node = temporary.node_tree.nodes.get(node_name)
                if source_node is None or socket_index >= len(source_node.inputs):
                    raise Rejected('The identified source texture socket could not be copied.')
                socket = source_node.inputs[socket_index]
            else:
                if not src_bsdf or socket_name not in src_bsdf.inputs:
                    raise Rejected('The source material graph cannot be transferred safely.')
                socket = src_bsdf.inputs[socket_name]
            output = next(n for n in temporary.node_tree.nodes if n.type == 'OUTPUT_MATERIAL')
            emission = temporary.node_tree.nodes.new('ShaderNodeEmission')
            if socket.is_linked:
                temporary.node_tree.links.new(socket.links[0].from_socket, emission.inputs['Color'])
            else:
                value = socket.default_value
                emission.inputs['Color'].default_value = ((value, value, value, 1)
                    if isinstance(value, (float, int)) else tuple(value))
            temporary.node_tree.links.new(emission.outputs[0], output.inputs['Surface'])
            source.data.materials.clear()
            source.data.materials.append(temporary)
            for polygon in source.data.polygons:
                polygon.material_index = 0
        # Previous comparisons hide high sources. Bake the intended pair only,
        # and refresh render visibility before building the bake dependency graph.
        for obj, _ in visibility:
            obj.hide_render = obj not in (source, target)
        select([source, target], target)
        bpy.context.view_layer.update()
        bpy.ops.object.bake(type='NORMAL' if normal else 'EMIT',
                            use_clear=True, margin=min(16, size // 8))
    finally:
        for obj, hidden in visibility:
            obj.hide_render = hidden
        source.data.materials.clear()
        for material in original:
            source.data.materials.append(material)
        for polygon, material_index in zip(source.data.polygons, original_indices):
            polygon.material_index = material_index
        if temporary:
            bpy.data.materials.remove(temporary)
    image.filepath_raw = str(STAGE / (image_name + '.png'))
    image.file_format = 'PNG'
    image.save()
    return image, texture


def transfer_material(source, target, label, unchanged):
    if unchanged:
        return {"method": "Original UVs, PBR nodes and color attributes preserved",
                "normal_baked": False, "textures": 0}
    material_ids = {polygon.material_index for polygon in source.data.polygons}
    if len(material_ids) != 1:
        raise Rejected('A rebaked component must use one identified material.')
    material_id = next(iter(material_ids))
    source_material = (source.data.materials[material_id]
                       if material_id < len(source.data.materials) else None)
    if source_material is None:
        source_material = bpy.data.materials.new(label + '_SourceMaterial')
        source_material.use_nodes = True
        source.data.materials.append(source_material)
    src_bsdf = principle(source_material)
    if not src_bsdf:
        raise Rejected('Only supported glTF Principled PBR material graphs can be rebaked.')
    ao_nodes = [node for node in source_material.node_tree.nodes
                if node.type == 'GROUP' and node.inputs.get('Occlusion') is not None
                and node.inputs['Occlusion'].is_linked]
    ao_path, ao_strength = None, None
    if ao_nodes:
        if len(ao_nodes) != 1 or ao_nodes[0].node_tree.name not in (
                'glTF Material Output', 'glTF Settings'):
            raise Rejected('Unsupported or ambiguous occlusion material graph.')
        ao_node = ao_nodes[0]
        ao_socket = ao_node.inputs['Occlusion']
        ao_strength = 1.0
        upstream = ao_socket.links[0].from_node
        # Preserve the importer/exporter's white-to-AO strength mix verbatim.
        # Only its raw texture input is rebaked, avoiding a second application
        # of strength after the new image is connected to the copied mix.
        if upstream.type == 'MIX':
            if (upstream.data_type != 'RGBA' or upstream.blend_type != 'MIX'
                    or upstream.inputs['Factor'].is_linked
                    or upstream.inputs[6].is_linked
                    or tuple(upstream.inputs[6].default_value) != (1, 1, 1, 1)
                    or not upstream.inputs[7].is_linked):
                raise Rejected('Unsupported occlusion strength graph.')
            ao_strength = float(upstream.inputs['Factor'].default_value)
            if not 0 <= ao_strength <= 1:
                raise Rejected('Occlusion strength must be in [0, 1].')
            ao_node, ao_socket = upstream, upstream.inputs[7]
            upstream = ao_socket.links[0].from_node
        if (upstream.type != 'SEPARATE_COLOR' or upstream.mode != 'RGB'
                or ao_socket.links[0].from_socket.name != 'Red'
                or not upstream.inputs['Color'].is_linked
                or upstream.inputs['Color'].links[0].from_node.type != 'TEX_IMAGE'
                or upstream.inputs['Color'].links[0].from_node.image is None):
            raise Rejected('Only glTF texture-red occlusion graphs are supported for rebaking.')
        ao_path = (ao_node.name, list(ao_node.inputs).index(ao_socket))
    select([target], target)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    size = int(REQUEST.get('texture_size', 1024))
    if size not in (128, 256, 512, 1024, 2048):
        raise Rejected('Texture size must be 128, 256, 512, 1024 or 2048.')
    bpy.ops.uv.smart_project(angle_limit=math.radians(66),
                             island_margin=min(.1, 18.0 / size),
                             area_weight=0, correct_aspect=True, scale_to_bounds=False)
    # Keep both 16-pixel bake gutters apart. FRACTION is measured in final UV
    # space rather than scaled by each island, unlike smart-project's margin.
    bpy.ops.uv.pack_islands(margin_method='FRACTION', margin=min(.3, 36.0 / size),
                            rotate=True, scale=True)
    bpy.ops.object.mode_set(mode='OBJECT')
    material = source_material.copy()
    material.name = label + '_BakedPBR'
    target.data.materials.clear()
    target.data.materials.append(material)
    for polygon in target.data.polygons:
        polygon.material_index = 0
    bsdf = principle(material)
    textures = 0
    for channel in ('Base Color', 'Roughness', 'Metallic', 'Emission Color', 'Alpha'):
        if not src_bsdf.inputs[channel].is_linked:
            continue
        image, texture = bake_channel(source, target, source_material, material,
                                      channel, label + '_' + channel.replace(' ', ''), size)
        textures += 1
        material.node_tree.links.new(texture.outputs['Color'], bsdf.inputs[channel])
        if channel == 'Alpha':
            # glTF needs base-color alpha. Pack the scalar alpha bake into the
            # base-color texture instead of relying on an unrelated alpha image.
            base_texture = next((n for n in material.node_tree.nodes
                                 if n.type == 'TEX_IMAGE' and n.image and
                                 n.image.name == label + '_BaseColor'), None)
            if base_texture:
                rgba = np.array(base_texture.image.pixels[:], dtype=np.float32).reshape((-1, 4))
                alpha = np.array(image.pixels[:], dtype=np.float32).reshape((-1, 4))[:, 0]
                rgba[:, 3] = alpha
                base_texture.image.pixels[:] = rgba.ravel()
                base_texture.image.save()
                material.node_tree.links.new(base_texture.outputs['Alpha'], bsdf.inputs['Alpha'])
            else:
                raise Rejected('Textured transparency requires a base-color texture.')
    if ao_path is not None:
        image, texture = bake_channel(source, target, source_material, material,
                                      'Occlusion', label + '_Occlusion', size,
                                      source_socket_path=ao_path)
        destination = material.node_tree.nodes[ao_path[0]].inputs[ao_path[1]]
        material.node_tree.links.new(texture.outputs['Color'], destination)
        textures += 1
    image, texture = bake_channel(source, target, source_material, material,
                                  'Normal', label + '_Normal', size, normal=True)
    normal = material.node_tree.nodes.new('ShaderNodeNormalMap')
    material.node_tree.links.new(texture.outputs['Color'], normal.inputs['Color'])
    material.node_tree.links.new(normal.outputs['Normal'], bsdf.inputs['Normal'])
    textures += 1
    # glTF uses the image sampler, including nearest-neighbor pixel-art intent.
    if any(n.type == 'TEX_IMAGE' and n.interpolation == 'Closest'
           for n in source_material.node_tree.nodes):
        for node in material.node_tree.nodes:
            if node.type == 'TEX_IMAGE':
                node.interpolation = 'Closest'
    return {"method": "Cycles CPU selected-to-active PBR/normal bake",
            "normal_baked": True, "textures": textures, "texture_size": size,
            "occlusion_baked": ao_path is not None, "occlusion_strength": ao_strength,
            "padding_pixels": min(16, size // 8)}


def configure_scene(diagonal):
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = 12
    scene.render.threads_mode = 'FIXED'
    scene.render.threads = 2
    scene.render.bake.use_selected_to_active = True
    scene.render.bake.use_cage = False
    scene.render.bake.cage_extrusion = diagonal * .02
    scene.render.bake.max_ray_distance = diagonal * .04
    scene.render.bake.margin_type = 'EXTEND'
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'Medium High Contrast' if False else 'None'
    scene.view_settings.exposure = 0
    scene.view_settings.gamma = 1
    scene.render.film_transparent = True
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs[0].default_value = (.35, .35, .35, 1)
    scene.world.node_tree.nodes['Background'].inputs[1].default_value = .8
    scene.render.resolution_x = 192
    scene.render.resolution_y = 192
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.image_settings.color_depth = '8'
    camera_data = bpy.data.cameras.new('SmartQuadCamera')
    camera = bpy.data.objects.new('SmartQuadCamera', camera_data)
    bpy.context.collection.objects.link(camera)
    camera_data.type = 'ORTHO'
    camera_data.ortho_scale = diagonal * 1.1
    scene.camera = camera
    for location, power in (((2, -3, 4), 700), ((-3, -1, 2), 400)):
        light_data = bpy.data.lights.new('SmartQuadLight', 'AREA')
        light_data.energy = power
        light_data.size = diagonal * 2
        light = bpy.data.objects.new('SmartQuadLight', light_data)
        bpy.context.collection.objects.link(light)
        light.location = Vector(location) * diagonal
        light.rotation_euler = (-light.location).to_track_quat('-Z', 'Y').to_euler()


VIEWS = ((1, -2, 1), (-1, 2, 1), (2, 1, 1), (-2, -1, 1), (0, 0, 3), (0, 0, -3))


def render_views(objects, label, center, diagonal):
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    selected = set(objects)
    for obj in meshes:
        obj.hide_render = obj not in selected
    scene = bpy.context.scene
    arrays = []
    for index, direction in enumerate(VIEWS):
        progress('compare', 70 + index, f'Rendering identical camera {index + 1}/6: {label}')
        scene.camera.location = center + Vector(direction).normalized() * diagonal * 3
        scene.camera.rotation_euler = (center - scene.camera.location).to_track_quat('-Z', 'Y').to_euler()
        scene.render.filepath = str(STAGE / f'{label}-view-{index}.png')
        bpy.ops.render.render(write_still=True)
        image = bpy.data.images.load(scene.render.filepath, check_existing=False)
        array = np.array(image.pixels[:], dtype=np.float32).reshape((192, 192, 4))
        arrays.append(array.copy())
        bpy.data.images.remove(image)
    return arrays


def compare_renders(before, after, label):
    comparisons = []
    for a, b in zip(before, after):
        a_mask, b_mask = a[:, :, 3] > .5, b[:, :, 3] > .5
        intersection = int(np.count_nonzero(a_mask & b_mask))
        union = int(np.count_nonzero(a_mask | b_mask))
        mask = a_mask & b_mask
        delta = np.abs(a[:, :, :3] - b[:, :, :3])[mask]
        comparisons.append({"silhouette_iou": intersection / union if union else 1.0,
                            "color_mean_absolute_error": float(delta.mean()) if delta.size else 0,
                            "color_p95_absolute_error": float(np.quantile(delta, .95)) if delta.size else 0})
    combined = np.concatenate((np.concatenate(before, axis=1),
                               np.concatenate(after, axis=1)), axis=0)
    image = bpy.data.images.new(label, width=192*6, height=192*2, alpha=True)
    image.pixels[:] = combined.ravel()
    image.filepath_raw = str(STAGE / (label + '.png'))
    image.file_format = 'PNG'
    image.save()
    minimum_iou = min(v['silhouette_iou'] for v in comparisons)
    max_color_mae = max(v['color_mean_absolute_error'] for v in comparisons)
    if minimum_iou < .97:
        raise Rejected(f'{label}: silhouette gate failed ({minimum_iou:.4f} < 0.97).')
    if REQUEST.get('preserve_materials', True) and max_color_mae > .10:
        raise Rejected(f'{label}: material appearance gate failed ({max_color_mae:.4f} > 0.10).')
    return {"views": comparisons, "minimum_silhouette_iou": minimum_iou,
            "maximum_color_mae": max_color_mae, "sheet": str(STAGE / (label + '.png'))}


def glb_counts(path):
    document = gltf_json(path)
    triangles = vertices = 0
    for mesh in document.get('meshes', []):
        for primitive in mesh['primitives']:
            if primitive.get('mode', 4) != 4 or 'indices' not in primitive:
                raise Rejected('The exported GLB is not an indexed triangle mesh.')
            triangles += document['accessors'][primitive['indices']]['count'] // 3
            vertices += document['accessors'][primitive['attributes']['POSITION']]['count']
            if REQUEST.get('preserve_materials', True) and 'material' not in primitive:
                raise Rejected('The exported primitive lost its material.')
    return {"triangles": triangles, "vertices": vertices,
            "materials": len(document.get('materials', [])),
            "images": len(document.get('images', [])),
            "uv_primitives": sum('TEXCOORD_0' in p['attributes'] for m in document.get('meshes', [])
                                 for p in m['primitives'])}


def export_game(objects, path, expected):
    select(objects, objects[0])
    for obj in objects:
        obj.hide_render = False
    bpy.ops.export_scene.gltf(filepath=str(path), export_format='GLB',
                              use_selection=True, export_yup=True,
                              export_apply=False, export_texcoords=True,
                              export_normals=True, export_tangents=True,
                              export_materials='EXPORT', export_vertex_color='MATERIAL',
                              export_all_vertex_colors=True,
                              export_animations=False, export_skins=False,
                              export_morph=False, export_cameras=False,
                              export_lights=False)
    counts = glb_counts(path)
    existing = set(bpy.data.objects)
    checker = REQUEST.get('check_exe')
    if checker:
        check_path = STAGE / (path.stem + '-glb-check.json')
        run = subprocess.run([str(checker), str(path), str(check_path)],
                             capture_output=True, text=True, timeout=60)
        if run.returncode or not check_path.is_file():
            raise Rejected('The independent GLB structural checker failed: ' + run.stderr[-1000:])
        check = json.loads(check_path.read_text(encoding='utf-8-sig'))
        if check.get('ok') is not True:
            raise Rejected('The independent GLB structural checker rejected the export.')
        counts['independent_check'] = check
    bpy.ops.import_scene.gltf(filepath=str(path), merge_vertices=False)
    roundtrip = [o for o in bpy.data.objects if o not in existing and o.type == 'MESH']
    # Preserve the untouched exported face soup first. Exact seam reconstruction
    # on the imported COPY is permitted only for closed manifold components;
    # serialized vertices and UV/material splits are counted separately above.
    for obj in roundtrip:
        select([obj], obj)
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    audit(roundtrip, path.stem + '-roundtrip-face-soup')
    roundtrip, seam_reconstruction = resolve_exact_seams(roundtrip)
    try:
        # Validate each imported triangle and all geometric crossings natively.
        rt = audit(roundtrip, path.stem + '-roundtrip')
        if rt['triangles'] != expected['triangles'] or counts['triangles'] != expected['triangles']:
            raise Rejected('GLB round-trip changed the actual triangle count.')
        if len(roundtrip) != len(objects):
            raise Rejected('GLB round-trip changed the disconnected object structure.')
    finally:
        for obj in roundtrip:
            bpy.data.objects.remove(obj, do_unlink=True)
        for obj in list(bpy.data.objects):
            if obj not in existing and obj.type != 'MESH':
                bpy.data.objects.remove(obj, do_unlink=True)
    return counts


def process_level(sources, budget, label, diagonal, allowed_deviation=None):
    total_area = sum(sum(p.area for p in s.data.polygons) for s in sources)
    limit = deviation_limit() if allowed_deviation is None else allowed_deviation
    if not 0 < limit <= .05:
        raise Rejected('The level surface tolerance is outside its safety range.')
    accepted, details = [], []
    for index, source in enumerate(sources):
        progress('remesh', 25 + min(index, 15), f'{label}: component {index+1}/{len(sources)}')
        area = sum(p.area for p in source.data.polygons)
        original_triangles = len(mesh_triangles(source)[1])
        base = max(8, round(budget * area / max(total_area, 1e-20) / 2))
        candidate = None
        quality = None
        last_error = ''
        for attempt, target in enumerate((base, min(original_triangles // 2, base * 2))):
            try:
                candidate, backend, unchanged = remesh(source, target,
                    f'{label}_component_{index:03}_try_{attempt}', attempt)
                topology = local_topology(candidate)
                if any(topology[k] for k in ('duplicate_faces', 'degenerate_faces', 'inconsistent_winding_edges',
                                             'overfull_edges', 'invalid_vertex_links')):
                    raise Rejected('Remesher topology is invalid.')
                fraction = topology['quad_faces'] / max(1, topology['faces'])
                if fraction < .70:
                    raise Rejected(f'Editable quad fraction {fraction:.3f} is below 0.70.')
                quality = surface_quality(source, candidate, diagonal)
                if (quality['maximum_surface_deviation'] > limit or
                        (REQUEST.get('preserve_sharp_edges', True) and quality['feature_deviation'] > limit) or
                        not quality['components_preserved'] or
                        not quality['orientation_preserved'] or
                        quality['source_euler_characteristic'] != quality['output_euler_characteristic'] or
                        quality['source_boundary'] != quality['output_boundary']):
                    raise Rejected('The component surface/feature/boundary gate failed: ' + str(quality))
                break
            except Rejected as error:
                last_error = str(error)
                if candidate:
                    bpy.data.objects.remove(candidate, do_unlink=True)
                    candidate = None
                DIAGNOSTICS.append({"level": label, "component": index,
                                    "attempt": attempt, "rejection": last_error})
        if candidate is None:
            if REQUEST.get('backend') == 'quadwild-bimdf':
                raise Rejected('QuadWild failed the quality gate; no replacement engine or '
                               'triangle-pairing fallback was applied: ' + last_error)
            # Final safe option uses Blender's existing triangle pairing only.
            candidate = geometry_preserving_quads(source, f'{label}_component_{index:03}_retained')
            topology = local_topology(candidate)
            if topology['quad_faces'] / max(1, topology['faces']) < .70:
                raise Rejected('No safe editable quad result for this component: ' + last_error)
            backend, unchanged = 'geometry-preserving Blender triangle pairing', True
            quality = surface_quality(source, candidate, diagonal)
            if (quality['maximum_surface_deviation'] > limit or
                    (REQUEST.get('preserve_sharp_edges', True) and quality['feature_deviation'] > limit) or
                    not quality['components_preserved'] or
                    not quality['orientation_preserved'] or
                    quality['source_euler_characteristic'] != quality['output_euler_characteristic'] or
                    quality['source_boundary'] != quality['output_boundary']):
                raise Rejected('The fallback surface/feature/boundary gate failed: ' + str(quality))
            DIAGNOSTICS.append({"level": label, "component": index,
                                "notice": "Original geometry retained; triangle budget may be exceeded."})
        progress('bake', 45 + min(index, 15), f'{label}: transferring PBR and normal detail')
        material = transfer_material(source, candidate, f'{label}_{index:03}', unchanged)
        details.append({"component": index, "backend": backend,
                        "quality": quality, "material_transfer": material})
        accepted.append(candidate)
    checked = audit(accepted, label + '-geometry')
    return accepted, {"components": details, "audit": checked,
                      "maximum_relative_surface_deviation_allowed": limit,
                      "budget": budget, "budget_met": checked['triangles'] <= budget}


def main():
    global REQUEST, OUT, STAGE, AUDIT
    parser = argparse.ArgumentParser()
    parser.add_argument('--request', required=True)
    parser.add_argument('--audit-exe', required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    REQUEST = json.loads(Path(args.request).read_text(encoding='utf-8-sig'))
    AUDIT = str(Path(args.audit_exe).resolve())
    input_path = Path(REQUEST['input']).resolve()
    source_document = gltf_json(input_path)
    serialized_vertices = (sum(source_document['accessors'][p['attributes']['POSITION']]['count']
                              for m in source_document.get('meshes', []) for p in m['primitives'])
                           if source_document else None)
    OUT = Path(REQUEST['output_dir']).resolve()
    if not input_path.is_file() or input_path == OUT or input_path.parent == OUT:
        raise Rejected('Input must exist outside a new output directory.')
    if OUT.exists() and any(p.name != 'request.json' for p in OUT.iterdir()):
        raise Rejected('Output directory must be new or empty; existing assets are never overwritten.')
    OUT.mkdir(parents=True, exist_ok=True)
    STAGE = OUT / '_staging'
    STAGE.mkdir()
    if shutil.disk_usage(OUT).free < 2 * 1024 ** 3:
        raise Rejected('At least 2 GiB free disk space is required before processing.')
    budget = int(REQUEST.get('triangle_budget', 3000))
    if not 32 <= budget <= 200000:
        raise Rejected('Triangle budget must be between 32 and 200,000.')
    progress('import', 2, 'Importing a non-destructive copy of the source')
    sources = import_asset(input_path)
    # Inspect the untouched indexed face soup before any seam reconstruction.
    # Coincident open sheets cannot be reliably distinguished from UV seams;
    # only exact reconstruction into closed manifold components is accepted.
    input_face_soup = audit(sources, 'input-face-soup')
    sources, seam_reconstruction = resolve_exact_seams(sources)
    input_audit = audit(sources, 'input') if seam_reconstruction else input_face_soup
    sources = split_components(sources)
    if len(sources) > 128:
        raise Rejected('More than 128 disconnected components exceeds the bounded offline profile.')
    points = np.concatenate([mesh_triangles(o)[0] for o in sources])
    low, high = points.min(axis=0), points.max(axis=0)
    diagonal = float(np.linalg.norm(high-low))
    if diagonal <= 1e-8:
        raise Rejected('The input has negligible spatial extent.')
    center = Vector((low+high)/2)
    configure_scene(diagonal)
    source_views = render_views(sources, 'Source', center, diagonal)
    candidates, level = process_level(sources, budget, 'Game', diagonal)
    game_objects, simplification = simplify_game(sources, candidates, budget, diagonal,
                                                SimpleNamespace(**globals()))
    game_audit = simplification.get('audit', level['audit'])
    game_views = render_views(game_objects, 'Game', center, diagonal)
    visual = compare_renders(source_views, game_views, 'BeforeAfter')
    progress('export', 82, 'Exporting and independently reloading the Game GLB')
    game_path = STAGE / 'Game.glb'
    game_counts = export_game(game_objects, game_path, game_audit)
    # The editable master retains actual quads, UVs, materials and baked maps.
    select(candidates, candidates[0])
    for obj in bpy.context.scene.objects:
        if obj.type == 'MESH':
            obj.hide_render = obj not in candidates
            obj.hide_set(obj not in candidates)
    for image in bpy.data.images:
        if image.source == 'FILE' and image.has_data:
            image.pack()
    for source in sources:
        source.hide_render = True
        source.hide_set(True)
    bpy.ops.wm.save_as_mainfile(filepath=str(STAGE / 'QuadMaster.blend'), check_existing=False)
    with (STAGE / 'QuadMaster.obj').open('w', encoding='ascii') as stream:
        offset = 0
        for obj in candidates:
            stream.write('o ' + obj.name.replace(' ', '_') + '\n')
            for vertex in obj.data.vertices:
                stream.write('v %.9g %.9g %.9g\n' % tuple(vertex.co))
            for face in obj.data.polygons:
                stream.write('f ' + ' '.join(str(i+offset+1) for i in face.vertices) + '\n')
            offset += len(obj.data.vertices)
    lod_paths, lod_metrics = [], []
    count = min(3, max(0, int(REQUEST.get('lods', 0))))
    if count:
        path = STAGE / 'LOD0.glb'
        shutil.copy2(game_path, path)
        lod_paths.append(path)
    previous_lod_triangles = game_counts['triangles']
    for number in range(1, count):
        progress('lod', 85 + number*3, f'Building LOD{number} independently from the source')
        objects, metrics = process_level(sources, max(32, budget // (2**number)),
                                         'LOD' + str(number), diagonal,
                                         allowed_deviation=min(.02, deviation_limit() * (2**number)))
        lod_engine = SimpleNamespace(**globals())
        lod_engine.deviation_limit = lambda: metrics['maximum_relative_surface_deviation_allowed']
        lod_game, reduction = simplify_game(sources, objects, metrics['budget'], diagonal, lod_engine)
        metrics['simplification'] = reduction
        views = render_views(lod_game, 'LOD' + str(number), center, diagonal)
        metrics['visual'] = compare_renders(source_views, views, 'LOD' + str(number) + 'Comparison')
        path = STAGE / ('LOD' + str(number) + '.glb')
        metrics['export'] = export_game(lod_game, path, reduction.get('audit', metrics['audit']))
        if metrics['export']['triangles'] >= previous_lod_triangles:
            raise Rejected('A requested LOD does not reduce the actual exported triangle count; '
                           'no redundant or lower-quality chain was accepted.')
        previous_lod_triangles = metrics['export']['triangles']
        lod_paths.append(path)
        lod_metrics.append(metrics)
        for obj in set(objects + lod_game):
            bpy.data.objects.remove(obj, do_unlink=True)
    quad_faces = sum(sum(len(p.vertices) == 4 for p in obj.data.polygons) for obj in candidates)
    face_count = sum(len(obj.data.polygons) for obj in candidates)
    files = [game_path, STAGE / 'QuadMaster.blend', STAGE / 'QuadMaster.obj',
             STAGE / 'BeforeAfter.png'] + lod_paths
    # Commit happens last. Staging diagnostics survive failure; no failed asset
    # is advertised as a deliverable. Copy retains evidence and is reversible.
    for path in files:
        shutil.copy2(path, OUT / path.name)
    report = {"schema_version": 1, "quality_pass": True, "status": "accepted",
              "message": "Validated editable quads and textured Game GLB exported.",
              "preview_sheet": str(OUT / 'BeforeAfter.png'),
              "stats": {"input_triangles": input_audit['triangles'],
                        "input_vertices": serialized_vertices or input_audit['vertices'],
                        "input_geometry_vertices": input_audit['vertices'],
                        "output_triangles": game_counts['triangles'],
                        "output_vertices": game_counts['vertices'],
                        "quad_faces": quad_faces, "quad_fraction": quad_faces / max(1, face_count),
                        "requested_triangle_budget": budget,
                        "budget_met": game_counts['triangles'] <= budget,
                        "runtime_seconds": time.monotonic() - START},
              "outputs": {"quad_master": str(OUT / 'QuadMaster.blend'),
                          "quad_obj": str(OUT / 'QuadMaster.obj'),
                          "preview": str(OUT / 'BeforeAfter.png'),
                          "game": str(OUT / 'Game.glb'),
                          "lods": [str(OUT / p.name) for p in lod_paths]},
              "source": {"file": str(input_path),
                         "sha256": hashlib.sha256(input_path.read_bytes()).hexdigest()},
              "metrics": {"input": input_audit, "input_face_soup": input_face_soup,
                          "seam_reconstruction": seam_reconstruction, "game": level,
                          "game_simplification": simplification,
                          "export": game_counts, "visual": visual, "lods": lod_metrics},
              "limitations": ["Surface and feature deviation are bounded sampled measurements, not a formal Hausdorff proof.",
                              "Hard edges, intentional open boundaries and material seams retain original topology when safe remeshing is not established.",
                              "The OBJ is geometry-only; the packed BLEND is the authoritative editable textured master.",
                              "Animations, rigs, morphs and unsupported glTF material extensions are rejected."],
              "diagnostics": DIAGNOSTICS}
    (OUT / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    progress('complete', 100, report['message'])


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        message = str(error)
        print(json.dumps({"event": "error", "message": message}), flush=True)
        if OUT and OUT.exists():
            report = {"schema_version": 1, "quality_pass": False, "status": "rejected",
                      "message": message, "stats": {"runtime_seconds": time.monotonic()-START},
                      "outputs": {}, "diagnostics": DIAGNOSTICS,
                      "traceback": traceback.format_exc()}
            (OUT / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        raise
