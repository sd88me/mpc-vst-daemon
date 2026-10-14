/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Daemon Listen engine: audio passes through unchanged and a copy reaches a reader of the shared ring. */
#include "engine.h"
#include <stdlib.h>
#include <unistd.h>
#include "../src/daemon_tap.h"
#include <stdio.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main(void) {
    char tp[] = "/tmp/daemon_tap_t_XXXXXX"; int f = mkstemp(tp); if (f < 0) return 1; close(f); setenv("DAEMON_TAP_PATH", tp, 1);
    const mpc_engine_t *E = mpc_engine(); void *t = E->create(NULL);
    const tap_ring_t *r = tap_attach(); CHECK(r != NULL, "attach after the tap created the ring"); if (!r) return 1;
    uint64_t rp = tap_pos(r);
    int16_t in[256], out[256], got[256 * 4];
    for (int b = 0; b < 3; b++) { for (int i = 0; i < 256; i++) in[i] = (int16_t)(b * 1000 + i); E->process(t, in, out, 128); CHECK(!memcmp(in, out, sizeof in), "passes through"); }
    int n = tap_read(r, &rp, got, 1000);
    CHECK(n == 384 && got[0] == 0 && got[255] == 255 && got[256] == 1000 && got[767] == 2255, "read %d frames: %d %d %d %d", n, got[0], got[255], got[256], got[767]);
    for (int b = 0; b < 400; b++) E->process(t, in, out, 128);           /* a reader 51200 frames behind skips ahead to the newest 32768 */
    n = tap_read(r, &rp, got, 128); CHECK(n == 128, "lapped reader still reads: %d", n);
    CHECK(tap_pos(r) - rp <= TAP_FRAMES, "reader inside the ring");
    char s[100]; E->get_param(t, "status", s, sizeof s); CHECK(!strncmp(s, "FEEDING DAEMON", 14), "status '%s'", s);
    E->destroy(t); unlink(tp);
    puts(fails ? "FAILED" : "PASSED"); return fails != 0;
}
