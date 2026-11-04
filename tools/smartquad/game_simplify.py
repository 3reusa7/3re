"""Conservative Blender QEM collapse for game copies; Quad Masters stay intact."""
import math

import bpy

_SERIAL = 0


def simplify_game(sources, masters, budget, diagonal, engine):
    global _SERIAL
    _SERIAL += 1
    label = f'GameSimplified_{_SERIAL:03}'
    before = [engine.local_topology(obj) for obj in masters]
    total = sum(part['triangles'] for part in before)
    details = []
    metrics = {'method': 'Blender DECIMATE collapse on independent mesh copies',
               'input_triangles': total, 'output_triangles': total, 'budget': budget,
               'budget_met': total <= budget, 'applied': False, 'components': details}
    if total <= budget:
        metrics['reason'] = 'Quad Master already satisfies the game triangle budget.'
        return masters, metrics
    if len(sources) != len(masters) or not math.isfinite(diagonal) or diagonal <= 0:
        metrics['reason'] = 'Source/component identity or scale is unavailable; masters retained.'
        engine.DIAGNOSTICS.append({'stage': 'game-simplification', 'notice': metrics['reason']})
        return masters, metrics
    initial_materials, initial_images = set(bpy.data.materials), set(bpy.data.images)
    clones, changed, result = [], [], list(masters)

    def drop(obj):
        mesh = obj.data
        bpy.data.objects.remove(obj, do_unlink=True)
        if mesh.users == 0:
            bpy.data.meshes.remove(mesh)

    def rollback(reason):
        for clone in clones:
            drop(clone)
        for item in details:
            if item['status'] == 'simplified':
                item['status'] = 'retained_after_rollback'
        for material in list(bpy.data.materials):
            if material not in initial_materials and material.users == 0:
                bpy.data.materials.remove(material)
        for image in list(bpy.data.images):
            if image not in initial_images and image.users == 0:
                bpy.data.images.remove(image)
        metrics.update(reason=reason, output_triangles=total, budget_met=False, applied=False)
        engine.DIAGNOSTICS.append({'stage': 'game-simplification', 'notice': reason})
        return masters, metrics

    eligible = []
    for index, (source, master, topology) in enumerate(zip(sources, masters, before)):
        reason = None
        if topology['boundary_edges'] or engine.local_topology(source)['boundary_edges']:
            reason = 'Intentional open boundaries retained exactly.'
        elif (len({p.material_index for p in source.data.polygons}) != 1
              or len({p.material_index for p in master.data.polygons}) != 1):
            reason = 'Material boundaries retained exactly.'
        elif (engine.REQUEST.get('preserve_sharp_edges', True)
              and (engine.sharp_segments(source) or engine.sharp_segments(master))):
            reason = 'Hard feature component retained; collapse constraints are not established.'
        if reason:
            details.append({'component': index, 'status': 'protected', 'reason': reason})
        else:
            eligible.append(index)
    fixed = sum(part['triangles'] for i, part in enumerate(before) if i not in eligible)
    available = max(0, budget - fixed)
    areas = {i: sum(p.area for p in masters[i].data.polygons) for i in eligible}
    area_sum = sum(areas.values())
    if not eligible or available < 4 * len(eligible) or area_sum <= 0:
        return rollback('Protected components prevent this budget; original masters retained.')
    limit = engine.deviation_limit()
    invalid = ('duplicate_faces', 'degenerate_faces', 'overfull_edges',
               'inconsistent_winding_edges', 'invalid_vertex_links')
    for index in eligible:
        source, master, original = sources[index], masters[index], before[index]
        target = max(4, min(original['triangles'], int(available * areas[index] / area_sum)))
        if target >= original['triangles']:
            details.append({'component': index, 'status': 'retained', 'reason': 'Already within allocation.'})
            continue
        clone = master.copy()
        clone.data = master.data.copy()
        clone.name = f'{label}_{index:03}'
        bpy.context.collection.objects.link(clone)
        clones.append(clone)
        try:
            engine.progress('simplify', 65, f'Simplifying game component {index + 1}')
            engine.select([clone], clone)
            modifier = clone.modifiers.new('GameOnlyQEM', 'DECIMATE')
            modifier.decimate_type = 'COLLAPSE'
            modifier.ratio = target / original['triangles']
            modifier.use_collapse_triangulate = True
            bpy.ops.object.modifier_apply(modifier=modifier.name)
            tree = engine.source_bvh(source)
            for vertex in clone.data.vertices:
                hit = tree.find_nearest(vertex.co)
                if hit[0] is None or hit[3] > limit * diagonal:
                    raise engine.Rejected('A collapsed vertex exceeded its constrained source surface.')
                vertex.co = hit[0]
            clone.data.update()
            after = engine.local_topology(clone)
            quality = engine.surface_quality(source, clone, diagonal)
            if (any(after[key] for key in invalid) or after['boundary_edges']
                    or after['components'] != original['components']
                    or after['euler_characteristic'] != original['euler_characteristic']
                    or original['signed_volume'] * after['signed_volume'] <= 0
                    or after['triangles'] >= original['triangles']
                    or quality['maximum_surface_deviation'] > limit
                    or not quality['orientation_preserved'] or not quality['components_preserved']
                    or quality['source_euler_characteristic'] != quality['output_euler_characteristic']
                    or quality['source_boundary'] != quality['output_boundary']
                    or (engine.REQUEST.get('preserve_sharp_edges', True)
                        and quality['feature_deviation'] > limit)):
                raise engine.Rejected('Simplified geometry failed topology, feature or surface gates.')
            result[index] = clone
            changed.append(index)
            details.append({'component': index, 'status': 'simplified', 'target_triangles': target,
                            'before': original, 'after': after, 'quality': quality})
        except (engine.Rejected, RuntimeError, ValueError) as error:
            clones.remove(clone)
            drop(clone)
            details.append({'component': index, 'status': 'retained', 'reason': str(error)})
    if not changed:
        return rollback('No quality-preserving triangle reduction passed; original masters retained.')
    try:
        checked = engine.audit(result, label + '-geometry')
        for index in changed:
            transfer = engine.transfer_material(sources[index], result[index], f'{label}_{index:03}', False)
            next(item for item in details if item['component'] == index)['material_transfer'] = transfer
        metrics.update(output_triangles=checked['triangles'], budget_met=checked['triangles'] <= budget,
                       applied=True, audit=checked)
        metrics['reason'] = ('Quality gates passed.' if metrics['budget_met'] else
                             'Safe reduction accepted; requested budget remains unmet to preserve geometry.')
        return result, metrics
    except (engine.Rejected, RuntimeError, ValueError) as error:
        return rollback('Game simplification rejected; original masters retained: ' + str(error))
