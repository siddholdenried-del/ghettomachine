/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal (fm1-base's template, which this starts from) */
/* GhettoMachine: four tracks, the drums and T2..T4. The app's half of the contract with the platform
 * (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "GHETTOMACHINE"         /* BongTech GhettoMachine: the splash, crash and safe-mode screens, the reader */
#define APP_TAGLINE "BONGTECH"          /* (the platform's plain splash; app_splash draws its own) */
#define APP_SPLASH_RGB RGB(222, 217, 185)  /* the logo's bone (Bong Cult's Dungeon BBS look) */
#define APP_SPLASH                       /* the boot screen: the logo in Sedgwick Ave Display (tools/gen_logo.py) */
void app_splash(void);
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity from the keys (lib/touch.h): played pads */
/* the USB audio input (the master and the stems) keeps a whole block more in hand, so a render may take all of its
 * block's time: recorded from 0.5 on the FM-1, the shallow ring ran dry a dozen times in 9 s of a groove (each a run
 * of up to ~260 frames held, a click in the recording, not on the FM-1). Its ring goes to the pool; 5.8 ms more
 * latency. (device/unity.c FM1B_UAC_DEEP) */
#define FM1B_UAC_DEEP 1

/* the store: the user presets (sounds and kits; the project itself is a file, app.c PROJ_FILE),
 * the panel calibration (the platform's) and Bluetooth's slot (the radio's address and bond), and their offsets */
int app_ble_wanted(void);               /* Bluetooth MIDI (the own stack: fm1.toml): on only where the project's
                                         * SETUP > MIDI says so (off by default: the radio quiet) */
#define APP_BLE_AT_BOOT app_ble_wanted()
#define APP_BTDEV                        /* a Bluetooth keyboard or controller the FM-1 connects out to (lib/btdev.h) */
enum { OBJ_USER, OBJ_PANEL, OBJ_BLE, OBJ_NOBJ };   /* OBJ_USER: the user presets (app.c user_t) */
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u}

/* the companion's files (fm1.toml companion = true): none of the app's own since the sampler went (0.5: the samples were
 * its three); the hooks stay because they bring plat_file_*, which keeps the project */
#define APP_FILES
int app_file(uint32_t i, plat_file_info_t *f);
int app_file_read(uint32_t i, uint32_t off, uint8_t *dst, uint32_t n);
int app_file_begin(uint32_t i, uint32_t size);
int app_file_write(uint32_t i, uint32_t off, const uint8_t *src, uint32_t n);
int app_file_commit(uint32_t i, uint32_t size, uint32_t crc);
int app_file_delete(uint32_t i);
int app_file_busy(void);                 /* 1: no file written now (playing) */

/* the contract: the platform calls these */
void app_init(void);                     /* boot: the engines, then the saved project (or the default one) */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);                  /* the first screen, after the splash */
void app_frame(void);                    /* the UI: input and a redraw, about 60 times a second */
void app_input_only(void);               /* between frames: input only */
void app_say(const char *a, const char *b);   /* a short message on the screen and to a screen reader */

/* the host simulator's checks (host/plat_host.c "expect") */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
