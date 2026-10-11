/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#include "../src/daemon_macro.h"
#include "../src/daemon_fp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* test data only: the coordinates are made up for the fake screen, NOT a recording of any real MPC or Force */
static const char *GOOD =
 "{\"version\":1,\"screen\":[1280,800],\"macros\":{"
 "\"select_track\":{\"params\":{\"track\":{\"type\":\"int\",\"min\":1,\"max\":16}},\"steps\":[{\"press\":\"MENU\"},{\"wait\":10},{\"tap\":[\"100 + 140*(track-1)\",300],\"require_change\":true,\"label\":\"track cell\"}]},"
 "\"load_plugin\":{\"params\":{\"track\":{\"type\":\"int\",\"min\":1,\"max\":16},\"plugin\":{\"type\":\"string\"}},\"steps\":[{\"call\":\"select_track\"},{\"tap\":[640,400]},{\"scroll\":[300,400],\"direction\":\"down\",\"amount\":2},{\"find_tap\":\"{plugin}\",\"hint\":\"the plugin list\"},{\"wait_stable\":500}]},"
 "\"stale\":{\"steps\":[{\"tap\":[50,750],\"require_change\":true}]},"
 "\"offscreen\":{\"steps\":[{\"tap\":[2000,10]}]},"
 "\"burst\":{\"steps\":[{\"repeat\":3,\"steps\":[{\"tap\":[10,10]}]}]},"
 "\"ping\":{\"steps\":[{\"call\":\"pong\"}]},\"pong\":{\"steps\":[{\"call\":\"ping\"}]},"
 "\"badbutton\":{\"steps\":[{\"press\":\"NOPE\"}]}"
 "}}";

static int vision_ok(void *u, const char *target, const char *hint, const char *png, int w, int h, int *x, int *y, char *why, size_t wc) {
    (void)u; (void)hint; (void)why; (void)wc; (void)w; (void)h;
    if (!png || strlen(png) < 20) return -1;
    if (!strcmp(target, "Ghost")) { snprintf(why, wc, "no Ghost on screen"); return -1; }
    *x = 777; *y = 333; return 0;
}

int main(void) {
    char err[200];
    macros_t *m = macros_parse(GOOD, err, sizeof err); CHECK(m != NULL, "parse good: %s", err); if (!m) return 1;
    CHECK(macros_has(m, "load_plugin") && !macros_has(m, "nothing"), "has");
    int w, h; macros_screen(m, &w, &h); CHECK(w == 1280 && h == 800, "screen %dx%d", w, h);

    const char *bad[][2] = {
        { "not json", "not valid JSON" }, { "{\"version\":2,\"screen\":[1,1],\"macros\":{\"a\":{\"steps\":[]}}}", "version must be 1" },
        { "{\"version\":1,\"macros\":{\"a\":{\"steps\":[]}}}", "\"screen\"" }, { "{\"version\":1,\"screen\":[1280,800],\"macros\":{}}", "no macros" },
        { "{\"version\":1,\"screen\":[1280,800],\"macros\":{\"a\":{\"steps\":[{\"tap\":[1,2],\"bogus\":1}]}}}", "unknown key \"bogus\"" },
        { "{\"version\":1,\"screen\":[1280,800],\"macros\":{\"a\":{\"steps\":[{\"tap\":[1,2],\"press\":\"X\"}]}}}", "exactly one action" },
        { "{\"version\":1,\"screen\":[1280,800],\"macros\":{\"a\":{\"steps\":[{\"call\":\"zzz\"}]}}}", "unknown macro" },
        { "{\"version\":1,\"screen\":[1280,800],\"macros\":{\"a\":{\"steps\":[{\"tap\":[1]}]}}}", "needs [x, y]" },
        { "{\"version\":1,\"screen\":[1280,800],\"macros\":{\"a\":{\"steps\":[{\"scroll\":[1,2]}]}}}", "direction" },
        { "{\"version\":1,\"screen\":[1280,800],\"macros\":{\"a\":{\"steps\":{}}}}", "must be a list" },
    };
    for (int i = 0; i < 10; i++) { macros_t *b = macros_parse(bad[i][0], err, sizeof err); CHECK(b == NULL && strstr(err, bad[i][1]), "invalid file %d: '%s'", i, err); macros_free(b); }
    CHECK(macros_load("/nonexistent/macros.json", err, sizeof err) == NULL && strstr(err, "no macros file"), "missing file '%s'", err);

    int in[2], out[2]; if (pipe(in) || pipe(out)) return 1;
    pid_t pid = fork();
    if (!pid) { dup2(in[0], 0); dup2(out[1], 1); close(in[1]); close(out[0]); execlp("python3", "python3", "-I", "test/fake_remote.py", (char *)NULL); _exit(127); }
    close(in[0]); close(out[1]);
    FILE *so = fdopen(out[0], "r"); char line[400]; if (!fgets(line, sizeof line, so)) return 1;
    mcp_t *mcp = mcp_open("127.0.0.1", atoi(line), err, sizeof err); CHECK(mcp != NULL, "mcp: %s", err); if (!mcp) return 1;
    char res[400];

    CHECK(macro_run(m, mcp, "select_track", "{\"track\":3}", vision_ok, NULL, res, sizeof res) == 0 && strstr(res, "ran select_track"), "select_track: %s", res);
    CHECK(macro_run(m, mcp, "select_track", "{\"track\":0}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "must be 1 to 16"), "track 0: %s", res);
    CHECK(macro_run(m, mcp, "select_track", "{\"track\":99}", vision_ok, NULL, res, sizeof res) == -1, "track 99: %s", res);
    CHECK(macro_run(m, mcp, "select_track", "{}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "missing parameter track"), "no track: %s", res);
    CHECK(macro_run(m, mcp, "select_track", "{\"track\":\"2\"}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "must be a number"), "text track: %s", res);
    CHECK(macro_run(m, mcp, "nothing", "{}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "no macro called"), "unknown macro: %s", res);
    CHECK(macro_run(m, mcp, "select_track", "nope", vision_ok, NULL, res, sizeof res) == -1, "bad args");

    CHECK(macro_run(m, mcp, "load_plugin", "{\"track\":2,\"plugin\":\"9W9\"}", vision_ok, NULL, res, sizeof res) == 0 && strstr(res, "ran load_plugin"), "load_plugin: %s", res);
    CHECK(macro_run(m, mcp, "load_plugin", "{\"track\":2,\"plugin\":\"Ghost\"}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "could not find on screen") && strstr(res, "no Ghost"), "plugin not on screen: %s", res);
    CHECK(macro_run(m, mcp, "load_plugin", "{\"track\":2,\"plugin\":\"9W9\"}", NULL, NULL, res, sizeof res) == -1 && strstr(res, "no vision"), "no vision: %s", res);
    CHECK(macro_run(m, mcp, "load_plugin", "{\"track\":2}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "missing parameter plugin"), "no plugin: %s", res);
    CHECK(macro_run(m, mcp, "stale", "{}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "did not change"), "stale coordinate: %s", res);
    CHECK(macro_run(m, mcp, "offscreen", "{}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "off the 1280x800 screen"), "off screen: %s", res);
    CHECK(macro_run(m, mcp, "burst", "{}", vision_ok, NULL, res, sizeof res) == 0 && strstr(res, "(4 steps)"), "repeat: %s", res);
    CHECK(macro_run(m, mcp, "ping", "{}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "nested too deep"), "call cycle: %s", res);
    CHECK(macro_run(m, mcp, "badbutton", "{}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "No button"), "bad button: %s", res);

    /* fingerprints: record two screens from the fake, then check macros against them */
    {
        uint8_t ta[FP_BYTES], tb[FP_BYTES]; char ha[FP_BYTES * 2 + 1], hb[FP_BYTES * 2 + 1], js[6000], e2[100], tx[200];
        mcp_call(mcp, "tap", "{\"x\":10,\"y\":10,\"screenshot\":\"none\"}", tx, sizeof tx, NULL);
        fp_capture(mcp, 100, 100, 400, 300, ta, e2, sizeof e2);
        mcp_call(mcp, "tap", "{\"x\":640,\"y\":400,\"screenshot\":\"none\"}", tx, sizeof tx, NULL);
        fp_capture(mcp, 100, 100, 400, 300, tb, e2, sizeof e2);
        mcp_call(mcp, "tap", "{\"x\":10,\"y\":10,\"screenshot\":\"none\"}", tx, sizeof tx, NULL);
        fp_hex(ta, ha); fp_hex(tb, hb);
        snprintf(js, sizeof js, "{\"version\":1,\"screen\":[1280,800],\"fingerprints\":{"
            "\"pageA\":{\"region\":[100,100,400,300],\"thumb\":\"%s\"},\"pageB\":{\"region\":[100,100,400,300],\"thumb\":\"%s\",\"tolerance\":12}},\"macros\":{"
            "\"go_b\":{\"steps\":[{\"tap\":[640,400]},{\"expect\":\"pageB\"}]},"
            "\"back_a\":{\"steps\":[{\"tap\":[10,10]},{\"expect\":\"pageA\"}]},"
            "\"wrong\":{\"steps\":[{\"tap\":[10,10]},{\"expect\":\"pageB\",\"timeout_ms\":300}]}}}", ha, hb);
        macros_t *fm = macros_parse(js, err, sizeof err); CHECK(fm != NULL, "fingerprint file: %s", err); if (fm) {
            CHECK(macro_run(fm, mcp, "go_b", "{}", NULL, NULL, res, sizeof res) == 0 && strstr(res, "(2 steps)"), "expect passes on the right screen: %s", res);
            CHECK(macro_run(fm, mcp, "back_a", "{}", NULL, NULL, res, sizeof res) == 0, "and on the way back: %s", res);
            CHECK(macro_run(fm, mcp, "wrong", "{}", NULL, NULL, res, sizeof res) == -1 && strstr(res, "does not look like \"pageB\"") && strstr(res, "difference"), "expect stops on the wrong screen: %s", res);
            macros_free(fm);
        }
        const char *badfp[][2] = {
            { "\"fingerprints\":[],\"macros\":{\"a\":{\"steps\":[{\"wait\":1}]}}", "must be an object" },
            { "\"fingerprints\":{\"x\":{\"region\":[0,0,2000,100],\"thumb\":\"00\"}},\"macros\":{\"a\":{\"steps\":[{\"wait\":1}]}}", "inside the 1280x800 screen" },
            { "\"fingerprints\":{\"x\":{\"region\":[0,0,10,10],\"thumb\":\"abcd\"}},\"macros\":{\"a\":{\"steps\":[{\"wait\":1}]}}", "hex digits" },
            { "\"fingerprints\":{},\"macros\":{\"a\":{\"steps\":[{\"expect\":\"nope\"}]}}", "unknown fingerprint" },
        };
        for (int i = 0; i < 4; i++) { char j2[1200]; snprintf(j2, sizeof j2, "{\"version\":1,\"screen\":[1280,800],%s}", badfp[i][0]); macros_t *b = macros_parse(j2, err, sizeof err); CHECK(b == NULL && strstr(err, badfp[i][1]), "bad fingerprint file %d: '%s'", i, err); macros_free(b); }
    }

    macros_t *other = macros_parse("{\"version\":1,\"screen\":[1920,1080],\"macros\":{\"a\":{\"steps\":[{\"tap\":[1,1]}]}}}", err, sizeof err);
    CHECK(macro_run(other, mcp, "a", "{}", vision_ok, NULL, res, sizeof res) == -1 && strstr(res, "recorded for a 1920x1080 screen, this one is 1280x800"), "screen mismatch: %s", res);
    macros_free(other); macros_free(m); mcp_close(mcp);

    close(in[1]); char log[8000] = ""; while (fgets(line, sizeof line, so)) strncat(log, line, sizeof log - strlen(log) - 1);
    CHECK(strstr(log, "CALL tap {\"screenshot\":\"none\",\"x\":380,\"y\":300}"), "track 3 cell tapped at 380,300");
    CHECK(strstr(log, "CALL press_button {\"name\":\"MENU\",\"screenshot\":\"none\"}"), "button pressed");
    CHECK(strstr(log, "CALL tap {\"screenshot\":\"none\",\"x\":777,\"y\":333}"), "the found plugin was tapped where the vision step said");
    CHECK(strstr(log, "CALL scroll {\"amount\":2,\"direction\":\"down\",\"screenshot\":\"none\",\"x\":300,\"y\":400}"), "scroll");
    CHECK(strstr(log, "CALL wait_for_screen {\"screenshot\":\"none\",\"timeout_ms\":500,\"until\":\"stable\"}"), "wait for stable");
    CHECK(!strstr(log, "\"x\":2000") && !strstr(log, "\"x\":-"), "off-screen touches never reached the device");
    int taps = 0; for (char *p = log; (p = strstr(p, "CALL tap ")); p++) taps++;
    CHECK(taps >= 8, "taps seen %d", taps);
    fclose(so); waitpid(pid, NULL, 0);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
