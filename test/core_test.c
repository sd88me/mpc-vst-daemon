#include "../src/daemon_core.h"
#include "../src/daemon_json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *REPLY =
 "Here you go:\n```json\n{\"transcript\":\"four bar \\\"chords\\\"\",\"notes\":["
 "{\"pitch\":60,\"start_time\":0.0,\"duration\":1.0,\"velocity\":100,\"mute\":0},"
 "{\"pitch\":64,\"start_time\":0.0,\"duration\":1.0,\"velocity\":300},"          /* velocity clamps to 127 */
 "{\"pitch\":200,\"start_time\":1.0,\"duration\":1.0,\"velocity\":90},"           /* dropped: pitch */
 "{\"pitch\":62,\"start_time\":-1,\"duration\":1.0,\"velocity\":90},"             /* dropped: negative start */
 "{\"pitch\":62,\"start_time\":2.0,\"duration\":0,\"velocity\":90},"              /* dropped: no length */
 "{\"pitch\":65,\"start_time\":1.5,\"duration\":9.0,\"velocity\":0},"             /* cut at 4 beats, velocity 1 */
 "{\"pitch\":67,\"start_time\":9.0,\"duration\":1.0,\"velocity\":90},"            /* dropped: past the end */
 "{\"pitch\":69,\"start_time\":3.0,\"duration\":0.5,\"velocity\":90,\"mute\":1}]," /* dropped: muted */
 "\"feedback\":{\"result\":\"Plucky.\",\"clip_name\":\"faxing berlin!\"}} // trailing\n```";

typedef struct { int on, off, active[128], stuck; } cnt_t;
static void emit(void *u, int on, int pitch, int vel) {
    cnt_t *c = u;
    if (on) { c->on++; c->active[pitch]++; if (vel < 1 || vel > 127) c->stuck = 1; }
    else { c->off++; c->active[pitch]--; if (c->active[pitch] < 0) c->stuck = 1; }
}
static int sounding(cnt_t *c) { int n = 0; for (int i = 0; i < 128; i++) n += c->active[i]; return n; }

int main(void) {
    static dpart_t part;
    int n = daemon_parse_reply(REPLY, 4.0f, &part);
    CHECK(n == 3, "kept %d notes, want 3", n);
    CHECK(part.notes[1].vel == 127, "velocity clamp %d", part.notes[1].vel);
    CHECK(part.notes[2].vel == 1 && fabsf(part.notes[2].dur - 2.5f) < 1e-4, "cut note vel %d dur %f", part.notes[2].vel, part.notes[2].dur);
    CHECK(part.loop_beats == 4, "loop %f", part.loop_beats);
    CHECK(!strcmp(part.clip_name, "faxing_berlin_"), "clip name '%s'", part.clip_name);
    CHECK(!strcmp(part.transcript, "four bar \"chords\""), "transcript '%s'", part.transcript);
    CHECK(daemon_parse_reply("no json here", 4, &part) == -1, "text with no object");
    CHECK(daemon_parse_reply("{\"transcript\":\"x\"}", 4, &part) == -1, "object without notes");
    CHECK(daemon_parse_reply("{\"notes\":[{\"pitch\":60,", 4, &part) == -1, "truncated reply");
    CHECK(daemon_parse_reply("{\"notes\":[]}", 4, &part) == 0, "empty notes array is 0, not an error");
    n = daemon_parse_reply(REPLY, 4.0f, &part);

    /* playback: 124 BPM, 128-frame blocks of 44.1 kHz, block-misaligned to the 4-beat loop, 5 loops from song start */
    double ppb = 124.0 / 60.0 * 128.0 / 44100.0, ppq = 0;
    dplayer_t pl; cnt_t c; memset(&c, 0, sizeof c); daemon_player_init(&pl, &part);
    long blocks = 0;
    while (ppq < 20.0 - 1e-9) { daemon_feed(&pl, ppq, ppq + ppb, emit, &c); ppq += ppb; blocks++; }
    int want = 0; for (int i = 0; i < part.n; i++) for (double x = part.notes[i].start; x < ppq; x += 4) want++;   /* every pass that starts before the last block's end */
    CHECK(c.on == want, "note-ons over %.3f beats: %d want %d", ppq, c.on, want);
    daemon_player_stop(&pl, emit, &c);
    CHECK(c.on == c.off && sounding(&c) == 0 && !c.stuck, "ons %d offs %d sounding %d stuck %d", c.on, c.off, sounding(&c), c.stuck);

    /* mid-song start: bar 3 beat 2 (ppq 9.0): first boundary after the playhead is not skipped, nothing doubled */
    memset(&c, 0, sizeof c); daemon_player_init(&pl, &part); ppq = 9.0;
    for (int i = 0; i < 2000; i++) { daemon_feed(&pl, ppq, ppq + ppb, emit, &c); ppq += ppb; }
    double played = 2000 * ppb;                         /* beats covered */
    int expect = 0; for (int i = 0; i < part.n; i++) for (double t = part.notes[i].start; t < 9.0 + played + 4; t += 4) if (t >= 9.0 && t < 9.0 + played) expect++;
    CHECK(c.on == expect, "mid-song start: %d note-ons, want %d", c.on, expect);

    /* a locate silences what was sounding and resumes without a stuck note */
    daemon_feed(&pl, 100.0, 100.0 + ppb, emit, &c);
    CHECK(sounding(&c) == 0, "after a locate %d notes still sounding", sounding(&c));
    /* tempo jitter: ppqPos stepping back a little must not fire a note twice */
    memset(&c, 0, sizeof c); daemon_player_init(&pl, &part); ppq = 0; int jit = 0;
    for (int i = 0; i < 400; i++) { daemon_feed(&pl, ppq, ppq + ppb, emit, &c); ppq += ppb; if (i % 50 == 49) { ppq -= 0.001; jit++; } }
    CHECK(!c.stuck, "stuck note after jitter");

    /* SMF */
    static uint8_t smf[8192];
    int len = daemon_write_smf(&part, 1, 124, smf, sizeof smf);
    CHECK(len > 40 && !memcmp(smf, "MThd", 4) && !memcmp(smf + 14, "MTrk", 4), "smf header len %d", len);
    uint32_t tl = smf[18] << 24 | smf[19] << 16 | smf[20] << 8 | smf[21];
    CHECK((int)tl == len - 22 && smf[len - 3] == 0xff && smf[len - 2] == 0x2f, "smf track length %u vs %d", tl, len - 22);
    FILE *f = fopen("/tmp/daemon_test.mid", "wb"); fwrite(smf, 1, len, f); fclose(f);
    CHECK(daemon_write_smf(&part, 0, 120, smf, 16) == -1, "small buffer must fail");

    /* request body */
    static char req[1 << 18]; static uint8_t wav[3001];
    for (int i = 0; i < 3001; i++) wav[i] = (uint8_t)(i * 7);
    dreq_t rq = { "Chords", "F Minor", "gm", 124, 4 };
    int rl = daemon_build_request(&rq, "the \"feel\" of faxing berlin\nplease", wav, sizeof wav, req, sizeof req);
    CHECK(rl > 500 && rl == (int)strlen(req), "request len %d", rl);
    f = fopen("/tmp/daemon_req.json", "w"); fputs(req, f); fclose(f);
    CHECK(daemon_build_request(&rq, "x", wav, sizeof wav, req, 400) == -1, "small request buffer must fail");
    rq.type = "Beats"; rl = daemon_build_request(&rq, NULL, NULL, 0, req, sizeof req);
    CHECK(rl > 0 && strstr(req, "General MIDI drum notes"), "beats request");

    /* Gemini envelope */
    char txt[512];
    const char *env = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"{\\\"notes\\\":[]}\"}],\"role\":\"model\"},\"finishReason\":\"STOP\"}]}";
    CHECK(daemon_extract_text(env, txt, sizeof txt) == 12 && !strcmp(txt, "{\"notes\":[]}"), "envelope text '%s'", txt);
    CHECK(daemon_extract_text("{\"error\":{\"code\":404,\"message\":\"model gone\"}}", txt, sizeof txt) == -1 && !strcmp(txt, "model gone"), "api error '%s'", txt);
    CHECK(daemon_extract_text("<html>", txt, sizeof txt) == -1, "not json");
    CHECK(daemon_extract_text("{\"candidates\":[{\"finishReason\":\"SAFETY\"}]}", txt, sizeof txt) == -1 && !strcmp(txt, "no text in reply"), "blocked reply '%s'", txt);

    /* capture: 1 s of 44.1 kHz stereo 440 Hz in 128-frame blocks -> about 16000 samples at 16 kHz, same pitch */
    static dcap_t cap; daemon_cap_reset(&cap);
    for (int blk = 0; blk < 44100 / 128; blk++) {
        int16_t in[256];
        for (int i = 0; i < 128; i++) { double t = (blk * 128 + i) / 44100.0; in[2 * i] = in[2 * i + 1] = (int16_t)(8000 * sin(6.283185307 * 440 * t)); }
        daemon_cap_push(&cap, in, 128);
    }
    double secs = (44100 / 128) * 128 / 44100.0;
    CHECK(abs(cap.n - (int)lround(secs * 16000)) <= 2, "capture length %d for %.4f s", cap.n, secs);
    int zc = 0; for (int i = 1; i < cap.n; i++) if ((cap.pcm[i - 1] < 0) != (cap.pcm[i] < 0)) zc++;
    CHECK(fabs(zc / 2.0 / secs - 440) < 4, "capture pitch %.1f Hz", zc / 2.0 / secs);
    CHECK(daemon_cap_peak(&cap) > 7500 && daemon_cap_peak(&cap) < 8100, "capture peak %d", daemon_cap_peak(&cap));
    static uint8_t wavb[DAEMON_CAP_MAX * 2 + 64];
    int wl = daemon_cap_wav(&cap, wavb, sizeof wavb);
    CHECK(wl == 44 + 2 * cap.n && !memcmp(wavb, "RIFF", 4) && !memcmp(wavb + 36, "data", 4), "wav %d", wl);
    CHECK(daemon_cap_wav(&cap, wavb, 40) == -1, "wav into a small buffer");
    daemon_cap_reset(&cap); for (int k = 0; k < 8000; k++) { int16_t z[256] = {0}; daemon_cap_push(&cap, z, 128); }
    CHECK(cap.n == DAEMON_CAP_MAX, "capture caps at %d (got %d)", DAEMON_CAP_MAX, cap.n);

    /* JSON module: parse, access, write, round trip, rejects */
    {
        const char *src = "{\"a\":[1,2.5,-3e2,true,null,\"x\\ny\\\"q\\\"\"],\"o\":{\"k\":\"v\",\"n\":7}}";
        jv *v = djson_parse(src, strlen(src));
        CHECK(v && djson_len(djson_get(v, "a")) == 6 && djson_num(djson_get(v, "o"), "n", 0) == 7 && !strcmp(djson_str(djson_get(v, "o"), "k", ""), "v"), "json access");
        CHECK(!strcmp(djson_at(djson_get(v, "a"), 5)->str, "x\ny\"q\"") && djson_at(djson_get(v, "a"), 6) == NULL, "json string escapes");
        char out[256]; int n = djson_write(v, out, sizeof out);
        CHECK(n > 0 && !strcmp(out, "{\"a\":[1,2.5,-300,true,null,\"x\\ny\\\"q\\\"\"],\"o\":{\"k\":\"v\",\"n\":7}}"), "json write '%s'", out);
        { char tiny[10]; CHECK(djson_write(v, tiny, sizeof tiny) == -1, "json write into a small buffer"); }
        jv *w = djson_parse(out, strlen(out)); char out2[256]; djson_write(w, out2, sizeof out2);
        CHECK(!strcmp(out, out2), "json round trip");
        djson_free(w); djson_free(v);
        const char *bad[] = { "", "{", "{\"a\":}", "[1,2", "{\"a\":1} x", "\"unterminated", "nul" };
        for (int i = 0; i < 7; i++) { jv *b = djson_parse(bad[i], strlen(bad[i])); CHECK(b == NULL, "rejects '%s'", bad[i]); djson_free(b); }
        char ctl[] = "{\"s\":\"a\\u0001b\"}"; jv *c = djson_parse(ctl, strlen(ctl)); char o3[64]; djson_write(c, o3, sizeof o3);
        CHECK(c != NULL, "control escape parses"); djson_free(c);
    }
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
