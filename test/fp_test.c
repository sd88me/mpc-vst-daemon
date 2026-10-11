/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#include "../src/daemon_fp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint8_t *slurp(const char *dir, const char *name, size_t *n) {
    char p[300]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    uint8_t *b = malloc(1 << 20); *n = fread(b, 1, 1 << 20, f); fclose(f); return b;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/daemon_fp_fixtures";
    uint8_t buf[64];
    CHECK(fp_b64_decode("QUJD", buf, sizeof buf) == 3 && !memcmp(buf, "ABC", 3), "b64 plain");
    CHECK(fp_b64_decode("QUI=", buf, sizeof buf) == 2 && !memcmp(buf, "AB", 2), "b64 padding");
    CHECK(fp_b64_decode("QU\nJD", buf, sizeof buf) == 3, "b64 skips newlines");
    CHECK(fp_b64_decode("QU!D", buf, sizeof buf) == -1, "b64 bad character");
    CHECK(fp_b64_decode("QUJDRA==", buf, 2) == -1, "b64 into a small buffer");

    char err[120]; uint8_t *rgb; int w, h; size_t n;
    const char *imgs[] = { "grad_rgb", "grad_rgba" };
    for (int i = 0; i < 2; i++) {
        char pn[80], rn[80]; snprintf(pn, sizeof pn, "%s.png", imgs[i]); snprintf(rn, sizeof rn, "%s.raw", imgs[i]);
        size_t pl, rl; uint8_t *png = slurp(dir, pn, &pl), *want = slurp(dir, rn, &rl);
        CHECK(png && want, "fixture %s missing", imgs[i]); if (!png || !want) return 1;
        CHECK(fp_png_decode(png, pl, &rgb, &w, &h, err, sizeof err) == 0 && w == 64 && h == 48 && rl == (size_t)w * h * 3 && !memcmp(rgb, want, rl), "decode %s (all five filter types): %s", imgs[i], err);
        free(rgb); free(png); free(want);
    }
    uint8_t *png = slurp(dir, "grad_rgb.png", &n);
    CHECK(fp_png_decode(png, 20, &rgb, &w, &h, err, sizeof err) == -1 && strstr(err, "not a PNG"), "too short: '%s'", err);
    CHECK(fp_png_decode((const uint8_t *)"GIF89a-not-a-png-at-all-not-a-png-at-all", 40, &rgb, &w, &h, err, sizeof err) == -1, "not a PNG");
    png[n - 30] ^= 0xff; png[n / 2] ^= 0xff;
    int rc = fp_png_decode(png, n, &rgb, &w, &h, err, sizeof err); CHECK(rc == -1 && strstr(err, "damaged"), "damaged data: '%s'", err); if (!rc) free(rgb);
    free(png);
    png = slurp(dir, "interlaced.png", &n); CHECK(fp_png_decode(png, n, &rgb, &w, &h, err, sizeof err) == -1 && strstr(err, "unsupported"), "interlaced refused: '%s'", err); free(png);
    png = slurp(dir, "depth16.png", &n); CHECK(fp_png_decode(png, n, &rgb, &w, &h, err, sizeof err) == -1 && strstr(err, "unsupported"), "16-bit refused: '%s'", err); free(png);

    uint8_t a[FP_BYTES], b[FP_BYTES], c[FP_BYTES];
    #define THUMB(name, out) do { size_t l; uint8_t *p = slurp(dir, name, &l); fp_png_decode(p, l, &rgb, &w, &h, err, sizeof err); fp_thumb(rgb, w, h, out); free(rgb); free(p); } while (0)
    THUMB("half.png", a); THUMB("half_noisy.png", b); THUMB("half_inverted.png", c);
    CHECK(a[0] == 0 && a[FP_W - 1] == 255 && a[FP_BYTES - 1] == 255 && a[FP_BYTES - FP_W] == 0, "thumb layout: %d %d", a[0], a[FP_W - 1]);
    CHECK(fp_distance(a, a) == 0, "same image, distance 0");
    CHECK(fp_distance(a, b) > 0 && fp_distance(a, b) < 12, "noise stays under the default tolerance: %.2f", fp_distance(a, b));
    CHECK(fp_distance(a, c) > 200, "inverted image is far: %.1f", fp_distance(a, c));
    CHECK(fp_distance(a, c) == fp_distance(c, a), "symmetric");
    char hex[FP_BYTES * 2 + 1]; uint8_t back[FP_BYTES];
    fp_hex(a, hex); CHECK(strlen(hex) == FP_BYTES * 2 && fp_unhex(hex, back) == 0 && !memcmp(a, back, FP_BYTES), "hex round trip");
    CHECK(fp_unhex("abc", back) == -1 && fp_unhex("zz", back) == -1, "bad hex rejected");
    hex[10] = 'g'; CHECK(fp_unhex(hex, back) == -1, "non-hex digit rejected");

    /* through the fake remote addin: region screenshots of two different screens */
    int in[2], out[2]; if (pipe(in) || pipe(out)) return 1;
    pid_t pid = fork();
    if (!pid) { dup2(in[0], 0); dup2(out[1], 1); close(in[1]); close(out[0]); execlp("python3", "python3", "-I", "test/fake_remote.py", (char *)NULL); _exit(127); }
    close(in[0]); close(out[1]);
    FILE *so = fdopen(out[0], "r"); char line[300]; if (!fgets(line, sizeof line, so)) return 1;
    mcp_t *m = mcp_open("127.0.0.1", atoi(line), err, sizeof err); CHECK(m != NULL, "mcp: %s", err); if (!m) return 1;
    uint8_t sa[FP_BYTES], sa2[FP_BYTES], sb[FP_BYTES]; char text[300];
    CHECK(fp_capture(m, 100, 100, 400, 300, sa, err, sizeof err) == 0, "capture A: %s", err);
    CHECK(fp_capture(m, 100, 100, 400, 300, sa2, err, sizeof err) == 0 && fp_distance(sa, sa2) == 0, "the same screen twice gives the same fingerprint");
    mcp_call(m, "tap", "{\"x\":640,\"y\":400,\"screenshot\":\"none\"}", text, sizeof text, NULL);
    CHECK(fp_capture(m, 100, 100, 400, 300, sb, err, sizeof err) == 0 && fp_distance(sa, sb) > 40, "another screen is far: %.1f", fp_distance(sa, sb));
    CHECK(fp_capture(m, 1200, 700, 400, 300, sb, err, sizeof err) == 0, "a region past the edge is cut to the screen by the addin");
    mcp_close(m); close(in[1]); fclose(so); waitpid(pid, NULL, 0);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
