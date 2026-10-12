/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* Validate a macros.json before copying it to a device: tools/macro_check macros.json
 * Build: gcc -O1 -o macro_check tools/macro_check.c src/daemon_macro.c src/daemon_fp.c src/daemon_mcp.c src/daemon_json.c -lm -ldl */
#include "../src/daemon_macro.h"
#include <stdio.h>
int main(int argc, char **argv) {
    char err[300];
    if (argc < 2) { fprintf(stderr, "usage: macro_check macros.json\n"); return 2; }
    macros_t *m = macros_load(argv[1], err, sizeof err);
    if (!m) { printf("INVALID: %s\n", err); return 1; }
    int w, h; macros_screen(m, &w, &h);
    printf("OK: screen %dx%d; select_track %s, load_plugin %s, import_clip %s\n", w, h, macros_has(m, "select_track") ? "yes" : "no", macros_has(m, "load_plugin") ? "yes" : "no", macros_has(m, "import_clip") ? "yes" : "no");
    macros_free(m); return 0;
}
