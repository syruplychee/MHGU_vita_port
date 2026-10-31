"""Strict v11 transferable-cache reader for offline shader evidence.

Format authority: reference video_core/shader_environment.cpp Serialize and
vk_pipeline_cache.h. Captures environments and opaque pipeline keys, not draws.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

STAGES=('vertex_b','tess_control','tess_eval','geometry','fragment','compute','vertex_a')

class Reader:
    def __init__(self,data): self.data=data; self.pos=0
    def take(self,n):
        if n<0 or n>len(self.data)-self.pos: raise ValueError(f'truncated cache at {self.pos}')
        b=self.data[self.pos:self.pos+n]; self.pos+=n; return b
    def unpack(self,fmt): return struct.unpack(fmt,self.take(struct.calcsize(fmt)))

def parse(data):
    r=Reader(data)
    if r.take(12)!=b'yuzucach'+struct.pack('<I',11): raise ValueError('expected transferable cache v11')
    result=[]
    while r.pos<len(data):
        begin=r.pos; count,=r.unpack('<I'); envs=[]
        if not 1<=count<=6: raise ValueError('invalid stage count')
        for _ in range(count):
            size,nt,nf,nc,nr,local,bound,start,low,high,viewport,stage=r.unpack('<5Q7I')
            if stage>=len(STAGES) or size<8 or size%8 or high<low or high-low+8!=size or start!=low:
                raise ValueError('invalid captured code extent/stage')
            if max(nt,nf,nc,nr)>65536: raise ValueError('environment map too large')
            code=r.take(size); maps=[]
            for n,fmt in ((nt,'<II'),(nf,'<II'),(nc,'<QI'),(nr,'<QI')):
                entries=[r.unpack(fmt) for _ in range(n)]
                if len({e[0] for e in entries})!=n: raise ValueError('duplicate environment key')
                maps.append(entries)
            tail=r.take(16 if stage==5 else 80)
            if stage!=5 and (size<88 or code[:80]!=tail): raise ValueError('SPH/code mismatch')
            geometry=r.take(32) if stage==3 else b''
            envs.append(dict(stage_id=stage,stage=STAGES[stage],start=start,local=local,bound=bound,
                viewport=viewport,maps=maps,code=code,tail=tail,geometry=geometry))
        compute=envs[0]['stage_id']==5
        if compute and count!=1 or not compute and any(e['stage_id']==5 for e in envs):
            raise ValueError('mixed pipeline kinds')
        key=r.take(24 if compute else 1032)
        hashes=struct.unpack_from('<Q' if compute else '<6Q',key)
        if sum(bool(h) for h in hashes)!=count: raise ValueError('pipeline shader hash count mismatch')
        result.append(dict(offset=begin,end=r.pos,envs=envs,key=key))
    if not result: raise ValueError('empty shader cache')
    return result

def export(cache,out,provenance):
    data=cache.read_bytes(); records=parse(data); out.mkdir(parents=True,exist_ok=True)
    manifest={'schema':1,'source_sha256':hashlib.sha256(data).hexdigest(),
              'provenance':provenance,'actual_draw_resources_captured':False,'pipelines':[]}
    for i,p in enumerate(records):
        folder=out/f'pipeline-{i:04d}'; folder.mkdir(exist_ok=True)
        (folder/'key.bin').write_bytes(p['key']); row={'index':i,'stages':[]}
        for j,e in enumerate(p['envs']):
            stem=f'{j}-{e["stage"]}'; (folder/f'{stem}.bin').write_bytes(e['code'])
            maps=e['maps']; counts=[len(m) for m in maps]
            text=' '.join(map(str,[e[k] for k in ('stage_id','start','local','bound','viewport')]+counts))+'\n'
            text+=''.join(f'{k} {v}\n' for m in maps for k,v in m)
            (folder/f'{stem}.env').write_text(text,encoding='utf-8',newline='\n')
            row['stages'].append({'stage':e['stage'],'stem':stem,
                'code_sha256':hashlib.sha256(e['code']).hexdigest(),
                'environment_sha256':hashlib.sha256(text.encode()).hexdigest(),
                'code_bytes':len(e['code'])})
        manifest['pipelines'].append(row)
    manifest['pipeline_count']=len(records)
    manifest['environment_count']=sum(len(p['envs']) for p in records)
    manifest['unique_shader_payloads']=len({hashlib.sha256(e['code']).digest() for p in records for e in p['envs']})
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8',newline='\n')
    return manifest

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--cache',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True); p.add_argument('--provenance',type=Path,required=True)
    a=p.parse_args(); m=export(a.cache,a.output,json.loads(a.provenance.read_text(encoding='utf-8-sig')))
    print(json.dumps({k:v for k,v in m.items() if k not in ('pipelines','provenance')}))

if __name__=='__main__': main()
