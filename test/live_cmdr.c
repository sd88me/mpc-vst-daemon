/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* My commander client against the real addin (through an SSH tunnel to its port): prints what it sees, read-only.
 * usage: live_cmdr <port> */
#include "../src/daemon_cmdr.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    char err[200]; cmdr_t *c = cmdr_open("127.0.0.1", argc > 1 ? atoi(argv[1]) : 6730, err, sizeof err);
    if (!c) { printf("open failed: %s\n", err); return 1; }
    static char b[1 << 17];
    int n = cmdr_plugins_summary(c, b, sizeof b); printf("plugins (%d bytes): %.1500s\n", n, b);
    n = cmdr_project(c, b, sizeof b); printf("project (%d bytes): %.1200s\n", n, b);
    n = cmdr_project(c, b, sizeof b);   /* a second round trip: the transport message is only read while a request waits */
    n = cmdr_transport_state(c, b, sizeof b); printf("transport: %.300s\n", b);
    cmdr_close(c); return 0;
}
