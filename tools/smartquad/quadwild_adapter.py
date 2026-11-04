"""File-based adapter to the upstream QuadWild-BiMDF CLI, not a solver.

Called by the contained Blender worker launched by Qt QProcess. A nonzero
engine exit always rejects the result, even if the engine wrote an OBJ first.
"""
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time

class QuadWildFailure(RuntimeError):
    pass

def triangular_obj_measurements(path):
    vertices, faces = [], []
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == 'v':
            xyz = tuple(map(float, fields[1:4]))
            if len(xyz) != 3 or not all(map(math.isfinite, xyz)):
                raise QuadWildFailure('Non-finite or invalid vertex in engine input.')
            vertices.append(xyz)
        elif fields[0] == 'f':
            if len(fields) != 4:
                raise QuadWildFailure('QuadWild preprocessing requires triangular input.')
            faces.append(tuple(int(v.split('/')[0]) - 1 for v in fields[1:]))
    if not vertices or not faces or len(vertices) > 200000 or len(faces) > 400000:
        raise QuadWildFailure('Empty or oversized intermediate mesh.')
    area, edge_sum = 0., 0.
    for face in faces:
        if min(face) < 0 or max(face) >= len(vertices) or len(set(face)) != 3:
            raise QuadWildFailure('Invalid triangular indices.')
        a, b, c = (vertices[i] for i in face)
        u = [b[i]-a[i] for i in range(3)]
        v = [c[i]-a[i] for i in range(3)]
        cross = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
        area += math.sqrt(sum(x*x for x in cross)) / 2
        edge_sum += math.dist(a, b) + math.dist(b, c) + math.dist(c, a)
    if area <= 0 or edge_sum <= 0:
        raise QuadWildFailure('Degenerate intermediate mesh.')
    return area, edge_sum / (3 * len(faces))

def run_stage(command, cwd, log, timeout):
    started = time.monotonic()
    # Logs go to disk; engine verbosity cannot grow the GUI/worker RAM footprint.
    with Path(log).open('xb') as stream:
        try:
            result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=timeout, check=False,
                                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except subprocess.TimeoutExpired as error:
            raise QuadWildFailure(f'QuadWild exceeded its {timeout:g}s stage limit.') from error
    if result.returncode:
        raise QuadWildFailure(f'QuadWild stage failed: exit {result.returncode} '
                              f'(0x{result.returncode & 0xffffffff:08X}); see {log}.')
    return time.monotonic() - started

def remesh(executable, input_obj, target_quads, mechanical, seconds=180):
    executable, input_obj = Path(executable).resolve(), Path(input_obj).resolve()
    runtime = executable.parent
    quadrangulate = runtime / 'quad_from_patches.exe'
    if executable.name.lower() != 'quadwild.exe' or not executable.is_file() or not quadrangulate.is_file():
        raise QuadWildFailure('Both validated QuadWild Windows executables are required.')
    if not 8 <= target_quads <= 100000:
        raise QuadWildFailure('Quad face target is outside the bounded range.')
    triangular_obj_measurements(input_obj)
    source_hash = hashlib.sha256(input_obj.read_bytes()).hexdigest()
    prep = runtime / 'config/prep_config' / ('basic_setup_Mechanical.txt' if mechanical else 'basic_setup_Organic.txt')
    flow = runtime / 'config/main_config/flow.txt'
    config = runtime / 'config/satsuma/default.json'
    # Do not infer solver availability from a program name or silently fall back.
    settings = json.loads(config.read_text())
    def require_lemon(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key == 'matching_solver' and child != 'Lemon':
                    raise QuadWildFailure('Only the open-source Lemon matching backend is allowed.')
                require_lemon(child)
        elif isinstance(value, list):
            for child in value:
                require_lemon(child)
    require_lemon(settings)
    prepared = input_obj.with_name(input_obj.stem + '_rem_p0.obj')
    output = prepared.with_name(prepared.stem + '_1_quadrangulation_smooth.obj')
    if prepared.exists() or output.exists():
        raise QuadWildFailure('Refusing to reuse or overwrite an earlier engine result.')
    elapsed = run_stage([str(executable), str(input_obj), '2', str(prep)], runtime,
                        input_obj.with_suffix('.prepare.log'), seconds)
    area, edge = triangular_obj_measurements(prepared)
    scale = math.sqrt(area / target_quads) / edge
    if not math.isfinite(scale) or not .01 <= scale <= 100:
        raise QuadWildFailure('Requested quad density is outside the bounded engine range.')
    lines = flow.read_text().splitlines()
    if sum(line.startswith('scaleFact ') for line in lines) != 1 or 'useFlowSolver 1' not in lines:
        raise QuadWildFailure('Unsupported QuadWild flow configuration schema.')
    lines = [f'scaleFact {scale:.9g}' if line.startswith('scaleFact ') else line for line in lines]
    run_config = input_obj.with_suffix('.flow.txt')
    run_config.write_text('\n'.join(lines) + '\n')
    stats = input_obj.with_suffix('.solver.json')
    elapsed += run_stage([str(quadrangulate), str(prepared), '1', str(run_config), str(stats)], runtime,
                         input_obj.with_suffix('.quads.log'), max(1, seconds-elapsed))
    if not output.is_file() or output.stat().st_size == 0 or not stats.is_file():
        raise QuadWildFailure('QuadWild did not complete its output contract.')
    if hashlib.sha256(input_obj.read_bytes()).hexdigest() != source_hash:
        raise QuadWildFailure('QuadWild modified its input copy.')
    return output, {'backend': 'QuadWild-BiMDF', 'target_quads': target_quads,
                    'scale_factor': scale, 'runtime_seconds': elapsed,
                    'solver': 'BiMDF/Lemon', 'budget_is_approximate': True,
                    'input_sha256': source_hash}
