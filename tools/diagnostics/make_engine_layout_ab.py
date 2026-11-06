"""Generate a private same-process layout A/B build without changing sources.

Compile the two outputs in place of the matching production C files. The private
controller alternates original/prepared layout paths in four 384-frame phases
after 270 seconds. No game state, effect, input or draw is skipped.
"""
import argparse
import hashlib
import json
from pathlib import Path


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('reviewed A/B anchor missing or ambiguous: ' + old[:60])
    return source.replace(old, new)


def generate(root, output):
    output.mkdir(parents=True, exist_ok=False)
    engine_path = root / 'src/platform/engine_layout.c'
    gxm_path = root / 'src/platform/gxm_runtime.c'
    engine = engine_path.read_text(encoding='utf-8')
    gxm = gxm_path.read_text(encoding='utf-8')
    setter = '''static unsigned engine_layout_benchmark_mode=1u;
void vp_engine_layout_benchmark_set(unsigned mode) {
    __atomic_store_n(&engine_layout_benchmark_mode,mode,__ATOMIC_RELAXED);
    FILE *log=fopen("ux0:data/mhgu-vita-port/engine-layout-ab.log","ab");
    if (log) {
        fprintf(log,"ENGINE_LAYOUT_AB_COUNT entering_mode=%u\\n",mode);
        vp_engine_layout_report(log);
        fclose(log);
    }
}
'''
    anchor = '__attribute__((optimize("no-tree-loop-distribute-patterns")))\nint vp_engine_layout_prepare'
    engine = replace_once(engine, anchor, setter + anchor)
    engine = replace_once(engine, '    own_increment(&pool->calls);\n    if (pool->busy)',
        '    own_increment(&pool->calls);\n'
        '    if (!__atomic_load_n(&engine_layout_benchmark_mode,__ATOMIC_RELAXED)) {\n'
        '        own_increment(&pool->original);\n        return 0;\n    }\n    if (pool->busy)')
    controller = '''extern void vp_engine_layout_benchmark_set(unsigned);
static unsigned engine_layout_ab_start,engine_layout_ab_phase=UINT32_MAX;
static void engine_layout_ab_tick(uint64_t now) {
    if (!engine_layout_ab_start && now>=270000000ull && !(gxm_perf_present_frames&31u))
        engine_layout_ab_start=(unsigned)gxm_perf_present_frames;
    if (!engine_layout_ab_start) return;
    unsigned phase=((unsigned)gxm_perf_present_frames-engine_layout_ab_start)/384u;
    if (phase>4u || phase==engine_layout_ab_phase) return;
    engine_layout_ab_phase=phase;
    unsigned enabled=phase==4u?1u:(phase&1u);
    vp_engine_layout_benchmark_set(enabled);
    glog("ENGINE_LAYOUT_AB_PHASE frame=%llu start=%u phase=%u enabled=%u t_us=%llu\\n",
        (unsigned long long)gxm_perf_present_frames,engine_layout_ab_start,phase,
        enabled,(unsigned long long)now);
}
'''
    gxm = replace_once(gxm, 'static void gxm_perf_present(uint64_t fn_us, uint64_t now) {',
                       controller + '\nstatic void gxm_perf_present(uint64_t fn_us, uint64_t now) {')
    gxm = replace_once(gxm, '    gxm_perf_present_frames++;\n',
                       '    gxm_perf_present_frames++;\n    engine_layout_ab_tick(now);\n')
    rows = []
    for name, path, source in [('engine_layout.c', engine_path, engine), ('gxm_runtime.c', gxm_path, gxm)]:
        data = source.encode('utf-8')
        (output / name).write_bytes(data)
        rows.append({'source': str(path), 'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                     'output': str(output / name), 'output_sha256': hashlib.sha256(data).hexdigest()})
    (output / 'provenance.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.root.resolve(), args.output.resolve())


if __name__ == '__main__':
    main()
