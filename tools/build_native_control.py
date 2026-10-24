#!/usr/bin/env python3
"""Build the firmware-pinned ARM lab bridge and temporary loader; never deploy."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT=Path(__file__).resolve().parent.parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig',default=os.environ.get('ZIG','zig'))
    args=parser.parse_args()
    out=ROOT/'build/native-control';out.mkdir(parents=True,exist_ok=True)
    state=json.loads((ROOT/'tools/profiles/key37-mpc3.9.1.2.json').read_text())
    control=json.loads((ROOT/'tools/profiles/key37-mpc3.9.1.2-control.json').read_text())
    assert state['buildId']==control['buildId']
    lines=['#include <stdint.h>']
    def array(name,data):return 'static const unsigned char '+name+'[]={'+','.join(str(v) for v in data)+'};'
    lines.append(array('build_id',bytes.fromhex(state['buildId'])))
    for key,name in [('project','PROJECT_VTABLE'),('trackPool','POOL_VTABLE'),('track','TRACK_VTABLE')]:
        lines.append(f'#define {name} {state["vtables"][key]}u')
    for kind,name in [('plugin','PLUGIN_VTABLE'),('drum','DRUM_VTABLE')]:
        v=next(int(v) for v,k in state['programTypes'].items() if k==kind);lines.append(f'#define {name} {v}u')
    v=next(f['rva'] for f in control['functions'] if f['name']=='juce::MessageManager::callAsync')
    lines.append(f'#define CALL_ASYNC {v}u')
    lines.append('static const struct { uint32_t offset,vtable; } secondary[]={'+','.join('{'+str(o)+','+str(v)+'}' for o,v in state['projectSecondaryVtables'].items())+'};')
    lines.append('struct binding { uint32_t property_offset,value_offset,setter; };')
    lines.append('static const struct binding bindings[]={'+','.join('{'+','.join(str(b[k]) for k in ['propertyOffset','scriptValueOffset','setterRva'])+'}' for b in control['bindings'])+'};')
    lines.append('static const struct { uint32_t rva; unsigned char bytes[32]; } fingerprints[]={'+','.join('{'+str(f['rva'])+',{'+','.join(str(b) for b in bytes.fromhex(f['prefixHex']))+'}}' for f in control['functions'])+'};')
    (out/'native_profile.h').write_text('\n'.join(lines)+'\n')
    compiler=shlex.split(args.zig)+['cc','-target','arm-linux-gnueabihf.2.31','-mcpu=cortex_a7','-O2','-Wall','-Wextra','-Werror']
    subprocess.run(compiler+[str(ROOT/'tools/mpc_hotload.c'),'-ldl','-o',str(out/'hotload')],check=True)
    subprocess.run(compiler+[str(ROOT/'tools/mpc_native_client.c'),'-o',str(out/'native_client')],check=True)
    subprocess.run(compiler+['-shared','-fPIC',str(ROOT/'tools/tests/mpc_hotload_fixture.c'),'-o',str(out/'test_payload.so')],check=True)
    (out/'exports.map').write_text('{ local: *; };\n')
    subprocess.run(compiler+['-shared','-fPIC','-fvisibility=hidden','-I'+str(out),str(ROOT/'tools/mpc_native_bridge.c'),'-Wl,--version-script='+str(out/'exports.map'),'-ldl','-lpthread','-o',str(out/'native_bridge.so')],check=True)
    symbols=subprocess.run(['readelf','-W','--dyn-syms',str(out/'native_bridge.so')],check=True,capture_output=True,text=True).stdout
    for line in symbols.splitlines():
        fields=line.split()
        if len(fields)>=8 and fields[4] in ('GLOBAL','WEAK') and fields[6]!='UND':
            raise RuntimeError('Bridge must not export interposing symbols: '+line)
    print(out)

if __name__=='__main__':main()
