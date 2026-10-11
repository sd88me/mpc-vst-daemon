/* The engine end to end with a fake network and MIDI port: talk -> capture -> request -> reply -> parts played from song_pos. */
#include "engine.h"
#include "io_fake.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const mpc_engine_t *E;
static char *get(void *i, const char *k) { static char b[600]; b[0] = 0; E->get_param(i, k, b, sizeof b); return b; }
static void wait_not_thinking(void *i) {
    for (int t = 0; t < 400; t++) { if (strncmp(get(i, "status"), "THINKING", 8)) return; usleep(10000); }
    printf("timeout waiting for the worker\n");
}
static void speak(void *i, int loud) {
    int16_t in[256], out[256];
    E->set_param(i, "talk", "1");
    for (int b = 0; b < 600; b++) {
        for (int f = 0; f < 128; f++) in[2 * f] = in[2 * f + 1] = loud ? (int16_t)(9000 * sin(6.2832 * 200 * (b * 128 + f) / 44100.0)) : 0;
        E->process(i, in, out, 128);
    }
    E->set_param(i, "talk", "1");
}
static const char *OK_REPLY = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"{\\\"transcript\\\":\\\"a dark bassline\\\",\\\"notes\\\":["
    "{\\\"pitch\\\":41,\\\"start_time\\\":0,\\\"duration\\\":0.5,\\\"velocity\\\":100},{\\\"pitch\\\":44,\\\"start_time\\\":1,\\\"duration\\\":0.5,\\\"velocity\\\":90},"
    "{\\\"pitch\\\":48,\\\"start_time\\\":2,\\\"duration\\\":1,\\\"velocity\\\":95}],\\\"feedback\\\":{\\\"result\\\":\\\"Dark and slow.\\\",\\\"clip_name\\\":\\\"dark_bass\\\"}}\"}]}}]}";

int main(void) {
    E = mpc_engine();
    char dir[] = "/tmp/daemon_eng_XXXXXX"; if (!mkdtemp(dir)) return 1;

    /* no key: the first request says so and nothing goes to the network */
    void *a = E->create(dir);
    speak(a, 1);
    CHECK(!strncmp(get(a, "status"), "ERROR no api_key", 16), "no key: '%s'", get(a, "status"));
    CHECK(fake.posts == 0, "posted without a key");
    E->destroy(a);

    char path[300]; snprintf(path, sizeof path, "%s/daemon.conf", dir);
    FILE *f = fopen(path, "w"); fputs("api_key=KEY123\nmodel=test-model\nurl_base=http://x/models\n", f); fclose(f);
    a = E->create(dir);
    CHECK(!strcmp(get(a, "status"), "READY") && !strcmp(get(a, "face"), "Idle"), "start state '%s' face '%s'", get(a, "status"), get(a, "face"));
    E->set_param(a, "talk", "1"); CHECK(!strcmp(get(a, "face"), "Listening"), "face while listening '%s'", get(a, "face"));
    { int16_t z[256] = {0}, o[256]; for (int i = 0; i < 100; i++) E->process(a, z, o, 128); }
    E->set_param(a, "talk", "1"); CHECK(!strcmp(get(a, "face"), "Error"), "face after a silent take '%s'", get(a, "face"));

    /* silence: caught before it costs a request */
    speak(a, 0);
    CHECK(!strncmp(get(a, "status"), "ERROR heard nothing", 19), "silent mic: '%s'", get(a, "status"));
    CHECK(fake.posts == 0, "posted silence");

    /* a spoken request */
    snprintf(fake.resp, sizeof fake.resp, "%s", OK_REPLY); fake.code = 200;
    E->set_param(a, "type", "Melody"); E->set_param(a, "key", "A"); E->set_param(a, "scale", "Dorian"); E->set_param(a, "bars", "2");
    E->song_pos(a, 0, 100, 0, 128);
    speak(a, 1); wait_not_thinking(a);
    CHECK(!strcmp(get(a, "status"), "DONE 3 NOTES") && !strcmp(get(a, "face"), "Done"), "after reply: '%s' face '%s'", get(a, "status"), get(a, "face"));
    CHECK(!strcmp(fake.url, "http://x/models/test-model:generateContent") && !strcmp(fake.key, "KEY123"), "url '%s' key '%s'", fake.url, fake.key);
    CHECK(strstr(fake.body, "audio/wav") && strstr(fake.body, "A Dorian") && strstr(fake.body, "Length: 8 beats") && strstr(fake.body, "Part type: Melody"), "request body: %.200s", fake.body);
    CHECK(strstr(fake.body, "Tempo 100 BPM"), "tempo from the host not in the request");
    CHECK(!strcmp(get(a, "transcript"), "a dark bassline") && !strcmp(get(a, "reply"), "Dark and slow."), "transcript '%s' reply '%s'", get(a, "transcript"), get(a, "reply"));
    int rev = atoi(get(a, "display_rev")); CHECK(rev >= 4, "display_rev %d", rev);

    /* playing: 3 notes over a 3-beat part, two passes of the loop, MIDI on channel 2 (index 1), nothing doubled, all ended */
    double ppb = 100 / 60.0 * 128 / 44100.0, ppq = 0; fake.nev = 0;
    while (ppq < 5.9) { E->song_pos(a, ppq, 100, 1, 128); ppq += ppb; }
    int on = 0, off = 0, badch = 0; for (int i = 0; i < fake.nev; i++) { on += fake.ev[i].on; off += !fake.ev[i].on; badch += fake.ev[i].ch != 1; }
    CHECK(on == 6 && off >= 5 && !badch, "6 note-ons over two passes: on %d off %d badch %d", on, off, badch);
    E->song_pos(a, ppq, 100, 0, 128);                    /* transport stops: whatever is sounding ends */
    on = off = 0; for (int i = 0; i < fake.nev; i++) { on += fake.ev[i].on; off += !fake.ev[i].on; }
    CHECK(on == off, "after stop: on %d off %d", on, off);

    /* regen reuses the audio; a typed prompt goes out as text */
    int p0 = fake.posts; E->set_param(a, "regen", "1"); wait_not_thinking(a);
    CHECK(fake.posts == p0 + 1 && strstr(fake.body, "audio/wav"), "regen posts the same audio again (posts %d)", fake.posts - p0);
    p0 = fake.posts; E->set_param(a, "prompt", "four on the floor"); wait_not_thinking(a);
    CHECK(fake.posts == p0 + 1 && !strstr(fake.body, "audio/wav") && strstr(fake.body, "Request: four on the floor"), "typed prompt body: %.120s", fake.body);

    /* the project chunk round-trips the settings */
    E->set_param(a, "type", "Beats"); E->set_param(a, "key", "D#"); E->set_param(a, "scale", "Blues"); E->set_param(a, "bars", "7"); E->set_param(a, "channel", "10");
    char st[64]; snprintf(st, sizeof st, "%s", get(a, "state"));
    void *b2 = E->create(dir); E->set_param(b2, "state", st);
    CHECK(!strcmp(get(b2, "state"), st) && !strcmp(get(b2, "type"), "Beats") && !strcmp(get(b2, "key"), "D#") && !strcmp(get(b2, "scale"), "Blues") && !strcmp(get(b2, "bars"), "7"), "state '%s' -> '%s'", st, get(b2, "state"));
    E->set_param(b2, "state", "garbage"); CHECK(!strcmp(get(b2, "type"), "Beats"), "a bad chunk leaves the settings alone");
    E->destroy(b2);
    E->set_param(a, "type", "Melody"); E->set_param(a, "key", "A"); E->set_param(a, "scale", "Dorian"); E->set_param(a, "bars", "2"); E->set_param(a, "channel", "2");

    /* .mid on request */
    E->set_param(a, "save", "1");
    char mid[400]; snprintf(mid, sizeof mid, "%s/clips/dark_bass.mid", dir);
    FILE *m = fopen(mid, "rb"); char hd[4] = {0}; if (m) { if (fread(hd, 1, 4, m) != 4) hd[0] = 0; fclose(m); }
    CHECK(!memcmp(hd, "MThd", 4), "saved %s", mid);
    CHECK(!strncmp(get(a, "status"), "SAVED dark_bass", 15), "save status '%s'", get(a, "status"));

    /* failures say what happened and keep the old part */
    fake.code = 404; snprintf(fake.resp, sizeof fake.resp, "{\"error\":{\"message\":\"model gone\"}}");
    E->set_param(a, "regen", "1"); wait_not_thinking(a);
    CHECK(!strcmp(get(a, "status"), "ERROR 404 model gone"), "404: '%s'", get(a, "status"));
    fake.code = 0; snprintf(fake.resp, sizeof fake.resp, "Could not resolve host");
    E->set_param(a, "regen", "1"); wait_not_thinking(a);
    CHECK(!strncmp(get(a, "status"), "ERROR no network", 16), "offline: '%s'", get(a, "status"));
    fake.code = 200; snprintf(fake.resp, sizeof fake.resp, "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"sorry\"}]}}]}");
    E->set_param(a, "regen", "1"); wait_not_thinking(a);
    CHECK(!strcmp(get(a, "status"), "ERROR reply not usable"), "junk reply: '%s'", get(a, "status"));
    CHECK(!strcmp(get(a, "transcript"), "four on the floor") || get(a, "transcript")[0], "old part kept");
    ppq = 0; fake.nev = 0; for (int k = 0; k < 400; k++) { E->song_pos(a, ppq, 100, 1, 128); ppq += ppb; }
    CHECK(fake.nev > 0, "the earlier part still plays after a failed request");

    E->destroy(a);
    CHECK(fake.closed == 3, "MIDI port closed once per instance: %d", fake.closed);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
