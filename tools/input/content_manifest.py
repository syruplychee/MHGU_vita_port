"""Create or verify a streaming SHA256 manifest of the complete local RomFS tree."""
import argparse
import hashlib
import json
from pathlib import Path
import os


def entries(root):
    for folder, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        for name in dirs:
            path=Path(folder)/name
            if path.is_symlink() or getattr(path,'is_junction',lambda:False)():
                raise ValueError('linked directory in content tree: '+str(path))
        for name in sorted(files):
            path=Path(folder)/name
            if path.is_symlink():
                raise ValueError('symlink in content tree: '+str(path))
            stat=path.stat();h=hashlib.sha256()
            with path.open('rb') as f:
                for block in iter(lambda:f.read(4*1024*1024),b''):
                    h.update(block)
            after=path.stat()
            if (stat.st_size,stat.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):
                raise ValueError('input changed during hashing: '+str(path))
            yield {'path':path.relative_to(root).as_posix(),'bytes':stat.st_size,'sha256':h.hexdigest()}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--romfs',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--verify',type=Path)
    a=p.parse_args()
    if not a.romfs.is_dir():
        raise ValueError('RomFS directory does not exist')
    rows=list(entries(a.romfs));rows.sort(key=lambda r:r['path'])
    if not rows:
        raise ValueError('empty content tree')
    encoded=''.join(json.dumps(r,sort_keys=True,separators=(',',':'))+'\n' for r in rows).encode()
    report={'schema':1,'profile':'update-1.4.0','root':str(a.romfs.resolve()),'files':len(rows),
            'bytes':sum(r['bytes'] for r in rows),'entries_sha256':hashlib.sha256(encoded).hexdigest(),
            'entries':rows,'scope':'local tree identity; does not prove base/update composition completeness'}
    if a.verify:
        old=json.loads(a.verify.read_text())
        if old['entries']!=rows:
            raise ValueError('content tree differs from manifest')
        report['verified_against']=str(a.verify)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    temporary=a.output.with_suffix(a.output.suffix+'.tmp')
    temporary.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    temporary.replace(a.output)
    print(json.dumps({k:report[k] for k in ('files','bytes','entries_sha256')}))


if __name__=='__main__':
    main()
