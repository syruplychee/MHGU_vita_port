"""Verify the complete P5 integer probe image and its backend execution.

Each case covers five pixels. An input-only mutation must turn exactly its
five pixels red while all other cases stay green; the negative remains a
failed positive verification even when it is recognized as the expected guard.
"""
import argparse
import json
from pathlib import Path
import struct

from verify_gxm_s2_pairs import verify


def verify_integer(run, bundle, cases_path, negative_case=None):
    run, bundle = Path(run), Path(bundle)
    baseline = verify(run / 'gxm-s2p.log', bundle / 'pair-manifest.json',
                      emulator_log=run / 'emulator.log', process_path=run / 'process.json',
                      cache_dir=run / 'cache-after', require_execution=True)
    cases = json.loads(Path(cases_path).read_text())['cases']
    raw = (run / 'gxm-s2p.rgba').read_bytes()
    problems = []
    results = []
    if len(raw) != 64 * 16 * 4:
        problems.append('wrong image size')
    else:
        pixels = struct.unpack('<1024I', raw)
        for index, case in enumerate(cases):
            x, first_y = 13 + index % 38, 3 if index < 38 else 8
            observed = [pixels[y * 64 + x] for y in range(first_y, first_y + 5)]
            results.append(dict(case=index, op=case['op'], value=case['value'],
                                expected_word=case['expected'], pixel_values=observed,
                                passed=observed == [0xff00ff00] * 5))
        for y in range(16):
            for x in range(64):
                expected = 0xff000040
                if 13 <= x <= 50 and 3 <= y <= 12:
                    index = x - 13 + (38 if y >= 8 else 0)
                    expected = (0xff0000ff if index == negative_case else 0xff00ff00) if index < len(cases) else 0xffff0000
                if pixels[y * 64 + x] != expected:
                    problems.append('unexpected pixel %d,%d: %08x != %08x' %
                                    (x, y, pixels[y * 64 + x], expected))
    failed = [r['case'] for r in results if not r['passed']]
    backend_ok = baseline['complete'] and baseline['execution_complete']
    return dict(schema=1, backend_verified=backend_ok, cases=len(cases),
                cases_passed=len(results) - len(failed), failed_cases=failed,
                passed=backend_ok and not problems and not failed,
                expected_negative_detected=(negative_case is not None and backend_ok
                                            and not problems and failed == [negative_case]),
                problems=problems, case_results=results, execution=baseline)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run', type=Path, required=True)
    ap.add_argument('--bundle', type=Path, required=True)
    ap.add_argument('--cases', type=Path, required=True)
    ap.add_argument('--expect-negative-case', type=int)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    result = verify_integer(args.run, args.bundle, args.cases, args.expect_negative_case)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('execution','case_results')}))
    accepted = result['expected_negative_detected'] if args.expect_negative_case is not None else result['passed']
    raise SystemExit(0 if accepted else 1)
