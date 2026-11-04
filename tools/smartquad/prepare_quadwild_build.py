"""Minimal, asserted portability/exit fix for QuadWild v0.0.2 only."""
import argparse
import difflib
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('source', type=Path)
args = ap.parse_args()
source = args.source.resolve()
changes = {
    'components/quad_from_patches/mesh_types.h': [
        ('\nvoid '+name+'(', '\ninline void '+name+'(')
        for name in ('ComputePerFacePatch', 'SelectVertMeshPatchBorders', 'CheckVertInPatch',
                     'CheckManifoldPatch', 'ExportPatch', 'CheckIntegrity', 'OrientIfNeeded')],
    'components/quad_from_patches/CMakeLists.txt': [
        ('add_executable(quad_from_patches main.cpp)',
         'add_executable(quad_from_patches main.cpp smooth_bridge.cpp)\n'
         '# Bound GCC compiler heap growth; this does not change generated solver logic.\n'
         'if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")\n'
         '    target_compile_options(quad_from_patches PRIVATE --param=ggc-min-expand=10 --param=ggc-min-heapsize=32768)\n'
         'endif()\n'
         'if(MINGW)\n'
         '    target_compile_options(quad_from_patches PRIVATE -Wa,-mbig-obj)\n'
         'endif()')],
    'libs/CoMISo/Base/Config/BaseDefines.hh': [
        ('#ifndef _MSC_VER\n#define sprintf_s snprintf\n#endif',
         '// MinGW also supplies sprintf_s; do not macro-rewrite its CRT declarations.\n'
         '#if !defined(_MSC_VER) && !defined(_WIN32)\n#define sprintf_s snprintf\n#endif')],
    'components/quad_from_patches/main.cpp': [
        ('#include "smooth_mesh.h"', '#include "smooth_bridge.h"'),
        ('      json_file << std::setw(4) << json;\n    }\n}',
         '      json_file << std::setw(4) << json;\n    }\n    return 0;\n}')],
    'CMakeLists.txt': [
        ('add_definitions(/D_USE_MATH_DEFINES /DNOMINMAX)',
         'add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)'),
        ('add_definitions(/DHETYPE=auto)', 'add_compile_definitions(HETYPE=auto)'),
        ('URL http://download-mirror.savannah.gnu.org/releases/getfem/stable/gmm-5.4.2.tar.gz',
         'URL https://download-mirror.savannah.gnu.org/releases/getfem/stable/gmm-5.4.2.tar.gz')],
    'libs/lpsolve/CMakeLists.txt': [
        ('add_library(lpsolve::lpsolve ALIAS lpsolve)',
         '# lp_lib.h uses legacy WIN32, which strict C++20 MinGW does not predefine.\n'
         'if(WIN32)\n'
         '    target_compile_definitions(lpsolve INTERFACE WIN32)\n'
         'endif()\n'
         'add_library(lpsolve::lpsolve ALIAS lpsolve)')],
    # MinGW C: INLINE is __inline on _WIN32, a C99 inline definition that is never emitted out of
    # line unless some declaration in the file is not inline (undefined presolve_roundrhs at link).
    # One plain declaration makes lp_presolve.c emit it; the function itself is unchanged.
    'libs/lpsolve/lp_solve_5.5/lp_presolve.c': [
        ('INLINE REAL presolve_roundrhs(lprec *lp, REAL value, MYBOOL isGE)\n',
         'extern REAL presolve_roundrhs(lprec *lp, REAL value, MYBOOL isGE);\n'
         'INLINE REAL presolve_roundrhs(lprec *lp, REAL value, MYBOOL isGE)\n')],
}
patches = []
for relative, pairs in changes.items():
    path = source / relative
    original = path.read_text()
    modified = original
    # Preserve the original backup when tightening compiler-only memory options.
    if relative == 'components/quad_from_patches/CMakeLists.txt':
        if 'ggc-min-expand' in modified:
            modified = modified.replace('add_executable(quad_from_patches main.cpp)',
                                        'add_executable(quad_from_patches main.cpp smooth_bridge.cpp)')
        # -O0 (from an earlier 1.5 GiB compiler cap) made the solver much slower; keep the build's -O1.
        modified = modified.replace('PRIVATE -O0 --param=ggc-min-expand=10',
                                    'PRIVATE --param=ggc-min-expand=10')
        if 'ggc-min-expand' in modified and '-mbig-obj' not in modified:
            marker = '--param=ggc-min-heapsize=32768)\nendif()'
            assert modified.count(marker) == 1
            modified = modified.replace(marker, marker+'\nif(MINGW)\n'
                '    target_compile_options(quad_from_patches PRIVATE -Wa,-mbig-obj)\nendif()')
    for old, new in pairs:
        if new in modified:
            continue
        if modified.count(old) != 1:
            raise RuntimeError(f'Unexpected source version: {relative}')
        modified = modified.replace(old, new)
    if modified != original:
        backup = path.with_suffix(path.suffix + '.upstream')
        if not backup.exists():
            backup.write_bytes(path.read_bytes())
        path.write_text(modified)
    baseline = path.with_suffix(path.suffix + '.upstream').read_text()
    patches.extend(difflib.unified_diff(baseline.splitlines(True), modified.splitlines(True),
                                      fromfile='a/'+relative, tofile='b/'+relative))
# Compile the existing smoothing templates independently from quadrangulation
# and JSON serialization. The wrappers pass every value unchanged; no solver or
# smoothing implementation is copied or altered. This addresses the measured
# >1536 MiB compiler footprint of the original combined translation unit.
bridge_header = '''// GPL-3.0-or-later, like the upstream functions this compilation bridge calls.
#pragma once
#include "mesh_types.h"
void LocalUVSmooth(PolyMesh&, TriangleMesh&, const std::vector<std::pair<size_t,size_t>>&,
                   const std::vector<size_t>&, size_t);
void MultiCostraintSmooth(PolyMesh&, TriangleMesh&, const std::vector<std::pair<size_t,size_t>>&,
    const std::vector<size_t>&, const std::vector<size_t>&, const std::vector<size_t>&,
    const std::vector<size_t>&, PolyMesh::ScalarType, PolyMesh::ScalarType, size_t, size_t);
'''
bridge_source = '''// GPL-3.0-or-later. Compile upstream templates once in their own translation unit.
#include "smooth_bridge.h"
#include "smooth_mesh.h"
void LocalUVSmooth(PolyMesh& p, TriangleMesh& t, const std::vector<std::pair<size_t,size_t>>& f,
                   const std::vector<size_t>& c, size_t steps) {
    ::LocalUVSmooth<PolyMesh,TriangleMesh>(p,t,f,c,steps);
}
void MultiCostraintSmooth(PolyMesh& p, TriangleMesh& t, const std::vector<std::pair<size_t,size_t>>& f,
    const std::vector<size_t>& c, const std::vector<size_t>& tp, const std::vector<size_t>& qc,
    const std::vector<size_t>& qp, PolyMesh::ScalarType damping, PolyMesh::ScalarType edge,
    size_t steps, size_t back_steps) {
    ::MultiCostraintSmooth<PolyMesh,TriangleMesh>(p,t,f,c,tp,qc,qp,damping,edge,steps,back_steps);
}
'''
for name, content in [('smooth_bridge.h', bridge_header), ('smooth_bridge.cpp', bridge_source)]:
    relative = 'components/quad_from_patches/' + name
    path = source / relative
    if path.exists() and path.read_text() != content:
        raise RuntimeError(f'Refusing to overwrite an unexpected compilation bridge: {relative}')
    if not path.exists():
        path.write_text(content)
    patches.extend(difflib.unified_diff([], content.splitlines(True), fromfile='/dev/null', tofile='b/'+relative))
(source.parent.parent / 'local-build-fixes.patch').write_text(''.join(patches))
print('Preserved original sources; applied documented exit, Windows and bounded-compilation changes.')
