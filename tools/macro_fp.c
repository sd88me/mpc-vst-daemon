/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* Capture a screen fingerprint for macros.json: tools/macro_fp <port> <name> <x> <y> <w> <h> [host]
 * Reads the region through the remote addin (an SSH tunnel to its port 6720, or on the device itself) and prints the entry for the
 * "fingerprints" table. Choose a region that holds the page's fixed furniture (titles, tabs, labels), not meters or the playhead.
 * Build: gcc -O1 -o macro_fp tools/macro_fp.c src/daemon_fp.c src/daemon_mcp.c src/daemon_json.c -lm -ldl */
#include "../src/daemon_fp.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: macro_fp <port> <name> <x> <y> <w> <h> [host]\n"); return 2; }
    char err[200]; mcp_t *m = mcp_open(argc > 7 ? argv[7] : "127.0.0.1", atoi(argv[1]), err, sizeof err);
    if (!m) { fprintf(stderr, "%s\n", err); return 1; }
    int x = atoi(argv[3]), y = atoi(argv[4]), w = atoi(argv[5]), h = atoi(argv[6]); uint8_t t[FP_BYTES];
    if (fp_capture(m, x, y, w, h, t, err, sizeof err)) { fprintf(stderr, "%s\n", err); mcp_close(m); return 1; }
    char hex[FP_BYTES * 2 + 1]; fp_hex(t, hex);
    printf("\"%s\": { \"region\": [%d, %d, %d, %d], \"thumb\": \"%s\", \"tolerance\": 12 }\n", argv[2], x, y, w, h, hex);
    mcp_close(m); return 0;
}
