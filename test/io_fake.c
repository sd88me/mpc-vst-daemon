/* daemon_io.h for tests: records MIDI, answers HTTP from a script. */
#include "io_fake.h"
#include "daemon_io.h"
#include <stdio.h>
#include <string.h>

fake_t fake;
static void *mo(const char *c, const char *p) { (void)c; (void)p; return &fake; }
static void ms(void *h, int ch, int on, int pitch, int vel) {
    (void)h; if (fake.nev < FAKE_EV) { fake.ev[fake.nev].ch = ch; fake.ev[fake.nev].on = on; fake.ev[fake.nev].pitch = pitch; fake.ev[fake.nev].vel = vel; fake.nev++; }
}
static void mc(void *h) { (void)h; fake.closed++; }
static int hp(const char *url, const char *key, const char *body, size_t len, char *resp, size_t cap, int t) {
    (void)t; fake.posts++;
    snprintf(fake.url, sizeof fake.url, "%s", url); snprintf(fake.key, sizeof fake.key, "%s", key);
    size_t n = len < sizeof fake.body - 1 ? len : sizeof fake.body - 1; memcpy(fake.body, body, n); fake.body[n] = 0;
    if (fake.nq > 0 && fake.qi < fake.nq) snprintf(resp, cap, "%s", fake.queue[fake.qi++]); else snprintf(resp, cap, "%s", fake.resp);
    return fake.code;
}
static const daemon_io_t IO = { mo, ms, mc, hp };
const daemon_io_t *daemon_io(void) { return &IO; }
