"""Prepare an isolated, cache-enabled copy of the existing Windows reference runner.

Only main.cpp is rebuilt. Existing reference source, objects and executable are
read-only. Run the emitted build.cmd in a Windows command shell with VS installed.
This is an offline evidence tool, never part of the Vita runtime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--vs',type=Path,required=True)
    a=p.parse_args(); ref=a.reference.resolve(); out=a.output.resolve()
    out.mkdir(parents=True,exist_ok=True)
    build=ref/'build-msvc-s08-clean/src/mhgu_dyn'
    source=ref/'src/mhgu_dyn/main.cpp'; original=source.read_text(encoding='utf-8')
    anchor='        system.GetCpuManager().OnGpuReady();\n'
    assert original.count(anchor)==1
    modified=original.replace('#include "video_core/gpu.h"',
        '#include "video_core/gpu.h"\n#include "video_core/renderer_base.h"\n#include "video_core/rasterizer_interface.h"')
    modified=modified.replace(anchor,anchor+
        '        system.Renderer().ReadRasterizer()->LoadDiskResources(\n'
        '            system.GetApplicationProcessProgramID(), std::stop_token{},\n'
        '            [](VideoCore::LoadCallbackStage, size_t, size_t) {});\n')
    main=out/'main.cpp'; main.write_text(modified,encoding='utf-8',newline='\n')
    tlogs=build/'mhgu_dyn.dir/Release/mhgu_dyn.tlog'
    def read(p):
        b=p.read_bytes(); return b.decode('utf-16') if b.startswith(b'\xff\xfe') else b.decode('utf-8')
    lines=read(tlogs/'CL.command.1.tlog').splitlines()
    command=next(lines[i+1] for i,l in enumerate(lines) if l.startswith('^') and l.endswith('\\MAIN.CPP'))
    command=re.sub(r'/Fo"[^"]*"',lambda _:f'/Fo"{out / "main.obj"}"',command)
    command=re.sub(r'/Fd"[^"]*"',lambda _:f'/Fd"{out / "compiler.pdb"}"',command)
    command=command.replace(str(source).upper(),str(main))
    (out/'compile.rsp').write_text(command,encoding='utf-8',newline='\n')
    command=' '.join(l for l in read(tlogs/'link.command.1.tlog').splitlines() if l and not l.startswith('^'))
    for flag,name in [('OUT','mhgu_shader_capture.exe'),('PDB','capture.pdb'),('IMPLIB','capture.lib'),('PGD','capture.pgd'),('MAP','capture.map')]:
        command=re.sub('/'+flag+r':"[^"]*"',lambda _,n=name:f'/{flag}:"{out/n}"',command,flags=re.I)
    command=re.sub(r'(?<![A-Za-z_])MHGU_DYN\.DIR\\RELEASE\\MAIN\.OBJ',lambda _:f'"{out / "main.obj"}"',command,flags=re.I)
    (out/'link.rsp').write_text(command,encoding='utf-8',newline='\n')
    cmd=f'@echo off\ncall "{a.vs.resolve()}/Common7/Tools/VsDevCmd.bat" -arch=x64 -host_arch=x64\n'
    cmd+=f'cd /d "{build}"\ncl.exe @"{out/"compile.rsp"}"\nif errorlevel 1 exit /b 1\n'
    cmd+=f'link.exe @"{out/"link.rsp"}"\nexit /b %errorlevel%\n'
    (out/'build.cmd').write_text(cmd,encoding='utf-8',newline='\n')
    (out/'source.json').write_text(json.dumps({'original_main_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
        'modified_main_sha256':hashlib.sha256(main.read_bytes()).hexdigest(),
        'purpose':'Enable existing transferable shader cache before guest Run; no game code changes'},indent=2)+'\n',encoding='utf-8',newline='\n')
    print(out/'build.cmd')

if __name__=='__main__': main()
