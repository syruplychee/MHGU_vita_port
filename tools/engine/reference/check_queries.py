"""Compare folded ARM query helpers with locked original SDK instructions."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import *

from compare_sdk import SDK_SHA, relocated


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',type=Path,required=True)
    parser.add_argument('--elf',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.exists():
        parser.error('Output must be fresh')
    sdk=args.sdk.read_bytes()
    if hashlib.sha256(sdk).hexdigest()!=SDK_SHA:
        parser.error('SDK identity mismatch')
    with args.elf.open('rb') as file:
        elf=ELFFile(file)
        loads=[(s['p_vaddr'],s['p_memsz'],s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    u=Uc(UC_ARCH_ARM,UC_MODE_ARM)
    u.ctl_set_cpu_model(UC_CPU_ARM_MAX)
    mapped=set()
    for a,n,data in loads:
        mapped.update(range(a&~4095,(a+n+4095)&~4095,4096))
    ordered=sorted(mapped);start=prev=ordered[0]
    for p in ordered[1:]+[0x100000000]:
        if p!=prev+4096:
            u.mem_map(start,prev+4096-start);start=p
        prev=p
    for a,n,data in loads:
        u.mem_write(a,data)
    sdkbase=0x60000000
    u.mem_map(sdkbase,0xa00000)
    relocated(u,sdkbase,sdk)
    u.mem_map(0x30000000,0x10000)
    u.mem_map(0x40000000,0x1000)
    u.mem_map(0x4191f000,0x1000)
    u.mem_map(0x70000000,0x1000)
    put=lambda a,v:u.mem_write(a,struct.pack('<I',v))
    put(symbols['image_base'],0x40000000)
    put(0x4191f8cc,sdkbase+0x30cde4)
    put(0x30000000,1)
    put(0x3000100c,0x30002000)
    put(0x30002000,0x30003000)
    u.reg_write(UC_ARM_REG_C1_C0_2,0xf00000)
    u.reg_write(UC_ARM_REG_FPEXC,0x40000000)

    def call(function,arguments):
        u.reg_write(UC_ARM_REG_SP,0x3000fff0)
        u.reg_write(UC_ARM_REG_LR,0x70000000)
        for i,value in enumerate(arguments[:4]):u.reg_write(UC_ARM_REG_R0+i,value)
        for i,value in enumerate(arguments[4:]):put(0x3000fff0+i*4,value)
        u.emu_start(function,0x70000000,count=10000)
        if u.reg_read(UC_ARM_REG_PC)!=0x70000000:
            raise ValueError('Query did not return')
        return u.reg_read(UC_ARM_REG_R0)|(u.reg_read(UC_ARM_REG_R1)<<32)

    results=[]
    # Consecutive cases edit the same objects in place. No stale cached value
    # may survive changes to the offset, pool base, getter or pool pointer.
    for index,(base,offset,getter) in enumerate((
            (0,0,0x32708c),(0x400010000,0x462ed20,0x32708c),
            (0x4fffffff0,0x30,0x32708c),(0xfffffffffffffff0,0x30,0x32708c),
            (0x100000000,0xffffffff,0x32708c),
            (0xabcdef0123456789,0x87654321,0x327094))):
        put(0x30001010,offset)
        put(0x3000301c,sdkbase+getter)
        u.mem_write(0x30002028,struct.pack('<Q',base))
        u.mem_write(0x30002038,struct.pack('<Q',base^0xffff))
        original=call(sdkbase+0x30cde4,[0x30001000])
        native=call(symbols['buffer'],[0x30000000,0x1234,0x30001000])
        results.append(dict(kind='buffer',case=index,getter=hex(getter),ok=original==native))
    put(0x3000100c,0x30004000)
    put(0x30004000,0x30003000)
    u.mem_write(0x30004038,struct.pack('<Q',0x1234567812345678))
    results.append(dict(kind='pool-pointer-change',ok=
        call(sdkbase+0x30cde4,[0x30001000])==
        call(symbols['buffer'],[0x30000000,0x1234,0x30001000])))
    for texture,sampler in ((0,0),(1,1),(0xfffff,0x7ff),
                            (0,0x800),(0xffffffff,0),(0x12345678,0xffffffff)):
        original=call(sdkbase+0x318888,[0,texture,sampler])
        native=call(symbols['handle'],[0x30000000,0x1234,0,texture,sampler])
        results.append(dict(kind='handle',texture=texture,sampler=sampler,ok=original==native))
    report=dict(ok=all(r['ok'] for r in results),cases=results,
                sdk_sha256=hashlib.sha256(sdk).hexdigest(),
                elf_sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest())
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
    return 0 if report['ok'] else 1


if __name__=='__main__':
    raise SystemExit(main())
