#include "daemon_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a small JSON reader: just enough for the reply ---- */
typedef struct jv { char t; double num; char *str; struct jv *kid, *next; char *key; } jv;   /* t: o a s n b 0 */
typedef struct { const char *p, *end; int depth; } jp;

static void jfree(jv *v) { while (v) { jv *n = v->next; jfree(v->kid); free(v->str); free(v->key); free(v); v = n; } }
static void ws(jp *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\t' || *j->p == '\r')) j->p++; }
static jv *jparse(jp *j);

static char *jstring(jp *j) {      /* at the opening quote */
    j->p++;
    size_t cap = 16, n = 0; char *s = malloc(cap);
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            c = *j->p++;
            if (c == 'n') c = '\n'; else if (c == 't') c = ' ';
            else if (c == 'u') { j->p += (j->end - j->p >= 4) ? 4 : (j->end - j->p); c = '?'; }
        }
        if (n + 2 > cap) s = realloc(s, cap *= 2);
        s[n++] = c;
    }
    if (j->p >= j->end) { free(s); return NULL; }
    j->p++; s[n] = 0; return s;
}

static jv *jparse(jp *j) {
    ws(j);
    if (j->p >= j->end || j->depth > 16) return NULL;
    jv *v = calloc(1, sizeof *v);
    char c = *j->p;
    if (c == '"') { v->t = 's'; v->str = jstring(j); if (!v->str) { free(v); return NULL; } return v; }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']'; v->t = c == '{' ? 'o' : 'a'; j->p++; j->depth++;
        jv **tail = &v->kid;
        for (;;) {
            ws(j);
            if (j->p >= j->end) goto bad;
            if (*j->p == close) { j->p++; break; }
            char *key = NULL;
            if (v->t == 'o') {
                if (*j->p != '"') goto bad;
                key = jstring(j); if (!key) goto bad;
                ws(j); if (j->p >= j->end || *j->p != ':') { free(key); goto bad; } j->p++;
            }
            jv *k = jparse(j);
            if (!k) { free(key); goto bad; }
            k->key = key; *tail = k; tail = &k->next;
            ws(j);
            if (j->p < j->end && *j->p == ',') j->p++;
        }
        j->depth--; return v;
    bad: jfree(v); return NULL;
    }
    if (!strncmp(j->p, "true", 4) || !strncmp(j->p, "false", 5)) { v->t = 'b'; j->p += (*j->p == 't') ? 4 : 5; return v; }
    if (!strncmp(j->p, "null", 4)) { v->t = '0'; j->p += 4; return v; }
    char *e; v->num = strtod(j->p, &e);
    if (e == j->p) { free(v); return NULL; }
    v->t = 'n'; j->p = e; return v;
}

static jv *jget(jv *o, const char *key) {
    if (!o || o->t != 'o') return NULL;
    for (jv *k = o->kid; k; k = k->next) if (k->key && !strcmp(k->key, key)) return k;
    return NULL;
}
static double jnum(jv *o, const char *key, double def) { jv *k = jget(o, key); return k && k->t == 'n' ? k->num : def; }
static void jcopy(jv *o, const char *key, char *dst, size_t cap) {
    jv *k = jget(o, key); dst[0] = 0;
    if (k && k->t == 's') { strncpy(dst, k->str, cap - 1); dst[cap - 1] = 0; }
}

int daemon_parse_reply(const char *text, float max_beats, dpart_t *out) {
    memset(out, 0, sizeof *out);
    const char *a = strchr(text, '{'), *b = strrchr(text, '}');     /* tolerate ```json fences and chatter */
    if (!a || !b || b < a) return -1;
    jp j = { a, b + 1, 0 };
    jv *root = jparse(&j);
    if (!root || root->t != 'o') { jfree(root); return -1; }
    jv *notes = jget(root, "notes");
    if (!notes || notes->t != 'a') { jfree(root); return -1; }
    jcopy(root, "transcript", out->transcript, sizeof out->transcript);
    jv *fb = jget(root, "feedback");
    jcopy(fb, "result", out->feedback, sizeof out->feedback);
    jcopy(fb, "clip_name", out->clip_name, sizeof out->clip_name);
    for (char *c = out->clip_name; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) *c = '_';
    if (!out->clip_name[0]) strcpy(out->clip_name, "daemon");
    double end = 0;
    for (jv *k = notes->kid; k && out->n < DAEMON_MAX_NOTES; k = k->next) {
        double p = jnum(k, "pitch", -1), s = jnum(k, "start_time", -1), d = jnum(k, "duration", 0), v = jnum(k, "velocity", 100);
        if (jnum(k, "mute", 0) != 0) continue;
        if (p < 0 || p > 127 || s < 0 || d <= 0 || s >= max_beats) continue;
        if (s + d > max_beats) d = max_beats - s;
        if (d < 0.01) continue;
        if (v < 1) v = 1;
        if (v > 127) v = 127;
        dnote_t *n = &out->notes[out->n++];
        n->pitch = (uint8_t)p; n->vel = (uint8_t)v; n->start = (float)s; n->dur = (float)d;
        if (s + d > end) end = s + d;
    }
    jfree(root);
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
        p->unwrapped = ppq1 < 0 ? 0 : ppq1; p->primed = 1;
        return;
    }
    if (!p->primed) { p->unwrapped = ppq0 < 0 ? 0 : ppq0; p->primed = 1; }   /* song bar 1 = beat 0 of the part */
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
              "Positions and lengths are in beats (1.0 = a quarter note, 0.25 = a sixteenth). Beat 0.0 is the first beat of the part.\n"
              "Pitches are MIDI note numbers 0-127, velocities 1-127. Keep the part inside the requested number of beats and make it loop cleanly.\n"
              "transcript: what the user asked for, in their words (for spoken audio, a verbatim transcription).\n"
              "feedback.result: one or two plain sentences on what you made, and, if the request named an artist, track or genre you do not know well, say so and what you assumed. "
              "feedback.clip_name: at most 12 characters, letters, digits and underscores.\n");
    snprintf(num, sizeof num, "Tempo %.0f BPM. ", rq->bpm); sjson(&s, num);
    if (drums) sjson(&s, "Use General MIDI drum notes (36 kick, 38 snare, 42 closed hat, 46 open hat, 39 clap, 49 crash).");
    else { sjson(&s, "Keep every note in the scale: "); sjson(&s, rq->scale); sjson(&s, "."); }
    sstr(&s, "\"}]},\"contents\":[{\"role\":\"user\",\"parts\":[");
    if (wav16) { sstr(&s, "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\""); sb64(&s, wav16, wav_len); sstr(&s, "\"}},"); }
    sstr(&s, "{\"text\":\"");
    snprintf(num, sizeof num, "Part type: "); sjson(&s, num); sjson(&s, rq->type);
    snprintf(num, sizeof num, ". Length: %d beats. ", rq->bars * 4); sjson(&s, num);
    if (typed && typed[0]) { sjson(&s, "Request: "); sjson(&s, typed); }
    else sjson(&s, "The request is in the audio.");
    sstr(&s, "\"}]}],\"generationConfig\":{\"responseMimeType\":\"application/json\",\"responseSchema\":{\"type\":\"OBJECT\","
             "\"properties\":{\"transcript\":{\"type\":\"STRING\"},\"notes\":{\"type\":\"ARRAY\",\"items\":{\"type\":\"OBJECT\",\"properties\":{"
             "\"pitch\":{\"type\":\"INTEGER\"},\"start_time\":{\"type\":\"NUMBER\"},\"duration\":{\"type\":\"NUMBER\"},\"velocity\":{\"type\":\"INTEGER\"}},"
             "\"required\":[\"pitch\",\"start_time\",\"duration\",\"velocity\"]}},\"feedback\":{\"type\":\"OBJECT\",\"properties\":{"
             "\"result\":{\"type\":\"STRING\"},\"clip_name\":{\"type\":\"STRING\"}}}},\"required\":[\"transcript\",\"notes\",\"feedback\"]}}}");
    return s.bad ? -1 : (int)s.n;
}
