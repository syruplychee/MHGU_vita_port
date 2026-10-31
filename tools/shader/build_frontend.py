"""Build the pinned external Hades GLSL frontend on a Linux/WSL host (GCC C++20)."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess

PIN = 'f9f0fbc998279a663d0bc98937a0bde880efc8db'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--upstream', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=4)
    a = p.parse_args()
    upstream, out = a.upstream.resolve(), a.output.resolve()
    commit = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != PIN or subprocess.check_output(['git','-c','core.filemode=false','-c','core.autocrlf=true',
                                                 '-C',str(upstream),'status','--porcelain'], text=True):
        raise SystemExit('upstream must be the clean pinned checkout')
    vendor = upstream/'vendor'
    includes = [vendor, vendor/'range-v3/include', vendor/'fmt/include']
    includes += [vendor/f'boost/{n}/include' for n in ('config','assert','move','intrusive','container')]
    # GLSL only: no Vulkan, SPIRV-Cross, Rust, emulator, or sirit dependency.
    sources = [s for s in (vendor/'shader_compiler').rglob('*.cpp')
               if not any(x in s.parts for x in ('spirv','glasm'))]
    sources.append(Path(__file__).with_name('maxwell_glsl.cpp').resolve())
    out.mkdir(parents=True, exist_ok=True)
    flags = ['g++','-std=c++20','-O1','-DFMT_HEADER_ONLY','-w']
    flags += [f'-I{x}' for x in includes]
    def compile_one(s):
        obj = out/(hashlib.sha256(str(s).encode()).hexdigest()[:20]+'.o')
        stamp = obj.with_suffix('.sha256')
        digest = hashlib.sha256(s.read_bytes()+repr(flags).encode()+commit.encode()).hexdigest()
        if not obj.exists() or not stamp.exists() or stamp.read_text()!=digest:
            r = subprocess.run(flags+['-c',str(s),'-o',str(obj)],capture_output=True,text=True)
            if r.returncode:
                raise RuntimeError(f'{s}\n{r.stderr}')
            stamp.write_text(digest,encoding='utf-8',newline='\n')
        return obj
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        objects = list(pool.map(compile_one, sorted(sources)))
    exe=out/'maxwell-glsl'
    subprocess.run(['g++',*[str(o) for o in objects],'-o',str(exe)],check=True)
    report={'upstream_commit':commit,'source_count':len(sources),
            'adapter_sha256':hashlib.sha256(Path(__file__).with_name('maxwell_glsl.cpp').read_bytes()).hexdigest(),
            'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),
            'compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0]}
    (out/'build.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(report),flush=True)

if __name__=='__main__':
    main()
