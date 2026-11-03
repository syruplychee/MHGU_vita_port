from pathlib import Path
import hashlib,struct,json
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import *
from arguments import inputs
ROOT, image, REPO = inputs('material')
if hashlib.sha256(image).hexdigest()!='24c208c395c65727b21b5fa91af8381396e70cf421b0fcfee0345d8743298110':
 raise ValueError('Locked main image identity mismatch')
slots=[0x191fc78,0x191fc70,0x191fc74,0x191f8cc,0x191fc84,0x191f728,0x191fc9c,0x191fdd0,0x191fd60,0x191fc60,0x191fc64]
BASE=0x10000000
results=[]
for meta in sorted(ROOT.glob('case-*.txt'),key=lambda p:int(p.stem.split('-')[1])):
 lines=meta.read_text().splitlines();rc,ctx,state,mat,root,tq=map(int,lines[0].split())
 if rc: continue
 before=meta.with_name(meta.stem+'-before.bin').read_bytes()
 expected=meta.with_name(meta.stem+'-after.bin').read_bytes()
 events=[tuple(map(int,s.split())) for s in lines[1:]]
 u=Uc(UC_ARCH_ARM,UC_MODE_ARM)
 u.mem_map(0,max(0x2400000,(len(image)+4095)//4096*4096));u.mem_write(0,image)
 u.mem_map(BASE,len(before));u.mem_write(BASE,before)
 u.mem_map(0x20000000,4096);u.mem_map(0x30000000,0x10000)
 u.mem_write(0x183944c,struct.pack('<I',root))
 for i,slot in enumerate(slots):
  u.mem_write(slot,struct.pack('<I',0x20000000+i*4));u.mem_write(0x20000000+i*4,bytes.fromhex('1eff2fe1'))
 observed=[];queries=[tq]
 def r(i): return u.reg_read(UC_ARM_REG_R0+i)
 def word(p): return struct.unpack('<I',u.mem_read(p,4))[0]
 def qword(p): return struct.unpack('<Q',u.mem_read(p,8))[0]
 def result64(v):u.reg_write(UC_ARM_REG_R0,v&0xffffffff);u.reg_write(UC_ARM_REG_R1,v>>32)
 def hook(uc,p,size,data):
  if p==0x20000ff0:u.emu_stop();return
  if p==0x13efd7c:
   u.mem_write(r(0),bytes(r(1)));u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR));return
  if not 0x20000000<=p<0x20000000+len(slots)*4:return
  k=(p-0x20000000)//4;sp=u.reg_read(UC_ARM_REG_SP)
  a,b,c,e,d,f=0,0,0,0,0,0
  if k in (0,1,2,5):a,b,c=r(0),r(1),r(2)
  elif k==3:a=r(0)
  elif k==4:a,b,c,e,d=r(0),r(1),r(2),word(sp+8),qword(sp)
  elif k==6:a,b,c,d=r(0),r(1),r(2),qword(sp)
  elif k in (7,9,10):a,b=r(0),r(1)
  elif k==8:a,f=r(0),u.reg_read(UC_ARM_REG_S0)
  observed.append((k,a,b,c,e,d,f))
  if k==3:result64(0x0000000280000000)
  if k==5:
   result64(0x1122334455667788 if queries[0]==0 else 0x8877665544332211);queries[0]+=1
 u.hook_add(UC_HOOK_CODE,hook)
 u.reg_write(UC_ARM_REG_C1_C0_2,0xf00000);u.reg_write(UC_ARM_REG_FPEXC,0x40000000)
 u.reg_write(UC_ARM_REG_SP,0x3000fff0);u.reg_write(UC_ARM_REG_LR,0x20000ff0)
 for reg,value in [(UC_ARM_REG_R0,ctx),(UC_ARM_REG_R1,state),(UC_ARM_REG_R2,mat)]:u.reg_write(reg,value)
 try:u.emu_start(0xbc1764,0x20000ff0,count=100000)
 except Exception as ex:
  results.append({'case':meta.name,'ok':False,'error':str(ex)});continue
 actual=bytes(u.mem_read(BASE,len(before)))
 diff=[i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b]
 ok=observed==events and not diff and u.reg_read(UC_ARM_REG_PC)==0x20000ff0
 row={'case':meta.name,'ok':ok,'events_native':len(events),'events_original':len(observed),'state_difference_bytes':len(diff)}
 if not ok:
  for i in range(max(len(events),len(observed))):
   a=events[i] if i<len(events) else None;b=observed[i] if i<len(observed) else None
   if a!=b:row['first_event_difference']={'index':i,'native':a,'original':b};break
  row['first_memory_difference']=[hex(BASE+i) for i in diff[:12]]
 results.append(row);print(json.dumps(row))
(ROOT/'binary-comparison.json').write_text(json.dumps(results,indent=2))

raise SystemExit(0 if len(results)==10 and all(r['ok'] for r in results) else 1)
