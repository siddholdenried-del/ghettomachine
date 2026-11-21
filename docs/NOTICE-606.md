# 606 drum voice notices

- GhettoMachine’s 606 uses the C/FM-1 port from **Cesari** by briwil,
  [commit 44abc098](https://github.com/briwil/Felucca/tree/44abc098d5f6d11d776aa40a0a435b89d38411c8/firmware/src/x0x),
  GPL-3.0-only. The three DSP files under `app/dsp/drum606*` come from that port;
  GhettoMachine supplies the integration and keeps its own interface and sequencer.
- The port is based on **6W6**, by Charles Vestal and contributors,
  [schwung-6W6](https://github.com/charlesvestal/schwung-6W6), GPL-3.0. Its contributions
  include kit balance and velocity, drive, hi-hat choke, and the metal/cymbal and
  clap adaptations.
- **606-Inspired-Synth-Drums**, by Matthew Fecher / AudioKit Pro, supplies voice
  designs and coefficient data, including the clap filters. The original
  [upstream project](https://github.com/analogcode/606-Inspired-Synth-Drums)
  is MIT-licensed. Its [license text](MIT-AudioKit-606.txt) accompanies the code.
- All 606 sounds are synthesized. No 606 recordings were added. No changes were
  made to the 808 or 909 voice sources or their sample assets.
