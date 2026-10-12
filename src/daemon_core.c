/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#include "daemon_core.h"
#include "daemon_json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jcopy(jv *o, const char *key, char *dst, size_t cap) {
    jv *k = djson_get(o, key); dst[0] = 0;
    if (k && k->t == 's') { strncpy(dst, k->str, cap - 1); dst[cap - 1] = 0; }
}

int daemon_parse_reply(const char *text, float max_beats, dpart_t *out) {
    memset(out, 0, sizeof *out);
    const char *a = strchr(text, '{'), *b = strrchr(text, '}');     /* tolerate ```json fences and chatter */
    if (!a || !b || b < a) return -1;
    jv *root = djson_parse(a, (size_t)(b + 1 - a));
    if (!root || root->t != 'o') { djson_free(root); return -1; }
    jv *notes = djson_get(root, "notes");
    if (!notes || notes->t != 'a') { djson_free(root); return -1; }
    jcopy(root, "transcript", out->transcript, sizeof out->transcript);
    jv *fb = djson_get(root, "feedback");
    jcopy(fb, "result", out->feedback, sizeof out->feedback);
    jcopy(fb, "clip_name", out->clip_name, sizeof out->clip_name);
    for (char *c = out->clip_name; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) *c = '_';
    if (!out->clip_name[0]) strcpy(out->clip_name, "daemon");
    double end = 0;
    for (jv *k = notes->kid; k && out->n < DAEMON_MAX_NOTES; k = k->next) {
        double p = djson_num(k, "pitch", -1), s = djson_num(k, "start_time", -1), d = djson_num(k, "duration", 0), v = djson_num(k, "velocity", 100);
        if (k->t == 'a') {                                     /* compact form [pitch, start, duration, velocity] */
            jv *e0 = djson_at(k, 0), *e1 = djson_at(k, 1), *e2 = djson_at(k, 2), *e3 = djson_at(k, 3);
            if (!e0 || !e1 || !e2 || e0->t != 'n' || e1->t != 'n' || e2->t != 'n') continue;
            p = e0->num; s = e1->num; d = e2->num; v = e3 && e3->t == 'n' ? e3->num : 100;
        }
        if (djson_num(k, "mute", 0) != 0) continue;
        if (p < 0 || p > 127 || s < 0 || d <= 0 || s >= max_beats) continue;
        if (s + d > max_beats) d = max_beats - s;
        if (d < 0.01) continue;
        if (v < 1) v = 1;
        if (v > 127) v = 127;
        dnote_t *n = &out->notes[out->n++];
        n->pitch = (uint8_t)p; n->vel = (uint8_t)v; n->start = (float)s; n->dur = (float)d;
        if (s + d > end) end = s + d;
    }
    djson_free(root);
    out->loop_beats = (float)ceil(end > 1 ? end : 1);
    return out->n;
}

/* ---- player ---- */
void daemon_player_init(dplayer_t *p, const dpart_t *part) { memset(p, 0, sizeof *p); p->part = part; }

void daemon_player_stop(dplayer_t *p, daemon_emit_fn emit, void *user) {
    for (int i = 0; i < p->npend; i++) emit(user, 0, p->pend[i].pitch, 0);
    p->npend = 0; p->primed = 0;
}

static void drain_offs(dplayer_t *p, double upto, daemon_emit_fn emit, void *user) {
    for (int i = 0; i < p->npend;) {
        if (p->pend[i].off <= upto) { emit(user, 0, p->pend[i].pitch, 0); p->pend[i] = p->pend[--p->npend]; }
        else i++;
    }
}

void daemon_feed(dplayer_t *p, double ppq0, double ppq1, daemon_emit_fn emit, void *user) {
    if (!p->part || p->part->n == 0) return;
    double adv = ppq1 - ppq0;
    if (adv < 0 || adv > 1.0) {                      /* a locate or a jump: silence, resync at the new position */
        daemon_player_stop(p, emit, user);
        p->unwrapped = ppq1 < 0 ? 0 : ppq1; p->primed = 1; p->next = ppq1;
        return;
    }
    if (p->primed && fabs(ppq0 - p->next) > 0.2) {    /* the host looped or located (a small step is tempo jitter: ignore it, time by block length) */
        daemon_player_stop(p, emit, user);
    }
    if (!p->primed) { p->unwrapped = ppq0 < 0 ? 0 : ppq0; p->primed = 1; }   /* song bar 1 = beat 0 of the part */
    p->next = ppq1;
    double u0 = p->unwrapped, u1 = u0 + adv, L = p->part->loop_beats;
    drain_offs(p, u0, emit, user);
    for (int i = 0; i < p->part->n; i++) {
        const dnote_t *n = &p->part->notes[i];
        for (double m = ceil((u0 - n->start) / L); ; m += 1) {
            double t = n->start + m * L;               /* this pass of the note, in beats since resync */
            if (t >= u1) break;
            if (t < u0) continue;                      /* half-open [u0,u1): consecutive blocks never fire a note twice */
            for (int k = 0; k < p->npend; k++) if (p->pend[k].pitch == n->pitch) {   /* retrigger: end the old one first */
                emit(user, 0, n->pitch, 0); p->pend[k] = p->pend[--p->npend]; break; }
            if (p->npend < DAEMON_MAX_PENDING) {
                emit(user, 1, n->pitch, n->vel);
                p->pend[p->npend].pitch = n->pitch; p->pend[p->npend].off = t + n->dur; p->npend++;
            }
        }
    }
    drain_offs(p, u1, emit, user);
    p->unwrapped = u1;
}

/* ---- SMF ---- */
static size_t vlq(uint8_t *b, uint32_t v) { uint8_t t[5]; int n = 0; t[n++] = v & 0x7f; while (v >>= 7) t[n++] = (v & 0x7f) | 0x80; for (int i = 0; i < n; i++) b[i] = t[n - 1 - i]; return n; }

typedef struct { uint32_t tick; uint8_t on, pitch, vel; } sev;
static int cmp_sev(const void *a, const void *b) {
    const sev *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return (int)x->on - (int)y->on;                          /* offs before ons at the same tick */
}

int daemon_write_smf(const dpart_t *part, int channel, double bpm, uint8_t *buf, size_t cap) {
    if (part->n <= 0 || cap < 64 + (size_t)part->n * 12) return -1;
    sev *ev = malloc(sizeof(sev) * part->n * 2); int ne = 0;
    for (int i = 0; i < part->n; i++) {
        const dnote_t *n = &part->notes[i];
        uint32_t on = (uint32_t)lround(n->start * 480), off = on + (uint32_t)lround(n->dur * 480);
        if (off <= on) off = on + 1;
        ev[ne++] = (sev){ on, 1, n->pitch, n->vel }; ev[ne++] = (sev){ off, 0, n->pitch, 0 };
    }
    qsort(ev, ne, sizeof *ev, cmp_sev);
    uint8_t *t = buf + 22; size_t o = 0;
    uint32_t us = (uint32_t)(60000000.0 / (bpm > 20 ? bpm : 120));
    t[o++] = 0; t[o++] = 0xff; t[o++] = 0x51; t[o++] = 3; t[o++] = us >> 16; t[o++] = us >> 8; t[o++] = us;
    uint32_t last = 0;
    for (int i = 0; i < ne; i++) {
        o += vlq(t + o, ev[i].tick - last); last = ev[i].tick;
        t[o++] = (ev[i].on ? 0x90 : 0x80) | (channel & 15); t[o++] = ev[i].pitch; t[o++] = ev[i].vel;
    }
    uint32_t end = (uint32_t)(part->loop_beats * 480); if (end < last) end = last;
    o += vlq(t + o, end - last); t[o++] = 0xff; t[o++] = 0x2f; t[o++] = 0;
    free(ev);
    memcpy(buf, "MThd\0\0\0\6\0\0\0\1\x01\xe0MTrk", 18);
    buf[18] = o >> 24; buf[19] = o >> 16; buf[20] = o >> 8; buf[21] = o;
    return (int)(22 + o);
}

/* ---- request ---- */
static const char b64t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
typedef struct { char *b; size_t cap, n; int bad; } sb;
static void sput(sb *s, const char *x, size_t len) { if (s->n + len + 1 > s->cap) { s->bad = 1; return; } memcpy(s->b + s->n, x, len); s->n += len; s->b[s->n] = 0; }
static void sstr(sb *s, const char *x) { sput(s, x, strlen(x)); }
static void sjson(sb *s, const char *x) {                    /* a JSON string body, escaped */
    for (; *x; x++) {
        char t[8];
        if (*x == '"' || *x == '\\') { t[0] = '\\'; t[1] = *x; sput(s, t, 2); }
        else if (*x == '\n') sput(s, "\\n", 2);
        else if ((unsigned char)*x < 0x20) sput(s, " ", 1);
        else sput(s, x, 1);
    }
}
static void sb64(sb *s, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        char q[4] = { b64t[v >> 18 & 63], b64t[v >> 12 & 63], i + 1 < n ? b64t[v >> 6 & 63] : '=', i + 2 < n ? b64t[v & 63] : '=' };
        sput(s, q, 4);
    }
}

int daemon_build_request(const dreq_t *rq, const char *typed, const uint8_t *wav16, size_t wav_len, char *buf, size_t cap) {
    sb s = { buf, cap, 0, 0 }; if (cap) buf[0] = 0;
    char num[64];
    int drums = rq->type && (strstr(rq->type, "Beat") || strstr(rq->type, "Drum"));
    sstr(&s, "{\"system_instruction\":{\"parts\":[{\"text\":\"");
    sjson(&s, "You write short musical parts for a hardware sampler. Reply only with the JSON object the schema describes.\n"
              "Positions and lengths are in beats (1.0 = a quarter note, 0.25 = a sixteenth). Beat 0.0 is the first beat of the part. Each note is an array [pitch, start_time, duration, velocity].\n"
              "Pitches are MIDI note numbers 0-127, velocities 1-127. Keep the part inside the requested number of beats and make it loop cleanly.\n"
              "transcript: what the user asked for, in their words (for spoken audio, a verbatim transcription).\n"
              "feedback.result: one or two plain sentences on what you made, and, if the request named an artist, track or genre you do not know well, say so and what you assumed. "
              "feedback.clip_name: at most 12 characters, letters, digits and underscores.\n");
    snprintf(num, sizeof num, "Tempo %.0f BPM. ", rq->bpm); sjson(&s, num);
    sjson(&s, "For drums and beats use General MIDI drum notes (36 kick, 38 snare, 42 closed hat, 46 open hat, 39 clap, 49 crash) and ignore the scale. ");
    if (!drums) { sjson(&s, "Keep every note in the scale: "); sjson(&s, rq->scale); sjson(&s, ", unless the request names another key or scale."); }
    sstr(&s, "\"}]},\"contents\":[{\"role\":\"user\",\"parts\":[");
    if (wav16) { sstr(&s, "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\""); sb64(&s, wav16, wav_len); sstr(&s, "\"}},"); }
    sstr(&s, "{\"text\":\"");
    sjson(&s, "Menu defaults (what the request says wins over them): Part type: "); sjson(&s, rq->type);
    snprintf(num, sizeof num, ". Length: %d beats. ", rq->bars * 4); sjson(&s, num);
    if (typed && typed[0]) { sjson(&s, "Request: "); sjson(&s, typed); }
    else sjson(&s, "The request is in the audio.");
    sstr(&s, "\"}]}],\"generationConfig\":{\"thinkingConfig\":{\"thinkingLevel\":\"low\"},\"responseMimeType\":\"application/json\",\"responseSchema\":{\"type\":\"OBJECT\","
             "\"properties\":{\"transcript\":{\"type\":\"STRING\"},\"notes\":{\"type\":\"ARRAY\",\"items\":{\"type\":\"ARRAY\",\"items\":{\"type\":\"NUMBER\"}}},\"feedback\":{\"type\":\"OBJECT\",\"properties\":{"
             "\"result\":{\"type\":\"STRING\"},\"clip_name\":{\"type\":\"STRING\"}}}},\"required\":[\"transcript\",\"notes\",\"feedback\"]}}}");
    return s.bad ? -1 : (int)s.n;
}

/* ---- Gemini envelope ---- */
int daemon_extract_text(const char *resp, char *out, size_t cap) {
    if (cap) out[0] = 0;
    jv *root = djson_parse(resp, strlen(resp));
    int r = -1;
    if (root && root->t == 'o') {
        jv *cands = djson_get(root, "candidates");
        jv *c0 = cands && cands->t == 'a' ? cands->kid : NULL;
        jv *parts = djson_get(djson_get(c0, "content"), "parts");
        size_t n = 0;
        if (parts && parts->t == 'a')
            for (jv *p = parts->kid; p; p = p->next) {
                jv *t = djson_get(p, "text");
                if (t && t->t == 's') { size_t l = strlen(t->str); if (n + l + 1 > cap) l = cap - n - 1; memcpy(out + n, t->str, l); n += l; out[n] = 0; }
            }
        if (n) r = (int)n;
        else {
            jv *m = djson_get(djson_get(root, "error"), "message");
            snprintf(out, cap, "%s", m && m->t == 's' ? m->str : "no text in reply");
        }
    } else snprintf(out, cap, "reply is not JSON");
    djson_free(root);
    return r;
}

/* ---- capture ---- */
void daemon_cap_reset(dcap_t *c) { c->n = 0; c->phase = 0; c->prev = 0; }

void daemon_cap_push(dcap_t *c, const int16_t *in_lr, int frames) {
    const double step = 44100.0 / 16000.0;       /* input samples per output sample */
    for (int i = 0; i < frames && c->n < DAEMON_CAP_MAX; i++) {
        int16_t cur = (int16_t)(((int)in_lr[2 * i] + (int)in_lr[2 * i + 1]) / 2);
        while (c->phase < 1.0 && c->n < DAEMON_CAP_MAX) {   /* output samples falling between prev and cur */
            c->pcm[c->n++] = (int16_t)lround(c->prev + (cur - c->prev) * c->phase);
            c->phase += step;
        }
        c->phase -= 1.0;
        c->prev = cur;
    }
}

int daemon_cap_peak(const dcap_t *c) {
    int pk = 0;
    for (int i = 0; i < c->n; i++) { int v = abs(c->pcm[i]); if (v > pk) pk = v; }
    return pk > 32767 ? 32767 : pk;
}

int daemon_cap_wav(const dcap_t *c, uint8_t *out, size_t cap) {
    size_t data = (size_t)c->n * 2;
    if (cap < 44 + data) return -1;
    uint32_t riff = (uint32_t)(36 + data), rate = 16000, brate = 32000;
    memcpy(out, "RIFF", 4); memcpy(out + 4, &riff, 4); memcpy(out + 8, "WAVEfmt ", 8);
    uint32_t fmt = 16; uint16_t pcm = 1, ch = 1, align = 2, bits = 16; uint32_t dl = (uint32_t)data;
    memcpy(out + 16, &fmt, 4); memcpy(out + 20, &pcm, 2); memcpy(out + 22, &ch, 2); memcpy(out + 24, &rate, 4);
    memcpy(out + 28, &brate, 4); memcpy(out + 32, &align, 2); memcpy(out + 34, &bits, 2);
    memcpy(out + 36, "data", 4); memcpy(out + 40, &dl, 4);
    memcpy(out + 44, c->pcm, data);              /* the device is little-endian (armv7, aarch64) as is x86 */
    return (int)(44 + data);
}

/* ---- vision ---- */
int daemon_build_vision_request(const char *target, const char *hint, const char *png_b64, int w, int h, char *buf, size_t cap) {
    sb s = { buf, cap, 0, 0 }; if (cap) buf[0] = 0;
    char num[96];
    sstr(&s, "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"inline_data\":{\"mime_type\":\"image/png\",\"data\":\"");
    sstr(&s, png_b64);
    sstr(&s, "\"}},{\"text\":\"");
    snprintf(num, sizeof num, "This is a screenshot of the %dx%d touchscreen of an Akai MPC. ", w, h); sjson(&s, num);
    if (hint && hint[0]) { sjson(&s, "What it should be showing: "); sjson(&s, hint); sjson(&s, ". "); }
    sjson(&s, "Find the button, tile or list entry whose label reads: "); sjson(&s, target);
    sjson(&s, ". Answer with the centre of that one item as point [y, x], each from 0 to 1000 (fractions of the image height and width). "
              "If it is not clearly visible, or more than one item matches equally, say found=false and why in reason. Never guess.");
    sstr(&s, "\"}]}],\"generationConfig\":{\"thinkingConfig\":{\"thinkingLevel\":\"low\"},\"responseMimeType\":\"application/json\",\"responseSchema\":{\"type\":\"OBJECT\","
             "\"properties\":{\"found\":{\"type\":\"BOOLEAN\"},\"point\":{\"type\":\"ARRAY\",\"items\":{\"type\":\"NUMBER\"}},\"reason\":{\"type\":\"STRING\"}},\"required\":[\"found\"]}}}");
    return s.bad ? -1 : (int)s.n;
}

int daemon_parse_point(const char *text, int w, int h, int *x, int *y, char *why, size_t whycap) {
    const char *a = strchr(text, '{'), *b = strrchr(text, '}');
    if (why && whycap) why[0] = 0;
    if (!a || !b || b < a) { snprintf(why, whycap, "the model did not answer with JSON"); return -1; }
    jv *v = djson_parse(a, (size_t)(b + 1 - a));
    if (!v) { snprintf(why, whycap, "the model's answer was not valid JSON"); return -1; }
    jv *f = djson_get(v, "found"); jv *p = djson_get(v, "point");
    const char *reason = djson_str(v, "reason", "");
    int rc = -1;
    if (!f || f->num == 0) snprintf(why, whycap, "%s", reason[0] ? reason : "not found on the screen");
    else if (!p || p->t != 'a' || djson_len(p) != 2 || djson_at(p, 0)->t != 'n' || djson_at(p, 1)->t != 'n') snprintf(why, whycap, "no usable point in the answer");
    else {
        double py = djson_at(p, 0)->num, px = djson_at(p, 1)->num;
        if (py < 0 || py > 1000 || px < 0 || px > 1000) snprintf(why, whycap, "the point is outside the screen");
        else { *x = (int)(px * w / 1000.0 + 0.5); *y = (int)(py * h / 1000.0 + 0.5); if (*x >= w) *x = w - 1; if (*y >= h) *y = h - 1; rc = 0; }
    }
    djson_free(v);
    return rc;
}
