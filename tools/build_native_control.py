#!/usr/bin/env python3
"""Build the firmware-pinned ARM lab bridge and temporary loader; never deploy."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT=Path(__file__).resolve().parent.parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig',default=os.environ.get('ZIG','zig'))
    args=parser.parse_args()
    subprocess.run([sys.executable,str(ROOT/'native-api/build.py'),'--zig',args.zig],check=True)
    out=ROOT/'build/native-control';out.mkdir(parents=True,exist_ok=True)
    state=json.loads((ROOT/'tools/profiles/key37-mpc3.9.1.2.json').read_text())
    control=json.loads((ROOT/'tools/profiles/key37-mpc3.9.1.2-control.json').read_text())
    assert state['buildId']==control['buildId']
    lines=['#include <stdint.h>']
    for key,name in [('project','PROJECT_VTABLE'),('trackPool','POOL_VTABLE'),('track','TRACK_VTABLE')]:
        lines.append(f'#define {name} {state["vtables"][key]}u')
    for kind,name in [('plugin','PLUGIN_VTABLE'),('drum','DRUM_VTABLE')]:
        v=next(int(v) for v,k in state['programTypes'].items() if k==kind);lines.append(f'#define {name} {v}u')
    v=next(f['rva'] for f in control['functions'] if f['name']=='juce::MessageManager::callAsync')
    lines.append(f'#define CALL_ASYNC {v}u')
    lines.append('static const struct { uint32_t offset,vtable; } secondary[]={'+','.join('{'+str(o)+','+str(v)+'}' for o,v in state['projectSecondaryVtables'].items())+'};')
    (out/'native_profile.h').write_text('\n'.join(lines)+'\n')
    compiler=shlex.split(args.zig)+['cc','-target','arm-linux-gnueabihf.2.31','-mcpu=cortex_a7','-O2','-Wall','-Wextra','-Werror']
    subprocess.run(compiler+[str(ROOT/'tools/mpc_hotload.c'),'-ldl','-o',str(out/'hotload')],check=True)
    subprocess.run(compiler+[str(ROOT/'tools/mpc_native_client.c'),'-o',str(out/'native_client')],check=True)
    subprocess.run(compiler+['-shared','-fPIC',str(ROOT/'tools/tests/mpc_hotload_fixture.c'),'-o',str(out/'test_payload.so')],check=True)
    (out/'exports.map').write_text('{ local: *; };\n')
    subprocess.run(compiler+['-shared','-fPIC','-fvisibility=hidden','-I'+str(out),'-I'+str(ROOT/'native-api/adapter/include'),str(ROOT/'tools/mpc_native_bridge.c'),'-Wl,--version-script='+str(out/'exports.map'),'-L'+str(ROOT/'build/native-api/arm'),'-lmpc_native','-Wl,-rpath,$ORIGIN','-ldl','-lpthread','-o',str(out/'native_bridge.so')],check=True)
    (out/'libmpc_native.so').write_bytes((ROOT/'build/native-api/arm/libmpc_native.so').read_bytes())
    dynamic=subprocess.run(['readelf','-d',str(out/'native_bridge.so')],check=True,capture_output=True,text=True).stdout
    if 'Shared library: [libmpc_native.so]' not in dynamic or 'Library runpath: [$ORIGIN]' not in dynamic:
        raise RuntimeError('Lab host must resolve the adapter by SONAME beside itself')
    for line in dynamic.splitlines():
        if '(NEEDED)' in line and '/' in line.split('[')[-1].split(']')[0]:
            raise RuntimeError('Absolute shared-library dependency is not deployable: '+line)
    symbols=subprocess.run(['readelf','-W','--dyn-syms',str(out/'native_bridge.so')],check=True,capture_output=True,text=True).stdout
    for line in symbols.splitlines():
        fields=line.split()
        if len(fields)>=8 and fields[4] in ('GLOBAL','WEAK') and fields[6]!='UND':
            raise RuntimeError('Bridge must not export interposing symbols: '+line)
    print(out)

if __name__=='__main__':main()
