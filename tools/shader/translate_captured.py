"""Translate the captured Maxwell shader programs all the way to GXM.

Pipeline, each stage verified separately:

  1. preprocess   captured words -> canonical SPH+code image
  2. maxwell-glsl Maxwell -> GLSL            (pinned external frontend, offline)
  3. shader_ir    GLSL -> Cg                 (this project, tools/shader/shader_ir.py)
  4. shacc        Cg -> GXM shader binary    (pinned external compiler)

Stage 1 needs two documented code-stream rewrites before the frontend will accept
the image; both are counted and reported per program rather than applied silently:

  * FORMAT UNIT -> NOP.  A slot whose high word has top byte 0x00 cannot be any
    Maxwell instruction: across all 280 encodings the smallest possible top byte
    is 0x01 (MOV32I), because every pattern fixes at least one bit there. These
    slots carry the program's own data, not code, and leaving them in makes the
    frontend decode them as instructions and desynchronise.

  * FADD32I carry flag cleared.  The frontend refuses instructions whose CC bit
    (bit 47) is set ("FADD CC is not implemented"). CC only writes a condition
    code, which nothing in these graphics shaders reads, so the flag is dropped
    and the count recorded. The arithmetic result is unaffected.

Nothing here invents constant-buffer or texture state: the frontend answers
missing ones with documented defaults and reports them in a sidecar, so the GLSL
is structurally complete while every unknown value stays enumerated.

This tool runs INSIDE WSL: the Maxwell frontend is a Linux ELF and shader_ir is
driven by the WSL python3 interpreter. The GXM compiler is a Windows binary and is
reached through WSL interop, so only its arguments are converted back to Windows
path form.

Usage (from a WSL shell):
  python3 tools/shader/translate_captured.py \
      --program-dir artifacts/<run>/programs --output contents/build/vp04-translate
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

import check_cg_outputs

NOP_HI = 0x50B00000
NOP_LO = 0x00070F00
# Maxwell packs instructions into 32-byte bundles: one padding slot at +0 and three
# instruction slots at +8/+16/+24, with the grid anchored on absolute offsets mod 32.
# The frontend starts decoding at start_address + SPH size, so that sum has to be
# 32-byte aligned or every bundle is read 16 bytes off.
#
# In the real system the congruence holds by construction: the reference runtime builds
# its CFG at `StartAddress() + sizeof(ProgramHeader)` and never from an arbitrary base,
# and captured shader addresses are bundle-aligned (the captured SPH sits at 0x...F430,
# so code at +0x50 is 0 mod 32). Addressing the image from 0 broke a congruence that is
# always true in the real system, which is why every instruction was read 16 bytes off.
# The base below is derived rather than chosen.
SPH_BYTES = 0x50
CODE_START_ALIGNMENT = (-SPH_BYTES) % 32
assert (CODE_START_ALIGNMENT + SPH_BYTES) % 32 == 0, 'code start must be bundle aligned'


def win_path(p: Path) -> str:
    """WSL mount path -> Windows path, for the interop-invoked GXM compiler."""
    resolved = str(p.resolve())
    m = re.match(r'/mnt/([a-z])/(.*)', resolved)
    if not m:
        raise ValueError(f'not on a /mnt/<drive> mount: {resolved}')
    return f'{m.group(1).upper()}:\\' + m.group(2).replace('/', '\\')


PRE_INC = re.compile(r'(\+\+|--)\s*([A-Za-z_]\w*)')


def normalize_glsl(text):
    """Rewrite prefix ++/-- into a hoisted assignment, returning (text, count).

    The Maxwell frontend guards its loops with `if(--loop0<0 || !b_0){break;}`.
    shader_ir handles assignment, if and for but not the increment operators, so
    each `--x` / `++x` becomes its own assignment on the line above the statement
    that used it. A prefix increment yields the updated value, so performing the
    assignment first preserves exactly the value the expression then reads.

    Only whole-line hoisting is done. An increment whose result is consumed inside
    the same expression as another mutation could reorder side effects; that shape
    does not occur in the generated code, and a count of rewrites is reported so
    the assumption stays visible.
    """
    out, count = [], 0
    for line in text.splitlines():
        pending = []
        while True:
            m = PRE_INC.search(line)
            if not m:
                break
            op, name = m.group(1), m.group(2)
            pending.append(f'{name} = {name} {"+" if op == "++" else "-"} 1;')
            line = line[:m.start()] + name + line[m.end():]
            count += 1
        indent = line[:len(line) - len(line.lstrip())]
        out.extend(indent + p for p in pending)
        out.append(line)
    return '\n'.join(out) + '\n', count


def stage_of(name: str):
    """Captured files are named s<stage>_0x<hash>.txt; stage 1 and 5 are the only
    two the title ever binds (verified by always-on bind tracking)."""
    m = re.match(r's(\d+)_0x([0-9a-fA-F]+)$', name)
    if not m:
        raise ValueError(f'unexpected program name: {name}')
    stage = int(m.group(1))
    if stage == 1:
        return 'vertex', 0, 1          # frontend stage id 0, SPH shader_type 1
    if stage == 5:
        return 'fragment', 4, 5        # frontend stage id 4, SPH shader_type 5
    raise ValueError(f'unsupported stage {stage} in {name}')


def preprocess(words):
    """No code-stream rewriting is needed. Returns (words, stats).

    An earlier version replaced "format units" (words whose high byte is 0x00) with
    NOP and cleared the FADD carry flag, because the frontend rejected the image
    otherwise. Both were treating a symptom: the real cause was that the code region
    was not aligned to Maxwell's 32-byte instruction bundle.

    location.h advances with `offset += 8 + (offset % 32 == 24 ? 8 : 0)` and aligns a
    location at a 32-byte boundary by +8, so each 32-byte bundle holds one padding
    slot at +0 and three instruction slots at +8/+16/+24, and the grid is anchored on
    absolute offsets modulo 32. A code region starting at image byte 0x50 (80, which
    is 16 mod 32) put every bundle boundary 16 bytes off, so each step decoded the
    wrong word: the padding slot looked like a separate "unit" needing substitution,
    and real instructions were skipped.

    With the start address set so that start+0x50 is a multiple of 32 (see
    CODE_START_ALIGNMENT below) the frontend visits the correct three slots per
    bundle and decodes the program as written, padding slots included and untouched.
    """
    return list(words), {'format_units_to_nop': 0, 'fadd_cc_cleared': 0}

def write_image(words, path: Path):
    """Little-endian u32 words -> bytes. The captured window is already SPH+code."""
    data = bytearray(len(words) * 4)
    for i, w in enumerate(words):
        v = int(w, 16)
        data[i * 4 + 0] = v & 0xFF
        data[i * 4 + 1] = (v >> 8) & 0xFF
        data[i * 4 + 2] = (v >> 16) & 0xFF
        data[i * 4 + 3] = (v >> 24) & 0xFF
    path.write_bytes(bytes(data))


def run(cmd, timeout=180):
    """Run a command, returning (rc, stdout, stderr) with WSL localhost noise dropped."""
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, '', 'timeout'
    noise = re.compile(r'localhost|WSL|wsl:', re.I)
    out = '\n'.join(l for l in p.stdout.splitlines() if not noise.search(l))
    err = '\n'.join(l for l in p.stderr.splitlines() if not noise.search(l))
    return p.returncode, out, err


def translate_one(prog: Path, out_dir: Path, frontend: Path, shacc, shacc_conf, do_cg,
                  viewport_transform=0, unsigned_attributes=()):
    stage_name, stage_id, shader_type = stage_of(prog.stem)
    rec = {
        'program': prog.stem,
        'stage': stage_name,
        'frontend_stage_id': stage_id,
        'viewport_transform': viewport_transform,
        'unsigned_attributes': list(unsigned_attributes),
        'sph_shader_type': shader_type,
        'reached': 'none',
        'error': None,
    }

    words = [w.strip() for w in prog.read_text(encoding='utf-8').split() if w.strip()]
    first = int(words[0], 16) if words else 0
    rec['sph_magic'] = f'0x{first:08x}'
    if (first >> 24) != 0x00 and (first & 0xFF) not in (0x61, 0x62):
        rec['note'] = 'first word is not a recognised stage magic'

    patched, stats = preprocess(words)
    rec.update(stats)

    work = out_dir / prog.stem
    work.mkdir(parents=True, exist_ok=True)
    bin_path = work / 'code.bin'
    env_path = work / 'environment.txt'
    glsl_path = work / 'shader.glsl'
    write_image(patched, bin_path)
    # stage_id start_address local bound viewport  nt nf nc nr
    env_path.write_text(f'{stage_id} {CODE_START_ALIGNMENT} 0 0 {viewport_transform} 0 0 0 0\n', encoding='utf-8')

    rc, out, err = run([str(frontend), str(bin_path), str(env_path), str(glsl_path)],
                       timeout=240)
    rec['frontend_rc'] = rc
    rec['frontend_stdout'] = out.strip()[:400]
    if err.strip():
        rec['frontend_stderr'] = err.strip()[:400]

    if not glsl_path.exists() or glsl_path.stat().st_size == 0:
        # The frontend reports the exact instruction class it cannot handle.
        rec['error'] = (err.strip() or out.strip() or 'no GLSL produced')[:300]
        rec['reached'] = 'preprocess'
        return rec
    rec['glsl_bytes'] = glsl_path.stat().st_size
    rec['reached'] = 'glsl'

    for side, key in ((glsl_path.with_suffix('.glsl.resources.json'), 'resources'),
                      (Path(str(glsl_path) + '.missing-cbufs.json'), 'missing')):
        if side.exists():
            try:
                rec[key] = json.loads(side.read_text(encoding='utf-8'))
            except ValueError:
                rec[f'{key}_unparsed'] = True

    if not do_cg:
        return rec

    cg_path = work / 'shader.cg'
    norm_text, n_inc = normalize_glsl(glsl_path.read_text(encoding='utf-8'))
    rec['increment_hoists'] = n_inc
    norm_path = work / 'shader.normalized.glsl'
    norm_path.write_text(norm_text, encoding='utf-8', newline='\n')
    p = subprocess.run([sys.executable, str(Path(__file__).with_name('shader_ir.py')),
                        '--glsl', str(norm_path), '--stage', stage_name,
                        '--output', str(cg_path), '--unsigned-attributes',
                        ','.join(str(x) for x in unsigned_attributes)],
                       capture_output=True, text=True, timeout=180)
    if p.returncode != 0 or not cg_path.exists():
        rec['error'] = (p.stderr.strip() or 'shader_ir produced no Cg')[:300]
        rec['reached'] = 'glsl'
        return rec
    rec['cg_bytes'] = cg_path.stat().st_size
    rec['reached'] = 'cg'

    # Compiling is not the same as being usable: a shader that never gives a
    # POSITION component a computed value links, loads and renders nothing. Every
    # program is therefore classified for output coverage before it may be called
    # done, and an incomplete one is reported as such instead of counted as success.
    rows = check_cg_outputs.output_coverage(cg_path.read_text(encoding='utf-8'))
    gaps = []
    for r in rows:
        comp = r['components']
        if r['semantic'] == 'POSITION' and (comp['x'] != 'computed' or comp['y'] != 'computed'):
            gaps.append(f"{r['output']}.x is {comp['x']}, .y is {comp['y']}")
    rec['output_gaps'] = gaps
    rec['renderable'] = not gaps

    if not (shacc and shacc_conf):
        return rec
    gxp = work / 'shader.gxp'
    profile = 'sce_vp_psp2' if stage_name == 'vertex' else 'sce_fp_psp2'
    rc, out, err = run([str(shacc), '--config', win_path(shacc_conf), '-profile', profile,
                        '-O0', '-o', win_path(gxp), win_path(cg_path), '--timeout', '40'],
                       timeout=120)
    rec['shacc_rc'] = rc
    diag = [l for l in (out + '\n' + err).splitlines()
            if re.search(r'error \d+:|warning \d+:|Shacc compilation', l)]
    if diag:
        rec['shacc_diag'] = diag[:6]
    if gxp.exists() and gxp.stat().st_size > 0:
        rec['gxp_bytes'] = gxp.stat().st_size
        rec['reached'] = 'gxm'
    else:
        rec['error'] = ('shacc: ' + '; '.join(diag[:2])) if diag else 'shacc produced no GXP'
    return rec


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--program-dir', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--frontend', type=Path,
                    default=Path('local/shader-tools/frontend-build/maxwell-glsl'))
    ap.add_argument('--shacc', type=Path,
                    default=Path('local/shader-tools/shacc-bin/Vita3K-Shacc-windows-x86_64/vita3k-shacc.exe'))
    ap.add_argument('--shacc-config', type=Path, default=Path('local/shader-tools/shacc.conf'))
    ap.add_argument('--no-cg', action='store_true', help='stop after GLSL')
    ap.add_argument('--only', default=None, help='comma separated program stems')
    ap.add_argument('--attribute-types',type=Path,
                    help='JSON program -> captured unsigned input locations; checked against runtime GXP bindings')
    ap.add_argument('--viewport-transform', type=int, choices=(0, 1), default=0,
                    help='captured Maxwell viewport_scale_offset_enabled; part of the shader key')
    a = ap.parse_args()
    attribute_types=json.loads(a.attribute_types.read_text(encoding='utf-8')) if a.attribute_types else {}

    progs = sorted(a.program_dir.glob('s*_0x*.txt'))
    if a.only:
        want = {s.strip() for s in a.only.split(',')}
        progs = [p for p in progs if p.stem in want]
    if not progs:
        raise SystemExit(f'no programs under {a.program_dir}')
    a.output.mkdir(parents=True, exist_ok=True)

    shacc = a.shacc if a.shacc.exists() else None
    conf = a.shacc_config if a.shacc_config.exists() else None

    rows = []
    for p in progs:
        rec = translate_one(p, a.output, a.frontend, shacc, conf, not a.no_cg,
                            a.viewport_transform,attribute_types.get(p.stem,()))
        rows.append(rec)
        status = rec['reached']
        extra = f"  {rec['error'][:80]}" if rec.get('error') else ''
        print(f"{rec['program']:<14} {rec['stage']:<9} {status:<10}{extra}")

    reached = {}
    for r in rows:
        reached[r['reached']] = reached.get(r['reached'], 0) + 1
    renderable = sum(1 for r in rows if r.get('renderable'))
    print()
    print('stage reached:', ', '.join(f'{k}={v}' for k, v in sorted(reached.items())))
    print(f'renderable (every POSITION component computed): {renderable}/{len(rows)}')
    print('frontend :', a.frontend)
    print('shacc    :', shacc)

    summary = {
        'program_dir': str(a.program_dir),
        'frontend': str(a.frontend),
        'shacc': str(shacc) if shacc else None,
        'programs': rows,
        'reached_counts': reached,
        'renderable_count': renderable,
    }
    (a.output / 'translate.json').write_text(
        json.dumps(summary, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print('wrote', a.output / 'translate.json')


if __name__ == '__main__':
    main()
