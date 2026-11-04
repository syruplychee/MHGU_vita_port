"""Compare captured SDK output with original A32 and native preparation.

Offline only: execute the locked SDK encoders, including real address queries.
Missing pages and unimplemented external calls fail; no invented resource data.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.arm_const import *

MAIN_SHA = "24c208c395c65727b21b5fa91af8381396e70cf421b0fcfee0345d8743298110"
SDK_SHA = "ad41e17d4307eade701bb5c7713b988c41ded1993d0a4a2d5381c472d8d99336"
QUERY = {0x191f8cc, 0x191f728}
STOP = 0x4000fff0


def words(data):
    return struct.unpack('<' + 'I' * (len(data) // 4), data)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def capture(directory):
    raw = (directory / 'engine-full-calls.bin').read_bytes()
    if len(raw) < 44:
        raise ValueError('Truncated capture header')
    h = words(raw[:44])
    if h[:4] != (0x31435056, 1, 44, 280) or not h[4] or not h[5]:
        raise ValueError('Invalid capture header or empty capture')
    if len(raw) != 44 + h[4] * 280:
        raise ValueError('Truncated or trailing call records')
    info = dict(zip(('count', 'pages', 'blob_bytes', 'base', 'ctx', 'state', 'scratch'), h[4:]))
    blob = (directory / 'engine-full-encoded.bin').read_bytes()
    if len(blob) != info['blob_bytes']:
        raise ValueError('Truncated or trailing encoded output')
    page_data = (directory / 'engine-full-pages.bin').read_bytes()
    if len(page_data) != info['pages'] * 4100:
        raise ValueError('Page count mismatch')
    pages = {}
    for pos in range(0, len(page_data), 4100):
        address, = struct.unpack_from('<I', page_data, pos)
        if address & 4095 or address in pages:
            raise ValueError('Duplicate or unaligned page')
        pages[address] = page_data[pos + 4:pos + 4100]
    records = [words(raw[p:p+280]) for p in range(44, len(raw), 280)]
    cursor = 0
    for r in records:
        if r[1] in QUERY:
            if r[21] != 1:
                raise ValueError('Missing query result')
            continue
        for i in (64, 67):
            start, offset, size = r[i:i+3]
            if offset != cursor or size > len(blob) - offset:
                raise ValueError('Encoded ranges are not exhaustive and ordered')
            cursor += size
    if cursor != len(blob):
        raise ValueError('Unclaimed encoded bytes')
    return info, pages, records, blob


def relocated(u, base, data):
    """Apply only defined ELF symbols and R_ARM_RELATIVE from the locked NSO."""
    u.mem_write(base, data)
    word = lambda a: struct.unpack_from('<I', data, a)[0]
    mod = word(4)
    pos = mod + word(mod + 4)
    tags = {}
    while word(pos):
        tags[word(pos)] = word(pos + 4)
        pos += 8
    for tag, size_tag in ((17, 18), (23, 2)):
        for pos in range(tags.get(tag, 0), tags.get(tag, 0) + tags.get(size_tag, 0), 8):
            address, info = struct.unpack_from('<II', data, pos)
            kind = info & 255
            value = None
            if kind == 23:
                value = word(address) + base
            elif kind in (2, 21, 22):
                sym = tags[6] + (info >> 8) * 16
                if struct.unpack_from('<H', data, sym + 14)[0]:
                    value = word(sym + 4) + base + (word(address) if kind == 2 else 0)
            if value is not None:
                u.mem_write(base + address, struct.pack('<I', value & 0xffffffff))


def execute(native, main, sdk, info, pages, records, captured, loads, symbols, sdk_base):
    u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    # The original SDK contains VSEL; this is an offline decoder, not a shipped CPU path.
    u.ctl_set_cpu_model(UC_CPU_ARM_MAX)
    mapped = set()

    def mapping(address, size):
        for p in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
            mapped.add(p)

    base, ctx, state = info['base'], info['ctx'], info['state']
    mapping(base, 0x2400000)
    mapping(sdk_base, 0xa00000)
    for address in pages:
        mapping(address, 4096)
    for address, size, data in loads:
        mapping(address, size)
    mapping(0x30000000, 0x40000)
    mapping(0x40000000, 0x10000)
    mapping(state - 0x400, 0x1600)
    # These are captured output allocations, not missing input resources. Keep
    # their real addresses and exact initial prefix instead of a fake GPU arena.
    for r in records:
        if r[1] not in QUERY:
            for i in (64, 67):
                mapping(r[i], r[i+2])
    ordered = sorted(mapped)
    start = previous = ordered[0]
    for p in ordered[1:] + [0x100000000]:
        if p != previous + 4096:
            u.mem_map(start, previous + 4096 - start)
            start = p
        previous = p
    relocated(u, base, main)
    relocated(u, sdk_base, sdk)
    for address, data in pages.items():
        u.mem_write(address, data)
    if native:
        for address, size, data in loads:
            u.mem_write(address, data)
        u.mem_write(symbols['image_base'], struct.pack('<I', base))

    functions = {r[20]: r[1] for r in records}
    for r in records:
        u.mem_write(base + r[1], struct.pack('<I', r[20]))
    pending = None
    index = 0
    encoded = bytearray()
    queries = 0
    resource_calls = []
    resources = {}
    native_state = []
    differences = []
    current = {}

    def reg(i):
        return u.reg_read(UC_ARM_REG_R0 + i)

    def read(address, size):
        try:
            return bytes(u.mem_read(address, size))
        except Exception:
            current.update(missing_address=hex(address), missing_size=size,
                           pc=hex(u.reg_read(UC_ARM_REG_PC)))
            raise

    def word(address):
        return struct.unpack('<I', read(address, 4))[0]

    def resource(kind, address, size, **extra):
        data = read(address, size)
        key = f'{kind}:{address:08x}:{size}'
        value = dict(kind=kind, address=hex(address), bytes=size,
                     sha256=sha(data), **extra)
        if key in resources and resources[key] != value:
            raise ValueError(f'Resource changed within one captured queue: {key}')
        resources[key] = value

    def inspect_resources(slot):
        if slot == 0x191fc78:
            p = reg(1)
            resource('program_header', p, 0x100)
            resource('program_packet', word(p+8), word(p+0x10)*4,
                     program=hex(p), stages=word(p+0x14))
        elif slot == 0x191f8cc:
            p = reg(0)
            pool = word(p+12)
            address = struct.unpack('<Q', read(pool+0x28, 8))[0] + word(p+0x10)
            resource('buffer_header', p, 24, pool=hex(pool),
                     pool_offset=word(p+0x10), buffer_bytes=word(p+0x14),
                     gpu_address=hex(address), payload_captured=False)
            resource('pool_header', pool, 0x40)
            resource('pool_vtable', word(pool), 0x40)
        elif slot in (0x191fc70,0x191fc74):
            resource('vertex_attrib' if slot==0x191fc70 else 'vertex_stream',
                     reg(2), reg(1)*(4 if slot==0x191fc70 else 8))
        elif slot in (0x191fc58,0x191fc5c,0x191fc60,0x191fc64,0x191fc68,0x191fc6c):
            resource(f'state_{slot:x}', reg(1), 0x100)
        elif slot == 0x191fdb4:
            sp = u.reg_read(UC_ARM_REG_SP)
            for i in range(reg(1)):
                for array in (reg(2), reg(3)):
                    p = word(array+i*4) if array else 0
                    if p:
                        resource('render_target_or_view', p, 0x100)
            for p in words(read(sp, 8)):
                if p:
                    resource('render_target_or_view', p, 0x100)

    def finish():
        nonlocal pending, index, queries
        row = records[index]
        slot = pending['slot']
        if row[1] != slot:
            raise ValueError(f'API order differs at {index}: {slot:x} != {row[1]:x}')
        if slot in QUERY:
            value = reg(0) | (reg(1) << 32)
            expected = row[22] | (row[23] << 32)
            if value != expected:
                raise ValueError(f'Query differs at {index}: {value:x} != {expected:x}')
            queries += 1
        else:
            after = words(read(pending['cmd'], 80))
            for before_index, output_index in ((3, 64), (6, 67)):
                start = pending['before'][before_index]
                end = after[before_index]
                actual = read(start, end - start) if end >= start else b''
                target, offset, size = row[output_index:output_index+3]
                expected = captured[offset:offset+size]
                encoded.extend(actual)
                if start != target or end < start or actual != expected:
                    first = next((j for j, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                 min(len(actual), len(expected)))
                    differences.append({'event': index, 'slot': hex(slot),
                        'stream': 'gpu' if before_index == 3 else 'control',
                        'start': hex(start), 'expected_start': hex(target),
                        'bytes': len(actual), 'expected_bytes': len(expected),
                        'first_byte': first,
                        'actual': actual[max(0,first-8):first+24].hex(),
                        'expected': expected[max(0,first-8):first+24].hex()})
                    if end < start or len(actual) != len(expected):
                        raise ValueError(f'SDK output ranges differ: {differences[-1]}')
        index += 1
        pending = None

    def hook(uc, pc, size, data):
        nonlocal pending
        if pending is not None and pc == pending['return']:
            finish()
        if native and pc == (symbols['vp_engine_queue_native_run'] & ~1):
            native_state.append(reg(3))
        if pc in (base + 0x13efd7c, base + 0x13efdc4):
            u.mem_write(reg(0), bytes(reg(1)))
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
            return
        if pc in (base + 0xb03d98, base + 0xb03d2c):
            resource_calls.append((pc, reg(0), reg(1)))
            raise ValueError('External resource operation requires its own runtime evidence')
        if pc == base + 0x13f0ea4:
            a = struct.unpack('<d', struct.pack('<Q', u.reg_read(UC_ARM_REG_D0)))[0]
            b = struct.unpack('<d', struct.pack('<Q', u.reg_read(UC_ARM_REG_D1)))[0]
            u.reg_write(UC_ARM_REG_D0, struct.unpack('<Q', struct.pack('<d', math.pow(a, b)))[0])
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
            return
        if pc in functions and pending is None:
            if index >= len(records):
                raise ValueError('Unexpected extra SDK call')
            slot = functions[pc]
            current.update(event=index, slot=hex(slot), function=hex(pc))
            inspect_resources(slot)
            pending = {'slot': slot, 'return': u.reg_read(UC_ARM_REG_LR), 'cmd': reg(0)}
            if slot not in QUERY:
                pending['before'] = words(read(reg(0), 80))
            if index % 1000 == 0:
                print('SDK_PROGRESS', 'native' if native else 'original', index, flush=True)
        if pc in (sdk_base+0x30d60c, sdk_base+0x30d688):
            raise ValueError('SDK allocation callback is outside this fixed-block capture')

    def missing(uc, kind, address, size, value, data):
        current.update(missing_address=hex(address), missing_size=size,
                       pc=hex(u.reg_read(UC_ARM_REG_PC)))
        return False

    u.hook_add(UC_HOOK_CODE, hook)
    u.hook_add(UC_HOOK_MEM_INVALID, missing)
    u.reg_write(UC_ARM_REG_C1_C0_2, 0xf00000)
    u.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    u.reg_write(UC_ARM_REG_SP, 0x3003fff0 if native else state+0x11d0)
    u.reg_write(UC_ARM_REG_LR, STOP)
    u.reg_write(UC_ARM_REG_R0, ctx)
    try:
        u.emu_start(symbols['vp_engine_full_run'] if native else base+0xbbe9c8,
                    STOP, count=20000000)
        if pending is not None and u.reg_read(UC_ARM_REG_PC) == pending['return']:
            finish()
        if u.reg_read(UC_ARM_REG_PC) != STOP or index != len(records):
            raise ValueError('Instruction budget exhausted or incomplete call stream')
    except Exception as error:
        raise ValueError(f'{"native" if native else "original"}: {current}: {error}') from error
    return {'calls': index, 'queries': queries, 'bytes': len(encoded),
            'encoded_sha256': sha(encoded), 'external_calls': len(resource_calls),
            'encoding_differences': differences,
            'resources': resources,
            'state': read(native_state[0] if native else state, 0x1180),
            'context': read(ctx, 0x43bb80)}


def main_cli():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('main', 'sdk', 'elf', 'capture', 'function-map', 'output'):
        parser.add_argument('--'+name, required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Output must be a fresh directory')
    args.output.mkdir(parents=True)
    summary = {'ok': False, 'scope': 'one captured queue, actual SDK encoders, fixed allocation blocks'}
    try:
        main, sdk = args.main.read_bytes(), args.sdk.read_bytes()
        if sha(main) != MAIN_SHA or sha(sdk) != SDK_SHA:
            raise ValueError('Locked decoded NSO identity mismatch')
        info, pages, records, blob = capture(args.capture)
        repo = Path(__file__).resolve().parents[3]
        slot_names = {int(v, 16): n for n, v, text in
                      [line.split() for line in (repo/'config/graphics/nvn-slots-1.4.0.tsv').read_text().splitlines()[1:]]}
        function_map = {r['name']: int(r['function_offset'], 16) for r in
                        json.loads(args.function_map.read_text())}
        sdk_bases = {r[20] - function_map[slot_names[r[1]]] for r in records}
        if len(sdk_bases) != 1:
            raise ValueError(f'Call targets do not identify one original SDK: {sdk_bases}')
        sdk_base = sdk_bases.pop()
        with args.elf.open('rb') as file:
            elf = ELFFile(file)
            loads = [(s['p_vaddr'], s['p_memsz'], s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
            symbols = {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        inputs = [args.main,args.sdk,args.elf,args.function_map]
        inputs += [args.capture/name for name in ('engine-full-calls.bin',
                   'engine-full-pages.bin','engine-full-encoded.bin')]
        summary.update(capture=info, sdk_base=hex(sdk_base),
                       identities={str(p):sha(p.read_bytes()) for p in inputs})
        old = execute(False, main, sdk, info, pages, records, blob, loads, symbols, sdk_base)
        new = execute(True, main, sdk, info, pages, records, blob, loads, symbols, sdk_base)
        for name in ('state','context'):
            summary[name+'_equal'] = old[name] == new[name]
            summary[name+'_difference_bytes'] = sum(a!=b for a,b in zip(old[name],new[name]))
            del old[name], new[name]
        summary['resources_equal'] = old['resources'] == new['resources']
        resource_rows = old.pop('resources')
        new.pop('resources')
        summary['resource_count'] = len(resource_rows)
        summary['resource_types'] = {kind:sum(row['kind']==kind for row in resource_rows.values())
                                     for kind in sorted({row['kind'] for row in resource_rows.values()})}
        (args.output/'resources.json').write_text(json.dumps(list(resource_rows.values()),indent=2),encoding='utf-8')
        summary.update(original=old, native=new,
                       ok=summary['state_equal'] and summary['context_equal']
                          and summary['resources_equal']
                          and not old['encoding_differences'] and not new['encoding_differences'])
    except Exception as error:
        summary['error'] = str(error)
    (args.output/'comparison.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    print(json.dumps(summary,indent=2))
    return 0 if summary['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main_cli())
