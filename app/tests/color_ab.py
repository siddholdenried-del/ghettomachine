"""Play the same sequenced groove with Color off/on and compare the rendered WAVs.
   python3 app/tests/color_ab.py app/build/host/ghettomachine_host OUTDIR
   Run from repo root. Stereo WAVs ready to audition; USB captures verify dry stems.
"""
import array
import math
from pathlib import Path
import subprocess
import sys
import wave

host, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
track = int(sys.argv[3]) if len(sys.argv)>3 else 3
assert track in range(1,5)
demo = Path('app/tests/demo.txt').read_text().split('# play 4 bars')[0]
panel = (f'turn ALGO {track-4}\nwait 30\nexpect trk {track}\n' + 'tap FX\nwait 30\n' * 5 + 'expect page 17\n'
         'spin K1 17\nspin K4 @\nwait 30\nturn SELECT 1\nwait 30\n'
         'spin K1 49\nspin K2 53\nspin K3 37\nspin K4 51\nwait 30\n')
capture = ('tap PLAY\nwav groove-@.wav\nusbwav usb-@.wav\nwait 8000\n'
           'wavstop\nusbwavstop\ntap PLAY\nwait 850\nexpect stemovers 0\n')
for setting, toggle in [('off','0'),('on','1')]:
    script = out / f'{setting}.txt'
    script.write_text(demo + panel.replace('@',toggle) + f'expect color{track}_5 '+toggle+'\n' +
                      capture.replace('@',setting))
    log = subprocess.run([host, script, out], capture_output=True, text=True)
    (out/f'{setting}.log').write_text(log.stdout + log.stderr)
    assert log.returncode == 0, log.stdout
def pcm(path, channels):
    with wave.open(str(path),'rb') as w:
        assert w.getnchannels() == channels and w.getsampwidth() == 2 and w.getframerate() == 44100
        return array.array('h', w.readframes(w.getnframes()))
clean, wet = (pcm(out/f'usb-{s}.wav', 10) for s in ('off','on'))
assert len(clean)==len(wet) and len(clean)>3_440_000
chosen = 2*track
assert all(clean[i::10] == wet[i::10] for i in range(2,10) if i not in (chosen,chosen+1)), 'affected another track stem'
assert any(clean[i::10] != wet[i::10] for i in (chosen,chosen+1)), 'missing processed track stem'
assert any(clean[i::10] != wet[i::10] for i in range(2)), 'no processed master signal'
a,b = (pcm(out/f'groove-{s}.wav', 2) for s in ('off','on'))
assert len(a)==len(b)
def db(v): return 20*math.log10(max(1,v)/32768.)
def rms(s): return math.sqrt(sum(x*x for x in s)/len(s))
print(f'Only T{track} USB stem changed; other track stems bit-exact ({len(clean)//10} stereo frames each)')
print(f'Stereo master peaks {db(max(map(abs,a))):.1f}→{db(max(map(abs,b))):.1f} dBFS; '
      f'RMS {db(rms(a)):.1f}→{db(rms(b)):.1f} dBFS; change {db(rms([x-y for x,y in zip(a,b)])):.1f} dBFS')
assert max(map(abs,b)) < 32740, 'affected master hits output rail'
assert rms([x-y for x,y in zip(a,b)]) > 80, 'inaudibly small test difference'
