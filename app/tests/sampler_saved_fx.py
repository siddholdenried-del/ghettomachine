#!/usr/bin/env python3
"""Compare two SDK host builds against the same genuine format-8 sampler project and bank.
Pass two roots that each contain app/build/gen, app/build/host/o2 and third_party/fm1-base.
Only simulated data is written, under a temporary directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

fixture = Path(__file__).with_suffix('.inc').resolve()
def build(root, out, reference):
    app = root/'app'; sdk = root/'third_party/fm1-base'
    wrapper = out.with_suffix('.c')
    wrapper.write_text('#define main sdk_main\n#include "plat_host.c"\n#undef main\n#include "sampler_saved_fx.inc"\n')
    includes = [fixture.parent, app, app/'build/gen', app/'dsp', sdk/'host', sdk/'device', sdk/'include', sdk/'lib']
    flags = ['cc','-std=gnu11','-O2','-ffp-contract=off','-DPLAT_HOST','-DFM1B_CPU2=1','-DFM1B_UAC_CH=10','-DFM1B_COMPANION=1']
    if reference: flags.append('-DWRITE_REFERENCE')
    subprocess.run(flags+['-I'+str(p) for p in includes]+[wrapper,*sorted((app/'build/host/o2').glob('*.o')),'-lm','-o',out],check=True)

def streams(raw):
    lanes = [bytearray() for _ in range(4)]; start = 0
    for _ in range(300):
        for i, lane in enumerate(lanes):
            size = 256*(8 if i==0 else 4)
            lane.extend(raw[start:start+size]); start+=size
    assert start==len(raw) and all(any(lane) for lane in lanes)
    return [hashlib.sha256(lane).hexdigest() for lane in lanes]

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('original',type=Path);p.add_argument('candidate',type=Path)
    args=p.parse_args()
    with TemporaryDirectory() as td:
        tmp=Path(td); ref=tmp/'reference'; new=tmp/'candidate';result={}
        build(args.original.resolve(),ref,True);build(args.candidate.resolve(),new,False)
        for mode,active,sends in [('archived_on',1,1),('archived_off',0,1),('no_sends',0,0)]:
            img = tmp/(mode+'.img')
            subprocess.run([ref,'write',img,str(active),str(sends)],check=True,cwd=tmp,stdout=subprocess.DEVNULL)
            result[mode]={}
            for label,exe in [('original',ref),('candidate',new)]:
                pcm=tmp/(label+mode+'.pcm')
                subprocess.run([exe,'read',img,str(active),pcm],check=True,cwd=tmp,stdout=subprocess.DEVNULL)
                result[mode][label]=streams(pcm.read_bytes())
        off=result['archived_off'];on=result['archived_on'];dry=result['no_sends']
        assert off['candidate']==off['original']==on['candidate']
        assert all(a!=b for a,b in zip(on['original'],off['original']))
        assert dry['candidate']==dry['original']
        assert dry['candidate'][0]!=off['candidate'][0] and dry['candidate'][1:]==off['candidate'][1:]
        print(json.dumps({'per_lane':['int32 stereo master','int16 stereo T2 sampler USB','int16 stereo T3 sampler USB','int16 stereo T4 sampler USB'],
                          'frames_per_case':76800,'sample_frames_per_track':4096,'bank_rate':22050,'sha256':result,'passed':True},indent=2))
