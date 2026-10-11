/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#include "../src/daemon_cmdr.h"
#include "../src/daemon_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
    int in[2], out[2]; if (pipe(in) || pipe(out)) return 1;
    pid_t pid = fork();
    if (!pid) { dup2(in[0], 0); dup2(out[1], 1); close(in[1]); close(out[0]); execlp("python3", "python3", "-I", "test/fake_commander.py", (char *)NULL); _exit(127); }
    close(in[0]); close(out[1]);
    FILE *so = fdopen(out[0], "r"); char line[512];
    if (!fgets(line, sizeof line, so)) { printf("fake commander did not start\n"); return 1; }
    int port = atoi(line);

    char err[120] = "";
    CHECK(cmdr_open("127.0.0.1", 1, err, sizeof err) == NULL && strstr(err, "not reachable"), "closed port: '%s'", err);
    cmdr_t *c = cmdr_open("127.0.0.1", port, err, sizeof err);
    CHECK(c != NULL, "open: %s", err);
    if (!c) return 1;
    static char buf[1 << 16];
    int n = cmdr_plugins_summary(c, buf, sizeof buf);
    CHECK(n > 0 && !strcmp(buf, "[{\"id\":3,\"name\":\"Chordsmith\",\"vendor\":\"jacob-sabella\",\"synth\":true,\"params\":2},{\"id\":5,\"name\":\"Profit-8\",\"vendor\":\"sd88me\",\"synth\":true,\"params\":3}]"), "summary %s", buf);
    CHECK(cmdr_plugins_summary(c, buf, 20) == -1, "summary into a small buffer");
    n = cmdr_plugin_params(c, 5, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "\"name\":\"Cutoff\"") && strstr(buf, "\"text\":\"70%\""), "params %s", buf);
    CHECK(cmdr_plugin_params(c, 99, buf, sizeof buf) == -1, "unknown plugin id");
    n = cmdr_project(c, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "\"tempo\":124") && strstr(buf, "\"key\":\"F Minor\""), "project %s", buf);

    char text[64];
    CHECK(cmdr_set(c, 5, 0, 0.35, text, sizeof text, 1500) == 0 && !strcmp(text, "35%"), "set -> text '%s'", text);
    CHECK(cmdr_set(c, 5, 9, 0.5, text, sizeof text, 1500) == -1 && !strcmp(text, "no such parameter"), "bad index: '%s'", text);
    CHECK(cmdr_set(c, 5, 1, 7.0, text, sizeof text, 1500) == 0 && !strcmp(text, "100%"), "value clamped to 1: '%s'", text);
    CHECK(cmdr_transport(c, "play") == 0 && cmdr_transport(c, "rewind") == -1, "transport commands");
    n = cmdr_transport_state(c, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "\"tempo\":124"), "transport state %s", buf);
    usleep(200000);

    /* what the server received, in order */
    char seen[4096] = ""; for (int i = 0; i < 5; i++) { if (!fgets(line, sizeof line, so)) break; strncat(seen, line, sizeof seen - strlen(seen) - 1); }
    CHECK(strstr(seen, "{\"t\":\"project\"}") && strstr(seen, "\"t\":\"set\",\"id\":5,\"i\":0,\"value\":0.35") && strstr(seen, "\"i\":1,\"value\":1.0") && strstr(seen, "\"cmd\":\"play\""), "server saw: %s", seen);
    CHECK(!strstr(seen, "rewind"), "an invalid transport command was not sent");
    cmdr_close(c);
    close(in[1]); fclose(so);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
