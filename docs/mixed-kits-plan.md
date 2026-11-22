> **Superseded in part (21 Nov 2026):** the 606 and the 707 kits and the 707's Cowbell and Tambourine were cut. Mixed kits remain, between the 909 and the 808.

# Private prototype
Keep existing numeric model IDs and pattern lane ordering. Append 707 as engine/factory IDs, plus per-pad model overrides, scene snapshots, and user-slot map suffix. Preserve project 1–5 migrations, 32 KiB A/B project sectors and sample bank; let codec reject oversized projects before writing. Provide MODEL section inside drum EDIT and use existing SAVE+PRESETS gesture. Resolve each lane's engine before trigger, parameters, locks, LFO, and display; cut only a replaced lane. Mixed hats respect source engine choke setting. Match untouched factory outputs byte-for-byte, test actual host panel/project and binary size. No release, push, or flash.

# Compatibility
Factory audio for the old engines is unchanged. The whole active kit and the per-pattern maps are project format 6; readers still accept formats 1–5. Retain an old save backup before trying format 6 if you may downgrade. User kit records keep their old prefix and ID and append 11 model bytes per slot. The project save size increases by roughly 331 bytes (the existing capacity check still stops before an erase), with no sample-bank boundary change. A user kit stores parameters for each selected voice and the base engine's kit-wide controls, not a whole copy of four kits. The synthesized 707 bank includes 13 voices; its new factory uses 11 lanes, leaving cowbell and tambourine unmapped rather than replacing the pattern's other instruments.

# Checks
- Physical-style host scripts: mixed selection, factory return, slot save/recall, real 707 kick, two per-pattern maps sharing a base preset; previous lock, mute, 128-step, storage and DSP tests.
- Baseline 606/808/909 rendered L/R/reverb/delay compared byte-for-byte against 908fa590; 707 uses its own DSP.
- Device compile/link/disassembly and capacity checks only. No on-hardware timing, deployment or flash.
