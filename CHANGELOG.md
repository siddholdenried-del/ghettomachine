# GhettoMachine: build history

Moved out of MANUAL.md on 24 Nov 2026. The newest notes are in the manual's sections and the commit log.

**24 Nov 2026, later (project format 12):** mix per pattern (SETUP K4); MIDI out of the sequencer (SEQ OUT), MIDI
Continue, and Bluetooth MIDI on fm1-base's own stack (SETUP, SELECT: MIDI); the drums' beat generator (SEQ again on
their steps); the 303 generator's KEEP page. The stock 808 / 909 engines' state left the FM-1's RAM (15 KB: they
have not played there since the DrumEngine took over), which is what made room for the radio.

**Historical hardware test: 0.5** (first installed over USB on 20 Nov). The first recording from it clicked now and then, so
0.5 now keeps a deeper USB buffer, MASTER no longer changes what a computer records, and the drum steps' beats glow
(1, 5, 9, 13): that build went on the FM-1 on 21 Nov, and a recording from it had no clicks. REC + GLO (erase the
track's part, to record it again) went on it the same day, and later the 808 brought up to the 909's level with the
Dungeon BBS screen. The CPU pass (SETUP's CPU view; a cheaper 606, Color and phaser; effects that sleep on silence) went
on it the same day, and a second pass (the reverb at half the rate; the synth's voices, the reverb, the delay and the
master bus run from RAM) later that day. A third (the reverb, the delay and the master bus on the second core, a block
behind the first; the CPU view's peaks) went on it that evening. The sections' budgets went on it that night at 20 %
for the drums, 30 % for the synth and 25 % for the effects: played, it had no late blocks, but the budgets cut 138 drum
sounds in a minute and held the synth to 4 or 5 voices with a third of the first core unused. Loosened to 30 %, 40 %
and 25 %, that went on it the same night (7 to 9 voices, few drum cuts; the first core at about 70 %). The 606 and
the 707 were then cut (their code and memory given to the processor's budget) and FILL made to leave the drum clip
where it would have been: on the FM-1 since that night. The synth made cheaper without changing its sound (below)
went on it later that night (the synth's mix from 450 to about 250 us a block; steady playing at about 60 % of the
first core, the only late blocks at kit changes). Then, asked for: the budgets back at 20 %, 30 % and 25 % with a
guard for the block that takes too long, the LFO made each clip's own (project format 8), and no spike as clips
change (a kit that leaves stops sounding): on the FM-1 since 22 Nov. The tracks' mix, with their inserts and Color,
shared between the two cores: in the source, not on the FM-1 yet. 0.5 is 0.4 plus the USB stems. 0.4 is 0.3 plus the 303 generator, the new name and the boot screen's logo; 0.3 is 0.2 plus the performance
update (the scene grid onwards) and the synth update. 0.5 passes its simulator tests (2,194 checks, 39 more on the
synth itself and 48 on the master bus's sound). It reads a project saved by 0.2, 0.3 or 0.4, and by 0.5 before each clip had its own LFO.
