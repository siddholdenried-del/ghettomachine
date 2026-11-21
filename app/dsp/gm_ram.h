/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine 0.5 (CPU): the DSP that runs from RAM, out of the XIP cache's way. The engines' units never include the
 * platform's headers (fm1.toml o2: fastmath.h and plat.h never meet), so this is fm1-base plat.h's PLAT_RAM_CODE and
 * PLAT_FAR again, for them:
 *   GM_RAM        a function in section .app_ram (copied to RAM at boot). It may call only RAM code (other GM_RAM or
 *                 PLAT_RAM_CODE functions) or helpers inlined into it (GM_INLINE: always), and nothing through a
 *                 pointer: the build checks it over the disassembly (fm1-base tools/ramcode.py)
 *   GM_FAR(fn)    fn through a pointer the compiler cannot turn back into a direct call: how code in the flash calls
 *                 RAM code (a direct call doesn't reach that far)
 * The host and the browser: plain functions and calls. */
#pragma once

#define GM_INLINE static inline __attribute__((always_inline))
#ifdef PLAT_HOST
#define GM_RAM
#define GM_FAR(fn) (&(fn))
#else
#define GM_RAM __attribute__((section(".app_ram")))
GM_INLINE void *gm_far_ptr(void *fn)
{
    void *volatile held = fn;             /* read back from memory: the compiler no longer knows the target */
    return held;
}
#define GM_FAR(fn) ((__typeof__(&(fn)))gm_far_ptr((void *)&(fn)))
#endif
