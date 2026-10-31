"""Recompile captured normalized GLSL with the current IR backend.

Use after a backend-only change without rerunning the Maxwell frontend. The
translation reports and GLSL are inputs; the tool never edits or installs them.
Generated Cg/GXP and provenance stay in the requested ignored output directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from convert_cache import compile_serial
from shader_ir import emit_cg, lower


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage', choices=('vertex','fragment'), default='fragment')
    p.add_argument('--attribute-types', type=Path, default=Path('config/graphics/character-input-types.json'))
    p.add_argument('--translate', type=Path, nargs='+', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--only', help='optional comma-separated logical program names')
    p.add_argument('--shacc', type=Path, default=Path(
        'local/shader-tools/shacc-bin/Vita3K-Shacc-windows-x86_64/vita3k-shacc.exe'))
    p.add_argument('--shacc-config', type=Path, default=Path('local/shader-tools/shacc.conf'))
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    records = []
    names = set()
    attribute_types=json.loads(a.attribute_types.read_text(encoding='utf-8')) if a.stage=='vertex' else {}
    stage_id=1 if a.stage=='vertex' else 5
    for folder in a.translate:
        report = folder / 'translate.json'
        for record in json.loads(report.read_text(encoding='utf-8'))['programs']:
            name = record['program']
            if a.only and name not in a.only.split(','):
                continue
            if not re.fullmatch(r's'+str(stage_id)+r'_0x[0-9a-f]+', name):
                continue
            vp = record.get('viewport_transform', 0)
            if vp not in (0, 1):
                raise ValueError('unknown viewport variant')
            output_name = name if vp == 0 else name.replace('_0x', '_vp1_0x')
            if output_name in names:
                raise ValueError('duplicate program variant: ' + output_name)
            names.add(output_name)
            glsl = folder / name / 'shader.normalized.glsl'
            cg, _ = emit_cg(lower(glsl.read_text(encoding='utf-8'), a.stage, attribute_types.get(name,())))
            work = a.output / output_name
            work.mkdir(exist_ok=True)
            source = work / 'shader.cg'
            source.write_text(cg, encoding='utf-8', newline='\n')
            gxp = a.output / (output_name + '.gxp')
            args = [str(a.shacc.resolve()), '--config', str(a.shacc_config.resolve()),
                    '-profile', 'sce_vp_psp2' if a.stage=='vertex' else 'sce_fp_psp2', '-O0', '-o', str(gxp.resolve()),
                    str(source.resolve()), '--timeout', '40']
            result = compile_serial(args, a.shacc_config.parent)
            (work / 'compile.txt').write_bytes(result.stdout + result.stderr)
            if result.returncode or not gxp.is_file():
                raise RuntimeError('program compile failed: ' + output_name)
            records.append({'name': gxp.name, 'sha256': digest(gxp),
                            'glsl': str(glsl), 'glsl_sha256': digest(glsl),
                            'translation_report_sha256': digest(report),
                            'cg_sha256': digest(source)})
            print(output_name, 'PASS', flush=True)
    if not records:
        raise ValueError('no programs found')
    manifest = {'programs': records, 'shader_ir_sha256': digest(Path(__file__).with_name('shader_ir.py')),
                'compiler_sha256': digest(a.shacc), 'config_sha256': digest(a.shacc_config)}
    (a.output / 'manifest.json').write_text(json.dumps(manifest, indent=2),
                                            encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
