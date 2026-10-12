/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* Run one macro from macros.json against the remote addin (no vision step): tools/macro_run <port> <macros.json> <macro> '<args json>' [host]
 * For recording and testing macros on a device through an SSH tunnel to its port 6720.
 * Build: gcc -O1 -o macro_run tools/macro_run.c src/daemon_macro.c src/daemon_fp.c src/daemon_mcp.c src/daemon_json.c -lm -ldl */
#include "../src/daemon_macro.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: macro_run <port> <macros.json> <macro> '<args json>' [host]\n"); return 2; }
    char err[200], res[400]; macros_t *m = macros_load(argv[2], err, sizeof err);
    if (!m) { fprintf(stderr, "%s\n", err); return 1; }
    mcp_t *c = mcp_open(argc > 5 ? argv[5] : "127.0.0.1", atoi(argv[1]), err, sizeof err);
    if (!c) { fprintf(stderr, "%s\n", err); macros_free(m); return 1; }
    int rc = macro_run(m, c, argv[3], argv[4], NULL, NULL, res, sizeof res);
    printf("%s: %s\n", rc ? "STOPPED" : "OK", res);
    mcp_close(c); macros_free(m); return rc ? 1 : 0;
}
