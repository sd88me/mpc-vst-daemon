/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* Real engine + real Gemini + a fake commander (the MPC stand-in): types a prompt in agent mode, prints the plan, presses RUN and
 * prints what reached the "device". usage: live_agent <dir with daemon.conf> "<prompt>" */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static const mpc_engine_t *E;
static char *get(void *i, const char *k) { static char b[1000]; b[0] = 0; E->get_param(i, k, b, sizeof b); return b; }
static void wait_done(void *i, const char *what) {
    for (int t = 0; t < 5000; t++) { usleep(50000); const char *s = get(i, "status"); if (strncmp(s, "THINKING", 8) && strncmp(s, "RUNNING", 7)) return; if (t % 100 == 99) { printf("  ... %s\n", s); fflush(stdout); } }
    printf("timeout %s\n", what);
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int in[2], out[2]; if (pipe(in) || pipe(out)) return 1;
    pid_t pid = fork();
    if (!pid) { dup2(in[0], 0); dup2(out[1], 1); close(in[1]); close(out[0]); execlp("python3", "python3", "-I", "test/fake_commander.py", (char *)NULL); _exit(127); }
    close(in[0]); close(out[1]);
    FILE *so = fdopen(out[0], "r"); char line[400]; if (!fgets(line, sizeof line, so)) return 1;
    char path[400]; snprintf(path, sizeof path, "%s/daemon.conf", argv[1]);
    FILE *cf = fopen(path, "a"); fprintf(cf, "commander=127.0.0.1:%d\n", atoi(line)); fclose(cf);
    E = mpc_engine(); void *a = E->create(argv[1]);
    E->set_param(a, "agent", "On"); E->set_param(a, "key", "F"); E->set_param(a, "scale", "Minor");
    E->set_param(a, "prompt", argv[2]); wait_done(a, "plan");
    printf("status: %s\n", get(a, "status")); printf("reply:  %s\n", get(a, "reply")); printf("plan:   %s\n", get(a, "plan"));
    if (!strncmp(get(a, "status"), "PLAN", 4)) { E->set_param(a, "run", "1"); wait_done(a, "run"); printf("after RUN: %s\n", get(a, "status")); }
    E->destroy(a); close(in[1]);
    printf("--- what reached the device ---\n");
    while (fgets(line, sizeof line, so)) printf("%.300s", line);
    fclose(so); waitpid(pid, NULL, 0);
    return 0;
}
