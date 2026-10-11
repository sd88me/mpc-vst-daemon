/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* Real engine + libcurl, a recorded WAV (44.1 kHz stereo 16-bit) played into process() as the mic, TALK around it.
 * usage: live_speech <dir with daemon.conf> <speech.wav> */
#include "engine.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[2], "rb"); if (!f) return 2;
    static uint8_t raw[8 << 20]; size_t n = fread(raw, 1, sizeof raw, f); fclose(f);
    uint8_t *p = NULL; for (size_t i = 12; i + 8 < n; i++) if (!memcmp(raw + i, "data", 4)) { p = raw + i + 8; break; }
    if (!p) return 2;
    size_t frames = (n - (size_t)(p - raw)) / 4;
    const mpc_engine_t *E = mpc_engine(); void *a = E->create(argv[1]); char b[600];
    E->set_param(a, "type", "Chords");                  /* the spoken request has to override this: it says "beat" */
    E->set_param(a, "talk", "1");
    int16_t out[256], blk[256];
    for (size_t i = 0; i < frames + 20000; i += 128) {
        memset(blk, 0, sizeof blk);
        if (i < frames) memcpy(blk, p + i * 4, (frames - i < 128 ? frames - i : 128) * 4);
        E->process(a, blk, out, 128);
    }
    E->set_param(a, "talk", "1");
    for (int t = 0; t < 1200; t++) { usleep(50000); E->get_param(a, "status", b, sizeof b); if (strncmp(b, "THINKING", 8)) break; }
    printf("status:     %s\n", b);
    E->get_param(a, "transcript", b, sizeof b); printf("transcript: %s\n", b);
    E->get_param(a, "reply", b, sizeof b); printf("reply:      %s\n", b);
    E->destroy(a);
    return 0;
}
