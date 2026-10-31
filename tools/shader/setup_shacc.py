"""Prepare a pinned Shacc service in an isolated filesystem using installed firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary-dir',type=Path,required=True);p.add_argument('--compiler',type=Path,required=True)
    p.add_argument('--firmware-root',type=Path,required=True);p.add_argument('--root',type=Path,required=True)
    p.add_argument('--config',type=Path,required=True);a=p.parse_args()
    lock=json.loads((Path(__file__).resolve().parents[2]/'config/graphics/shader-toolchain.json').read_text())
    def checked(path,expected):
        if hashlib.sha256(path.read_bytes()).hexdigest()!=expected:raise ValueError('tool hash mismatch: '+str(path))
    binary=a.binary_dir.resolve();service=binary/'vita3k-shacc-service.vpk'
    checked(binary/'vita3k-shacc.exe',lock['shacc']['executable_sha256'])
    checked(service,lock['shacc']['service_vpk_sha256']);checked(a.compiler,lock['libshacccg']['sha256'])
    firmware=a.firmware_root.resolve();root=a.root.resolve();fs=root/'fs'
    if firmware==root or firmware in root.parents or root in firmware.parents:
        raise ValueError('compiler root must be isolated from installed firmware')
    for name in ('vs0','pd0','os0','sa0'):
        if not (firmware/name).is_dir():raise ValueError('missing installed firmware partition '+name)
    fs.mkdir(parents=True,exist_ok=True)
    for name in ('vs0','pd0','os0','sa0'):shutil.copytree(firmware/name,fs/name,dirs_exist_ok=True)
    dest=fs/'ur0/data/external';dest.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(a.compiler,dest/'libshacccg.suprx')
    with zipfile.ZipFile(service) as z:
        app=fs/'ux0/app/V3KSHACC1';app.mkdir(parents=True,exist_ok=True)
        for entry in z.infolist():
            name=entry.filename.replace('\\','/')
            if Path(name).is_absolute() or '..' in Path(name).parts or ':' in name:
                raise ValueError('service VPK traversal')
        z.extractall(app)
    a.config.parent.mkdir(parents=True,exist_ok=True)
    a.config.write_text(f'schema=1\nroot={root.as_posix()}\nservice-vpk={service.as_posix()}\n',encoding='utf-8',newline='\n')
    print('Prepared isolated Shacc configuration: '+str(a.config.resolve()))

if __name__=='__main__':main()
