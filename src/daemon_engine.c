/* Daemon engine for the shared wrapper (an effect: the mic comes in as the track's audio, a MIDI part goes out on its own port).
 * talk: capture the mic, a second press sends it. regen: ask again (typed prompt if there is one, else the same audio).
 * Network and parsing run on a worker thread; process() and song_pos() never block on it for longer than a short mutex. */
#define _GNU_SOURCE
#include "engine.h"
#include "daemon_core.h"
#include "daemon_io.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { S_READY, S_LISTENING, S_THINKING, S_DONE, S_ERROR };
static const char *TYPES[] = { "Monophony", "Melody", "Chords", "Melody+Chords", "Beats" };
static const char *KEYS[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *SCALES[] = { "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Harmonic Minor", "Minor Pentatonic", "Blues" };
#define N(a) ((int)(sizeof a / sizeof *a))

typedef struct {
    pthread_mutex_t mu; pthread_cond_t cv; pthread_t th;
    int th_up, quit, pending, with_audio;
    const daemon_io_t *io; void *midi;
    char dir[256], key[128], model[64], url_base[160];
    int state, rev;
    char status[96], transcript[256], reply[512];
    int type, keyi, scale, bars, channel, play;
    char typed[256];
    dcap_t cap; int have_audio;
    dpart_t part; int have_part, part_gen;     /* part_gen counts replacements so the player restarts on a new part */
    dplayer_t pl; int pl_gen; double bpm;
} dm_t;

static void set_status(dm_t *d, int st, const char *fmt, const char *a) {
    d->state = st; snprintf(d->status, sizeof d->status, fmt, a ? a : ""); d->rev++;
}

static void conf_load(dm_t *d) {
    snprintf(d->model, sizeof d->model, "gemini-3.8-flash");
    snprintf(d->url_base, sizeof d->url_base, "https://generativelanguage.googleapis.com/v1beta/models");
    char path[300]; snprintf(path, sizeof path, "%s/daemon.conf", d->dir);
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char *eq = strchr(line, '='); if (!eq || line[0] == '#') continue;
        *eq = 0; char *v = eq + 1;
        if (!strcmp(line, "api_key")) snprintf(d->key, sizeof d->key, "%s", v);
        else if (!strcmp(line, "model")) snprintf(d->model, sizeof d->model, "%s", v);
        else if (!strcmp(line, "url_base")) snprintf(d->url_base, sizeof d->url_base, "%s", v);   /* tests point this at a local server */
    }
    fclose(f);
}

static void *worker(void *arg) {
    dm_t *d = arg;
    char *body = malloc(1400000), *resp = malloc(600000), *text = malloc(600000);
    uint8_t *wav = malloc(44 + DAEMON_CAP_MAX * 2);
    dpart_t *np = malloc(sizeof *np);
    pthread_mutex_lock(&d->mu);
    for (;;) {
        while (!d->pending && !d->quit) pthread_cond_wait(&d->cv, &d->mu);
        if (d->quit) break;
        d->pending = 0;
        dreq_t rq = { TYPES[d->type], "", "gm", d->bpm > 0 ? d->bpm : 120, d->bars };
        char scale[48]; snprintf(scale, sizeof scale, "%s %s", KEYS[d->keyi], SCALES[d->scale]); rq.scale = scale;
        char typed[256]; snprintf(typed, sizeof typed, "%s", d->typed);
        int audio = d->with_audio && d->have_audio, wl = 0;
        if (audio) wl = daemon_cap_wav(&d->cap, wav, 44 + DAEMON_CAP_MAX * 2);
        char key[128], url[300]; snprintf(key, sizeof key, "%s", d->key);
        snprintf(url, sizeof url, "%s/%s:generateContent", d->url_base, d->model);
        pthread_mutex_unlock(&d->mu);

        int code = 0, bl = daemon_build_request(&rq, typed, audio ? wav : NULL, audio ? (size_t)wl : 0, body, 1400000);
        if (bl > 0) code = d->io->http_post(url, key, body, (size_t)bl, resp, 600000, 60);
        int ok = 0; char err[96] = "";
        if (bl < 0) snprintf(err, sizeof err, "request too large");
        else if (code == 0) snprintf(err, sizeof err, "no network: %.70s", resp);
        else if (daemon_extract_text(resp, text, 600000) < 0) snprintf(err, sizeof err, "%d %.80s", code, text);
        else if (daemon_parse_reply(text, 32.0f, np)   /* the request may ask for more or fewer bars than the menu: up to 8 */ < 0) snprintf(err, sizeof err, "reply not usable");
        else if (np->n == 0) snprintf(err, sizeof err, "no notes in the reply");
        else ok = 1;

        pthread_mutex_lock(&d->mu);
        if (ok) {
            memcpy(&d->part, np, sizeof *np); d->have_part = 1; d->part_gen++;
            snprintf(d->transcript, sizeof d->transcript, "%s", np->transcript[0] ? np->transcript : typed);
            snprintf(d->reply, sizeof d->reply, "%s", np->feedback);
            char n[24]; snprintf(n, sizeof n, "%d", np->n); set_status(d, S_DONE, "DONE %s NOTES", n);
        } else set_status(d, S_ERROR, "ERROR %s", err);
    }
    pthread_mutex_unlock(&d->mu);
    free(body); free(resp); free(text); free(wav); free(np);
    return NULL;
}

static void request(dm_t *d, int with_audio) {       /* d->mu held */
    if (!d->key[0]) { set_status(d, S_ERROR, "ERROR %s", "no api_key in daemon.conf"); return; }
    d->with_audio = with_audio; d->pending = 1; set_status(d, S_THINKING, "THINKING%s", ""); pthread_cond_signal(&d->cv);
}

static void *create(const char *data_dir) {
    dm_t *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->mu, NULL); pthread_cond_init(&d->cv, NULL);
    snprintf(d->dir, sizeof d->dir, "%s", data_dir ? data_dir : ".");
    d->io = daemon_io(); d->type = 2; d->keyi = 5; d->scale = 1; d->bars = 4; d->channel = 2; d->play = 1;
    conf_load(d);
    snprintf(d->status, sizeof d->status, "READY"); 
    d->midi = d->io->midi_open("Daemon", "MIDI Out");
    pthread_create(&d->th, NULL, worker, d); d->th_up = 1;
    return d;
}

static void destroy(void *inst) {
    dm_t *d = inst;
    pthread_mutex_lock(&d->mu); d->quit = 1; pthread_cond_signal(&d->cv); pthread_mutex_unlock(&d->mu);
    if (d->th_up) pthread_join(d->th, NULL);     /* a request in flight finishes first (60 s at most) */
    if (d->midi && d->pl.npend) { /* the host stopped sending blocks: end any sounding notes */ for (int i = 0; i < d->pl.npend; i++) d->io->midi_send(d->midi, d->channel - 1, 0, d->pl.pend[i].pitch, 0); }
    if (d->midi) d->io->midi_close(d->midi);
    pthread_mutex_destroy(&d->mu); pthread_cond_destroy(&d->cv);
    free(d);
}

static void midi_in(void *inst, const uint8_t *m, int n) { (void)inst; (void)m; (void)n; }

static int opt_index(const char **list, int n, const char *v) {
    char *e; long i = strtol(v, &e, 10);
    if (e != v && *e == 0) return (int)(i < 0 ? 0 : i >= n ? n - 1 : i);
    for (int k = 0; k < n; k++) if (!strcmp(list[k], v)) return k;
    return 0;
}

static int save_mid(dm_t *d) {                         /* d->mu held */
    if (!d->have_part) return -1;
    static uint8_t buf[1 << 16];
    int len = daemon_write_smf(&d->part, d->channel - 1, d->bpm, buf, sizeof buf);
    if (len < 0) return -1;
    char dir[300], path[400];
    snprintf(dir, sizeof dir, "%s/clips", d->dir); mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s.mid", dir, d->part.clip_name);
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    size_t w = fwrite(buf, 1, (size_t)len, f); fclose(f);
    return w == (size_t)len ? 0 : -1;
}

static void set_param(void *inst, const char *k, const char *v) {
    dm_t *d = inst; int on = atof(v) > 0.5;
    pthread_mutex_lock(&d->mu);
    if (!strcmp(k, "talk") && on) {
        if (d->state == S_LISTENING) {
            if (daemon_cap_peak(&d->cap) < 300) set_status(d, S_ERROR, "ERROR %s", "heard nothing: route the mic to this track");
            else { d->have_audio = 1; d->typed[0] = 0; request(d, 1); }
        } else if (d->state != S_THINKING) { daemon_cap_reset(&d->cap); d->have_audio = 0; set_status(d, S_LISTENING, "LISTENING%s", ""); }
    } else if (!strcmp(k, "regen") && on) {
        if (d->state != S_THINKING && d->state != S_LISTENING) {
            if (d->typed[0] || d->have_audio) request(d, d->typed[0] ? 0 : 1);
            else set_status(d, S_ERROR, "ERROR %s", "nothing to repeat: press TALK first");
        }
    } else if (!strcmp(k, "save") && on) {
        if (save_mid(d) == 0) { set_status(d, d->state, "SAVED %s", d->part.clip_name); } else set_status(d, S_ERROR, "ERROR %s", "nothing to save");
    } else if (!strcmp(k, "prompt")) { snprintf(d->typed, sizeof d->typed, "%s", v); d->have_audio = d->have_audio && !d->typed[0]; if (d->typed[0]) request(d, 0); }
    else if (!strcmp(k, "type")) d->type = opt_index(TYPES, N(TYPES), v);
    else if (!strcmp(k, "key")) d->keyi = opt_index(KEYS, N(KEYS), v);
    else if (!strcmp(k, "scale")) d->scale = opt_index(SCALES, N(SCALES), v);
    else if (!strcmp(k, "bars")) { d->bars = atoi(v); if (d->bars < 1) d->bars = 1; if (d->bars > 8) d->bars = 8; }
    else if (!strcmp(k, "channel")) { d->channel = atoi(v); if (d->channel < 1) d->channel = 1; if (d->channel > 16) d->channel = 16; }
    else if (!strcmp(k, "state")) {                      /* the project chunk: "type,key,scale,bars,channel,play" */
        int t, ky, sc, b, c, pl;
        if (sscanf(v, "%d,%d,%d,%d,%d,%d", &t, &ky, &sc, &b, &c, &pl) == 6 && t >= 0 && t < N(TYPES) && ky >= 0 && ky < N(KEYS) && sc >= 0 && sc < N(SCALES)) {
            d->type = t; d->keyi = ky; d->scale = sc; d->bars = b < 1 ? 1 : b > 8 ? 8 : b; d->channel = c < 1 ? 1 : c > 16 ? 16 : c; d->play = pl != 0; d->pl_gen = -1;
        }
    }
    else if (!strcmp(k, "play")) { d->play = on || !strcmp(v, "Play"); d->pl_gen = -1; }
    pthread_mutex_unlock(&d->mu);
}

static int get_param(void *inst, const char *k, char *b, int n) {
    dm_t *d = inst; int r = 0;
    pthread_mutex_lock(&d->mu);
    if (!strcmp(k, "status")) r = snprintf(b, n, "%s", d->status);
    else if (!strcmp(k, "transcript")) r = snprintf(b, n, "%s", d->transcript[0] ? d->transcript : "-");
    else if (!strcmp(k, "reply")) r = snprintf(b, n, "%s", d->reply[0] ? d->reply : "-");
    else if (!strcmp(k, "display_rev")) r = snprintf(b, n, "%d", d->rev);
    else if (!strcmp(k, "type")) r = snprintf(b, n, "%s", TYPES[d->type]);
    else if (!strcmp(k, "key")) r = snprintf(b, n, "%s", KEYS[d->keyi]);
    else if (!strcmp(k, "scale")) r = snprintf(b, n, "%s", SCALES[d->scale]);
    else if (!strcmp(k, "bars")) r = snprintf(b, n, "%d", d->bars);
    else if (!strcmp(k, "channel")) r = snprintf(b, n, "%d", d->channel);
    else if (!strcmp(k, "play")) r = snprintf(b, n, "%s", d->play ? "Play" : "Stop");
    else if (!strcmp(k, "state")) r = snprintf(b, n, "%d,%d,%d,%d,%d,%d", d->type, d->keyi, d->scale, d->bars, d->channel, d->play);
    pthread_mutex_unlock(&d->mu);
    return r;
}

static void render(void *inst, int16_t *o, int f) { (void)inst; memset(o, 0, (size_t)f * 4); }

static void process(void *inst, const int16_t *in, int16_t *out, int frames) {
    dm_t *d = inst;
    memset(out, 0, (size_t)frames * 4);                 /* a generator, not an audio effect: the mic is not passed on */
    if (pthread_mutex_trylock(&d->mu)) return;          /* never wait in the audio thread: skip this block of capture */
    if (d->state == S_LISTENING) {
        daemon_cap_push(&d->cap, in, frames);
        if (d->cap.n >= DAEMON_CAP_MAX) { d->have_audio = 1; d->typed[0] = 0; request(d, 1); }   /* 20 s: send what there is */
    }
    pthread_mutex_unlock(&d->mu);
}

static void emit(void *u, int on, int pitch, int vel) {
    dm_t *d = u;
    if (d->midi) d->io->midi_send(d->midi, d->channel - 1, on, pitch, vel);
}

static void song_pos(void *inst, double ppq, double bpm, int playing, int frames) {
    dm_t *d = inst;
    if (pthread_mutex_trylock(&d->mu)) return;
    if (bpm > 0) d->bpm = bpm;
    if (d->pl_gen != d->part_gen || d->pl.part != &d->part) {          /* a new part (or play toggled): silence the old one */
        daemon_player_stop(&d->pl, emit, d);
        daemon_player_init(&d->pl, &d->part); d->pl_gen = d->part_gen;
    }
    if (!d->have_part || !d->play || !playing || ppq < 0 || bpm <= 0) daemon_player_stop(&d->pl, emit, d);
    else daemon_feed(&d->pl, ppq, ppq + frames * bpm / 60.0 / 44100.0, emit, d);
    pthread_mutex_unlock(&d->mu);
}

static const mpc_engine_t ENGINE = { create, destroy, midi_in, set_param, get_param, render, process, song_pos };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
