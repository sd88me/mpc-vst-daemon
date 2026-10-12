/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* Daemon engine for the shared wrapper (an effect: the mic comes in as the track's audio, a MIDI part goes out on its own port).
 * talk: capture the mic, a second press sends it. regen: ask again (typed prompt if there is one, else the same audio).
 * Network and parsing run on a worker thread; process() and song_pos() never block on it for longer than a short mutex. */
#define _GNU_SOURCE
#include "engine.h"
#include "daemon_core.h"
#include "daemon_io.h"
#include "daemon_kbd.h"
#include "daemon_agent.h"
#include "daemon_json.h"
#include "daemon_macro.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { S_READY, S_LISTENING, S_THINKING, S_DONE, S_ERROR, S_TYPING, S_PLAN };
static const char *TYPES[] = { "Monophony", "Melody", "Chords", "Melody+Chords", "Beats" };
static const char *KEYS[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *SCALES[] = { "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Harmonic Minor", "Minor Pentatonic", "Blues" };
#define N(a) ((int)(sizeof a / sizeof *a))

typedef struct {
    pthread_mutex_t mu; pthread_cond_t cv; pthread_t th, th_drop;
    int th_up, drop_up, quit, pending, with_audio;
    const daemon_io_t *io; void *midi;
    char dir[256], key[128], model[64], url_base[160], input_dir[160]; int input_test;
    int state, rev;
    char status[96], transcript[256], reply[512];
    int type, keyi, scale, bars, channel, play;
    char typed[256];
    dcap_t cap; int have_audio;
    dpart_t part; int have_part, part_gen;     /* part_gen counts replacements so the player restarts on a new part */
    dplayer_t pl; int pl_gen; double bpm;
    kbd_t *kbd; int kbd_devs;
    int agent, mode;                           /* agent: tools on; mode: what the worker was asked for (1 generate, 2 plan, 3 run the plan) */
    aturn_t plan; int plan_ok; char steps[500]; char cmdr_host[48]; int cmdr_port;
    macros_t *macros; char remote_host[48]; int remote_port;
    long sp_calls, sp_busy, sp_playing, n_on, n_off; double last_ppq, last_bpm;    /* diagnostics, written to engine/daemon.log */
} dm_t;

static void set_status(dm_t *d, int st, const char *fmt, const char *a) {
    d->state = st; snprintf(d->status, sizeof d->status, fmt, a ? a : ""); d->rev++;
}

static void conf_load(dm_t *d) {
    snprintf(d->model, sizeof d->model, "gemini-3.8-flash");
    snprintf(d->url_base, sizeof d->url_base, "https://generativelanguage.googleapis.com/v1beta/models");
    snprintf(d->input_dir, sizeof d->input_dir, "/dev/input"); snprintf(d->cmdr_host, sizeof d->cmdr_host, "127.0.0.1"); d->cmdr_port = 6730; snprintf(d->remote_host, sizeof d->remote_host, "127.0.0.1"); d->remote_port = 6720;
    char path[300]; snprintf(path, sizeof path, "%s/daemon.conf", d->dir);
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char *eq = strchr(line, '='); if (!eq || line[0] == '#') continue;
        *eq = 0; char *v = eq + 1;
        if (!strcmp(line, "api_key")) snprintf(d->key, sizeof d->key, "%s", v);
        else if (!strcmp(line, "model")) snprintf(d->model, sizeof d->model, "%s", v);
        else if (!strcmp(line, "input_dir")) { snprintf(d->input_dir, sizeof d->input_dir, "%s", v); d->input_test = 1; }   /* tests: a FIFO dir */
        else if (!strcmp(line, "commander")) { char h[48]; int pt; if (sscanf(v, "%47[^:]:%d", h, &pt) == 2) { snprintf(d->cmdr_host, sizeof d->cmdr_host, "%s", h); d->cmdr_port = pt; } }
        else if (!strcmp(line, "remote")) { char h[48]; int pt; if (sscanf(v, "%47[^:]:%d", h, &pt) == 2) { snprintf(d->remote_host, sizeof d->remote_host, "%s", h); d->remote_port = pt; } }
        else if (!strcmp(line, "url_base")) snprintf(d->url_base, sizeof d->url_base, "%s", v);   /* tests point this at a local server */
    }
    fclose(f);
}


/* ---- agent: plan (read-only tools run now, changes are collected) and run (the changes) ---- */
typedef struct { char typed[256]; int audio, wl; const uint8_t *wav; double bpm; char scale[48]; int bars; char key[128], url[300], host[48]; int port; char host_remote[48]; int port_remote; } snap_t;
typedef struct { dm_t *d; const snap_t *s; mcp_t *mcp; } uictx;       /* what the tool callbacks get */

static int play_part_cb(void *u, const char *args, char *res, size_t cap) {
    dm_t *d = ((uictx *)u)->d; dpart_t *np = malloc(sizeof *np);
    int n = daemon_parse_reply(args, 32.0f, np);
    if (n <= 0) { snprintf(res, cap, "no usable notes"); free(np); return -1; }
    jv *a = djson_parse(args, strlen(args));
    const char *name = a ? djson_str(a, "name", "") : ""; int ch = a ? (int)djson_num(a, "channel", 0) : 0;
    if (name[0]) { snprintf(np->clip_name, sizeof np->clip_name, "%s", name); for (char *c = np->clip_name; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) *c = '_'; }
    pthread_mutex_lock(&d->mu);
    memcpy(&d->part, np, sizeof *np); d->have_part = 1; d->part_gen++;
    if (ch >= 1 && ch <= 16) d->channel = ch;
    pthread_mutex_unlock(&d->mu);
    snprintf(res, cap, "%d notes over %.0f beats on channel %d", n, np->loop_beats, ch >= 1 && ch <= 16 ? ch : d->channel);
    djson_free(a); free(np); return 0;
}


/* the vision step of a tap macro: ask Gemini where `target` is on this screenshot */
static int vision_gemini(void *u, const char *target, const char *hint, const char *png, int w, int h, int *x, int *y, char *why, size_t wc) {
    uictx *c = u; dm_t *d = c->d;
    char *body = malloc(strlen(png) + 2000), *resp = malloc(200000), *text = malloc(200000); int rc = -1;
    int bl = daemon_build_vision_request(target, hint, png, w, h, body, strlen(png) + 2000);
    if (bl < 0) snprintf(why, wc, "screenshot too large");
    else {
        int code = d->io->http_post(c->s->url, c->s->key, body, (size_t)bl, resp, 200000, 60);
        if (code == 0) snprintf(why, wc, "no network: %.100s", resp);
        else if (daemon_extract_text(resp, text, 200000) < 0) snprintf(why, wc, "%d %.100s", code, text);
        else rc = daemon_parse_point(text, w, h, x, y, why, wc);
    }
    free(body); free(resp); free(text); return rc;
}

/* select_track / load_plugin: the macro of the same name, on the remote addin's MCP */
static int ui_cb(void *u, const char *tool, const char *args, char *res, size_t cap) {
    uictx *c = u; dm_t *d = c->d;
    if (!d->macros || !macros_has(d->macros, tool)) { snprintf(res, cap, "no macro %s is recorded on this device", tool); return -1; }
    if (!c->mcp) { char err[120]; c->mcp = mcp_open(c->s->host_remote, c->s->port_remote, err, sizeof err); if (!c->mcp) { snprintf(res, cap, "%s", err); return -1; } }
    return macro_run(d->macros, c->mcp, tool, args, vision_gemini, c, res, cap);
}

static void publish_error(dm_t *d, const char *fmt, const char *a) { pthread_mutex_lock(&d->mu); set_status(d, S_ERROR, fmt, a); pthread_mutex_unlock(&d->mu); }

static void agent_plan(dm_t *d, const snap_t *s, char *body, char *resp) {
    char err[100] = ""; cmdr_t *c = cmdr_open(s->host, s->port, err, sizeof err);     /* NULL: planning still works for parts; tools say why */
    ahist_t h; agent_hist_init(&h);
    char ut[700]; snprintf(ut, sizeof ut, "%s", s->typed[0] ? s->typed : "The request is in the audio. Begin your final text with: Heard: \"...\" and then what the user said.");
    agent_hist_user(&h, ut, s->audio ? s->wav : NULL, s->audio ? (size_t)s->wl : 0);
    actx_t ctx = { s->bpm, s->scale, s->bars, c ? "" : "The commander addin is not reachable, so project and plugin tools will fail; say so.", d->macros != NULL };
    uictx uc = { d, s, NULL };
    char *tool_res = malloc(200000); char *rs[AGENT_MAX_CALLS]; aturn_t t;
    int done = 0;
    for (int round = 0; round < 6 && !done; round++) {
        int bl = agent_build_request(&ctx, &h, body, 1400000);
        if (bl < 0) { publish_error(d, "ERROR %s", "conversation too long"); done = 1; break; }
        { char r[24]; snprintf(r, sizeof r, "%d", round + 1); pthread_mutex_lock(&d->mu); set_status(d, S_THINKING, "THINKING, STEP %s", r); pthread_mutex_unlock(&d->mu); }
        int code = d->io->http_post(s->url, s->key, body, (size_t)bl, resp, 600000, 150);
        if (code == 0) { char m[100]; snprintf(m, sizeof m, "no network: %.70s", resp); publish_error(d, "ERROR %s", m); done = 1; break; }
        if (agent_parse_turn(resp, &t) < 0) { char m[100]; snprintf(m, sizeof m, "%d %.80s", code, t.text); publish_error(d, "ERROR %s", m); done = 1; break; }
        if (t.n == 0) {                                                                   /* an answer, nothing to do */
            pthread_mutex_lock(&d->mu); snprintf(d->reply, sizeof d->reply, "%.500s", t.text); d->steps[0] = 0; d->plan_ok = 0;
            snprintf(d->transcript, sizeof d->transcript, "%s", s->typed[0] ? s->typed : "(voice)");
            set_status(d, S_DONE, "DONE%s", ""); pthread_mutex_unlock(&d->mu); agent_turn_free(&t); done = 1; break;
        }
        int mut = 0; size_t used = 0;
        for (int i = 0; i < t.n; i++) {
            rs[i] = NULL;
            if (agent_tool_is_read_only(t.calls[i].name)) {
                rs[i] = tool_res + used; agent_exec(c, &t.calls[i], play_part_cb, ui_cb, &uc, rs[i], 200000 / AGENT_MAX_CALLS); used += strlen(rs[i]) + 1;
            } else mut++;
        }
        if (!mut) { agent_hist_model(&h, &t); agent_hist_results(&h, &t, (const char *const *)rs); agent_turn_free(&t); continue; }
        char steps[500] = "";
        for (int i = 0, k = 0; i < t.n; i++) if (!agent_tool_is_read_only(t.calls[i].name)) {
            char line[140]; agent_describe(c, &t.calls[i], line, sizeof line);
            size_t l = strlen(steps); k++; snprintf(steps + l, sizeof steps - l, "%s%d %s", k > 1 ? "   " : "", k, line);
        }
        pthread_mutex_lock(&d->mu);
        agent_turn_free(&d->plan); memcpy(&d->plan, &t, sizeof t); d->plan_ok = 1;      /* the plan owns the turn now */
        snprintf(d->steps, sizeof d->steps, "%s", steps); snprintf(d->reply, sizeof d->reply, "%.500s", t.text);
        snprintf(d->transcript, sizeof d->transcript, "%s", s->typed[0] ? s->typed : "(voice)");
        char n[16]; snprintf(n, sizeof n, "%d", mut); set_status(d, S_PLAN, "PLAN %s STEPS: press RUN", n);
        pthread_mutex_unlock(&d->mu);
        done = 1;
    }
    if (!done) publish_error(d, "ERROR %s", "too many steps without a plan");
    free(tool_res); agent_hist_free(&h); cmdr_close(c); mcp_close(uc.mcp);
}

static void agent_run(dm_t *d, const snap_t *s) {
    pthread_mutex_lock(&d->mu);
    if (!d->plan_ok) { pthread_mutex_unlock(&d->mu); return; }
    aturn_t plan = d->plan; plan.raw = NULL;                     /* a copy of the calls; the plan keeps the raw turn */
    pthread_mutex_unlock(&d->mu);
    char err[100] = ""; cmdr_t *c = cmdr_open(s->host, s->port, err, sizeof err);
    char *res = malloc(8000); int ok = 0, total = 0; char first_err[100] = ""; uictx uc = { d, s, NULL };
    for (int i = 0; i < plan.n; i++) if (!agent_tool_is_read_only(plan.calls[i].name)) total++;
    for (int i = 0; i < plan.n; i++) {
        if (agent_tool_is_read_only(plan.calls[i].name)) continue;
        if (agent_exec(c, &plan.calls[i], play_part_cb, ui_cb, &uc, res, 8000) == 0) ok++;
        else {
            if (!first_err[0]) snprintf(first_err, sizeof first_err, "%.50s: %.40s", plan.calls[i].name, res);
            if (agent_tool_is_ui(plan.calls[i].name)) break;       /* the screen is in an unknown state: do not go on */
        }
    }
    free(res); cmdr_close(c); mcp_close(uc.mcp);
    pthread_mutex_lock(&d->mu);
    d->plan_ok = 0; agent_turn_free(&d->plan);
    char m[160];
    if (ok == total) { snprintf(m, sizeof m, "%d", total); set_status(d, S_DONE, "RAN %s STEPS", m); }
    else { snprintf(m, sizeof m, "RAN %d of %d: %s", ok, total, first_err); set_status(d, S_ERROR, "%s", m); }
    pthread_mutex_unlock(&d->mu);
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
        d->pending = 0; int mode = d->mode;
        dreq_t rq = { TYPES[d->type], "", "gm", d->bpm > 0 ? d->bpm : 120, d->bars };
        char scale[48]; snprintf(scale, sizeof scale, "%s %s", KEYS[d->keyi], SCALES[d->scale]); rq.scale = scale;
        char typed[256]; snprintf(typed, sizeof typed, "%s", d->typed);
        int audio = d->with_audio && d->have_audio, wl = 0;
        if (audio) wl = daemon_cap_wav(&d->cap, wav, 44 + DAEMON_CAP_MAX * 2);
        char key[128], url[300]; snprintf(key, sizeof key, "%s", d->key);
        snprintf(url, sizeof url, "%s/%s:generateContent", d->url_base, d->model);
        pthread_mutex_unlock(&d->mu);

        if (mode == 2 || mode == 3) {
            snap_t sn; memset(&sn, 0, sizeof sn);
            snprintf(sn.typed, sizeof sn.typed, "%s", typed); sn.audio = audio; sn.wl = wl; sn.wav = wav; sn.bpm = rq.bpm; snprintf(sn.scale, sizeof sn.scale, "%s", scale); sn.bars = rq.bars;
            snprintf(sn.key, sizeof sn.key, "%s", key); snprintf(sn.url, sizeof sn.url, "%s", url); snprintf(sn.host, sizeof sn.host, "%s", d->cmdr_host); sn.port = d->cmdr_port; snprintf(sn.host_remote, sizeof sn.host_remote, "%s", d->remote_host); sn.port_remote = d->remote_port;
            if (mode == 2) agent_plan(d, &sn, body, resp); else agent_run(d, &sn);
            pthread_mutex_lock(&d->mu);
            continue;
        }
        int code = 0, bl = daemon_build_request(&rq, typed, audio ? wav : NULL, audio ? (size_t)wl : 0, body, 1400000);
        for (int tries = 0; bl > 0 && tries < 3; tries++) {       /* Gemini answers 503/429 under load: try again after a pause */
            code = d->io->http_post(url, key, body, (size_t)bl, resp, 600000, 60);
            if (code != 503 && code != 429 && code != 500) break;
            for (int w = 0; w < 30 * (tries + 1); w++) { usleep(100000); pthread_mutex_lock(&d->mu); int q = d->quit; pthread_mutex_unlock(&d->mu); if (q) break; }
        }
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
            snprintf(d->reply, sizeof d->reply, "%.500s", np->feedback);
            char n[24]; snprintf(n, sizeof n, "%d", np->n); set_status(d, S_DONE, "DONE %s NOTES", n);
        } else set_status(d, S_ERROR, "ERROR %s", err);
    }
    pthread_mutex_unlock(&d->mu);
    free(body); free(resp); free(text); free(wav); free(np);
    return NULL;
}

static void request(dm_t *d, int with_audio) {       /* d->mu held */
    if (!d->key[0]) { set_status(d, S_ERROR, "ERROR %s", "no api_key in daemon.conf"); return; }
    d->with_audio = with_audio; d->pending = 1; d->mode = d->agent ? 2 : 1; set_status(d, S_THINKING, "THINKING%s", ""); pthread_cond_signal(&d->cv);
}

/* keyboard callbacks run on the reader thread */
static void kb_changed(void *u, const char *text) {
    dm_t *d = u; pthread_mutex_lock(&d->mu);
    if (d->state == S_TYPING) { snprintf(d->transcript, sizeof d->transcript, "> %s_", text); d->rev++; }
    pthread_mutex_unlock(&d->mu);
}
static void kb_submit(void *u, const char *text) {
    dm_t *d = u; pthread_mutex_lock(&d->mu);
    if (d->state == S_TYPING) { snprintf(d->typed, sizeof d->typed, "%s", text); d->have_audio = 0; snprintf(d->transcript, sizeof d->transcript, "%s", text); request(d, 0); }
    pthread_mutex_unlock(&d->mu);
}
static void kb_cancel(void *u) {
    dm_t *d = u; pthread_mutex_lock(&d->mu);
    if (d->state == S_TYPING) { d->transcript[0] = 0; set_status(d, S_READY, "READY%s", ""); }
    pthread_mutex_unlock(&d->mu);
}
static void kb_found(void *u, int n) {
    dm_t *d = u; pthread_mutex_lock(&d->mu);
    d->kbd_devs = n;
    if (d->state == S_TYPING) set_status(d, S_TYPING, n ? "TYPING: Enter sends, Esc cancels%s" : "TYPING: no keyboard yet, connect one%s", "");
    pthread_mutex_unlock(&d->mu);
}

/* engine/prompt.txt: a text file dropped there (ssh, a phone's file share) is a typed prompt; read once, then deleted */
static void *drop_watch(void *arg) {
    dm_t *d = arg; char path[300]; snprintf(path, sizeof path, "%s/prompt.txt", d->dir);
    for (;;) {
        for (int i = 0; i < 5; i++) { usleep(100000); pthread_mutex_lock(&d->mu); int q = d->quit; pthread_mutex_unlock(&d->mu); if (q) return NULL; }
        { char lp[300]; snprintf(lp, sizeof lp, "%s/daemon.log", d->dir); FILE *lf = fopen(lp, "w");
          if (lf) { pthread_mutex_lock(&d->mu); long mbad = 0; int mrc = 0; if (d->io->midi_stat && d->midi) d->io->midi_stat(d->midi, &mbad, &mrc);
              fprintf(lf, "song_pos calls %ld busy %ld playing %ld last_ppq %.3f last_bpm %.2f\nhave_part %d notes %d play %d channel %d midi_port %d note_on %ld note_off %ld\nstate %d\nstatus %s\nmidi_bad %ld last_rc %d\n",
                      d->sp_calls, d->sp_busy, d->sp_playing, d->last_ppq, d->last_bpm, d->have_part, d->part.n, d->play, d->channel, d->midi != NULL, d->n_on, d->n_off, d->state, d->status, mbad, mrc);
              pthread_mutex_unlock(&d->mu); fclose(lf); } }
        FILE *f = fopen(path, "r"); if (!f) continue;
        char t[256]; size_t n = fread(t, 1, sizeof t - 1, f); fclose(f); unlink(path);
        while (n && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' ')) n--;
        t[n] = 0; if (!n) continue;
        pthread_mutex_lock(&d->mu);
        if (d->state != S_THINKING) { snprintf(d->typed, sizeof d->typed, "%s", t); d->have_audio = 0; snprintf(d->transcript, sizeof d->transcript, "%s", t); request(d, 0); }
        pthread_mutex_unlock(&d->mu);
    }
}

static void *create(const char *data_dir) {
    dm_t *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->mu, NULL); pthread_cond_init(&d->cv, NULL);
    snprintf(d->dir, sizeof d->dir, "%s", data_dir ? data_dir : ".");
    d->io = daemon_io(); d->type = 2; d->keyi = 5; d->scale = 1; d->bars = 4; d->channel = 2; d->play = 1;
    conf_load(d);
    { char mp[300], me[160]; snprintf(mp, sizeof mp, "%s/macros.json", d->dir); d->macros = macros_load(mp, me, sizeof me); }   /* none is fine: the screen tools are then not offered */
    snprintf(d->status, sizeof d->status, "READY"); 
    d->midi = d->io->midi_open("Daemon", "MIDI Out");
    pthread_create(&d->th, NULL, worker, d); d->th_up = 1;
    kbd_cb cb = { kb_changed, kb_submit, kb_cancel, kb_found, d };
    d->kbd = kbd_start(d->input_dir, d->input_test, cb);
    pthread_create(&d->th_drop, NULL, drop_watch, d); d->drop_up = 1;
    return d;
}

static void destroy(void *inst) {
    dm_t *d = inst;
    kbd_stop(d->kbd);                            /* before the lock: its callbacks take it */
    pthread_mutex_lock(&d->mu); d->quit = 1; pthread_cond_signal(&d->cv); pthread_mutex_unlock(&d->mu);
    if (d->drop_up) pthread_join(d->th_drop, NULL);
    if (d->th_up) pthread_join(d->th, NULL);     /* a request in flight finishes first (60 s at most) */
    if (d->midi && d->pl.npend) { /* the host stopped sending blocks: end any sounding notes */ for (int i = 0; i < d->pl.npend; i++) d->io->midi_send(d->midi, d->channel - 1, 0, d->pl.pend[i].pitch, 0); }
    if (d->midi) d->io->midi_close(d->midi);
    agent_turn_free(&d->plan); macros_free(d->macros);
    pthread_mutex_destroy(&d->mu); pthread_cond_destroy(&d->cv);
    free(d);
}

static void midi_in(void *inst, const uint8_t *m, int n) { (void)inst; (void)m; (void)n; }

/* readout text: at most 127 characters (PARAM_TEXT_MAX 128, the longest verified on a device), cut with "..." */
static int put_text(char *b, int n, const char *s) {
    int cap = n < 128 ? n : 128, l = (int)strlen(s);
    if (l < cap) return snprintf(b, (size_t)n, "%s", s);
    if (cap < 5) return snprintf(b, (size_t)n, "%.*s", cap - 1, s);
    return snprintf(b, (size_t)n, "%.*s...", cap - 4, s);
}

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
        } else if (d->state != S_THINKING) { if (d->state == S_TYPING) kbd_arm(d->kbd, 0); daemon_cap_reset(&d->cap); d->have_audio = 0; set_status(d, S_LISTENING, "LISTENING%s", ""); }
    } else if (!strcmp(k, "typing") && on) {
        if (d->state == S_TYPING) { d->transcript[0] = 0; set_status(d, S_READY, "READY%s", ""); kbd_arm(d->kbd, 0); }
        else if (d->state != S_THINKING && d->state != S_LISTENING) {
            d->transcript[0] = 0; set_status(d, S_TYPING, d->kbd_devs ? "TYPING: Enter sends, Esc cancels%s" : "TYPING: no keyboard yet, connect one%s", "");
            kbd_arm(d->kbd, 1);
        }
    } else if (!strcmp(k, "run") && on) {
        if (d->state == S_PLAN && d->plan_ok) { d->pending = 1; d->mode = 3; set_status(d, S_THINKING, "RUNNING%s", ""); pthread_cond_signal(&d->cv); }
        else set_status(d, d->state == S_THINKING ? S_THINKING : S_ERROR, "%s", d->state == S_THINKING ? "THINKING" : "ERROR nothing to run: ask for something with AGENT on");
    } else if (!strcmp(k, "agent")) {
        d->agent = on || !strcmp(v, "On");
        if (!d->agent && d->state == S_PLAN) { d->plan_ok = 0; agent_turn_free(&d->plan); d->steps[0] = 0; set_status(d, S_READY, "READY%s", ""); }
        d->rev++;
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
    if (!strcmp(k, "status")) r = put_text(b, n, d->status);
    else if (!strcmp(k, "transcript")) r = put_text(b, n, d->transcript[0] ? d->transcript : "-");
    else if (!strcmp(k, "reply")) r = put_text(b, n, d->reply[0] ? d->reply : "-");
    else if (!strcmp(k, "display_rev")) r = snprintf(b, n, "%d", d->rev);
    else if (!strcmp(k, "type")) r = snprintf(b, n, "%s", TYPES[d->type]);
    else if (!strcmp(k, "key")) r = snprintf(b, n, "%s", KEYS[d->keyi]);
    else if (!strcmp(k, "scale")) r = snprintf(b, n, "%s", SCALES[d->scale]);
    else if (!strcmp(k, "bars")) r = snprintf(b, n, "%d", d->bars);
    else if (!strcmp(k, "channel")) r = snprintf(b, n, "%d", d->channel);
    else if (!strcmp(k, "play")) r = snprintf(b, n, "%s", d->play ? "Play" : "Stop");
    else if (!strcmp(k, "plan")) r = put_text(b, n, d->steps[0] ? d->steps : "-");
    else if (!strcmp(k, "agent")) r = snprintf(b, n, "%s", d->agent ? "On" : "Off");
    else if (!strcmp(k, "face")) { static const char *F[] = { "Idle", "Listening", "Thinking", "Done", "Error", "Typing" };   /* the skin's picture per state */
        r = snprintf(b, n, "%s", F[d->state == S_READY ? 0 : d->state == S_LISTENING ? 1 : d->state == S_THINKING ? 2 : d->state == S_DONE || d->state == S_PLAN ? 3 : d->state == S_TYPING ? 5 : 4]); }
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
    if (on) d->n_on++; else d->n_off++;
    if (d->midi) d->io->midi_send(d->midi, d->channel - 1, on, pitch, vel);
}

static void song_pos(void *inst, double ppq, double bpm, int playing, int frames) {
    dm_t *d = inst;
    if (pthread_mutex_trylock(&d->mu)) { d->sp_busy++; return; }
    d->sp_calls++; d->sp_playing += playing != 0; d->last_ppq = ppq; d->last_bpm = bpm;
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
