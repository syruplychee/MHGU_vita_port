from pathlib import Path
import struct,json,re,hashlib
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import *
from arguments import inputs
ROOT, image, REPO = inputs('queue')
if hashlib.sha256(image).hexdigest()!='24c208c395c65727b21b5fa91af8381396e70cf421b0fcfee0345d8743298110':
 raise ValueError('Locked main image identity mismatch')
slots={int(v,16):name for name,v in re.findall(r'#define (VP_SLOT_\w+)\s+(0x[0-9a-f]+)u',(REPO/'src/platform/engine_nvn.h').read_text())}
three={0x191fd44,0x191fd48,0x191fd4c,0x191fc78,0x191fc70,0x191fc74,0x191fdc0};five={0x191fd0c,0x191fd18}
results=[]
def canonical(slot,c,st,s,read):
 if slot==0x191fdb4:return(slot,tuple(c[:2]),read(c[2],c[1]*4).hex(),read(c[3],c[1]*4).hex(),tuple(st[:2]))
 if slot==0x191fd50:return(slot,c[0],read(c[1],16).hex())
 if slot==0x191fd5c:return(slot,c[0],tuple(s[:3]))
 if slot in (0x191fd58,0x191fd54,0x191fd60):return(slot,c[0],s[0])
 if slot==0x191f8cc:return(slot,c[0])
 if slot==0x191fdbc:return(slot,c[0])
 if slot==0x191fcf4:return(slot,c[0],c[1],read(c[2],16).hex(),c[3])
 if slot==0x191fd00:return(slot,tuple(c),s[0])
 if slot==0x191fd80:return(slot,tuple(c[:3]),read(c[3],24).hex(),tuple(st[:3]))
 if slot==0x191fd84:return(slot,tuple(c[:3]),read(c[3],24).hex(),tuple(st[:2]),read(st[2],24).hex(),st[3])
 sizes={0x191fcd0:(4,0),0x191fcd8:(4,3),0x191fcdc:(4,2),0x191fce0:(4,5),0x191fc7c:(4,1),0x191fc84:(3,3),0x191fc9c:(3,2)}
 if slot in sizes:n,z=sizes[slot];return(slot,tuple(c[:n]),tuple(st[:z]))
 n=3 if slot in three else 4 if slot in five else 2
 return(slot,tuple(c[:n]),tuple(st[:1]) if slot in five else ())
for meta in sorted(ROOT.glob('case-*.txt'),key=lambda p:int(p.stem.split('-')[1])):
 rc,ctx,state,default,srgb,root,scratch,buffer=map(int,meta.read_text().split())
 if rc:continue
 before=meta.with_name(meta.stem+'-before.bin').read_bytes();expected=meta.with_name(meta.stem+'-after.bin').read_bytes()
 raw=meta.with_name(meta.stem+'-events.bin').read_bytes();native=[]
 for row in struct.iter_unpack('<20I',raw):native.append(canonical(row[1],row[2:6],row[6:14],row[15:19],lambda p,n:expected[p:p+n]))
 u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.mem_map(0,0x2400000);u.mem_write(0,image);u.mem_write(0,before)
 u.mem_map(0x40000000,0x10000);reverse={}
 for i,slot in enumerate(slots):
  p=0x40000000+i*4;reverse[p]=slot;u.mem_write(slot,struct.pack('<I',p));u.mem_write(p,bytes.fromhex('1eff2fe1'))
 for a,v in [(0x183f2b0,default),(0x183944c,root),(0x1838db0,srgb)]:u.mem_write(a,struct.pack('<I',v))
 observed=[];external=[];powers=[]
 def r(i):return u.reg_read(UC_ARM_REG_R0+i)
 def hook(uc,p,size,data):
  if p in (0x13efd7c,0x13efdc4):u.mem_write(r(0),bytes(r(1)));u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR));return
  if p in (0xb03d98,0xb03d2c):external.append((p,r(0),r(1)));u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR));return
  if p==0x13f0ea4:
   powers.append((u.reg_read(UC_ARM_REG_D0),u.reg_read(UC_ARM_REG_D1)))
   u.reg_write(UC_ARM_REG_D0,struct.unpack('<Q',struct.pack('<d',.25))[0]);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR));return
  if p not in reverse:return
  core=[r(i) for i in range(4)];sp=u.reg_read(UC_ARM_REG_SP)
  stack=struct.unpack('<8I',u.mem_read(sp,32));s=[u.reg_read(UC_ARM_REG_S0+i) for i in range(4)]
  observed.append(canonical(reverse[p],core,stack,s,lambda a,n:u.mem_read(a,n)))
  if reverse[p]==0x191f8cc:u.reg_write(UC_ARM_REG_R0,buffer&0xffffffff);u.reg_write(UC_ARM_REG_R1,buffer>>32)
 u.hook_add(UC_HOOK_CODE,hook);u.reg_write(UC_ARM_REG_C1_C0_2,0xf00000);u.reg_write(UC_ARM_REG_FPEXC,0x40000000)
 u.reg_write(UC_ARM_REG_SP,state+0x11d0);u.reg_write(UC_ARM_REG_LR,0x4000fff0);u.reg_write(UC_ARM_REG_R0,ctx)
 try:u.emu_start(0xbbe9c8,0x4000fff0,count=200000)
 except Exception as ex:print(meta.name,'ERROR',str(ex),hex(u.reg_read(UC_ARM_REG_PC)));results.append({'case':meta.name,'ok':False,'error':str(ex)});continue
 actual=bytes(u.mem_read(0,len(before)))
 def excluded(i):return scratch<=i<scratch+256 or state-0x400<=i<state or state+0x1180<=i<state+0x11d0
 diff=[i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b and not excluded(i)]
 row={'case':meta.name,'ok':native==observed and not diff and u.reg_read(UC_ARM_REG_PC)==0x4000fff0,'native_events':len(native),'original_events':len(observed),'memory_difference_bytes':len(diff),'pc':hex(u.reg_read(UC_ARM_REG_PC))}
 if not row['ok']:
  row['memory_offsets']=[hex(i) for i in diff[:15]]
  for i in range(max(len(native),len(observed))):
   a=native[i] if i<len(native) else None;b=observed[i] if i<len(observed) else None
   if a!=b:row['first_event_difference']={'index':i,'native':a,'original':b};break
 results.append(row);print(json.dumps(row))
(ROOT/'binary-comparison.json').write_text(json.dumps(results,indent=2))


raise SystemExit(0 if len(results)==14 and all(r['ok'] for r in results) else 1)
