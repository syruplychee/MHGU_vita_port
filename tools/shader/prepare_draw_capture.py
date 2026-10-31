"""Generate a source-patched reference build that records per-draw GPU state.

The reference runtime has no NVN interception and no per-draw observation point,
so the capture is added the same way the shader-cache run was: the MSVC response
files of the last real build are reused, one upstream translation unit is copied,
patched and recompiled, the video_core archive is rebuilt around it together with
our own capture unit, and the executable is relinked. The reference tree and its
object files are read only; nothing is written inside it.
"""
import argparse
import hashlib
from pathlib import Path
import re

ANCHOR = 'void GraphicsPipeline::ConfigureImpl(bool is_indexed) {\n'
CALL = ('    VpCapture::OnDraw(*maxwell3d, *gpu_memory, &key, sizeof(key), is_indexed);\n')
PATCHED_UNIT = 'src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp'
OBJECT_TOKEN = r'VIDEO_CORE.DIR\RELEASE\VK_GRAPHICS_PIPELINE.OBJ'
DRAW_UNIT = 'src/video_core/renderer_vulkan/vk_rasterizer.cpp'
DRAW_OBJECT_TOKEN = r'VIDEO_CORE.DIR\RELEASE\VK_RASTERIZER.OBJ'
DRAW_ANCHOR = 'void RasterizerVulkan::Draw(bool is_indexed, u32 instance_count) {\n'
DRAW_CALL = '    VpCapture::AfterDraw(*maxwell3d, *gpu_memory);\n'
CAPTURE_UNIT = 'src/reference_capture/vp_draw_capture.cpp'
HEADER_DIR = 'src/reference_capture'
MAIN_UNIT = 'src/mhgu_dyn/main.cpp'
MAIN_OBJECT_TOKEN = r'MHGU_DYN.DIR\RELEASE\MAIN.OBJ'
MAIN_ANCHOR = 'int main(int argc, char** argv) {\n'
MAIN_CALL = ('    Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);\n')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_tlog(path):
    raw = Path(path).read_bytes()
    text = raw.decode('utf-16') if raw.startswith(b'\xff\xfe') else raw.decode('utf-8')
    return text.splitlines()


def join_command(lines):
    return ' '.join(line for line in lines if line and not line.startswith('^'))


def retarget(command, output, source, include, recorded=None):
    """Point a recorded compile command at `source`.

    `recorded` is the file the command was created for; a command for a unit that
    did not take part in the recorded build has no such path and only needs the
    trailing source argument replaced.
    """
    command = re.sub(r'/Fo"[^"]*"', lambda _: f'/Fo"{output}"', command)
    command = re.sub(r'/Fd"[^"]*"', lambda _: f'/Fd"{output.with_suffix(".pdb")}"', command)
    if recorded is None:
        if not re.search(r'\S*\.CPP\s*$', command, flags=re.I):
            raise ValueError('compile command does not end in a source file')
        command = re.sub(r'\S*\.CPP\s*$', lambda _: f'"{source}"', command, flags=re.I)
    else:
        key = str(recorded).upper()
        if key not in command.upper():
            raise ValueError(f'compile command does not name {recorded}')
        command = re.sub(re.escape(key), lambda _: str(source), command, flags=re.I)
    return f'{command} /I"{include}"'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--vs', type=Path, required=True)
    parser.add_argument('--repository', type=Path,
                        default=Path(__file__).resolve().parents[2])
    a = parser.parse_args()

    reference = a.reference.resolve()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    build = reference / 'build-msvc-s08-clean'
    vc_build = build / 'src/video_core'
    dyn_build = build / 'src/mhgu_dyn'
    vc_tlog = vc_build / 'video_core.dir/Release/video_core.tlog'
    dyn_tlog = dyn_build / 'mhgu_dyn.dir/Release/mhgu_dyn.tlog'
    header_dir = (a.repository / HEADER_DIR).resolve()
    capture_unit = (a.repository / CAPTURE_UNIT).resolve()

    # --- patch the single translation unit -----------------------------------
    original = (reference / PATCHED_UNIT).read_text(encoding='utf-8')
    if original.count(ANCHOR) != 1:
        raise ValueError('per-draw capture anchor not found exactly once')
    # The include goes at the top: the line before the anchor is a template head.
    patched = '#include "vp_draw_capture.h"\n\n' + original.replace(ANCHOR, ANCHOR + CALL)
    unit = out / 'vk_graphics_pipeline.cpp'
    unit.write_text(patched, encoding='utf-8', newline='\n')

    # --- patch the draw unit for the post-draw readback hook -----------------
    draw_original = (reference / DRAW_UNIT).read_text(encoding='utf-8')
    if draw_original.count(DRAW_ANCHOR) != 1:
        raise ValueError('RasterizerVulkan::Draw anchor not found exactly once')
    draw_pos = draw_original.index(DRAW_ANCHOR)
    draw_end = draw_original.index('\n    });\n}', draw_pos) + len('\n    });\n')
    # After PrepareDraw returns the cache mutexes are released again, so the
    # texture-cache download path can be used without a recursive lock.
    draw_patched = ('#include "vp_draw_capture.h"\n\n' + draw_original[:draw_end] +
                    DRAW_CALL + draw_original[draw_end:])
    draw_unit = out / 'vk_rasterizer.cpp'
    draw_unit.write_text(draw_patched, encoding='utf-8', newline='\n')

    # --- pin the reference console mode to the target port's handheld model ---
    # The port reports OperationMode::Handheld, so its render targets are
    # 1280x720/640x360/320x180. The reference defaults to docked (1920x1080...).
    # Comparing draw for draw requires the same mode on both sides.
    main_original = (reference / MAIN_UNIT).read_text(encoding='utf-8')
    if main_original.count(MAIN_ANCHOR) != 1:
        raise ValueError('main entry anchor not found exactly once')
    main_patched = ('#include "common/settings.h"\n\n' +
                    main_original.replace(MAIN_ANCHOR, MAIN_ANCHOR + MAIN_CALL))
    main_unit = out / 'main.cpp'
    main_unit.write_text(main_patched, encoding='utf-8', newline='\n')

    # --- compile commands from the recorded build ----------------------------
    cl_lines = read_tlog(vc_tlog / 'CL.command.1.tlog')
    index = [i for i, line in enumerate(cl_lines)
             if line.startswith('^') and line.upper().endswith('VK_GRAPHICS_PIPELINE.CPP')]
    if len(index) != 1:
        raise ValueError('expected one recorded compile of the patched unit')
    template = cl_lines[index[0] + 1]
    (out / 'compile-pipeline.rsp').write_text(
        retarget(template, out / 'vk_graphics_pipeline.obj', unit, header_dir,
                 recorded=reference / PATCHED_UNIT),
        encoding='utf-8', newline='\n')
    draw_index = [i for i, line in enumerate(cl_lines)
                  if line.startswith('^') and line.upper().endswith('VK_RASTERIZER.CPP')]
    if len(draw_index) != 1:
        raise ValueError('expected one recorded compile of the draw unit')
    draw_template = cl_lines[draw_index[0] + 1]
    (out / 'compile-rasterizer.rsp').write_text(
        retarget(draw_template, out / 'vk_rasterizer.obj', draw_unit, header_dir,
                 recorded=reference / DRAW_UNIT),
        encoding='utf-8', newline='\n')
    (out / 'compile-capture.rsp').write_text(
        retarget(template, out / 'vp_draw_capture.obj', capture_unit, header_dir),
        encoding='utf-8', newline='\n')
    dyn_cl_lines = read_tlog(dyn_tlog / 'CL.command.1.tlog')
    main_index = [i for i, line in enumerate(dyn_cl_lines)
                  if line.startswith('^') and line.upper().endswith('MAIN.CPP')]
    if len(main_index) != 1:
        raise ValueError('expected one recorded compile of the main unit')
    (out / 'compile-main.rsp').write_text(
        retarget(dyn_cl_lines[main_index[0] + 1], out / 'main.obj', main_unit, header_dir,
                 recorded=reference / MAIN_UNIT),
        encoding='utf-8', newline='\n')

    # --- archive the patched units into a private copy of video_core.lib -----
    lib = join_command(read_tlog(vc_tlog / 'Lib.command.1.tlog'))
    if OBJECT_TOKEN.upper() not in lib.upper():
        raise ValueError('archive command does not name the patched object')
    if DRAW_OBJECT_TOKEN.upper() not in lib.upper():
        raise ValueError('archive command does not name the draw object')
    lib = re.sub(r'/OUT:"[^"]*"', lambda _: f'/OUT:"{out / "video_core.lib"}"', lib, flags=re.I)
    lib = re.sub(re.escape(OBJECT_TOKEN), lambda _: f'"{out / "vk_graphics_pipeline.obj"}"',
                 lib, flags=re.I)
    lib = re.sub(re.escape(DRAW_OBJECT_TOKEN), lambda _: f'"{out / "vk_rasterizer.obj"}"',
                 lib, flags=re.I)
    lib = f'{lib} "{out / "vp_draw_capture.obj"}"'
    (out / 'lib.rsp').write_text(lib, encoding='utf-8', newline='\n')

    # --- relink the runtime against it ---------------------------------------
    link = join_command(read_tlog(dyn_tlog / 'link.command.1.tlog'))
    for flag, name in [('OUT', 'mhgu_draw_capture.exe'), ('PDB', 'capture.pdb'),
                       ('IMPLIB', 'capture.lib'), ('PGD', 'capture.pgd'), ('MAP', 'capture.map')]:
        link = re.sub(r'/' + flag + r':"[^"]*"', lambda _, n=name: f'/{flag}:"{out / n}"',
                      link, flags=re.I)
    if r'..\VIDEO_CORE\RELEASE\VIDEO_CORE.LIB' not in link.upper():
        raise ValueError('link command does not reference the video_core archive')
    if MAIN_OBJECT_TOKEN.upper() not in link.upper():
        raise ValueError('link command does not name the main object')
    link = re.sub(re.escape(MAIN_OBJECT_TOKEN), lambda _: f'"{out / "main.obj"}"',
                  link, flags=re.I)
    link = re.sub(r'\.\.\\VIDEO_CORE\\RELEASE\\VIDEO_CORE\.LIB',
                  lambda _: f'"{out / "video_core.lib"}"', link, flags=re.I)
    (out / 'link.rsp').write_text(link, encoding='utf-8', newline='\n')

    (out / 'build.cmd').write_text('\n'.join([
        '@echo off',
        f'call "{a.vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 -host_arch=x64',
        f'cd /d "{vc_build}"',
        f'cl.exe @"{out / "compile-pipeline.rsp"}"',
        'if errorlevel 1 exit /b 1',
        f'cl.exe @"{out / "compile-rasterizer.rsp"}"',
        'if errorlevel 1 exit /b 1',
        f'cl.exe @"{out / "compile-main.rsp"}"',
        'if errorlevel 1 exit /b 1',
        f'cl.exe @"{out / "compile-capture.rsp"}"',
        'if errorlevel 1 exit /b 1',
        f'lib.exe @"{out / "lib.rsp"}"',
        'if errorlevel 1 exit /b 1',
        f'cd /d "{dyn_build}"',
        f'link.exe @"{out / "link.rsp"}"',
        'exit /b %errorlevel%',
        '']), encoding='utf-8', newline='\n')

    import json
    (out / 'source.json').write_text(json.dumps({
        'schema': 1,
        'purpose': 'Record every draw referenced from the geometry, constant, texture and '
                   'render-target registers; guest state is never modified.',
        'patched_unit': PATCHED_UNIT,
        'patched_unit_original_sha256': digest(reference / PATCHED_UNIT),
        'patched_unit_modified_sha256': digest(unit),
        'draw_unit': DRAW_UNIT,
        'draw_unit_original_sha256': digest(reference / DRAW_UNIT),
        'draw_unit_modified_sha256': digest(draw_unit),
        'main_unit': MAIN_UNIT,
        'main_unit_original_sha256': digest(reference / MAIN_UNIT),
        'main_unit_modified_sha256': digest(main_unit),
        'console_mode': 'handheld (matches the target port OperationMode)',
        'capture_unit': CAPTURE_UNIT,
        'capture_unit_sha256': digest(capture_unit),
        'capture_header_sha256': digest(header_dir / 'vp_draw_capture.h'),
        'hook': 'first statement of GraphicsPipeline::ConfigureImpl (pre-draw), and '
                'RasterizerVulkan::Draw after PrepareDraw returns (optional RT readback)',
        'environment': ['VP_DRAW_CAPTURE_DIR', 'VP_DRAW_CAPTURE_LIMIT', 'VP_DRAW_CAPTURE_SKIP',
                        'VP_DRAW_CAPTURE_BLOB_LIMIT', 'VP_DRAW_CAPTURE_TOTAL_LIMIT',
                        'VP_DRAW_CAPTURE_TEX_BYTES', 'VP_DRAW_CAPTURE_RT',
                        'VP_DRAW_CAPTURE_RT_LIMIT', 'VP_DRAW_CAPTURE_RT_BLOB_LIMIT'],
    }, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'output': str(out), 'patched': str(unit)}))


if __name__ == '__main__':
    main()
