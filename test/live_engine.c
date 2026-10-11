/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* The real engine and the real io_device.c (libcurl) on the host, a typed prompt: prints what the skin would show.
 * usage: live_engine <dir with daemon.conf> "<prompt>" */
#include "engine.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const mpc_engine_t *E = mpc_engine(); void *a = E->create(argv[1]); char b[600];
    E->set_param(a, "type", "Chords"); E->set_param(a, "bars", "4");
    E->set_param(a, "prompt", argv[2]);
    for (int t = 0; t < 1200; t++) { usleep(50000); E->get_param(a, "status", b, sizeof b); if (strncmp(b, "THINKING", 8)) break; }
    printf("status:     %s\n", b);
    E->get_param(a, "transcript", b, sizeof b); printf("transcript: %s\n", b);
    E->get_param(a, "reply", b, sizeof b); printf("reply:      %s\n", b);
    E->set_param(a, "save", "1"); E->get_param(a, "status", b, sizeof b); printf("after save: %s\n", b);
    E->destroy(a);
    return 0;
}
