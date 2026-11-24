# GhettoMachine: development preview manual

GhettoMachine, part of the BongWare firmware line engineered by BongTech, turns the M-VAVE FM-1 into a four-track groovebox:

- **Drums:** the DrumEngine, with twelve genre kits (ELECTRO, HOUSE, TECHNO, TRAP, BOOM BAP, DRUM & BASS, GARAGE, DISCO, DUB,
  LO-FI, INDUSTRIAL, MINIMAL) and a preset per genre for every drum. (The stock 808 / 909 engine's sound is not in the FM-1's firmware since 0.5: the drums' bus page turns between DRY and BUS only. It lives on in the simulator, whose tests play it.)
- **T2, T3, T4:** instrument tracks. Each is a MONO synth or a POLY synth.
- **One sequencer** of up to 128 steps a track, 16 patterns, and a song chain.
- **An LFO for every clip** (each track's part of each pattern has its own), an arpeggiator and one optional insert effect on T2–T4, and per-step locks.
- **A mixer**, a reverb, a tempo delay, and **a master bus with a mastering chain**: EQ, compressor, tape saturation,
  stereo width and a limiter, with presets and a loudness meter.
- **Made to be played live:** a scene grid (each track on its own pattern), seven perform keys (roll, fill, stutter,
  filter, throw, wash, crush, plus tape stop), a quick mixer with lit drum lane mutes and sixteen held moves a track
  (filter, sends, roll, stutter, octave),
  follow actions, sounds per pattern, tap tempo and MIDI clock.
- **A deeper synth:** OSC 2 with its own wave, FM and ring modulation, a wave shaper (fold, clip, rectify, crush),
  analog drift, a third envelope, a second LFO for each note, a six-slot mod matrix and four macro knobs.
- **A 303 pattern generator** on T2–T4: acid lines with slides and accents, made from a few knobs.
- **Stems over USB:** a computer records the master and each track on its own pair of inputs, all at once.

> **Historical hardware test: 0.5** (first installed over USB on 20 Nov). The first recording from it clicked now and then, so
> 0.5 now keeps a deeper USB buffer, MASTER no longer changes what a computer records, and the drum steps' beats glow
> (1, 5, 9, 13): that build went on the FM-1 on 21 Nov, and a recording from it had no clicks. REC + GLO (erase the
> track's part, to record it again) went on it the same day, and later the 808 brought up to the 909's level with the
> Dungeon BBS screen. The CPU pass (SETUP's CPU view; a cheaper 606, Color and phaser; effects that sleep on silence) went
> on it the same day, and a second pass (the reverb at half the rate; the synth's voices, the reverb, the delay and the
> master bus run from RAM) later that day. A third (the reverb, the delay and the master bus on the second core, a block
> behind the first; the CPU view's peaks) went on it that evening. The sections' budgets went on it that night at 20 %
> for the drums, 30 % for the synth and 25 % for the effects: played, it had no late blocks, but the budgets cut 138 drum
> sounds in a minute and held the synth to 4 or 5 voices with a third of the first core unused. Loosened to 30 %, 40 %
> and 25 %, that went on it the same night (7 to 9 voices, few drum cuts; the first core at about 70 %). The 606 and
> the 707 were then cut (their code and memory given to the processor's budget) and FILL made to leave the drum clip
> where it would have been: on the FM-1 since that night. The synth made cheaper without changing its sound (below)
> went on it later that night (the synth's mix from 450 to about 250 us a block; steady playing at about 60 % of the
> first core, the only late blocks at kit changes). Then, asked for: the budgets back at 20 %, 30 % and 25 % with a
> guard for the block that takes too long, the LFO made each clip's own (project format 8), and no spike as clips
> change (a kit that leaves stops sounding): on the FM-1 since 22 Nov. The tracks' mix, with their inserts and Color,
> shared between the two cores: in the source, not on the FM-1 yet. 0.5 is 0.4 plus the USB stems. 0.4 is 0.3 plus the 303 generator, the new name and the boot screen's logo; 0.3 is 0.2 plus the performance
> update (the scene grid onwards) and the synth update. 0.5 passes its simulator tests (2,194 checks, 39 more on the
> synth itself and 48 on the master bus's sound). It reads a project saved by 0.2, 0.3 or 0.4, and by 0.5 before each clip had its own LFO.

## Installing it, and getting back

Once GhettoMachine (or any fm1-base or Felucca firmware) is on the FM-1, an update needs only the USB cable. The Mac sends
the package over USB-MIDI and the FM-1's loader writes only the 4 KiB sectors that changed:

```
cd third_party/fm1-base/tools && ../../../.venv/bin/python fm1_install.py ../../../app/build/ghettomachine.fwsc --yes
```

**Tap SAVE first:** an install restarts the FM-1, and the saved project is what comes back. The project is
kept; only the firmware is written, never the boot code. A project and user presets saved by 0.2, 0.3 or
0.4 load into 0.5 (see "What is kept").

The first install over M-VAVE's own firmware went through the ESP32-S3 adapter (backup, dry run, write), and that
route still works for getting M-VAVE's firmware back.

## Finding your way

| Control | What it does |
| --- | --- |
| **PRESETS** | Presets, wherever you are: an instrument track's sound; on the drums the genre kit, or, in a drum's editor (EDIT with a pad picked), that drum's own presets. |
| **SELECT** | Turns the pages of every editor (the synth's EDIT, ENV, LFO, FX, the master bus, a drum's editor...), moves a cursor; with a step held, its chance (drums) or which of its notes to edit (T2–T4). |
| **ALGORITHM** | Picks the track: DRUMS, T2, T3, T4 (or hold **SEL** and press pad keys 1–4). The page stays wherever the next track has it too (STEPS, EDIT, LFO, the mixers ...), so the same thing can be edited track after track; a page the track hasn't (KEYS on the drums) goes to its own. With a step held: its substeps. |
| **K1–K4** | The four knobs of the page on screen. |
| **PLAY** | Starts and stops. |
| **REC** | Tap: recording on or off (turn a knob while it's on and playing: the knob is recorded). Hold it: the metronome, count-in and quantize (below). |
| **SEQ** | The steps. On T2–T4, press again for the 303 generator (ACID), again for the keys. |
| **EDIT, ENV, LFO, ARP, FX, GLO** | Their editor, on the page you left it on (SELECT turns its pages). Press the same button again: back to the screen you were on before you opened it. |
| **HOME** | PATTERNS, the scene grid, for playing live. Again: the QUICK MIXER, then the song, then SETUP, then back to the track. None of them stops or re-times what's playing. |
| **SEL + GLO** | The master bus, at once. |
| **SEL + FX (T2–T4)** | The selected instrument track's insert; SELECT chooses a mode or OFF. |
| **SAVE** | Tap: save the project. Hold it and press a pattern key: copy the pattern there. Hold it and turn PRESETS: save a preset. Hold it and press OCT−: the track's sound back to how it was saved. |
| **EDIT + OCT− / OCT+** | Undo, redo (6 levels). |

### The screen

The screen is the **Acid Dub Deck** look: three fixed bands, deep purple above and below a black work area.
**Acid green** marks notes, set values and what's selected (with black type on it); **yellow** marks what is
happening right now: the playhead (with its flag), a held move (which also says HELD), a level as it moves, the knob
you're turning. Purple draws the grids, the empty steps and the boxes. Top to bottom:

- **The header**, ruled off in acid green:
  - Line 1: **GM /** and the family: the track (DR, T2, T3, T4), or ALL on the shared pages (PATTERNS, SONG, SETUP,
    the mixer, FX, the master bus). At the right the pattern (the next one beside it, blinking yellow, when one is
    waiting; S03 while a song plays), the tempo, a yellow triangle while playing, a red dot while recording.
  - Line 2: **the page's title**, large: PATTERNS, QUICK MIXER, KICK / BODY, AMP ENV, MASTER COMP. To its right, small,
    what it's on (the sound, the kit, the preset) where it fits.
  - **Turn a knob or press a button and the change shows in yellow** on line 1 for a moment ("T3 LEVEL 99", "CY
    MUTED"), in place of the pattern and tempo. The title never moves.
- **The work area:** only what you're working on. Curves (the filter, the envelopes, the LFO, the oscillators, the EQ,
  the compressor, the tape) on a purple grid. The LFO's page is a scope: its wave scrolls past a yellow dot that
  stays in the middle (now), riding up and down with the LFO's value, what's to come at the right. The steps; the
  drums' lanes; a drum's voice drawn on its own page (an
  illustration of its sound, its decay and pitch following the lane's DECAY and TUNE). Where there's room, a last
  line gives a hint at the left ("WHITE: SCENE") and a state at the right, yellow when it's happening now ("P01
  PLAYING", "W4 HELD").
- **Live pictures** (0.5): the pages show what is happening, not only the settings.
  - **OSC, OSC 2, SHAPER:** while the track sounds, its real waveform, held still on each cycle ("LIVE"); quiet, the
    settings drawn.
  - **FILTER:** the newest voice's cutoff now, a yellow line with a fading trail, as the envelope, LFOs and matrix move
    it. **AMP / FILTER / MOD ENV:** each sounding voice a yellow dot where it is on the shape. **LFO 2:** each voice's
    own phase. **MATRIX:** each slot's move now, a bar either way. **ARP:** the step playing, yellow.
  - **A drum's page:** its last hit as it sounded (0.78 s, in dB); before one, an illustration. **DRUM BUS:** SAT's
    curve and the compressor's and limiter's reduction now. **Drum steps:** each lane's level beside its name,
    accented hits orange.
  - **MASTER EQ:** the master's spectrum behind the curve. **COMP 2, LIMIT:** their reduction over the last 6 s.
    **STEREO:** a vectorscope of the master (the middle up, the sides across). **CHAIN:** the loudness over the last
    22 s. **RETURNS:** what each return puts out now, beside its fader; FEEDBACK turns orange, then RUNAWAY, near
    self-oscillation (BONGDELAY says so too). **CPU:** the total over the last 22 s.
- **The knob strip:** four cells, **K1 to K4 in the order the knobs sit**: the knob's number, what it does, its value
  large. The value you're turning goes acid green with a yellow bar under it; a **locked** value is acid green with
  an **L**; an empty cell shows K3 and a dash; a reading (the compressor's reduction, the stereo phase) is dimmed.
- **PATTERNS** (HOME) also shows the processor's load (CPU) and the voices sounding.

## The drum track

- **Pads:** the 11 black keys are BD, SD, LT, MT, HT, RS, CP, CH, OH and CR/RD (909) or CY/CB (808). Pressing one picks it
  and plays it. Steps are kept when switching kits. (The 606 and the 707 kits, and the 707's Cowbell and Tambourine,
  were cut on 21 Nov: a project saved on the 606 loads on the 808, one saved on the 707 on the 909, and a pad that
  used one of their voices plays its kit's own.)
- **Mix models:** pick a pad, open **EDIT**, and turn **SELECT** fully right to **DRUM MODEL**. Turn **K1** to choose
  the 909's or the 808's voice for that pad. The kit reads **CUSTOM** as soon as a lane differs. It keeps
  the lane’s steps, MIDI note and mute, but cuts its previous sound. Factory presets restore their own lane maps.
- **Steps (SEQ):** the 16 white keys are the picked pad's steps. Press an empty step to light it; press and let go of a
  lit one to clear it. OCT− / OCT+ or SELECT move through step pages up to 128. The beats, steps 1, 5, 9 and 13 of the
  page, glow dimly while they're empty, and the screen's grid gives them a darker column, so a bar is easy to count.
  - Hold **SEQ** and press steps: accents.
  - Hold a step and turn **SELECT**: its chance (100 % to 5 %).
  - Hold a step and turn **ALGORITHM**: its substeps. The step plays 2, 3 or 4 times within its own length (a ratchet); 1 takes it away. It uses one of the part's locks. Works on drum hits and on synth steps (not while the track's arpeggiator is on). The grid shows a drum hit cut into as many pieces, a synth step as many yellow dots.
- **EDIT:** the pad's knobs: tune, tone, decay, its reverb and delay sends, and so on. SELECT for more; EDIT again goes
  back to where you were. The last page is the kit's own knobs (accent, velocity).
- **The two kits play at the same level:** each 808 sound is as loud as the 909 pad that plays its part (until 21 Nov
  the 808 played a groove about 12 dB quieter). The 808's accent is stronger than the 909's, as on a real 808: about
  6 dB against 3; the KIT page's Accent sets it.
  - Hold a step and turn a knob: that value for that step only (a lock). Held step + OCT− clears its locks.
- **ENV:** the LANE MUTE page. Tap a black pad key to toggle that drum lane in this pattern, or hold **ENV** and tap a pad from the drum editor. The grid shows each lane as ON or MUTE. OCT+ unmutes them all. With the DrumEngine (the default) a lane mute is the lane's sound only: it fades out in 1.5 ms and its hits go on underneath, so unmuting brings back a tail that's still ringing. (On the stock 808/909 engine a mute stops the lane's hits, and unmuting waits for the next one.) The QUICK MIXER's white keys are the same mutes.
- **Knobs on the steps page:** LENGTH and RATE of this track's part, SWING and TEMPO.

Hold **SEQ** and tap **OCT+** to repeat the selected track’s part into a second half, or **OCT−** to keep its first half (rounding an odd length down). This copies notes, accents, chance and locks without changing the track’s rate. The second half is erased on halving. A full repeat is blocked if it would exceed 128 steps or the part’s lock capacity. **EDIT + OCT−** undoes, **EDIT + OCT+** redoes. On instrument STEPS, black keys 1–8 pick the available banks of 16.

On instrument STEPS, hold a step and turn **K3 GATE**. Slow turns edit in eighths of a step; a fast turn snaps to whole bars at the selected rate. A note can hold for up to 128 steps.

A very dense collection of long parts can exceed the fixed FM-1 project save slot. If SAVE reports **PROJECT TOO LARGE**, the previous save is kept and the current edits stay in memory. Shorten or clear unused parts before saving again.

## The instrument tracks (T2–T4)

- **KEYS** (their home page): all 27 keys play. OCT− / OCT+ move the octave.
- **Steps (SEQ):** the screen shows the notes, pitch upward. The white keys are steps; black keys 1–8 pick an available bank.
  - Tap an empty step to insert the last note you played, or stamp the selected chord and inversion on that root. The
    screen shows what it will stamp. Tap a filled step to clear it.
  - Hold a step and press **ARP** to open **PICK NOTES**. Tap any white or black key to add that pitch; tap a lit key
    to remove it. A step holds up to four notes. **OCT− / OCT+** shift the picker's range without changing the track's
    playing octave. Press **ARP** or **SEQ** to get back to STEPS.
  - Hold a step: **K1** moves its note (or chord), **K2** sets velocity, **K3** sets gate (up to 128 steps; slow eighth-step edits, fast musical-bar snaps),
    **SELECT** sets chance.
  - **One note of a chord:** hold a step and turn **SELECT** to pick which of its notes the knobs move: ALL, or one
    (from the lowest; it turns yellow). **K1** then moves only that note, stepping past the step's other notes, and STEP
    HARMONY's **K3** OCTAVE moves it alone by octaves (a voicing). A held step's chance is STEP NOTES' **K4**.
  - **Four pages of tools, turned with SELECT** (the screen's title names the page; the step banks are on black keys 1–8 and OCT− / OCT+):
    - **STEP NOTES** (where STEPS opens): hold a step for **K1** NOTE, **K2** VELOCITY, **K3** GATE, **K4** CHANCE. With no
      step held the knobs set what the next new step gets: its note, velocity and gate.
    - **STEP HARMONY:** hold a step for **K1** CHORD (built on the step's root: MAJ, MIN, 7 ...), **K2** INVERT, **K3** OCTAVE,
      **K4** SUBSTEPS. The chord is read from the step's own notes; a set of notes that is no known chord shows CUSTOM,
      and K1 rebuilds it on its lowest note. With no step held, K1–K3 set the chord, inversion and octave new steps are stamped with.
    - **PART TOOLS** (every step of the part): **K1** TRANSPOSE in semitones, **K2** SHIFT the part a step later or earlier
      (locks and substeps move with their steps), **K3** VELOCITY and **K4** GATE up or down for all.
    - **PART GROOVE:** **K1** LENGTH, **K2** RATE, **K3** SWING, **K4** TEMPO, as on the drums' steps.
- **Recording (REC):**
  - **While playing:** what you play goes to the nearest step, with its gate from how long you held it.
  - **While stopped, on KEYS:** step recording. A note or a chord goes into the cursor's step, and the cursor moves on
    when you let go. SELECT moves the cursor, to leave a rest.

### Color (retired)

The unified engine has no Color, reverb or delay inside a track. Older projects keep their Color and insert settings as
saved data; nothing applies them. The FX button now goes through the four FX pages and back to the track's own page.

### Instrument insert effects

Pick **T2, T3 or T4**, then hold **SEL** and tap **FX**. Turn **SELECT** to choose the effect. **K1–K3** shape it; **K4 MIX** blends dry and effected signal (zero bypasses without losing the other knob values). Turn SELECT to **OFF** to remove the effect; selecting a new mode starts it at its defaults. Tap **FX** to leave this screen; plain FX from another page still opens the shared reverb, delay and filter.

| Insert | K1 | K2 | K3 |
| --- | --- | --- | --- |
| Phaser | Sweep rate | Sweep depth | Feedback |
| Trem / Pan | Sweep rate | Depth | Tremolo to stereo auto-pan |
| Chorus | Modulation rate | Depth | Stereo width |
| Flanger | Modulation rate | Depth | Feedback |
| Comb | Tuning | Feedback | Damping |
| Env Filter | Base cutoff | Level sensitivity | Resonance |
| Freq Shift | Signed coarse Hz | Fine Hz | Opposite stereo offsets |
| Micro Pitch | Detune depth | Delay | Stereo width |
| Resonator | Tuning | Decay | Color/second resonance |

Each track can run one insert at a time. It processes the instrument's stereo signal before mixer pan, the shared reverb/delay sends, and the USB stem. Mute clears the insert's own tail immediately; shared effects already fed by it can still ring out. Inserts are track-wide project settings, not per-pattern locks; older saves start with each insert OFF. Freq Shift adds or subtracts the same number of Hz from each frequency, rather than transposing by a musical interval; at low frequencies the hardware-friendly Hilbert filter has weaker rejection of the opposite sideband. An insert sleeps on
silence as Color does.

### The 303 generator (ACID)

On T2–T4, press **SEQ** twice: STEPS, then **ACID**. It writes the track's part (in this pattern) as a TB-303-style
line: one note a step, each one **HALF** (a 303's short note), **WHOLE** (held to just before the next note, over any
rests: a 303's tie) or a **SLIDE** into the next, some of them **accented**. Every change makes the line again at once,
also while it plays. It works with **any sound**: MONO or POLY, any preset.

- **OCT+ NEW:** a new line from a random seed, any of 65,535, decided by the moment you press (never the seed
  playing). **OCT− MUTATE:** a few steps change (about one in eight); the rest stay. **EDIT + OCT−** takes back a roll.
- **The ACID page:** **K1 DENSITY** (how many steps play), **K2 WHOLE** (how many notes are held whole instead of
  half), **K3 SLIDE** (how many slide into the next), **K4 ACCENT**.
- **SELECT** turns to the **NOTES** page: **K1 ROOT** (C1 to B4; C2 to start), **K2 SCALE** (MINOR, PHRYGIAN, DORIAN,
  PENTA, BLUES, HARM MIN, MAJOR, MIXOLYD), **K3 NOTES** (how many of the scale's notes it uses, the root and the fifth
  first: 1 is a one-note line), **K4 OCTAVE** (how many notes jump an octave, mostly up).
- **SELECT** again: the **LINE** page: **K1 SEED** (the line's number, 1 to 65535: turn it to step through the lines in order from where
  the dice landed, and back to find an earlier one), **K2 LENGTH**
  (the part's steps; the line is made again to fit), **K3 RATE**, **K4 SWING** (as on STEPS).
- **The knobs don't reshuffle.** Each step keeps its own dice; the knobs only set how many of them come up. Turning
  DENSITY up adds notes and keeps the ones there, and a knob or SEED turned back gives back exactly the line it gave.
- **The screen** shows the line by pitch: a half note is a dot, a whole note a bar as long as it's held, a slide a bar
  with a line to the next note; accented notes are white. The AC and SL rows under it show the accents and slides.
  The band names the seed (M3: mutated three times), the root and the scale.
- **Slides:** the next note glides in from the slid one without a new attack, on any sound: a MONO sound's voice, a
  POLY sound's last note's voice, whatever LEGATO says. It takes the sound's GLIDE time, or 58 ms (a 303's) with GLIDE
  at 0.
- **Accents:** velocity 127 (others 90), louder still, and a filter sweep that opens about an octave and falls away
  over about 200 ms, as a 303's. On any sound.
- **Each part keeps its own settings**, saved with the project and copied with a pattern. A part's first turn starts
  from the track's last settings, so a new pattern can begin as the same line: then MUTATE or NEW for a variation.
- **The steps are ordinary steps:** edit them on STEPS. But the next turn on ACID makes the line again from its
  settings and replaces hand edits. **EDIT + OCT−** undoes it (the steps and the settings together).
- It fills the part's length; steps past it are left as they are.

**303 marks on STEPS, by hand** (any track, any sound): hold a step and

- press **OCT+** for its length: **HALF**, **WHOLE**, **SLIDE**, and round again;
- turn **K2 VELOCITY** past 127 for **ACCENT** (the card says so); one click back takes the accent off.

On STEPS an accented note has a white bar over it and a slide a mark at its end.

### Sounds

- **EDIT, page VOICE:** **TYPE** (MONO, POLY), **LEVEL**, **VOICES** (POLY: up to 8; MONO: UNISON 1–4),
  **GLIDE** (MONO).
- **OSC:** WAVE (saw, square, triangle, sine, PWM, sync, sub), DETUNE, MIX, INTERVAL (OSC 2's, ±24 semitones).
- **TONE:** SUB, NOISE, PWM, DRIVE.
- **FILTER:** CUTOFF, RESO, ENV AMT, low-, band- or high-pass.
- **MORE:** KEYTRACK, VELOCITY, SPREAD (unison), LEGATO.
- **OSC 2:** **WAVE 2** (SAME as OSC 1, or saw, square, triangle, sine, PWM), **FM**, **RING**, **DRIFT** (below).
- **SHAPER:** its type (**FOLD**, **CLIP**, **RECT**, **CRUSH**), **AMOUNT** (0 is off), **BIAS**, and **PLACE**: before
  the filter (PRE FLT) or after it (POST FLT).
- **MACRO:** MACRO 1–4, four knobs that do whatever the matrix tells them to (below).
- **ENV:** the amp's attack, decay, sustain and release; SELECT for the filter's, and on for **MOD ENV**, a
  third envelope that moves whatever the matrix sends it to.
- **MONO** goes back to a key you still hold when you let go of the newer one. **LEGATO** (MORE): **OFF**, every note
  starts again; **ON**, tied notes don't restart the envelopes (and with GLIDE, a note glides from the last one while
  that still sounds); **SLIDE**, the same but only tied notes glide: a 303's slide.
- **The tracks share 16 voices.** If the processor gets busy, fewer voices sound rather than the audio breaking up.

### OSC 2, FM, ring and the shaper

- **WAVE 2** gives OSC 2 a wave of its own (SAME follows WAVE). OSC 2 sits INTERVAL semitones and DETUNE cents from
  OSC 1; MIX sets how much of each you hear.
- **FM:** OSC 2 bends OSC 1's phase, as in a DX-style FM synth. Set MIX to 0 to hear OSC 1 alone, with OSC 2 as the
  modulator; INTERVAL sets the ratio: 0 is 1:1, +12 is 2:1, +19 about 3:1, +24 4:1, odd values clangy and bell-like.
  Sine waves give the cleanest FM. The top notes get a little less of it, so they don't turn to grit.
- **RING** crossfades the mix to OSC 1 × OSC 2: metallic, the notes' sum and difference.
- **DRIFT** lets each voice wander a few cents (and its filter a little), slowly and each on its own, like an old
  analog synth. Great on pads and unison.
- **The shaper:** **FOLD** wraps loud peaks back (a sine folder: West Coast tones, bright and hollow), **CLIP** cuts
  them square, **RECT** flips the bottom half up (an octave up, buzzy), **CRUSH** takes bits away and holds samples (lo-fi).
  **BIAS** shifts the wave before it, for even harmonics. Before the filter, the filter tames it; after it, the shaper
  works on the filtered sound (CRUSH is the classic there).
- WAVE 2, FM and RING work with WAVE saw to PWM; SYNC and SUB are pairs of their own.

### Modulation: MOD ENV, LFO 2, the matrix and the macros

- **LFO 2** (LFO, third page): each note gets its own, so chords shimmer rather than move together. SHAPE, RATE
  (0.05–30 Hz), FADE IN, and RESTART: EACH NOTE (from the start of its cycle, for a steady vibrato) or FREE (each
  note anywhere in it).
- **MATRIX** (LFO, fourth page): six slots. Each sends a **SOURCE** to a **DEST** by an **AMOUNT** (−64 to +63).
  **K1** picks the slot; the screen lists all six.
  - Sources: LFO 1, LFO 2, MOD ENV, FILT ENV, AMP ENV, VELOCITY, NOTE (higher notes more), RANDOM (a new value each
    note), MACRO 1–4.
  - Destinations: PITCH, OSC 2 (its pitch), DETUNE, CUTOFF, RESO, PWM, MIX, SUB, NOISE, DRIVE, FM, RING, SHAPER (its
    amount), BIAS, LEVEL, PAN, LFO 2 RATE.
  - PITCH and OSC 2 go up to 4 octaves each way, finely near 0: an amount of 4 is a gentle vibrato, 32 an octave.
- **Things to try:**
  - MOD ENV → PITCH +32 with a short decay: a pitch drop, for zaps and synth drums.
  - LFO 2 → PITCH +4 with FADE IN: vibrato that grows as the note is held.
  - MOD ENV → FM, VELOCITY → FM: FM that's bright at the start and on hard notes, like a bell or an e-piano.
  - MACRO 1 → CUTOFF and MACRO 1 → SHAPER: one knob that opens and dirties the sound at once.
  - NOTE → PAN: low notes left, high notes right.
- **The macros are for playing:** turn them on EDIT's MACRO page. Like any sound knob, they can be locked to a step
  and recorded while playing (REC). So can the matrix's amounts: hold a step, press LFO, go to MATRIX, turn AMOUNT.

### Presets

- **PRESETS** goes through 24 factory sounds (basses, leads, pads, keys...), then your own **USER 1–16**. A `*` after
  the name means you've changed it since loading it.
- **ACID** (since 0.4) has LEGATO SLIDE (keys played tied glide, others don't) and GLIDE 25. A sound you saved from
  it before keeps its own settings.
- The last nine show off the synth update: **FM BELL**, **FM BASS**, **FM KEYS**, **FOLD LEAD**, **WEST PLUCK**,
  **RING METAL**, **DRIFT PAD**, **CRUSH BASS**, **WOBBLE**. MACRO 1 does something on each.
- **To keep a sound:** hold **SAVE**, turn **PRESETS** to pick a USER slot (the screen says EMPTY or REPLACE), and let go
  of SAVE.
- **On the drums**, PRESETS goes through the **genre kits**: ELECTRO (the factory kit, and what every project saved
  before them plays), HOUSE, TECHNO, TRAP, BOOM BAP, DRUM & BASS, GARAGE, DISCO, DUB, LO-FI, INDUSTRIAL, MINIMAL. A kit
  puts every drum on its genre's preset (PERC 2 on another genre's percussion, so the two differ). In a drum's editor
  PRESETS goes through that drum's own presets, also one per genre, so you can put a TRAP kick in a HOUSE kit. The kit
  is saved with the project (and with each pattern, with SOUNDS PER PATTERN); a drum's edits on top of it aren't saved
  yet, and neither are drum kits of your own (SAVE + PRESETS on the drums says NOT SAVED YET). The **909**, **808** and
  **USER KIT 1–8** are the stock engine's, in its test mode only.
- Your presets are kept apart from projects, the moment you save them.

### The sampler is gone (0.5)

The SAMPLER track type was taken out in 0.5 (it wasn't being used). A saved project with a SAMPLER track loads it as
**POLY** with its own synth sound, and an LFO that moved a sample's START moves nothing (CUTOFF at depth 0). Samples
already in the FM-1's flash are left where they are, unused; nothing erases them. REC + SEL no longer does anything.

## Chord mode

Press **ARP** twice (page 2) on T2–T4:

- **K2 CHORD:** OFF, MAJ, MIN, 7, MAJ7, MIN7, SUS2, SUS4, DIM, AUG, 5 (power chord), OCT, MIN9, MAJ9, 7#9, 7SUS4. Each key then plays that chord
  built on it. The page shows the chord on a keyboard; the KEYS page's band shows which chord is on.
  MIN9 and MAJ9 leave out the fifth for house and garage stabs; 7#9 is a four-note altered voicing for funk/electro,
  and 7SUS4 keeps the third out of the way for suspended techno stabs.
- **K3 INVERT:** ROOT, 1ST, 2ND, 3RD: the lowest notes move up an octave, for smoother changes between chords.
- Chords are recorded as chords (a step holds up to 4 notes), live or step by step, and go through the arpeggiator
  if it's on: one key, an arpeggiated chord.
- They need a **POLY** track; a MONO track plays one note at a time.
- Chord mode is for the FM-1's own keys; notes coming in over MIDI play as they are.

## Recording: hold REC

Hold **REC** for a moment and the screen turns red: recording's page. A quick tap of REC still just turns recording on
and off.

- **K1 CLICK:** the metronome. **OFF**; **ON** (a click on every beat while playing); **REC** (only while recording).
  The bar's first beat is higher. The click is in your headphones and speaker, under MASTER, but never in
  the master bus's meters.
- **K2 COUNT-IN:** **OFF**, **1 BAR** or **2 BARS**. With recording on, PLAY counts in first, clicking each beat, with
  the beats left large on the screen (4, 3, 2, 1). Then the pattern starts. PLAY during the count-in cancels it. A note
  you play in the count-in's last half beat (a little early) lands on step 1.
- **K3 QUANTIZE:** the grid notes snap to as you record: a step (**1/16**), **1/8** or **1/4** (at a track RATE of
  1/16; the names follow the track's RATE). Drums snap the same way.
- **K4 LEVEL:** the click's volume.
- **OCT− QUANTIZE:** snaps the notes already in this track's loop (this pattern) to that grid. Notes that land on the
  same step become a chord. Locks move with their steps. On the drums it moves the hits.
- **OCT+ LEGATO** (T2–T4): each note is held until the next one starts, the last one round to the first: the whole
  loop tied. On a MONO sound, turn **MORE > LEGATO** on too and the notes glide into each other without restarting.
  The longest a note can be is almost 32 steps (two bars at 1/16).
- **GLO ERASE:** empties the selected track's part in the pattern it's playing (a clip launched on HOME: that clip's
  pattern): its notes or hits, accents, chance and locks. LENGTH, RATE, the pad mutes and the ACID settings stay.
  Recording stays on or off as it was and the transport keeps running, so a misplayed take can be tried again at once;
  step recording's cursor goes back to step 1. The REC page says **GLO: ERASE PART**; the screen then says, say,
  **ERASED: T2 P03**.
- All three can be undone with **EDIT + OCT−**.
- The settings are kept with the project.

### Recording knob moves (motion)

With recording on and the pattern playing, **turning a sound's knob records it on the step that's playing**, as a lock
(the same as holding the step and turning the knob). Turn it through a bar and every step you passed gets its value:
a filter sweep, a decay that opens up. It works on T2–T4's sound pages and on the drums' pad knobs. Undo takes it back.
Turning a knob without recording on, or stopped, only changes the sound.

## LFO, arpeggiator, locks

- **LFO** (every track; LFO 1 on T2–T4, whose LFO pages go on to LFO 2 and MATRIX, above). **Each clip has its own:**
  the LFO page shows and sets the LFO of the pattern's part the track is playing. Switch a track (or a whole scene) to
  another pattern and its LFO changes with it, when the clip starts; come back and it is as you left it. A pattern
  copied (SAVE + its key) takes its LFOs along; a new project's clips all start with the LFO off; a project saved
  before this loads with every clip given its track's LFO, so it plays as it did until you turn one.
  - Page 1: SHAPE, RATE (free, or with SYNC from 4 bars to 1/32), DEPTH, DEST.
  - Page 2: SYNC, RETRIG (FREE, NOTE, BAR), FADE IN, PHASE.
  - LFO 1 is the track's (one for all its notes, and it can follow the tempo); it's also a source in the matrix.
  - On T2–T4, DEST is cutoff, pitch, level, PWM, resonance, detune, mix, drive, noise or sub.
  - **On the drums,** pick a pad, then turn DEST to pick one of its controls. The LFO moves that control at each of the
    pad's hits, for that hit alone (your sound stays as you set it): S&H on the hat's decay makes every hat different.
    With the DrumEngine the controls are the drum's own (TUNE, BODY, DECAY, TONE, DRIVE...; not MODEL or MODE); with the
    stock 808 / 909 they're the kit's knobs. An LFO set up for one engine moves nothing while the other plays.
- **ARP** (T2–T4):
  - Page 1: MODE (up, down, up and down, random, as played), RATE, OCTAVES, GATE.
  - Page 2: LATCH, and **chord mode** (below).
  - It plays what you hold, and the sequencer's chords too.
- **Locks on instrument steps:** hold a step on the steps page, tap EDIT (or ENV, or LFO for LFO 2 and the matrix's
  amounts), and turn knobs. SELECT turns pages while the step is held. A mark over the step shows it has locks.

## Mixer, FX and the master bus

- **GLO, the mixer:** LEVEL, PAN, REVERB, DELAY pages, then RETURNS, then the master bus's, one row SELECT turns
  through; GLO opens on whichever you left it on, and GLO again goes back. On LEVEL to DELAY, K1–K4 are DRUMS, T2, T3,
  T4. The QUICK MIXER (HOME, twice) has one track's LEVEL, REVERB, DELAY and PAN on K1–K4 instead. The drums' sends
  open the FX to their pads' own reverb and delay knobs.
- **RETURNS** (GLO, then SELECT past DELAY), for dub, in pairs: **K1 VERB RTN** (how loud BongVerb comes back into
  the mix) beside **K2 VERB DECAY**; **K3 DELAY RTN** beside **K4 FEEDBACK** (the delay's regen). Each is drawn over
  its knob. The same knobs as on the FX pages, so a change shows in both. The returns read in dB against their presets' level (0.0dB, the default).
  Past +3.5dB (the old top) they go on in quarter-dB steps to **+15.5dB**, drawn yellow, so the echoes and the
  reverb can come back much louder than the dry; the master's limiter holds the peaks. A big turn glides in over
  about 50 ms instead of clicking. Projects saved before sound as they did.
- **FX:** one shared **BongVerb** and one shared **BongDelay**. Each track's REVERB and DELAY sends feed them; their
  returns join the dry tracks before **BongMaster**, and never go back into a send.
  - BongVerb: decay, damp, pre-delay (0–100 ms), level (its return, in dB: to +15.5dB, as on RETURNS). Defaults are
    the Motion Plate preset.
  - BongDelay: time in note values (45–950 ms; longer values are halved), regen, tone, level (its return, to
    +15.5dB). Defaults are the Clock
    Echo preset, at 1/16.
  - MODE: BongVerb's Plate or Hall, and BongDelay's drive.
  - FILTER: a DJ filter (left of centre low-pass, right high-pass) and its resonance, for sweeping the whole mix.
  - Projects saved before BongFX keep decay, delay time, feedback (as regen), tone and both levels. The old reverb tone
    becomes damp (reversed); pre-delay, mode and drive take their defaults.

### The master bus

Everything you hear goes through it, in this order:

**EQ → COMP → (the DJ filter) → TAPE → STEREO → GAIN → LIMITER → MASTER**

- **Getting there:** press GLO past the mixer's DELAY page, or hold **SEL** and press **GLO**. Then GLO (or SELECT)
  goes through its eight pages; SELECT also goes back into the mixer.
- **CHAIN** (page 1): the five stages, lit when they're doing something, with the compressor's and the limiter's
  reduction under them; the **IN** and **OUT** peak meters; the **loudness** in LUFS (3 s, and M: the last 0.4 s).
  - K1 **PRESET:** CLEAN, GLUE, PUNCH, WARM, WIDE, LOUD, LO-FI. Each sets the whole chain at once. A `*` after the name
    means you've changed a knob since.
  - K2 **GAIN:** how hard the mix goes into the limiter: the loudness knob (0 to +12 dB).
  - K3 **CEILING:** the highest the output ever goes (−0.3 dB to start).
  - K4 **BYPASS:** the chain out (the limiter stays, at 0 dB GAIN), to compare. CLEAN is as loud as bypassed, so you
    hear the difference, not just "louder".
- **EQ** and **EQ 2:** LOW, MID and HIGH (±12 dB) and a LOW CUT; then where they sit: LOW FREQ, MID FREQ, MID Q (low
  is wide), HIGH FREQ. The curve on the screen is what the EQ does.
- **COMP** and **COMP 2:** THRESHOLD, RATIO, ATTACK, RELEASE; MAKEUP, MIX (parallel compression), PUMP (the mix ducks
  at each kick), and the reduction now. On from the start: 2:1 over −18 dB, +3 dB makeup. RATIO 1:1 turns it off.
- **TAPE:** DRIVE (rounds the peaks off and thickens; quiet sounds pass untouched), WARMTH (adds even harmonics), TONE
  (rolls the top off, like tape), MIX.
- **STEREO:** WIDTH (0 % mono, 100 % as it is, up to 200 %), BASS MONO (everything under it in the centre: tighter
  lows, safe on mono speakers), BALANCE, and a **correlation** meter: +1 is mono, near 0 is very wide, under 0 is out of
  phase (it would cancel in mono).
- **LIMIT:** GAIN and CEILING again, RELEASE, and LIMIT on or off (off, a loud mix clips).

**About MASTER:** the MASTER knob now comes after the whole chain, so it only sets how loud you hear it and never
changes what the chain does, or (since 0.5) what a computer records over USB. It also means **you'll turn MASTER up further than with X0X** for the same loudness;
for more, use GAIN or the LOUD preset.

Measured on a demo groove: bypassed and CLEAN about −15 LUFS; GLUE, WARM, WIDE and LO-FI about −13; PUNCH −12; LOUD −9.
Peaks stay under −0.7 dB.

## Recording on a computer: USB audio and stems

Plugged into a computer over USB, the FM-1 is also a sound card, with no driver to install: **GhettoMachine FM-1**,
10 inputs and 2 outputs, at 44.1 kHz. The inputs are the FM-1's sound; the outputs let the computer play through the
FM-1's speaker and headphones.

| Inputs | What's on them |
| --- | --- |
| **1–2** | The master: everything you hear, after the master bus (MASTER doesn't change it) |
| **3–4** | The drums alone |
| **5–6** | T2 alone |
| **7–8** | T3 alone |
| **9–10** | T4 alone |

In a DAW, pick GhettoMachine FM-1 as the audio input device, make a stereo track for each pair, and record them all at
once. Each stem is one track as it comes into the mix:

- **In a stem:** the track's LEVEL and PAN, its mutes and solo, its LFO, locks and everything it plays (ROLL and FILL
  too).
- **Only on the master:** the reverb and the delay (the sends go to them, not into the stems), the DJ filter and the
  whole master bus, MASTER, the metronome, and the perform keys that work on the output (STUTTER, TAPE STOP, CRUSH,
  THROW, WASH). Add your own effects to the stems, or record the master pair beside them.
- **Levels:** a stem sits at the level its meter shows on the screen; the four add up to what goes into the master
  bus, less the reverb and the delay. The demos peak 6 to 14 dB under full scale. A stem has no limiter: one that goes
  past full scale clips, so turn that track's LEVEL down.
- **The tracks' levels set the USB mix, MASTER doesn't:** the knob sets only the speaker and the headphones, so you can
  listen quietly (or not at all) while the computer records. The master pair comes off the master bus at its full
  level, its peaks kept under the limiter's CEILING.
- **The metronome is on the master pair** when it's on (hold REC, K1). Set it to OFF or REC while you record the
  master pair, or use the DAW's own click.
- **16 bits** a channel: USB at the FM-1's speed carries ten channels at 44.1 kHz with 16 bits each, and no more.

## HOME: PATTERNS, the scene grid

HOME is for playing live. Its first page, **PATTERNS**, is a grid: **16 patterns across** (the white keys), **a track a row** (DR, T2,
T3, T4, the pad keys 1–4). Each track plays its own pattern, its **clip**, so you can mix the drums of pattern 3 with the
bass of pattern 7. The playing clip is solid with a bar showing where it is; a clip waiting to start blinks. The band
reads **PLAYING 01** (PATTERN 01 when stopped) and, while another pattern waits for its turn, **NEXT 02** beside it,
its number blinking yellow in the header and its clips yellow on the grid; it starts when it always has (LAUNCH, below).

| On HOME | What it does |
| --- | --- |
| **A white key** | A **scene**: every track to that pattern. While playing, at the end of the bar. |
| **Hold a track key + a white key** | Only that track's clip changes. |
| **Hold a track key + OCT−** | That track stops (at the bar). Start it again with a clip. |
| **Tap a track key** | Mute or unmute it. |
| **Hold a track key + OCT+** | **Solo** it; again to go back to how the mutes were. |
| **Hold a white key + K1 / K2** | That scene's **follow action** and after how many bars (below). |
| **Black keys 5–11, held** | The **perform keys** (below). |
| **Hold OCT+** | TAPE STOP. |
| **Tap OCT−** | **Tap tempo:** two taps or more, the tempo follows them. |
| **SEL + SELECT** while playing | **Nudge:** the whole groove 5 ms later or earlier a detent, to line up with a record or another machine. |

A track mute cuts the track's voices and new sends at once, held notes included; unmuting waits for a new hit or note.
A drum lane mute (the QUICK MIXER's white keys, the drums' ENV page) is the lane's sound only, with the DrumEngine: its
hits go on silently, so unmuting brings back a tail still ringing. Sound already in the shared reverb and delay or a
perform loop rings out either way; muting never empties the effects the other tracks use.

Whatever you edit (steps, notes, locks) is the clip the track is on: SEQ, EDIT and the rest go to the track's clip,
not to "the pattern". The pattern number in the band is the last scene picked.

**LAUNCH** (the SONG page, K3) sets when clips and scenes start: at the **BAR** (the default), at the next **BEAT**,
or **NOW**.

### Follow actions

Each scene can move on by itself after 1–16 bars: hold its white key and turn **K1** to **NEXT**, **PREV**, **FIRST**
or **RANDOM** (another pattern that has something in it), **K2** for the bars. Set pattern 1 to NEXT after 4 bars and
pattern 2 to FIRST after 4, and the two take turns. Picking a scene yourself starts its bars again. The song, when it
plays, comes first.

### The perform keys

Hold one and it works; let go and the sound is as it was. **K1 while you hold it** changes how:

| Key | Held | K1 |
| --- | --- | --- |
| **5 ROLL** | Repeats each track's step as it was when you pressed, while the pattern goes on silently underneath; let go and it's where it would have been. | 1/8, 1/16, 1/32, 1/16T, 1/32T |
| **6 FILL** | The drums play the next pattern's part, by its own LENGTH and RATE. Let go, they are back on their own clip where it would have been, in step with the other tracks. | |
| **7 STUTTER** | Loops a slice of the output. | 1/32, 1/16, 1/8 |
| **8 FILTER** | The DJ filter at K1's place (left low-pass, right high-pass). | the sweep |
| **9 THROW** | Everything into the delay, with long feedback: an echo throw. | feedback |
| **10 WASH** | Everything into the reverb, with a long tail. | decay |
| **11 CRUSH** | Fewer bits and a lower rate on the output. | how much |
| **OCT+ TAPE STOP** | The output slows to a halt. | |

STUTTER and TAPE STOP borrow the delay's memory, so the delay's tail is gone while they're held.

## HOME again: the QUICK MIXER

Press HOME on PATTERNS. One track's mix on the four knobs, with the pattern playing on: opening it, picking a track or
using a key here never stops, restarts or re-times playback.

| On the QUICK MIXER | What it does |
| --- | --- |
| **A track key** (pad keys 1–4) | Picks DRUMS, T2, T3 or T4 to edit. One press: no mute. The picked track's key is lit, the others glow dimly (on PATTERNS the same keys mute, lit meaning playing). The screen and the readout show its values at once. |
| **K1 LEVEL · K2 REVERB · K3 DELAY · K4 PAN** | Always these four, the mixer's own values (the same as on GLO). A knob moves on from the value the track has, so picking another track never jumps or overwrites a level. |
| **Black keys 5–11, held** | The perform keys, as on PATTERNS (ROLL, FILL...), for every track: dim, lit while held. |
| **On the drums: white keys 1–11** | Mute or unmute that drum lane (BD, SD, LT... in pad order). Each lane's key is lit while the lane plays and dark when it's muted; the screen draws muted lanes dark and the readout names them ("MUTED CY", or "NO LANES MUTED"). With the DrumEngine the mute is the lane's sound only: its hits go on, so a tail still ringing comes back when you unmute. These are the pattern's lane mutes, the ENV page's. |
| **On T2–T4: the white keys, held** | The moves, below: all sixteen, on that track only. Dim, lit while held. |

The moves, held, on the selected instrument track:

| White key | Held |
| --- | --- |
| **1 FILTER CLOSE** | The track's own filter cutoff down by half its range: darker. |
| **2 FILTER OPEN** | Its cutoff up as much: brighter. Hold both and the last one pressed wins; let it go while the other is down and the other takes over. |
| **3 VERB SPLASH** | The track's reverb send raised (to 90 %, or left where it is if it's higher). |
| **4 DELAY THROW** | Its delay send raised the same way. The repeats ring on after you let go. |
| **5–9 ROLL 1/8, 1/16, 1/32, 1/16T, 1/32T** | This track's step as it was when you pressed, again and again at that rate, while its part goes on silently underneath; let go and it's where it would have been. The other tracks play on. |
| **10–12 STUTTER 1/8, 1/4, 1/2** | This track plays the 2, 4 or 8 of its own steps it's in (1/8, 1/4 or 1/2 of a bar at 1/16 a step), round and round, while its part goes on underneath; let go and it's where it would have been. |
| **13–16 OCT −2, −1, +1, +2** | This track's notes an octave or two down or up while held, the ones already sounding too. |

Within a group (the two filter keys, the five ROLLs, the three STUTTERs, the four OCTs) the last one pressed wins;
let it go while another of the group is down and that one takes over. Different groups combine: a ROLL under FILTER
CLOSE and OCT +1, say. A ROLL held on this track replays what you hear, a STUTTER's step included. The black ROLL key
rolls every track at its own rate (K1) on top.

The filter and send moves use the track's filter and the shared BongVerb and BongDelay, no new effects, and ramp in
and out (about 15 ms) so they don't click; OCT moves at once. None of them records anything, even with REC on, and
nothing about them is saved: let go and the track is at its settings as they are now, a knob turned while you held
one included. Leaving the page, picking another track or stopping lets them all go, so a move never sticks or moves
to another track.

## The song

Press HOME on the QUICK MIXER: up to 32 entries, each a pattern for 1–16 bars.

- White keys set the cursor's entry and move on; at the end they add one.
- SELECT moves the cursor. K1 changes the entry's pattern, K2 its bars, **K3 LAUNCH** (above), K4 turns SONG on or off.
- OCT+ copies the entry; OCT− deletes it.
- With SONG on, PLAY plays the song from the first entry, or from the cursor if you're on the SONG page, and goes round
  again after the last. Pad keys mute tracks here.
- **Copy a pattern:** hold SAVE and press a pattern key.

## SETUP

Press HOME on the SONG page.

- **K1 MIDI CLOCK:** **OFF**; **SEND** (24 clocks a beat while playing, with Start and Stop, out of USB); **FOLLOW**
  (the tempo, Start and Stop from the computer or a drum machine over USB). Following, the FM-1 starts on the first
  clock after Start, measures the tempo over the last 4 beats and pulls its groove to the clock every 16th, so they
  don't drift apart. A jump in tempo is caught within about two beats.
- **K2 SOUNDS:** **SHARED** (one sound per track, whatever the pattern) or **OWN** (**sounds per pattern**): each
  pattern keeps T2–T4's sounds and the drums' kit. A track moving to another pattern (a scene, a clip, the song, a
  follow action) takes that pattern's sound; a pattern with none yet takes the sound that's playing. So pattern 1 can
  be a 909 with a soft pad and pattern 2 an 808 with an acid bass. The grid shows which patterns have their own.
- **SELECT: CPU.** Turn SELECT to see where the processor's time goes, live: **DRUMS**, **SYNTH**, **INSERT+COLOR**,
  **REV+DLY CORE2**, **MASTER CORE2**, **OTHER** and the **TOTAL**, each as a share of the time the FM-1 has for each
  block of sound. The bar is the average; a tick past it marks the worst block of the last second or so (a pattern
  starting, a switch), and the band's **PEAK** is the total's. **LATE**: the blocks that weren't ready in time since it
  started (each a glitch). The reverb, the delay and the master bus run on the FM-1's second core (CORE2) while the
  first renders the next block; TOTAL is the first core's, and it waits for the second at the end of a block (in
  OTHER) and for its half of the synth's voices (in SYNTH). Turn SELECT back for the clock and the sounds.

## What is kept, and where

| What | Where | When |
| --- | --- | --- |
| The project: patterns, song, sounds (each pattern's too), mixer, FX, the master bus, LFOs, arps, follow actions, SETUP | a file with two copies, so a power cut while saving keeps the old one | when you tap SAVE (while playing: at the stop) |
| User presets and kits | a store of their own | as soon as you save one |

**From 0.4:** the same format; 0.4 and 0.5 read each other's projects.

**From 0.3:** a project saved by 0.3 loads as it was; its parts' generators start unset. The first SAVE writes 0.4's
format (0.5's too), which 0.3 can't read: going back to 0.3 starts a fresh project (the file stays, and 0.4 or 0.5
reads it again).

**From 0.2:** a project saved by 0.2 (or by its performance update) loads with every note, lock, sound and setting it
had; the synth update's settings start at their defaults, so every sound plays as before. Your USER presets are
converted the same way. 0.2 can't read the new format either.

**SAVE lights up when there's something unsaved.** Hold SAVE and press OCT− to take the track picked back to its
sound as saved (the drums: all their knobs), for when an experiment went too far. With sounds per pattern on, that's
the sound saved with the pattern it's on.

## MIDI

- **In:** channel 10 plays the pads (General MIDI drum notes), channels 2–4 play T2–T4, and channel 1 plays the
  track picked.
- **Out:** the keys send on their track's channel (pads on channel 10).
- **Clock:** sent or followed, over USB (SETUP, above).

## Not in 0.5

- CPU on the FM-1: SETUP's CPU view shows the load by stage. If it runs short, voices drop first. A voice using FM,
  the shaper or the matrix takes about one and a half times a plain one's time (measured on the Mac). Since 21 Nov (the CPU pass) the
  606's clap renders at 22.05 kHz (its filters hold under 1 % of their energy above 11 kHz: it sounds the same at a third
  of the work), its hats and cymbal skip a sine per line per 32 samples, Color skips an inaudible DIGITAL and works out
  WOW's delays every 16 samples, the phaser its coefficients every 16, and a silent track's Color and insert sleep: a
  busy scene costs about a fifth less, and a quiet one two-thirds less (counted in instructions on the Mac). The second
  pass (21 Nov, later): the reverb runs at 22.05 kHz (every octave band of its tail within 0.8 dB of before, its decay
  times the same; it gives up what it held above 11 kHz) for a third less reverb and delay; the master bus does the same
  sums with fewer memory trips (its sound bit for bit as it was), a sixth less; and the synth's voices, the reverb, the
  delay and the master bus run from the FM-1's RAM instead of through its flash's cache, which both cores share. The
  synth's sound is bit for bit as it was. What costs most on the FM-1, roughly: each synth voice sounding, about 5 % of a
  block (a unison stack such as SUPERSAW is 4 a note, and a pad's long release keeps the last chord's voices going
  under the next), then the drums, the inserts and Color, the master bus and the reverb and delay. (The 606 cost about
  2.5 times the 909; it and the 707 were cut on 21 Nov.) The third pass (21 Nov,
  after the FM-1 still went late as patterns started and switched): the FM-1's two cores split each block's work. The
  first renders block N (the sequencer, the drums, the synth's voices, the inserts and Color) while the second finishes
  block N - 1 (the reverb and the delay, the master bus, STUTTER, TAPE STOP and CRUSH, the
  metronome); before, the second core only took half the synth's voices. So everything is heard a block (5.8 ms) later
  than before, the USB stems with it; the sound is otherwise bit for bit the same. Loading a kit's knobs (a user kit, a
  pattern switch with SOUNDS PER PATTERN, RELOAD, a project) holds the audio off for one pad at a time, not for all
  four kits at once (about a millisecond on the FM-1).
- **Budgets** (21 Nov, in the source): each section has a share of the processor it can't pass, so one can't take the
  others' time: the **drums 20 %**, the **synth 30 %**, the **effects 25 %** (the inserts, Color, the reverb and the
  delay together). Over its share a section gives up its least important work, without a glitch in the rest:
  - the synth's voices are capped at what 30 % buys (about 6 or 7 on the FM-1), at what a voice is measured to cost
    as you play; a note over the
    cap takes the place of the quietest voice letting go, else the oldest. A shed voice fades out in about 10 ms
    whatever its release (before, a pad's long release kept it costing for seconds);
  - the drums' oldest sound still ringing is cut (a cymbal's or a tom's tail before a hit just played);
  - an effect leaves the chain: Color on the drums, then on T4, T3, T2, then the inserts on T4, T3, T2, one at a time,
    and comes back, from silence, when there is room (one every 4 s). The reverb, the delay and the master bus stay.
  SETUP's CPU view has a notch under the DRUMS and SYNTH bars at their budgets, and a section's number turns red for a
  second when it gave something up (and while an effect is out). And a **guard**: a block that takes over 90 % of its
  time on the first core (a kit changed while the old one's tails ring, every section at its peak together) cuts
  every drum sound not just hit and sheds two voices at once, before the next block can run late. (The budgets were
  30 % and 40 % for a day: fuller, but blocks ran late again.)
- **No spike as clips change** (21 Nov, in the source). On the FM-1 the late blocks at a scene's change came from two
  kits sounding at once: the old kit's tails under the new kit's hits took the drums to 44–48 % of a block. A kit that
  leaves now stops sounding as the new one comes. A scene's own kit (SOUNDS PER PATTERN) is loaded in the audio's own
  time at the bar: it now sets only the knobs that differ from the ones in place, not every knob of every pad. With
  SOUNDS shared (SETUP, K2) a scene's change costs no more than any bar's first step.
- **The tracks' mix on both cores** (22 Nov, in the source; the same samples). Each instrument track's voices summed,
  its insert and Color, its pan and sends were the first core's work, about a fifth of a block with inserts on three
  tracks. The second core now takes some of the tracks, by what each cost the block before, while the first does the
  rest; and it takes its half of the voices from two voices up (it was four). Releasing STUTTER or TAPE STOP no longer holds the
  sound off for 1.5 ms while the delay's memory is cleared, and the 606's clap costs a third as much to hit (its filter
  bank is kept between hits at the same TUNE).
- **The synth, cheaper, the same sound** (21 Nov, in the source; every sound and every test recording bit for bit as
  before): a voice with **MIX at 0** no longer works out its second oscillator (a bass or a lead costs about a
  quarter less); a sound with **no matrix slot in use** skips the matrix; the track mix does one sum a sample for a
  track whose voices are all centred, and leaves out a track with nothing sounding whose insert and Color are
  asleep (three silent tracks with inserts cost 3 % of a block); the synth's lookup tables and its mix run from RAM.
  The master bus's EQ, TAPE, STEREO and meters went back to the flash to make the room: they run on the second core,
  only while they're on.
- The first recording of the stems from the FM-1 had each track on its own pair, but the USB buffer ran dry about a
  dozen times in 9 seconds, each a click in the recording (not in the FM-1's own sound). 0.5 now keeps a deeper buffer
  (about 6 ms more delay); recorded from on 21 Nov with MASTER at 0, it had no clicks and the master pair kept its
  level.
- No stem for the reverb and delay returns (the ten inputs are all used), no 48 kHz or 24-bit stems.
- No per-track EQ or compression, no multiband compression on the master; no locks on LFO 1 or arpeggiator settings
  (LFO 2's and the matrix's amounts lock); the arpeggio isn't recorded, only what you play.
- The matrix can't move another slot's amount, and LFO 2 doesn't follow the tempo (LFO 1 does, and is a source).
- Projects from earlier builds (before 0.2) start fresh. A project saved by 0.2 loads converted, with no follow
  actions and sounds per pattern off if it was saved before the performance update.
- MIDI clock is timed to the audio block it arrives in, so following, the FM-1 sits about 3 ms behind the clock on
  average (up to 6). Continue starts from the top. The clock is sent out of USB only.
- Sounds per pattern keeps the tracks' sounds and the kit, not the mixer, FX or master bus.
- The 303 generator: no per-step dice locking (keep this step, re-roll the rest), no 303-style transpose track, no
  drum generator. A part of any length, up to 128 steps, gets a line of that length, not a 16-step line repeated. The arpeggiator
  ignores slides (it plays each note as its own).
