"""Build a local B44 OpenGL candidate without per-draw driver introspection.

Reuses the existing local compiler/link inputs. Does not modify upstream,
recompile shaders, clear caches, or change the B44 texture-target fix.
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--serialize-io', action='store_true',
                        help='Protect shared IO handle/path maps across guest threads')
    parser.add_argument('--keep-driver-queries', action='store_true',
                        help='Keep B44 GL objects for an isolated IO fix')
    parser.add_argument('--context-ownership', action='store_true',
                        help='Keep the GL context on the render thread during drawable loss')
    parser.add_argument('--texture-query-source', type=Path,
                        help='Rebuild only texture.cpp from this override, removing diagnostic GL queries')
    parser.add_argument('--quiet-gxm-diagnostics', action='store_true',
                        help='Move per-draw GXM metadata diagnostics to Trace')
    parser.add_argument('--depth-bias', action='store_true',
                        help='Enable polygon offset when GXM depth bias is nonzero')
    parser.add_argument('--vertex-mad-rounding', action='store_true',
                        help='Keep vertex FP32 MAD rounding consistent with separate multiply/add')
    args = parser.parse_args()
    base, out = args.base.resolve(), args.output.resolve()
    inherited = json.loads((base / 'build.json').read_text(encoding='utf-8-sig'))
    if out.exists():
        raise ValueError('Use a fresh output directory')
    out.mkdir(parents=True)
    for directory in ('src', 'tmp', 'bin', 'link-obj'):
        (out / directory).mkdir()
    vcvars = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
    gl_names = (('sync_state',) if args.depth_bias else ()) if args.keep_driver_queries else ('draw', 'sync_state', 'texture', 'surface_cache')
    if args.texture_query_source:
        gl_names = ('texture',)
    if args.vertex_mad_rounding:
        gl_names += ('compile_program',)
    names = gl_names + (('io_runtime',) if args.serialize_io else ()) + (('alu',) if args.vertex_mad_rounding else ()) + (('game_window',) if args.context_ownership else ())
    if args.quiet_gxm_diagnostics:
        names += ('gxm_diagnostics', 'texture_cache_diagnostics')
    object_paths = {
        'io_runtime': 'vita3k/io/CMakeFiles/io.dir/src/io.cpp.obj',
        'alu': 'vita3k/shader/CMakeFiles/shader.dir/src/translator/alu.cpp.obj',
        'game_window': 'vita3k/gui-qt/CMakeFiles/gui-qt.dir/src/game_window.cpp.obj',
        'gxm_diagnostics': 'vita3k/modules/CMakeFiles/modules.dir/SceGxm/SceGxm.cpp.obj',
        'texture_cache_diagnostics': 'vita3k/renderer/CMakeFiles/renderer.dir/src/texture/cache.cpp.obj',
    }
    for name in names:
        source_base = args.texture_query_source.resolve() if name == 'texture' and args.texture_query_source else base
        if name in ('gxm_diagnostics', 'texture_cache_diagnostics'):
            relative = ('modules/SceGxm/SceGxm.cpp' if name == 'gxm_diagnostics'
                        else 'renderer/src/texture/cache.cpp')
            original_source = ROOT.parent / 'vita3k-src/Vita3K/vita3k' / relative
            source = original_source.read_text(encoding='utf-8-sig')
            prefixes = ('VP_GXM_DRAW ', 'VP_GXM_SET_TEX ') if name == 'gxm_diagnostics' else ('VP_GXM_CACHE ',)
            for prefix in prefixes:
                before = 'LOG_INFO("' + prefix
                if source.count(before) != 1:
                    raise ValueError('Expected unique diagnostic: ' + prefix)
                source = source.replace(before, 'LOG_TRACE("' + prefix)
        elif name == 'game_window':
            from build_qt_context_candidate import patch
            source = patch((ROOT.parent / 'vita3k-src/Vita3K/vita3k/gui-qt/src/game_window.cpp').read_text(encoding='utf-8-sig'))
        elif name == 'io_runtime':
            source = (ROOT.parent / 'vita3k-src/Vita3K/vita3k/io/src/io.cpp').read_text(encoding='utf-8-sig')
            source = source.replace('#include <cassert>',
                '#include <mutex>\nstatic std::recursive_mutex vp_io_handles_mutex;\n#include <cassert>')
            source, locked = re.subn(r'^([^\n]+\([^\n]*\bIOState\b[^\n]*\) \{)$',
                r'\1\n    std::lock_guard<std::recursive_mutex> vp_io_guard(vp_io_handles_mutex);',
                source, flags=re.M)
            if locked < 18:
                raise ValueError('Incomplete IO lock insertion: ' + str(locked))
        elif name == 'alu':
            source = (ROOT.parent / 'vita3k-src/Vita3K/vita3k/shader/src/translator/alu.cpp').read_text(encoding='utf-8-sig')
            before = '    spv::Id fma_result = m_b.createBuiltinCall(m_b.getTypeId(vsrc0), std_builtins, GLSLstd450Fma, { vsrc0, vsrc1, vsrc2 });'
            if source.count(before) != 2:
                raise ValueError('Vertex MAD patch context differs')
            source = source.replace(before, '''    spv::Id fma_result;
    if (!m_program.is_fragment() && inst.opr.dest.type == DataType::F32) {
        // The offline Cg path emits multiply/add. Contracting only some MADs
        // breaks depth equality between the prepass and colour programs.
        const auto type = m_b.getTypeId(vsrc0);
        spv::Id product = m_b.createBinOp(spv::OpFMul, type, vsrc0, vsrc1);
        m_b.addDecoration(product, spv::DecorationNoContraction);
        fma_result = m_b.createBinOp(spv::OpFAdd, type, product, vsrc2);
        m_b.addDecoration(fma_result, spv::DecorationNoContraction);
    } else {
        fma_result = m_b.createBuiltinCall(m_b.getTypeId(vsrc0), std_builtins, GLSLstd450Fma, { vsrc0, vsrc1, vsrc2 });
    }''')
        else:
            source = (source_base / 'src' / (name + '.cpp')).read_text(encoding='utf-8-sig')
        if name == 'texture' and args.texture_query_source:
            source, removed = re.subn(
                r'(?m)^[ \t]*GLint vp_active=0,vp_bound=0;\n'
                r'[ \t]*glGetIntegerv\(GL_ACTIVE_TEXTURE,&vp_active\);\n'
                r'[ \t]*glGetIntegerv\([^\n]+&vp_bound\);\n'
                r'[ \t]*LOG_(?:TRACE|INFO)\("VP_B44_(?:SELECT|CONFIG)[^\n]+\n',
                '', source)
            if removed != 2:
                raise ValueError('Expected select/configure diagnostic query blocks')
        if name == 'compile_program' and args.vertex_mad_rounding:
            before = 'compile_shader(renderer.shaders_path, renderer.shader_version,\n            vert_hash_hex'
            if source.count(before) != 1:
                raise ValueError('Vertex cache patch context differs')
            source = source.replace(before, 'compile_shader(renderer.shaders_path, renderer.shader_version + "-vpmad1",\n            vert_hash_hex')
            source = source.replace('shader_log_path, shader_version, shader_cache)',
                'shader_log_path, shader_version + (type == GL_VERTEX_SHADER ? "-vpmad1" : ""), shader_cache)')
            source = source.replace('shader_log_path, shader_version + "spv", shader_cache)',
                'shader_log_path, shader_version + (type == GL_VERTEX_SHADER ? "-vpmad1spv" : "spv"), shader_cache)')
        if name == 'sync_state' and args.depth_bias:
            before = '    // Depth Bias\n    if (is_front) {'
            if source.count(before) != 1:
                raise ValueError('Depth bias patch context differs')
            source = source.replace(before,
                '    // Depth Bias\n    if (is_front) {\n'
                '        if (factor || unit) glEnable(GL_POLYGON_OFFSET_FILL);\n'
                '        else glDisable(GL_POLYGON_OFFSET_FILL);')
        if name == 'draw' and not args.keep_driver_queries:
            begin = source.index('    // Draw.\n')
            end = source.index('    const GLenum mode = translate_primitive(type);', begin)
            source = source[:begin] + '    // Draw without diagnostic driver queries.\n' + source[end:]
            begin = source.index('    {\n        GLint vp[4]')
            end = source.index('    if (instance_count == 1)', begin)
            source = source[:begin] + source[end:]
            begin = source.index('        GLenum vp_draw_err = glGetError();')
            end = source.index('    } else {\n        glDrawElementsInstanced', begin)
            source = source[:begin] + source[end:]
        # Diagnostic text remains opt-in through Trace; GL state and the actual
        # target-reuse correction remain intact. Real warnings/errors stay.
        if not args.keep_driver_queries:
            source = re.sub(r'LOG_INFO\(("VP_[\s\S]*?);', r'LOG_TRACE(\1;', source)
        (out / 'src' / (name + '.cpp')).write_text(source, encoding='utf-8')
        if name in object_paths:
            upstream = ROOT.parent / 'vita3k-src/Vita3K/build-debug'
            build = (upstream / 'build.ninja').read_text(encoding='utf-8-sig')
            object_path = object_paths[name]
            match = re.search(r'^build ' + re.escape(object_path.replace('/', '\\')) + r':.*\n((?:  .*\n)+)', build, re.M)
            if not match:
                raise ValueError('Missing pinned compile command: ' + object_path)
            fields = dict(re.findall(r'^  (\w+) = (.*)$', match[1], re.M))
            rsp = ' '.join(fields[k] for k in ('DEFINES', 'FLAGS', 'INCLUDES'))
            if name in ('gxm_diagnostics', 'texture_cache_diagnostics'):
                rsp += f' /I"{original_source.parent}"'
            rsp += f' /Fo"{out / (name + ".obj")}" /Fd"{out / (name + ".pdb")}" /FS -c "{out / "src" / (name + ".cpp")}"'
        else:
            rsp = (source_base / (name + '.rsp')).read_text(encoding='utf-8-sig')
        for suffix in ('.obj', '.pdb'):
            rsp = rsp.replace(str(source_base / (name + suffix)), str(out / (name + suffix)))
        rsp = rsp.replace(str(source_base / 'src' / (name + '.cpp')), str(out / 'src' / (name + '.cpp')))
        (out / (name + '.rsp')).write_text(rsp, encoding='utf-8')
        script = out / (name + '.bat')
        script.write_text(f'@echo off\ncall "{vcvars}" >nul 2>nul\n'
                          f'set "TMP={out / "tmp"}"\nset "TEMP={out / "tmp"}"\n'
                          f'cl.exe @"{out / (name + ".rsp")}"\n', encoding='utf-8')
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(['cmd', '/c', str(script)], cwd=ROOT, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    ninja = (base / 'link.ninja').read_text(encoding='utf-8-sig')
    for name in gl_names:
        source_base = args.texture_query_source.resolve() if name == 'texture' and args.texture_query_source else base
        previous_object = str(source_base / (name + '.obj')).replace('\\', '/').replace(':', '$:')
        if ninja.count(previous_object) != 1:
            raise ValueError('Expected unique override object: ' + previous_object)
        ninja = ninja.replace(previous_object,
                              str(out / (name + '.obj')).replace('\\', '/').replace(':', '$:'))
    for name in names:
        if name not in object_paths:
            continue
        # A standalone object overrides the original archive member.
        key = ' CXX_EXECUTABLE_LINKER__vita3k_RelWithDebInfo '
        if ninja.count(key) != 1:
            raise ValueError('Missing unique executable link rule')
        ninja = ninja.replace(key, key + str(out / (name + '.obj')).replace('\\', '/').replace(':', '$:') + ' ', 1)
    # Other object and library paths keep their original resolved locations.
    old = str(base).replace('\\', '/')
    new = str(out).replace('\\', '/')
    ninja = ninja.replace(old.replace(':', '$:') + '/bin/Vita3K.exe', new.replace(':', '$:') + '/bin/Vita3K.exe')
    for suffix in ('/Vita3K', '/link.rsp', '/link-obj'):
        ninja = ninja.replace(old + suffix, new + suffix)
        ninja = ninja.replace(old.replace(':', '$:') + suffix,
                              new.replace(':', '$:') + suffix)
    ninja = ninja.replace('builddir = ' + old.replace(':', '$:'),
                          'builddir = ' + new.replace(':', '$:'))
    (out / 'link.ninja').write_text(ninja, encoding='utf-8')
    script = out / 'link.bat'
    script.write_text(f'@echo off\ncall "{vcvars}" >nul 2>nul\n'
                      f'set "TMP={out / "tmp"}"\nset "TEMP={out / "tmp"}"\n'
                      f'"{ROOT.parent / "tools/ninja/ninja.exe"}" -f "{out / "link.ninja"}" '
                      f'"{out / "bin/Vita3K.exe"}"\n', encoding='utf-8')
    upstream_build = ROOT.parent / 'vita3k-src/Vita3K/build-debug'
    with (out / 'link.log').open('w') as log:
        subprocess.run(['cmd', '/c', str(script)], cwd=upstream_build, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    for item in (base / 'bin').iterdir():
        if item.name in ('Vita3K.exe', 'vita3k.log', 'cache', 'shaderlog'):
            continue
        dest = out / 'bin' / item.name
        if item.is_dir():
            shutil.copytree(item, dest)
        elif item.suffix.lower() in ('.dll', '.yml', '.txt'):
            shutil.copyfile(item, dest)
    cache = base / 'bin/cache'
    if cache.exists():
        shutil.copytree(cache, out / 'bin/cache')
    (out / 'build.json').write_text(json.dumps(dict(base=str(base),
        executable=str(out / 'bin/Vita3K.exe'), target_reuse_fix=True,
        per_draw_driver_introspection=bool(args.keep_driver_queries and inherited.get('per_draw_driver_introspection', True)
                                         and not args.texture_query_source),
        depth_bias=bool(args.depth_bias or inherited.get('depth_bias', False)),
        vertex_mad_rounding=bool(args.vertex_mad_rounding or inherited.get('vertex_mad_rounding', False)),
        serialize_io=bool(args.serialize_io or inherited.get('serialize_io', False)),
        context_ownership=bool(args.context_ownership or inherited.get('context_ownership', False)),
        texture_query_cleanup=bool(args.texture_query_source or inherited.get('texture_query_cleanup', False)),
        quiet_gxm_diagnostics=bool(args.quiet_gxm_diagnostics or inherited.get('quiet_gxm_diagnostics', False))), indent=2) + '\n')
    print(out / 'bin/Vita3K.exe')


if __name__ == '__main__':
    main()
