#!/usr/bin/env python3
"""GhettoMachine's factory sounds (0.5, the second set): 129 presets in six banks by kind, so PRESETS never steps
through a mix: MONO BASS (24), MONO LEAD, POLY, STAB, HIT, DRUM (21 each). Writes app/dsp/preset_table.inc, one
ENTRY(name, mono, color, V(the 26 first parameters), X(the rest, or 0)) a sound, in the order of dsp/synth.h SP_*.
  python3 app/tools/make_presets.py app/dsp/preset_table.inc
After a change: tests/synth_levels.c --write app/dsp/synth_levels.inc (the level trims)."""
import sys
SRC = dict(lfo1='MS_LFO1', lfo2='MS_LFO2', menv='MS_MENV', fenv='MS_FENV', aenv='MS_AENV', vel='MS_VEL', note='MS_NOTE',
           rnd='MS_RND', m1='MS_M1', m2='MS_M2', m3='MS_M3', m4='MS_M4', press='MS_PRESS')
DST = dict(pitch='MD_PITCH', osc2='MD_OSC2', detune='MD_DETUNE', cut='MD_CUTOFF', res='MD_RESO', pwm='MD_PWM', mix='MD_MIX',
           sub='MD_SUB', noise='MD_NOISE', drive='MD_DRIVE', fm='MD_FM', ring='MD_RING', shape='MD_SHAPE', bias='MD_BIAS',
           level='MD_LEVEL', pan='MD_PAN', l2rate='MD_L2RATE')
EXT = dict(wave2='SP_WAVE2', fm='SP_FM', ring='SP_RING', drift='SP_DRIFT', shtype='SP_SHTYPE', shape='SP_SHAPE', bias='SP_BIAS',
           shpos='SP_SHPOS', m1='SP_M1', m2='SP_M2', m3='SP_M3', m4='SP_M4', ma='SP_MA', md='SP_MD', ms='SP_MS', mr='SP_MR',
           l2shape='SP_L2SHAPE', l2rate='SP_L2RATE', l2fade='SP_L2FADE', l2key='SP_L2KEY')
SAW, SQR, TRI, SIN, PWM, SYNC, SUBW = range(7)
LP, BP, HP = range(3)
OUT = []
def P(name, mono, color='CLEAN', wave=SAW, det=0, mix=0, intv=0, sub=0, noise=0, pwm=64, drive=0, cut=90, res=30, fenv=20,
      ftype=LP, ktr=64, vel=64, spread=20, leg=1, voices=None, glide=0, aa=0, ad=60, as_=100, ar=40, fa=0, fd=50, fs=40,
      fr=40, mods=(), cut_db=0, **ext):
    if cut_db:                            # a loud sound brought down inside itself: MACRO 4 (127) into LEVEL
        ext = dict(ext, m4=127)
        mods = list(mods) + [('m4', 'level', -cut_db)]
    assert len(name) <= 13 and len(mods) <= 6, name
    v = [wave, det, mix, intv, sub, noise, pwm, drive, cut, res, fenv, ftype, ktr, vel, spread, leg,
         voices if voices is not None else (1 if mono else 8), glide, aa, ad, as_, ar, fa, fd, fs, fr]
    x = []
    for k, val in ext.items():
        x += [EXT[k], str(val)]
    for n, (s, d, a) in enumerate(mods, 1):
        x.append('MOD(%d,%s,%s,%d)' % (n, SRC[s], DST[d], a))
    OUT.append('ENTRY("%s",%d,%s,V(%s),%s)' % (name, 1 if mono else 0, color, ','.join(map(str, v)),
                                                 'X(' + ','.join(x) + ')' if x else '0'))
def bank(title, n):
    OUT.append('/* ---- %s (%d) ---- */' % (title, n))

# ------------------------------------------------------------------ MONO BASS (24): ids 0..23
bank('MONO BASS', 24)
B = dict(mono=1, vel=70, ktr=64)
P('ACID SAW', color='EDGE', wave=SAW, drive=25, cut=42, res=108, fenv=48, vel=80, leg=2, glide=25, ad=50, as_=90, ar=25, fd=45, fs=0, fr=25, mono=1, cut_db=14)
P('ACID SQUARE', color='EDGE', wave=SQR, drive=20, cut=40, res=104, fenv=50, vel=80, leg=2, glide=25, ad=50, as_=90, ar=25, fd=42, fs=0, fr=25, mono=1, cut_db=14)
P('ACID DRIVE', color='EDGE', wave=SAW, drive=85, cut=46, res=100, fenv=44, vel=85, leg=2, glide=25, ad=52, as_=95, ar=24, fd=48, fs=0, fr=25, mono=1,
  shtype=1, shape=30, mods=[('m1', 'shape', 50)], cut_db=14)
P('RUBBER', wave=SQR, sub=30, pwm=48, drive=20, cut=46, res=70, fenv=51, vel=75, leg=2, glide=21, ad=43, as_=73, ar=25, fd=40, fs=4, fr=27, mono=1)
P('SUB SINE', wave=SIN, sub=40, drive=10, cut=60, res=0, fenv=0, ktr=0, vel=40, ad=60, as_=115, ar=28, fd=50, fs=40, fr=25, aa=2, mono=1)
P('808 SUB', wave=SIN, drive=30, cut=70, res=0, fenv=0, ktr=20, vel=50, ad=92, as_=0, ar=60, mono=1, md=38, mods=[('menv', 'pitch', 26), ('m1', 'drive', 60)])
P('REESE', color='EDGE', wave=SAW, det=34, mix=64, drive=30, cut=48, res=25, fenv=10, vel=50, ad=70, as_=110, ar=35, fd=60, fs=60, fr=35, mono=1,
  drift=30, l2rate=30, l2key=1, mods=[('lfo2', 'detune', 20), ('m1', 'cut', 45)])
P('FM BASS', wave=SIN, intv=0, drive=20, cut=95, res=15, fenv=10, ad=58, as_=95, ar=25, fd=50, fs=50, fr=30, mono=1, fm=32, md=44, mr=30,
  mods=[('menv', 'fm', 46), ('vel', 'fm', 20), ('m1', 'fm', 40)])
P('FM GROWL', color='EDGE', wave=SIN, intv=12, drive=45, cut=80, res=30, fenv=20, ad=62, as_=100, ar=28, fd=55, fs=30, fr=30, mono=1, fm=70, md=60,
  shtype=0, shape=35, l2rate=70, mods=[('menv', 'fm', 30), ('lfo2', 'fm', 22), ('m1', 'shape', 50)], cut_db=14)
P('ELECTRO BASS', wave=SQR, sub=55, pwm=64, drive=35, cut=54, res=40, fenv=38, vel=75, ad=44, as_=60, ar=22, fd=36, fs=10, fr=24, mono=1)
P('ZAP BASS', color='EDGE', wave=SAW, drive=30, cut=40, res=85, fenv=60, vel=75, ad=46, as_=50, ar=22, fd=30, fs=0, fr=22, mono=1, md=24,
  mods=[('menv', 'pitch', 30), ('m1', 'res', 30)])
P('PULSE BASS', wave=PWM, sub=30, pwm=40, drive=20, cut=58, res=35, fenv=30, ad=55, as_=95, ar=26, fd=45, fs=20, fr=28, mono=1, l2rate=60, l2key=1,
  mods=[('lfo2', 'pwm', 40), ('m1', 'cut', 40)])
P('SYNC BASS', color='EDGE', wave=SYNC, det=45, mix=90, drive=25, cut=70, res=30, fenv=35, ad=52, as_=90, ar=25, fd=44, fs=10, fr=26, mono=1, md=50,
  mods=[('menv', 'detune', 50), ('m1', 'detune', 50)])
P('FOLD BASS', wave=TRI, sub=25, drive=10, cut=85, res=15, fenv=15, ad=56, as_=95, ar=26, fd=50, fs=30, fr=28, mono=1, shtype=0, shape=55, md=48,
  mods=[('menv', 'shape', 40), ('m1', 'shape', 50)])
P('CRUSH BASS', color='DUST', wave=SQR, sub=60, cut=78, res=30, fenv=30, ad=55, as_=90, ar=28, fd=45, fs=30, fr=30, mono=1, shtype=3, shape=62, shpos=1,
  mods=[('m1', 'shape', 55)])
P('WOBBLE', color='EDGE', wave=SAW, det=8, mix=64, intv=-12, drive=40, cut=38, res=62, fenv=0, ad=60, as_=110, ar=30, mono=1, l2shape=1, l2rate=52, l2key=3,
  mods=[('lfo2', 'cut', 36), ('m1', 'l2rate', 40)])
P('DEEP HOUSE', color='TAPE', wave=TRI, sub=44, drive=8, cut=62, res=14, fenv=15, ktr=48, vel=58, glide=10, aa=2, ad=54, as_=95, ar=29, fd=43, fs=20, fr=32, mono=1, wave2=4)
P('ORGAN BASS', wave=SQR, det=3, mix=64, intv=12, cut=88, res=5, fenv=0, vel=30, aa=1, ad=30, as_=127, ar=18, mono=1)
P('PLUCK BASS', wave=SAW, sub=20, drive=15, cut=36, res=45, fenv=58, vel=80, ad=48, as_=0, ar=30, fd=34, fs=0, fr=30, mono=1)
P('OCTAVE BASS', wave=SAW, det=6, mix=64, intv=-12, drive=25, cut=58, res=35, fenv=32, ad=56, as_=90, ar=26, fd=46, fs=20, fr=28, mono=1)
P('NOISE BASS', color='DUST', wave=SAW, noise=40, drive=60, cut=50, res=50, fenv=40, ad=50, as_=70, ar=24, fd=38, fs=10, fr=24, mono=1)
P('RES SWEEP', wave=SAW, drive=20, cut=30, res=95, fenv=55, ad=70, as_=110, ar=35, fa=58, fd=70, fs=60, fr=40, mono=1, mods=[('m1', 'cut', 45)], cut_db=14)
P('DARK ROLL', wave=SAW, sub=35, drive=30, cut=38, res=30, fenv=22, vel=75, ad=40, as_=40, ar=20, fd=32, fs=0, fr=22, mono=1)
P('HOOVER LOW', color='EDGE', wave=PWM, det=40, mix=64, intv=-12, pwm=30, drive=30, cut=70, res=20, fenv=15, glide=30, ad=70, as_=110, ar=35, mono=1, voices=3,
  spread=50, l2rate=64, l2key=1, mods=[('lfo2', 'pwm', 45), ('menv', 'pitch', -14)], md=40)

# ------------------------------------------------------------------ MONO LEAD (21)
bank('MONO LEAD', 21)
P('SAW LEAD', wave=SAW, det=12, mix=64, drive=10, cut=85, res=30, fenv=20, glide=30, aa=2, ad=60, as_=100, ar=45, fd=55, fs=40, fr=45, mono=1, l2fade=55,
  mods=[('lfo2', 'pitch', 4), ('m1', 'cut', 40)])
P('SQUARE LEAD', wave=SQR, drive=10, cut=88, res=25, fenv=15, glide=25, aa=2, ad=60, as_=100, ar=42, mono=1, l2fade=50, mods=[('lfo2', 'pitch', 4)])
P('SYNC LEAD', color='EDGE', wave=SYNC, det=50, mix=100, drive=15, cut=100, res=20, fenv=10, glide=20, aa=2, ad=60, as_=100, ar=40, mono=1, md=64,
  mods=[('menv', 'detune', 60), ('m1', 'detune', 60)])
P('ACID LEAD', color='EDGE', wave=SAW, drive=40, cut=60, res=112, fenv=45, leg=2, glide=30, ad=55, as_=95, ar=30, fd=48, fs=10, fr=30, mono=1)
P('PWM LEAD', wave=PWM, det=10, mix=64, pwm=40, cut=92, res=20, fenv=10, glide=25, aa=2, ad=60, as_=105, ar=45, mono=1, l2rate=55, l2key=1,
  mods=[('lfo2', 'pwm', 50)])
P('FM LEAD', wave=SIN, intv=12, cut=110, res=10, fenv=0, glide=20, aa=2, ad=60, as_=100, ar=40, mono=1, fm=45, md=55, l2fade=50,
  mods=[('menv', 'fm', 30), ('lfo2', 'pitch', 4), ('m1', 'fm', 50)], cut_db=14)
P('FOLD LEAD', wave=TRI, cut=110, res=20, fenv=10, glide=20, aa=2, ad=60, as_=110, ar=40, mono=1, shape=52, md=70, l2fade=60,
  mods=[('lfo2', 'pitch', 4), ('menv', 'shape', 30), ('m1', 'shape', 60)], cut_db=14)
P('RING LEAD', color='EDGE', wave=SAW, det=15, intv=7, cut=105, res=15, fenv=5, glide=15, aa=2, ad=60, as_=100, ar=40, mono=1, wave2=4, ring=100, md=45,
  mods=[('menv', 'osc2', -18), ('m1', 'osc2', 24)])
P('SINE GLIDE', wave=SIN, cut=127, res=0, fenv=0, ktr=0, glide=55, aa=4, ad=60, as_=115, ar=45, mono=1, l2fade=40, mods=[('lfo2', 'pitch', 3)])
P('WHISTLE', drive=20, color='TAPE', wave=SIN, noise=12, cut=100, res=20, fenv=0, ftype=BP, ktr=127, glide=35, aa=30, ad=60, as_=110, ar=45, mono=1, l2rate=96, l2fade=45,
  mods=[('lfo2', 'pitch', 5)])
P('HOOVER', color='EDGE', wave=PWM, det=45, mix=64, intv=12, pwm=28, drive=25, cut=88, res=15, fenv=10, glide=35, aa=2, ad=70, as_=110, ar=40, mono=1, voices=4,
  spread=70, l2rate=62, l2key=1, md=45, mods=[('lfo2', 'pwm', 45), ('menv', 'pitch', -16)])
P('LASER', color='EDGE', wave=SAW, cut=100, res=70, fenv=30, ad=50, as_=0, ar=40, fd=40, fs=0, fr=40, mono=1, md=46, mods=[('menv', 'pitch', 52), ('m1', 'res', 30)])
P('SIREN', wave=SQR, pwm=64, cut=95, res=30, fenv=0, aa=4, ad=60, as_=115, ar=40, mono=1, l2shape=1, l2rate=70, l2key=1, mods=[('lfo2', 'pitch', 26), ('m1', 'l2rate', 40)])
P('TALK', wave=SAW, det=8, mix=64, drive=20, cut=58, res=95, fenv=40, ftype=BP, glide=20, ad=60, as_=100, ar=35, fa=30, fd=55, fs=30, fr=35, mono=1,
  mods=[('m1', 'cut', 50), ('press', 'cut', 30)])
P('DETUNE LEAD', wave=SAW, det=28, mix=64, drive=10, cut=92, res=20, fenv=10, glide=15, aa=2, ad=60, as_=105, ar=45, mono=1, drift=25)
P('SUPERSAW', wave=SAW, det=22, mix=64, cut=96, res=15, fenv=10, vel=50, spread=70, voices=4, aa=5, ad=60, as_=110, ar=55, fd=60, fs=80, fr=50, mono=1, drift=20)
P('SOLO BRASS', wave=SAW, det=8, mix=50, cut=50, res=20, fenv=42, glide=12, aa=28, ad=68, as_=95, ar=42, fa=32, fd=58, fs=60, fr=42, mono=1, l2fade=60,
  mods=[('lfo2', 'pitch', 3), ('press', 'cut', 25)])
P('CHIP LEAD', color='DUST', wave=PWM, pwm=18, cut=120, res=0, fenv=0, ktr=0, ad=55, as_=100, ar=25, mono=1, l2shape=4, l2rate=100, mods=[('m1', 'pwm', 50)])
P('SCREAM', color='EDGE', wave=SAW, drive=100, cut=72, res=105, fenv=30, glide=25, aa=2, ad=60, as_=105, ar=40, fd=50, fs=50, fr=40, mono=1, shtype=1, shape=45,
  l2fade=45, mods=[('lfo2', 'pitch', 5), ('m1', 'cut', 40)], cut_db=14)
P('DUB SIREN', color='DUST', wave=SIN, cut=127, res=0, fenv=0, ktr=0, aa=2, ad=60, as_=115, ar=50, mono=1, l2shape=2, l2rate=58, l2key=1,
  mods=[('lfo2', 'pitch', 34), ('m1', 'l2rate', 50)])
P('PORTA LEAD', color='TAPE', wave=TRI, det=6, mix=40, cut=84, res=25, fenv=18, glide=62, aa=3, ad=60, as_=105, ar=48, mono=1, drift=30, l2fade=55,
  mods=[('lfo2', 'pitch', 4)])

# ------------------------------------------------------------------ POLY (21): keys, then pads
bank('POLY', 21)
P('E PIANO', color='TAPE', wave=SIN, intv=12, cut=127, res=0, fenv=0, ktr=0, ad=82, as_=20, ar=58, mono=0, fm=22, md=58, mr=50,
  mods=[('menv', 'fm', 28), ('vel', 'fm', 34), ('m1', 'fm', 40)], cut_db=26)
P('FM BELL', wave=SIN, intv=22, cut=127, res=0, fenv=0, ktr=0, ad=95, as_=0, ar=85, mono=0, fm=15, md=75, mr=60,
  mods=[('menv', 'fm', 40), ('vel', 'fm', 20), ('m1', 'fm', 50)], cut_db=26)
P('GLASS', wave=SIN, intv=19, cut=127, res=0, fenv=0, ktr=0, ad=78, as_=0, ar=70, mono=0, fm=28, md=40, mr=40, wave2=4, mods=[('menv', 'fm', 36), ('m1', 'ring', 40)], cut_db=26)
P('ORGAN', wave=SQR, det=3, mix=64, intv=12, cut=110, res=0, fenv=0, ktr=0, vel=20, aa=2, ad=20, as_=127, ar=20, fa=0, fd=20, fs=127, fr=20, mono=0, cut_db=14)
P('HOUSE ORGAN', color='TAPE', wave=TRI, det=2, mix=70, intv=19, cut=92, res=5, fenv=22, ktr=30, vel=45, ad=40, as_=100, ar=24, fd=30, fs=30, fr=24, mono=0, wave2=4,
  mods=[('m1', 'mix', 40)], cut_db=14)
P('PLUCK', wave=SAW, det=8, mix=50, cut=30, res=40, fenv=55, ad=50, as_=0, ar=45, fd=42, fs=0, fr=42, mono=0)
P('DIGI PLUCK', wave=SIN, intv=12, cut=120, res=0, fenv=0, ktr=0, ad=52, as_=0, ar=45, mono=0, fm=40, md=36, mr=36, mods=[('menv', 'fm', 50), ('vel', 'fm', 25)], cut_db=14)
P('CLAV', color='EDGE', wave=PWM, pwm=22, cut=64, res=55, fenv=40, ftype=BP, ktr=100, vel=90, ad=48, as_=10, ar=30, fd=36, fs=0, fr=30, mono=0)
P('VIBES', wave=SIN, intv=24, cut=127, res=0, fenv=0, ktr=0, ad=84, as_=0, ar=75, mono=0, fm=10, md=30, mr=30, l2rate=84, l2key=1,
  mods=[('menv', 'fm', 30), ('lfo2', 'level', -14)], cut_db=26)
P('SINE KEYS', wave=SIN, det=6, mix=50, cut=127, res=0, fenv=0, ktr=0, aa=2, ad=65, as_=30, ar=55, mono=0)
P('SOFT PAD', color='TAPE', wave=SAW, det=20, mix=64, noise=4, cut=60, res=10, fenv=10, ktr=32, vel=40, aa=75, ad=80, as_=110, ar=85, fa=70, fd=80, fs=90, fr=85, mono=0)
P('PWM STRINGS', wave=PWM, det=8, mix=40, pwm=40, cut=75, res=20, fenv=8, ktr=48, vel=40, aa=60, ad=80, as_=110, ar=80, fa=50, fd=70, fs=100, fr=80, mono=0,
  l2rate=50, l2key=1, mods=[('lfo2', 'pwm', 35)], cut_db=14)
P('DRIFT PAD', color='DRIFT', wave=SAW, det=14, mix=64, cut=60, res=15, fenv=15, ktr=32, vel=40, aa=70, ad=80, as_=110, ar=85, fa=80, fd=90, fs=100, fr=85, mono=0,
  drift=70, l2rate=40, l2key=1, mods=[('lfo2', 'pan', 40), ('lfo2', 'cut', 8), ('m1', 'cut', 40)])
P('GLASS PAD', wave=SIN, intv=12, cut=127, res=0, fenv=0, ktr=0, vel=40, aa=72, ad=85, as_=105, ar=88, mono=0, fm=18, ma=80, md=90, ms=70, mr=85, drift=30,
  mods=[('menv', 'fm', 26), ('m1', 'fm', 40)], cut_db=26)
P('DARK PAD', color='TAPE', wave=SAW, det=18, mix=64, sub=30, cut=40, res=20, fenv=10, ktr=20, vel=35, aa=78, ad=85, as_=115, ar=90, fa=85, fd=90, fs=90, fr=90, mono=0,
  drift=35, mods=[('m1', 'cut', 45), ('press', 'cut', 30)])
P('SWEEP PAD', wave=SAW, det=16, mix=64, cut=34, res=55, fenv=40, ktr=32, vel=35, aa=68, ad=85, as_=110, ar=88, fa=96, fd=100, fs=70, fr=90, mono=0,
  l2rate=24, l2key=1, mods=[('lfo2', 'cut', 16), ('lfo2', 'pan', 30)])
P('VOX PAD', color='TAPE', wave=PWM, det=10, mix=50, pwm=30, cut=66, res=70, fenv=5, ftype=BP, ktr=64, vel=35, aa=70, ad=85, as_=110, ar=85, mono=0, drift=40,
  l2rate=30, l2key=1, mods=[('lfo2', 'cut', 10), ('m1', 'cut', 40)])
P('SHIMMER', wave=SAW, det=24, mix=64, intv=12, cut=70, res=20, fenv=0, ftype=HP, ktr=64, vel=35, aa=72, ad=85, as_=105, ar=90, mono=0, wave2=4, ring=40, drift=40,
  l2rate=45, l2key=1, mods=[('lfo2', 'pan', 45)])
P('TAPE PAD', color='DUST', wave=TRI, det=22, mix=64, noise=8, cut=58, res=10, fenv=8, ktr=32, vel=35, aa=66, ad=85, as_=110, ar=86, mono=0, drift=110,
  l2rate=18, l2key=1, mods=[('lfo2', 'pitch', 3)], cut_db=14)
P('MOTION PAD', color='DRIFT', wave=PWM, det=12, mix=64, pwm=50, cut=56, res=35, fenv=10, ktr=32, vel=35, aa=64, ad=85, as_=110, ar=86, mono=0, l2shape=1, l2rate=44,
  l2key=3, mods=[('lfo2', 'cut', 22), ('lfo2', 'pan', 50), ('m1', 'l2rate', 40)])
P('NOISE WASH', color='DUST', wave=SAW, det=30, mix=64, noise=90, cut=50, res=60, fenv=30, ftype=BP, ktr=0, vel=30, aa=80, ad=90, as_=100, ar=95, fa=100, fd=100, fs=60, fr=95,
  mono=0, mods=[('m1', 'cut', 50)])

# ------------------------------------------------------------------ STAB (21): short chords (POLY)
bank('STAB', 21)
S = dict(mono=0, as_=0, vel=75)
P('RAVE STAB', color='EDGE', wave=PWM, det=30, mix=64, intv=12, drive=30, cut=85, res=20, fenv=10, ad=45, ar=40, fd=45, fs=0, fr=40, **S)
P('HOUSE STAB', color='TAPE', wave=SQR, det=6, mix=64, intv=7, cut=60, res=25, fenv=35, ad=44, ar=34, fd=36, fs=0, fr=34, **S)
P('DUB CHORD', color='DUST', wave=SAW, det=10, mix=64, intv=7, cut=58, res=70, fenv=30, ftype=BP, ad=46, ar=48, fd=40, fs=0, fr=48, mods=[('m1', 'cut', 45)], **S)
P('TECHNO STAB', color='EDGE', wave=SAW, det=14, mix=64, drive=35, cut=48, res=55, fenv=45, ad=40, ar=30, fd=30, fs=0, fr=30, **S)
P('MINOR STAB', wave=SAW, det=8, mix=64, intv=3, cut=64, res=30, fenv=35, ad=44, ar=36, fd=36, fs=0, fr=36, **S)
P('MAJOR STAB', wave=SAW, det=8, mix=64, intv=4, cut=66, res=30, fenv=35, ad=44, ar=36, fd=36, fs=0, fr=36, **S)
P('FIFTH STAB', wave=SQR, det=8, mix=64, intv=7, cut=70, res=25, fenv=30, ad=44, ar=36, fd=36, fs=0, fr=36, **S)
P('OCTAVE STAB', wave=SAW, det=10, mix=64, intv=12, cut=72, res=25, fenv=30, ad=42, ar=34, fd=34, fs=0, fr=34, **S)
P('DETROIT STAB', color='TAPE', wave=SAW, det=12, mix=50, intv=7, cut=44, res=45, fenv=50, ad=50, ar=44, fd=44, fs=0, fr=44, wave2=2, mods=[('vel', 'cut', 30)], **S)
P('PWM STAB', wave=PWM, det=14, mix=64, pwm=34, cut=74, res=20, fenv=25, ad=46, ar=38, fd=38, fs=0, fr=38, **S)
P('FM STAB', wave=SIN, intv=12, cut=120, res=0, fenv=0, ktr=0, ad=44, ar=36, fm=55, md=34, mr=34, mods=[('menv', 'fm', 50), ('vel', 'fm', 30)], **S, cut_db=14)
P('ORGAN STAB', wave=SQR, det=3, mix=64, intv=12, cut=100, res=5, fenv=0, ad=36, ar=22, **S)
P('BRASS STAB', wave=SAW, det=10, mix=64, cut=42, res=20, fenv=50, ad=52, ar=40, fa=18, fd=44, fs=0, fr=40, **S)
P('SYNC STAB', color='EDGE', wave=SYNC, det=50, mix=100, cut=95, res=15, fenv=10, ad=44, ar=34, md=40, mods=[('menv', 'detune', 60)], **S)
P('HOOVER STAB', color='EDGE', wave=PWM, det=44, mix=64, intv=12, pwm=28, drive=25, cut=86, res=15, fenv=10, ad=50, ar=40, spread=60, md=36, mods=[('menv', 'pitch', -14)], **S)
P('PIANO STAB', wave=SIN, intv=12, cut=127, res=0, fenv=0, ktr=0, ad=56, ar=40, fm=30, md=40, mr=40, mods=[('menv', 'fm', 34), ('vel', 'fm', 36)], **S, cut_db=26)
P('ACID STAB', color='EDGE', wave=SAW, det=6, mix=40, drive=30, cut=40, res=105, fenv=52, ad=42, ar=30, fd=32, fs=0, fr=30, **S)
P('NOISE STAB', drive=40, color='DUST', wave=SAW, det=12, mix=64, noise=70, cut=60, res=50, fenv=40, ftype=BP, ad=40, ar=36, fd=32, fs=0, fr=36, **S)
P('RING STAB', color='EDGE', wave=SAW, det=15, intv=7, cut=100, res=10, fenv=0, ad=46, ar=40, wave2=4, ring=105, md=36, mods=[('menv', 'osc2', -20)], **S)
P('DEEP STAB', color='TAPE', wave=TRI, det=10, mix=64, intv=7, sub=20, cut=50, res=20, fenv=30, ad=50, ar=44, fd=42, fs=0, fr=44, wave2=1, **S)
P('WIDE STAB', wave=SAW, det=26, mix=64, cut=56, res=20, fenv=25, ftype=HP, spread=100, ad=44, ar=40, fd=36, fs=0, fr=40, drift=20, **S)

# ------------------------------------------------------------------ HIT (21): one-shots and effects (MONO)
bank('HIT', 21)
H = dict(mono=1, as_=0, vel=70, leg=0)
P('ZAP', color='EDGE', wave=SAW, cut=70, res=80, fenv=50, ad=44, ar=40, fd=36, fs=0, fr=40, md=34, mods=[('menv', 'pitch', 58)], **H)
P('LASER HIT', color='EDGE', wave=SQR, cut=90, res=60, fenv=30, ad=50, ar=44, fd=44, fs=0, fr=44, md=48, mods=[('menv', 'pitch', 63), ('menv', 'pwm', 30)], **H)
P('BLIP', wave=SIN, cut=127, res=0, fenv=0, ktr=0, ad=26, ar=22, md=18, mods=[('menv', 'pitch', 20)], **H)
P('PING', drive=30, wave=SIN, noise=20, cut=96, res=96, fenv=0, ftype=BP, ktr=127, ad=64, ar=60, **H)
P('CLICK', drive=40, wave=SQR, noise=60, cut=84, res=10, fenv=0, ftype=HP, ktr=0, ad=14, ar=12, **H)
P('THUD', wave=SIN, sub=50, drive=40, cut=40, res=10, fenv=30, ktr=0, ad=44, ar=36, fd=30, fs=0, fr=30, md=26, mods=[('menv', 'pitch', 30)], **H)
P('RISER', wave=SAW, det=30, mix=64, noise=40, cut=30, res=70, fenv=60, mono=1, leg=0, vel=40, aa=100, ad=60, as_=127, ar=30, fa=112, fd=60, fs=127, fr=30,
  ma=112, md=60, ms=127, mr=30, mods=[('menv', 'pitch', 30), ('m1', 'res', 30)])
P('FALL', wave=SAW, det=20, mix=64, cut=96, res=50, fenv=-40, ad=92, ar=60, fa=0, fd=90, fs=0, fr=60, md=92, mods=[('menv', 'pitch', 44)], **H)
P('NOISE HIT', drive=40, color='DUST', wave=SAW, noise=127, cut=84, res=15, fenv=40, ftype=BP, ktr=0, ad=50, ar=44, fd=40, fs=0, fr=44, **H)
P('IMPACT', color='EDGE', wave=SIN, sub=80, noise=70, drive=90, cut=46, res=20, fenv=50, ktr=0, ad=84, ar=70, fd=50, fs=0, fr=60, md=40, shtype=1, shape=40,
  mods=[('menv', 'pitch', 36)], **H, cut_db=14)
P('GLITCH', color='DUST', wave=SQR, cut=110, res=20, fenv=0, ad=40, ar=20, shtype=3, shape=80, l2shape=5, l2rate=118, l2key=1,
  mods=[('lfo2', 'pitch', 50), ('lfo2', 'shape', 40)], **H)
P('BLEEP', wave=SQR, pwm=64, cut=120, res=0, fenv=0, ktr=0, ad=34, ar=14, mono=1, as_=110, vel=60, leg=0)
P('SONAR', color='TAPE', wave=SIN, cut=127, res=0, fenv=0, ktr=0, ad=88, ar=84, fm=8, md=30, mods=[('menv', 'fm', 20), ('menv', 'pitch', 6)], **H)
P('METAL HIT', color='EDGE', wave=SQR, det=15, intv=7, cut=110, res=10, fenv=0, ktr=32, ad=70, ar=66, wave2=4, ring=115, md=30, mods=[('menv', 'osc2', -24)], **H)
P('FM HIT', wave=SIN, intv=19, cut=127, res=0, fenv=0, ktr=0, ad=54, ar=46, fm=90, md=30, mr=30, mods=[('menv', 'fm', 60)], **H)
P('SWEEP DOWN', wave=SAW, noise=60, cut=110, res=85, fenv=-60, ktr=0, ad=84, ar=60, fa=0, fd=84, fs=0, fr=60, **H)
P('SWEEP UP', wave=SAW, noise=60, cut=24, res=85, fenv=60, ktr=0, mono=1, leg=0, vel=50, aa=40, ad=60, as_=110, ar=40, fa=96, fd=60, fs=127, fr=40)
P('ALARM', color='EDGE', wave=SQR, cut=110, res=20, fenv=0, mono=1, leg=0, vel=60, ad=60, as_=115, ar=30, l2shape=4, l2rate=92, l2key=1, mods=[('lfo2', 'pitch', 22)])
P('CHIRP', wave=SIN, cut=127, res=0, fenv=0, ktr=0, ad=32, ar=28, md=30, mods=[('menv', 'pitch', -48)], **H)
P('BUZZ', color='EDGE', wave=SAW, drive=110, cut=64, res=40, fenv=20, ad=48, ar=30, shtype=2, shape=70, **H)
P('DROP', wave=SIN, sub=40, drive=30, cut=70, res=0, fenv=0, ktr=0, ad=96, ar=80, md=96, mods=[('menv', 'pitch', 40)], **H)

# ------------------------------------------------------------------ DRUM (21): synth drums (MONO)
bank('DRUM', 21)
D = dict(mono=1, as_=0, vel=80, leg=0, ktr=0)
P('KICK', wave=SIN, drive=30, cut=80, res=0, fenv=0, ad=52, ar=40, md=22, mods=[('menv', 'pitch', 40)], **D)
P('KICK HARD', color='EDGE', wave=SIN, noise=10, drive=90, cut=70, res=10, fenv=30, ad=48, ar=36, fd=20, fs=0, fr=30, md=18, shtype=1, shape=35, mods=[('menv', 'pitch', 46)], **D)
P('KICK LONG', wave=SIN, sub=30, drive=20, cut=70, res=0, fenv=0, ad=88, ar=70, md=30, mods=[('menv', 'pitch', 34)], **D)
P('SNARE', wave=TRI, noise=110, drive=20, cut=96, res=20, fenv=20, ad=44, ar=40, fd=36, fs=0, fr=40, md=16, mods=[('menv', 'pitch', 24)], **D)
P('SNARE TIGHT', color='EDGE', wave=SQR, noise=120, drive=40, cut=80, res=15, fenv=10, ftype=BP, ad=34, ar=28, md=12, mods=[('menv', 'pitch', 20)], **D)
P('CLAP', drive=40, color='DUST', wave=SAW, noise=127, cut=74, res=25, fenv=10, ftype=BP, ad=40, ar=44, l2shape=3, l2rate=120, mods=[('lfo2', 'level', -40)], **D)
P('RIM', drive=40, wave=TRI, noise=20, cut=124, res=0, fenv=0, ftype=LP, ad=18, ar=16, **D)
P('TOM LOW', wave=SIN, drive=20, cut=70, res=0, fenv=0, ad=62, ar=50, md=30, mods=[('menv', 'pitch', 22)], **D)
P('TOM MID', wave=SIN, drive=20, cut=80, res=0, fenv=0, ad=56, ar=46, md=28, mods=[('menv', 'pitch', 22)], **D)
P('TOM HIGH', wave=TRI, drive=15, cut=90, res=0, fenv=0, ad=50, ar=42, md=26, mods=[('menv', 'pitch', 22)], **D)
P('HAT CLOSED', drive=40, wave=SQR, det=40, mix=64, intv=7, noise=127, cut=100, res=20, fenv=0, ftype=HP, ad=20, ar=18, wave2=4, ring=80, **D)
P('HAT OPEN', drive=40, wave=SQR, det=40, mix=64, intv=7, noise=127, cut=100, res=20, fenv=0, ftype=HP, ad=70, ar=60, wave2=4, ring=80, **D)
P('SHAKER', drive=30, color='DUST', wave=SAW, noise=127, cut=92, res=10, fenv=0, ftype=BP, aa=18, ad=30, ar=26, mono=1, as_=0, vel=90, leg=0, ktr=0)
P('COWBELL', drive=40, color='EDGE', wave=SQR, det=0, mix=64, intv=7, cut=92, res=35, fenv=0, ftype=BP, ktr=64, ad=50, ar=44, mono=1, as_=0, vel=80, leg=0)
P('CLAVE', wave=SIN, cut=127, res=0, fenv=0, ad=22, ar=20, md=8, mods=[('menv', 'pitch', 10)], mono=1, as_=0, vel=80, leg=0, ktr=64)
P('CONGA', wave=SIN, noise=8, drive=10, cut=100, res=0, fenv=0, ad=42, ar=36, md=14, mods=[('menv', 'pitch', 14)], mono=1, as_=0, vel=85, leg=0, ktr=64)
P('ZAP TOM', color='EDGE', wave=SAW, cut=60, res=85, fenv=50, ad=50, ar=42, fd=40, fs=0, fr=40, md=40, mods=[('menv', 'pitch', 54)], **D)
P('NOISE SNAP', drive=30, color='DUST', wave=SAW, noise=127, cut=100, res=40, fenv=-30, ftype=BP, ad=26, ar=22, fd=26, fs=0, fr=22, **D)
P('CYMBAL', drive=30, wave=SQR, det=50, mix=64, intv=11, noise=110, cut=98, res=10, fenv=0, ftype=HP, ad=96, ar=90, wave2=4, ring=100, **D)
P('FM TOM', wave=SIN, intv=7, cut=127, res=0, fenv=0, ad=58, ar=48, fm=60, md=26, mr=26, mods=[('menv', 'fm', 40), ('menv', 'pitch', 20)], **D)
P('SUB DROP', wave=SIN, sub=60, drive=20, cut=60, res=0, fenv=0, ad=100, ar=90, md=100, mods=[('menv', 'pitch', 36)], **D)

names = [l.split('"')[1] for l in OUT if l.startswith('ENTRY')]
assert len(names) == 129 and len(set(names)) == 129, (len(names), [n for n in names if names.count(n) > 1])
open(sys.argv[1], 'w').write('/* GhettoMachine\'s factory sounds, the second set (0.5). Written by tools/make_presets.py: edit that, not this. */\n'
                             + '\n'.join(OUT) + '\n')
print(len(names), 'presets')
