from pathlib import Path
import struct,json,re,hashlib
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import *
from arguments import inputs
ROOT, image, REPO = inputs('state')
if hashlib.sha256(image).hexdigest()!='24c208c395c65727b21b5fa91af8381396e70cf421b0fcfee0345d8743298110':
 raise ValueError('Locked main image identity mismatch')
slots={int(v,16):name for name,v in re.findall(r'#define (VP_SLOT_\w+)\s+(0x[0-9a-f]+)u',(REPO/'src/platform/engine_nvn.h').read_text())}
three={0x191fd44,0x191fd48,0x191fd4c};five={0x191fd0c,0x191fd18};results=[]
for meta in sorted(ROOT.glob('case-*.txt'),key=lambda p:int(p.stem.split('-')[1])):
 rc,kind,ctx,state,obj,default,scratch=map(int,meta.read_text().split())
 if rc:continue
 before=meta.with_name(meta.stem+'-before.bin').read_bytes();expected=meta.with_name(meta.stem+'-after.bin').read_bytes()
 raw=meta.with_name(meta.stem+'-events.bin').read_bytes();native=[]
 def readbuf(mem,p,n):return bytes(mem[p:p+n])
 def canonical(slot,core,stack,s,reader):
  if slot==0x191fdb4:
   n=core[1];return (slot,tuple(core[:2]),reader(core[2],n*4).hex(),reader(core[3],n*4).hex(),tuple(stack[:2]))
  if slot==0x191fd50:return(slot,core[0],reader(core[1],16).hex())
  if slot==0x191fd5c:return(slot,core[0],tuple(s[:3]))
  n=3 if slot in three else 4 if slot in five else 2
  return(slot,tuple(core[:n]),tuple(stack[:1]) if slot in five else ())
 for row in struct.iter_unpack('<20I',raw):native.append(canonical(row[1],row[2:6],row[6:14],row[15:19],lambda p,n:readbuf(expected,p,n)))
 u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.mem_map(0,0x2400000);u.mem_write(0,image);u.mem_write(0,before)
 u.mem_map(0x40000000,0x10000);u.mem_map(0x30000000,0x10000)
 reverse={}
 for i,slot in enumerate(slots):
  p=0x40000000+i*4;reverse[p]=slot
  u.mem_write(slot,struct.pack('<I',p));u.mem_write(p,bytes.fromhex('1eff2fe1'))
 u.mem_write(0x183f2b0,struct.pack('<I',default));observed=[]
 def r(i):return u.reg_read(UC_ARM_REG_R0+i)
 def hook(uc,p,size,data):
  if p==0x13efd7c:u.mem_write(r(0),bytes(r(1)));u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR));return
  if p not in reverse:return
  core=[r(i) for i in range(4)];sp=u.reg_read(UC_ARM_REG_SP)
  stack=struct.unpack('<8I',u.mem_read(sp,32));s=[u.reg_read(UC_ARM_REG_S0+i) for i in range(4)]
  observed.append(canonical(reverse[p],core,stack,s,lambda a,n:u.mem_read(a,n)))
 u.hook_add(UC_HOOK_CODE,hook);u.reg_write(UC_ARM_REG_C1_C0_2,0xf00000);u.reg_write(UC_ARM_REG_FPEXC,0x40000000)
 u.reg_write(UC_ARM_REG_SP,0x3000fff0);u.reg_write(UC_ARM_REG_LR,0x4000fff0)
 for reg,v in [(UC_ARM_REG_R0,ctx),(UC_ARM_REG_R1,state),(UC_ARM_REG_R2,obj)]:u.reg_write(reg,v)
 try:u.emu_start(0xbc13e0 if kind else 0xbc0e98,0x4000fff0,count=100000)
 except Exception as ex:results.append({'case':meta.name,'ok':False,'error':str(ex)});continue
 actual=bytes(u.mem_read(0,len(before)))
 diff=[i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b and not scratch<=i<scratch+128]
 row={'case':meta.name,'kind':kind,'ok':native==observed and not diff and u.reg_read(UC_ARM_REG_PC)==0x4000fff0,'native_events':len(native),'original_events':len(observed),'memory_difference_bytes':len(diff)}
 if not row['ok']:
  row['memory_offsets']=[hex(i) for i in diff[:12]]
  for i in range(max(len(native),len(observed))):
   a=native[i] if i<len(native) else None;b=observed[i] if i<len(observed) else None
   if a!=b:row['first_event_difference']={'index':i,'native':a,'original':b};break
 results.append(row);print(json.dumps(row))
(ROOT/'binary-comparison.json').write_text(json.dumps(results,indent=2))


raise SystemExit(0 if len(results)==6 and all(r['ok'] for r in results) else 1)
