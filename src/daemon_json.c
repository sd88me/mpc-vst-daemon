/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#include "daemon_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a small JSON reader: just enough for the reply ---- */
typedef struct { const char *p, *end; int depth; } jp;

void djson_free(jv *v) { while (v) { jv *n = v->next; djson_free(v->kid); free(v->str); free(v->key); free(v); v = n; } }
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
    bad: djson_free(v); return NULL;
    }
    if (!strncmp(j->p, "true", 4) || !strncmp(j->p, "false", 5)) { v->t = 'b'; v->num = *j->p == 't'; j->p += (*j->p == 't') ? 4 : 5; return v; }
    if (!strncmp(j->p, "null", 4)) { v->t = '0'; j->p += 4; return v; }
    char *e; v->num = strtod(j->p, &e);
    if (e == j->p) { free(v); return NULL; }
    v->t = 'n'; j->p = e; return v;
}

jv *djson_get(jv *o, const char *key) {
    if (!o || o->t != 'o') return NULL;
    for (jv *k = o->kid; k; k = k->next) if (k->key && !strcmp(k->key, key)) return k;
    return NULL;
}
double djson_num(jv *o, const char *key, double def) { jv *k = djson_get(o, key); return k && k->t == 'n' ? k->num : def; }

jv *djson_parse(const char *text, size_t len) {
    jp j = { text, text + len, 0 };
    jv *v = jparse(&j);
    if (!v) return NULL;
    ws(&j);
    if (j.p != j.end) { djson_free(v); return NULL; }
    return v;
}
jv *djson_at(jv *arr, int i) { if (!arr || arr->t != 'a') return NULL; jv *k = arr->kid; while (k && i-- > 0) k = k->next; return k; }
const char *djson_str(jv *obj, const char *key, const char *def) { jv *k = djson_get(obj, key); return k && k->t == 's' ? k->str : def; }
int djson_len(jv *arr) { int n = 0; if (arr && (arr->t == 'a' || arr->t == 'o')) for (jv *k = arr->kid; k; k = k->next) n++; return n; }

typedef struct { char *b; size_t cap, n; int bad; } wr;
static void wput(wr *w, const char *s, size_t l) { if (w->n + l + 1 > w->cap) { w->bad = 1; return; } memcpy(w->b + w->n, s, l); w->n += l; w->b[w->n] = 0; }
static void wstr(wr *w, const char *s) {
    wput(w, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s; char t[8];
        if (c == '"' || c == '\\') { t[0] = '\\'; t[1] = (char)c; wput(w, t, 2); }
        else if (c == '\n') wput(w, "\\n", 2);
        else if (c < 0x20) { snprintf(t, sizeof t, "\\u%04x", c); wput(w, t, 6); }
        else wput(w, (const char *)&c, 1);
    }
    wput(w, "\"", 1);
}
static void wval(wr *w, const jv *v) {
    char t[40];
    if (!v) { wput(w, "null", 4); return; }
    switch (v->t) {
    case 's': wstr(w, v->str); break;
    case 'n': if (v->num == (long long)v->num && v->num > -1e15 && v->num < 1e15) snprintf(t, sizeof t, "%lld", (long long)v->num); else snprintf(t, sizeof t, "%.9g", v->num); wput(w, t, strlen(t)); break;
    case 'b': wput(w, v->num != 0 ? "true" : "false", v->num != 0 ? 4 : 5); break;
    case '0': wput(w, "null", 4); break;
    default: {
        int obj = v->t == 'o'; wput(w, obj ? "{" : "[", 1);
        for (const jv *k = v->kid; k; k = k->next) {
            if (k != v->kid) wput(w, ",", 1);
            if (obj) { wstr(w, k->key ? k->key : ""); wput(w, ":", 1); }
            wval(w, k);
        }
        wput(w, obj ? "}" : "]", 1);
    }
    }
}
int djson_write(const jv *v, char *buf, size_t cap) { wr w = { buf, cap, 0, 0 }; if (cap) buf[0] = 0; wval(&w, v); return w.bad ? -1 : (int)w.n; }
