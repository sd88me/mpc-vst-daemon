/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* The device implementation of daemon_io.h. No link-time dependencies: libasound.so.2 and libcurl.so.4 (both already in MPC OS)
 * are opened on first use, and the few ALSA structures needed are declared here. Compiles on any host; where the libraries are
 * missing the calls just fail. */
#define _GNU_SOURCE
#include "daemon_io.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ALSA sequencer, by hand: snd_seq_event_t is 28 bytes: type, flags, tag, queue, time (8), source{client,port},
 * dest{client,port}, data (12; a note is channel, note, velocity, off_velocity, duration) ---- */
typedef struct { uint8_t b[28]; } seq_ev;
enum { EV_NOTEON = 6, EV_NOTEOFF = 7, ADDR_SUBSCRIBERS = 254, QUEUE_DIRECT = 253, OPEN_OUTPUT = 1,
       CAP_READ = 1, CAP_SUBS_READ = 32, PT_MIDI_GENERIC = 2, PT_APPLICATION = 1 << 20 };

typedef struct {
    void *lib, *seq; int port; long bad; int last_rc;
    int (*open_)(void **, const char *, int, int);
    int (*set_name)(void *, const char *);
    int (*mk_port)(void *, const char *, unsigned, unsigned);
    int (*out_direct)(void *, void *);
    int (*close_)(void *);
} midi_t;

/* One ALSA client for the whole MPC session. MPC keeps each MIDI port's switches (Global, Control, Track) under the port's client
 * number, which ALSA hands out afresh every time a client is opened; a client re-created on every project load would come back
 * as a new device with its switches off. So the client is opened once and never closed. The handle is remembered in the process
 * environment (DAEMON_ALSA_SEQ=<handle>:<port>), not in this library, so a newer daemon.so can be loaded without a restart. libasound
 * stays loaded because MPC itself uses it. */
static midi_t *g_midi;

static void *midi_open(const char *client, const char *port) {
    if (g_midi) return g_midi;
    midi_t *m = calloc(1, sizeof *m);
    m->lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!m->lib) { free(m); return NULL; }
    m->open_ = dlsym(m->lib, "snd_seq_open"); m->set_name = dlsym(m->lib, "snd_seq_set_client_name");
    m->mk_port = dlsym(m->lib, "snd_seq_create_simple_port"); m->out_direct = dlsym(m->lib, "snd_seq_event_output_direct");
    m->close_ = dlsym(m->lib, "snd_seq_close");
    if (!m->open_ || !m->set_name || !m->mk_port || !m->out_direct || !m->close_) { dlclose(m->lib); free(m); return NULL; }
    const char *env = getenv("DAEMON_ALSA_SEQ"); unsigned long h = 0; int pt = -1;
    if (env && sscanf(env, "%lx:%d", &h, &pt) == 2 && h && pt >= 0) { m->seq = (void *)h; m->port = pt; g_midi = m; return m; }
    if (m->open_(&m->seq, "default", OPEN_OUTPUT, 0) < 0) { dlclose(m->lib); free(m); return NULL; }
    m->set_name(m->seq, client);
    m->port = m->mk_port(m->seq, port, CAP_READ | CAP_SUBS_READ, PT_MIDI_GENERIC | PT_APPLICATION);
    if (m->port < 0) { m->close_(m->seq); dlclose(m->lib); free(m); return NULL; }
    char v[64]; snprintf(v, sizeof v, "%lx:%d", (unsigned long)m->seq, m->port); setenv("DAEMON_ALSA_SEQ", v, 1);
    g_midi = m;
    return m;
}

static void midi_send(void *h, int ch, int on, int pitch, int vel) {
    midi_t *m = h; seq_ev e; memset(&e, 0, sizeof e);
    e.b[0] = on ? EV_NOTEON : EV_NOTEOFF; e.b[3] = QUEUE_DIRECT;
    e.b[12] = 0; e.b[13] = (uint8_t)m->port;                       /* source: client filled in by the kernel */
    e.b[14] = ADDR_SUBSCRIBERS; e.b[15] = 0;
    e.b[16] = (uint8_t)(ch & 15); e.b[17] = (uint8_t)pitch; e.b[18] = (uint8_t)vel;
    int rc = m->out_direct(m->seq, &e); m->last_rc = rc; if (rc < 0) m->bad++;
}

static void midi_stat(void *h, long *bad, int *last_rc) { midi_t *m = h; *bad = m ? m->bad : -1; *last_rc = m ? m->last_rc : 0; }

static void midi_close(void *h) { (void)h; }   /* the shared client stays open: see g_midi */

/* ---- libcurl ---- */
enum { OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_POSTFIELDS = 10015, OPT_HTTPHEADER = 10023, OPT_CAINFO = 10065,
       OPT_WRITEFUNCTION = 20011, OPT_TIMEOUT = 13, OPT_NOSIGNAL = 99, OPT_POSTFIELDSIZE = 60, INFO_RESPONSE_CODE = 0x200002 };
typedef struct { char *p; size_t n, cap; } sink_t;
static size_t on_data(char *d, size_t sz, size_t nm, void *u) {
    sink_t *s = u; size_t l = sz * nm;
    if (s->n + l + 1 > s->cap) l = s->cap - s->n - 1;      /* keep what fits: an over-long reply then fails to parse, safely */
    memcpy(s->p + s->n, d, l); s->n += l; s->p[s->n] = 0;
    return sz * nm;
}

static int http_post(const char *url, const char *key, const char *body, size_t len, char *resp, size_t cap, int timeout_s) {
    void *lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { snprintf(resp, cap, "libcurl.so.4 not found"); return 0; }
    void *(*init)(void) = dlsym(lib, "curl_easy_init");
    int (*setopt)(void *, int, ...) = dlsym(lib, "curl_easy_setopt");
    int (*perform)(void *) = dlsym(lib, "curl_easy_perform");
    int (*getinfo)(void *, int, ...) = dlsym(lib, "curl_easy_getinfo");
    void (*cleanup)(void *) = dlsym(lib, "curl_easy_cleanup");
    void *(*slist_add)(void *, const char *) = dlsym(lib, "curl_slist_append");
    void (*slist_free)(void *) = dlsym(lib, "curl_slist_free_all");
    const char *(*strerr)(int) = dlsym(lib, "curl_easy_strerror");
    int code = 0;
    if (!init || !setopt || !perform || !getinfo || !cleanup || !slist_add || !slist_free) { snprintf(resp, cap, "libcurl is missing calls"); dlclose(lib); return 0; }
    void *c = init(), *hdr = NULL;
    char kh[400]; hdr = slist_add(hdr, "Content-Type: application/json");
    if (strchr(key, ':')) {                          /* whole header lines, one per '\n' (Claude: x-api-key and anthropic-version) */
        snprintf(kh, sizeof kh, "%s", key); for (char *l = strtok(kh, "\n"); l; l = strtok(NULL, "\n")) hdr = slist_add(hdr, l);
    } else { snprintf(kh, sizeof kh, "x-goog-api-key: %s", key); hdr = slist_add(hdr, kh); }
    sink_t s = { resp, 0, cap }; if (cap) resp[0] = 0;
    setopt(c, OPT_URL, url); if (body) { setopt(c, OPT_POSTFIELDS, body); setopt(c, OPT_POSTFIELDSIZE, (long)len); }   /* no body: a GET (the model list) */
    setopt(c, OPT_HTTPHEADER, hdr); setopt(c, OPT_WRITEFUNCTION, on_data); setopt(c, OPT_WRITEDATA, &s);
    setopt(c, OPT_TIMEOUT, (long)timeout_s); setopt(c, OPT_NOSIGNAL, 1L);
    setopt(c, OPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    int rc = perform(c);
    if (rc != 0) snprintf(resp, cap, "%s", strerr ? strerr(rc) : "network error");
    else { long hc = 0; getinfo(c, INFO_RESPONSE_CODE, &hc); code = (int)hc; }
    slist_free(hdr); cleanup(c); dlclose(lib);
    return code;
}

static const daemon_io_t IO = { midi_open, midi_send, midi_close, http_post, midi_stat };
const daemon_io_t *daemon_io(void) { return &IO; }
