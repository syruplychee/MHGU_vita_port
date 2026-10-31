"""Bind the real pipeline-0000 probe's packaged GXP to its conversion evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
EXPECTED={'v':'9932938d131d1b24d6185feea28b95ff86c7e14c4aef9d653c3dc0f17a031353',
          'f':'a410b57abecf884dd4d63d6d3e52c16ca76ba495ffca7aed9e50cb09f7d61708'}

def verify(folder):
    manifest=json.loads((folder/'bundle.json').read_text())
    if manifest.get('schema')!=1 or manifest.get('fixture')!='mhgu-pipeline0':raise ValueError('unrecognized shader fixture')
    if manifest.get('profile')!='update-1.4.0' or manifest.get('maxwell_sha256')!=EXPECTED:
        raise ValueError('wrong source shader/profile identity')
    for stage in ('v','f'):
        if sha(folder/f'triangle_{stage}.gxp')!=manifest['gxp_sha256'][stage]:raise ValueError('shader bundle hash mismatch')
    return manifest

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--converted',type=Path)
    p.add_argument('--inputs',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--verify',action='store_true');a=p.parse_args()
    if a.verify:verify(a.output);print('verified real shader bundle');return
    if not a.converted or not a.inputs:p.error('--converted and --inputs are required')
    report=json.loads((a.converted/'report.json').read_text())
    if sha(a.inputs/'manifest.json')!=report['inputs_manifest_sha256']:raise ValueError('input provenance mismatch')
    pair=report['pipelines'][0]
    if pair['index']!=0 or pair['status']!='compiled_pair':raise ValueError('pipeline 0 not a compiled stage pair')
    # This probe's resource/vertex ABI is deliberately specific. Other shaders
    # must get a separate fixture, never an assumed compatible input layout.
    vertex,fragment=pair['stages']
    if vertex['stage']!='vertex_b' or fragment['stage']!='fragment':raise ValueError('unexpected stage order')
    if vertex['resources']:raise ValueError('unexpected vertex resources')
    resources=fragment['resources']
    if len(resources)!=2 or resources[0]['name']!='fs_cbuf3' or resources[0]['upload_vectors']!=1:
        raise ValueError('unexpected constant-buffer ABI')
    if resources[1]['name']!='tex0' or resources[1]['nvn_descriptor']!={'type':2,'depth':0,'secondary':0,
        'bank':2,'offset':32,'count':1,'shift':0,'size_shift':3}:raise ValueError('unexpected texture ABI')
    # A semantic fingerprint binds the bounded fixture without publishing game code.
    inputs=json.loads((a.inputs/'manifest.json').read_text())
    args=inputs['provenance']['arguments']
    if '--build-profile' not in args or args[args.index('--build-profile')+1]!='update-1.4.0':
        raise ValueError('requires the locked 1.4.0 capture profile')
    a.output.mkdir(parents=True,exist_ok=True);hashes={}
    for kind,stage in zip(('v','f'),(vertex,fragment)):
        source=a.converted/'pipeline-0000'/f'{stage["stem"]}.gxp'
        if sha(source)!=stage['gxp_sha256']:raise ValueError('converted GXP changed')
        # Keep the complete code identity in the bundle, including the fragment.
        if stage['code_sha256']!=EXPECTED[kind]:raise ValueError('wrong Maxwell fixture')
        shutil.copyfile(source,a.output/f'triangle_{kind}.gxp');hashes[kind]=sha(source)
    manifest={'schema':1,'fixture':'mhgu-pipeline0','profile':'update-1.4.0','gxp_sha256':hashes,
              'maxwell_sha256':{k:s['code_sha256'] for k,s in zip(('v','f'),(vertex,fragment))},
              'conversion_report_sha256':sha(a.converted/'report.json'),'resources':resources,
              'draw_resources':'self-authored; not an original draw capture'}
    (a.output/'bundle.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8',newline='\n')
    verify(a.output);print(json.dumps(manifest,indent=2))

if __name__=='__main__':main()
