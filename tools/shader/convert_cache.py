"""Convert captured stage pairs to typed IR/Cg/GXP; unsupported paths stay explicit.

The Shacc workspace is a serial compiler service. Do not run multiple instances
against the same Shacc root. No generated game payload is checked into Git.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
# An embedded interpreter ships a ._pth that keeps the script directory off sys.path.
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from shader_ir import Unsupported,lower,emit_cg,link_pair

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def write_json(p,value):p.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8',newline='\n')
def linux_path(p):
    p=p.resolve().as_posix()
    return '/mnt/'+p[0].lower()+p[2:] if len(p)>2 and p[1]==':' else p

def compile_serial(args,root,timeout=50):
    lock=Path(root)/'.vp-shacc.lock'
    fd=os.open(lock,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
    try:
        os.write(fd,str(os.getpid()).encode());os.close(fd)
        return subprocess.run(args,capture_output=True,timeout=timeout)
    finally:
        lock.unlink()

def check_resources(ir,reflection,resources):
    if reflection['stage']!=(0 if ir['stage']=='vertex' else 4):raise Unsupported('reflection stage mismatch')
    if reflection['images'] or reflection['storage_buffers']:raise Unsupported('storage/images not supported')
    for r in resources:
        if r['storage']=='cbuf':
            bank=int(r['name'].split('cbuf')[1]);found=[d for d in reflection['constant_buffers'] if d['bank']==bank]
            if len(found)!=1 or found[0]['count']!=1:raise Unsupported('unknown cbuf descriptor')
            r['nvn_bank']=bank;r['upload_vectors']=r['required_vectors']
        else:
            if not r['name'].startswith('tex'):raise Unsupported('unrecognized sampler binding')
            i=int(r['name'][3:]);descs=reflection['textures']
            if i>=len(descs):raise Unsupported('sampler reflection missing')
            d=descs[i]
            if d['type']!=2 or d['depth'] or d['secondary'] or d['count']!=1 or d['shift']:
                raise Unsupported('only direct non-depth Color2D texture descriptors supported')
            r['nvn_descriptor']=d
    return resources

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inputs',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--frontend',type=Path,required=True);p.add_argument('--wsl-distro',default='Ubuntu-24.04')
    p.add_argument('--shacc',type=Path,required=True);p.add_argument('--shacc-config',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    inputs=json.loads((a.inputs/'manifest.json').read_text());reports=[]
    frontend=a.frontend.resolve(); shacc=a.shacc.resolve()
    lock=json.loads((Path(__file__).resolve().parents[2]/'config/graphics/shader-toolchain.json').read_text())
    config={}
    for line in a.shacc_config.read_text(encoding='utf-8').splitlines():
        key,value=line.split('=',1)
        if key in config:raise ValueError('duplicate Shacc configuration key')
        config[key]=value
    if set(config)!={'schema','root','service-vpk'} or config['schema']!='1':raise ValueError('unsupported Shacc configuration')
    compiler=Path(config['root'])/'fs/ur0/data/external/libshacccg.suprx'
    if (digest(shacc)!=lock['shacc']['executable_sha256'] or
        digest(Path(config['service-vpk']))!=lock['shacc']['service_vpk_sha256'] or
        digest(compiler)!=lock['libshacccg']['sha256']):raise ValueError('compiler toolchain identity mismatch')
    build=json.loads((frontend.parent/'build.json').read_text())
    if (build['upstream_commit']!=lock['maxwell_frontend']['commit'] or digest(frontend)!=build['binary_sha256']
        or digest(Path(__file__).with_name('maxwell_glsl.cpp'))!=build['adapter_sha256']):
        raise ValueError('frontend build identity mismatch')
    toolchain={'frontend_sha256':digest(frontend),'shacc_sha256':digest(shacc),
               'frontend_build':build,'service_sha256':digest(Path(config['service-vpk'])),
               'libshacccg_sha256':digest(compiler),
               'cg_flags':['-O0'],'middle_layer_sha256':digest(Path(__file__).with_name('shader_ir.py'))}
    for pipeline in inputs['pipelines']:
        folder=f'pipeline-{pipeline["index"]:04d}';dest=a.output/folder;dest.mkdir(exist_ok=True)
        report={'index':pipeline['index'],'stages':[]};irs={}
        for stage in pipeline['stages']:
            row=dict(stage);report['stages'].append(row);stem=stage['stem'];source=a.inputs/folder/f'{stem}.bin';env=source.with_suffix('.env')
            glsl=dest/f'{stem}.glsl'
            try:
                if digest(source)!=stage['code_sha256'] or digest(env)!=stage['environment_sha256']:raise Unsupported('input hash mismatch')
                if stage['stage'] not in ('vertex_b','fragment'):raise Unsupported('stage not in initial GXM subset')
                args=[str(frontend),str(source.resolve()),str(env.resolve()),str(glsl.resolve())]
                if os.name=='nt':args=['wsl','-d',a.wsl_distro,'--',*[linux_path(Path(x)) for x in args]]
                r=subprocess.run(args,capture_output=True,timeout=45)
                (dest/f'{stem}.frontend.log').write_bytes(r.stdout+r.stderr)
                if r.returncode:raise Unsupported('Maxwell frontend failed; see frontend.log')
                ir=lower(glsl.read_text(), 'vertex' if stage['stage']=='vertex_b' else 'fragment')
                cg,resources=emit_cg(ir)
                reflection=json.loads(glsl.with_suffix('.glsl.resources.json').read_text())
                row['resources']=check_resources(ir,reflection,resources)
                row['lowering_notes']=ir['lowering_notes']
                write_json(dest/f'{stem}.ir.json',ir);cg_path=dest/f'{stem}.cg'
                cg_path.write_text(cg,encoding='utf-8',newline='\n')
                profile='sce_vp_psp2' if ir['stage']=='vertex' else 'sce_fp_psp2'
                gxp=dest/f'{stem}.gxp'
                # Shacc must produce a new output; never accept a stale success.
                if gxp.exists():gxp.unlink()
                args=[str(shacc),'--config',str(a.shacc_config.resolve()),'-profile',profile,
                      '-O0','-o',str(gxp.resolve()),str(cg_path.resolve()),'--timeout','40']
                r=compile_serial(args,config['root'])
                (dest/f'{stem}.shacc.log').write_bytes(r.stdout+r.stderr)
                if r.returncode or not gxp.exists():raise Unsupported('Cg compiler failed; see shacc.log')
                binary=gxp.read_bytes()
                if len(binary)<32 or binary[:4]!=b'GXP\0' or struct.unpack_from('<I',binary,8)[0]>len(binary):
                    raise Unsupported('invalid GXP output header')
                row.update(status='compiled',glsl_sha256=digest(glsl),ir_sha256=digest(dest/f'{stem}.ir.json'),
                           cg_sha256=digest(cg_path),gxp_sha256=digest(gxp),gxp_bytes=len(binary))
                irs[ir['stage']]=ir
            except (Unsupported,subprocess.TimeoutExpired) as e:row.update(status='unsupported',reason=str(e))
        try:
            if set(irs)!= {'vertex','fragment'}:raise Unsupported('both stages must compile')
            report['linked_varyings']=link_pair(irs['vertex'],irs['fragment']);report['status']='compiled_pair'
        except Unsupported as e:report.update(status='unsupported',reason=str(e))
        reports.append(report)
        print(f'{folder}: {report["status"]}',flush=True)
    # Stages whose result depends on a uniform only the real draw can supply; the value
    # must come from the captured resource extent, never from a guess.
    needs=[f'pipeline-{p["index"]:04d}/{s["stem"]}' for p in reports for s in p['stages']
           if any(r.get('inv_size') for r in (s.get('resources') or []))]
    report={'schema':1,'task':'VP01.6.2','toolchain':toolchain,'inputs_manifest_sha256':digest(a.inputs/'manifest.json'),
            'counts':dict(collections.Counter(r['status'] for r in reports)),'pipelines':reports,
            'resource_supplied_uniforms':needs,
            'actual_game_draw_replayed':False,'hardware_verified':False}
    write_json(a.output/'report.json',report)
    print(json.dumps(report['counts']))

if __name__=='__main__':main()
