/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#define _GNU_SOURCE
#include "daemon_macro.h"
#include "daemon_json.h"
#include "daemon_fp.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_STEPS_PER_RUN 200
#define MAX_DEPTH 4
#define MAX_PARAMS 8

struct macros { jv *root; int w, h; };

/* ---- expressions: numbers, parameter names, + - * / ( ) and unary minus ---- */
typedef struct { const char *p; const char **names; const double *vals; int n, err; } ex_t;
static double ex_sum(ex_t *e);
static void ex_ws(ex_t *e) { while (*e->p == ' ') e->p++; }
static double ex_atom(ex_t *e) {
    ex_ws(e);
    if (*e->p == '(') { e->p++; double v = ex_sum(e); ex_ws(e); if (*e->p != ')') e->err = 1; else e->p++; return v; }
    if (*e->p == '-') { e->p++; return -ex_atom(e); }
    if (isdigit((unsigned char)*e->p) || *e->p == '.') { char *end; double v = strtod(e->p, &end); e->p = end; return v; }
    if (isalpha((unsigned char)*e->p) || *e->p == '_') {
        const char *s = e->p; while (isalnum((unsigned char)*e->p) || *e->p == '_') e->p++;
        for (int i = 0; i < e->n; i++) if (strlen(e->names[i]) == (size_t)(e->p - s) && !strncmp(e->names[i], s, (size_t)(e->p - s))) return e->vals[i];
    }
    e->err = 1; return 0;
}
static double ex_prod(ex_t *e) {
    double v = ex_atom(e);
    for (;;) { ex_ws(e); if (*e->p == '*') { e->p++; v *= ex_atom(e); } else if (*e->p == '/') { e->p++; double d = ex_atom(e); if (d == 0) e->err = 1; else v /= d; } else return v; }
}
static double ex_sum(ex_t *e) {
    double v = ex_prod(e);
    for (;;) { ex_ws(e); if (*e->p == '+') { e->p++; v += ex_prod(e); } else if (*e->p == '-') { e->p++; v -= ex_prod(e); } else return v; }
}
static int eval(const char *s, const char **names, const double *vals, int n, double *out) {
    ex_t e = { s, names, vals, n, 0 }; double v = ex_sum(&e); ex_ws(&e);
    if (e.err || *e.p) return -1;
    *out = v; return 0;
}
/* a step value that is a number or a string expression */
static int eval_jv(jv *v, const char **names, const double *vals, int n, double *out) {
    if (!v) return -1;
    if (v->t == 'n') { *out = v->num; return 0; }
    if (v->t == 's') return eval(v->str, names, vals, n, out);
    return -1;
}

/* ---- validation ---- */
static const char *STEP_KEYS[] = { "tap", "double_tap", "long_press", "press", "wait", "wait_stable", "wait_change", "scroll", "find_tap", "find_empty", "call", "repeat", "expect", NULL };
static const char *OPT_KEYS[] = { "label", "require_change", "hold_ms", "hint", "direction", "amount", "steps", "count", "args", "timeout_ms", "skip_if", "scroll_at", "tries", "if_param", "double", NULL };
static int in_list(const char *k, const char **l) { for (; *l; l++) if (!strcmp(k, *l)) return 1; return 0; }

static int check_steps(macros_t *m, jv *steps, const char *macro, char *err, size_t cap, int depth) {
    if (!steps || steps->t != 'a') { snprintf(err, cap, "macro %s: steps must be a list", macro); return -1; }
    if (depth > MAX_DEPTH) { snprintf(err, cap, "macro %s: repeat nested too deep", macro); return -1; }
    int i = 0;
    for (jv *s = steps->kid; s; s = s->next, i++) {
        if (s->t != 'o') { snprintf(err, cap, "macro %s step %d: not an object", macro, i + 1); return -1; }
        int kinds = 0;
        for (jv *k = s->kid; k; k = k->next) {
            if (in_list(k->key, STEP_KEYS)) kinds++;
            else if (!in_list(k->key, OPT_KEYS)) { snprintf(err, cap, "macro %s step %d: unknown key \"%s\"", macro, i + 1, k->key); return -1; }
        }
        if (kinds != 1) { snprintf(err, cap, "macro %s step %d: needs exactly one action", macro, i + 1); return -1; }
        jv *c;
        if ((c = djson_get(s, "call")) != NULL) {
            if (c->t != 's' || !djson_get(djson_get(m->root, "macros"), c->str)) { snprintf(err, cap, "macro %s step %d: calls unknown macro", macro, i + 1); return -1; }
        }
        for (const char *key = "tap"; key; key = !strcmp(key, "tap") ? "double_tap" : !strcmp(key, "double_tap") ? "long_press" : NULL)
            if ((c = djson_get(s, key)) != NULL && (c->t != 'a' || djson_len(c) != 2)) { snprintf(err, cap, "macro %s step %d: %s needs [x, y]", macro, i + 1, key); return -1; }
        if ((c = djson_get(s, "scroll")) != NULL && (c->t != 'a' || djson_len(c) != 2 || !djson_get(s, "direction"))) { snprintf(err, cap, "macro %s step %d: scroll needs [x, y] and a direction", macro, i + 1); return -1; }
        if ((c = djson_get(s, "skip_if")) != NULL && (c->t != 's' || !djson_get(djson_get(m->root, "fingerprints"), c->str))) { snprintf(err, cap, "macro %s step %d: skip_if names an unknown fingerprint", macro, i + 1); return -1; }
        if ((c = djson_get(s, "tries")) != NULL && (c->num < 0 || c->num > 8 || !djson_get(s, "find_tap") || !djson_get(s, "scroll_at"))) { snprintf(err, cap, "macro %s step %d: tries (0 to 8) goes with find_tap and scroll_at", macro, i + 1); return -1; }
        if ((c = djson_get(s, "expect")) != NULL) {
            if (c->t != 's' || !djson_get(djson_get(m->root, "fingerprints"), c->str)) { snprintf(err, cap, "macro %s step %d: expects an unknown fingerprint", macro, i + 1); return -1; }
        }
        if ((c = djson_get(s, "find_empty")) != NULL) {
            jv *o = djson_get(c, "origin"), *st = djson_get(c, "step"), *sz = djson_get(c, "size"); const char *like = djson_str(c, "like", "");
            int n = (int)djson_num(c, "count", 0);
            if (c->t != 'o' || !o || djson_len(o) != 2 || !st || djson_len(st) != 2 || !sz || djson_len(sz) != 2 || n < 1 || n > 16 || (int)djson_at(sz, 0)->num < 4 || (int)djson_at(sz, 1)->num < 4) { snprintf(err, cap, "macro %s step %d: find_empty needs origin [x, y], step [dx, dy], size [w, h] and a count of 1 to 16", macro, i + 1); return -1; }
            if (!djson_get(djson_get(m->root, "fingerprints"), like)) { snprintf(err, cap, "macro %s step %d: find_empty is like an unknown fingerprint", macro, i + 1); return -1; }
        }
        if ((c = djson_get(s, "repeat")) != NULL) { if (check_steps(m, djson_get(s, "steps"), macro, err, cap, depth + 1)) return -1; }
    }
    return 0;
}

macros_t *macros_parse(const char *json, char *err, size_t errcap) {
    macros_t *m = calloc(1, sizeof *m);
    m->root = djson_parse(json, strlen(json));
    if (!m->root || m->root->t != 'o') { snprintf(err, errcap, "macros file is not valid JSON"); macros_free(m); return NULL; }
    if ((int)djson_num(m->root, "version", 0) != 1) { snprintf(err, errcap, "macros file: version must be 1"); macros_free(m); return NULL; }
    jv *scr = djson_get(m->root, "screen");
    if (!scr || djson_len(scr) != 2) { snprintf(err, errcap, "macros file: \"screen\": [width, height] is required (the screen it was recorded on)"); macros_free(m); return NULL; }
    m->w = (int)djson_at(scr, 0)->num; m->h = (int)djson_at(scr, 1)->num;
    jv *fps = djson_get(m->root, "fingerprints");
    if (fps && fps->t != 'o') { snprintf(err, errcap, "macros file: \"fingerprints\" must be an object"); macros_free(m); return NULL; }
    for (jv *f = fps ? fps->kid : NULL; f; f = f->next) {
        jv *rg = djson_get(f, "region"); uint8_t tmp[FP_BYTES];
        if (!rg || djson_len(rg) != 4 || (int)djson_at(rg, 2)->num <= 0 || (int)djson_at(rg, 3)->num <= 0 || (int)djson_at(rg, 0)->num < 0 || (int)djson_at(rg, 1)->num < 0 ||
            (int)(djson_at(rg, 0)->num + djson_at(rg, 2)->num) > m->w || (int)(djson_at(rg, 1)->num + djson_at(rg, 3)->num) > m->h) { snprintf(err, errcap, "fingerprint %s: region must be [x, y, w, h] inside the %dx%d screen", f->key, m->w, m->h); macros_free(m); return NULL; }
        if (fp_unhex(djson_str(f, "thumb", ""), tmp)) { snprintf(err, errcap, "fingerprint %s: thumb must be %d hex digits", f->key, FP_BYTES * 2); macros_free(m); return NULL; }
    }
    jv *ms = djson_get(m->root, "macros");
    if (!ms || ms->t != 'o' || !ms->kid) { snprintf(err, errcap, "macros file: no macros"); macros_free(m); return NULL; }
    for (jv *mc = ms->kid; mc; mc = mc->next) {
        jv *ps = djson_get(mc, "params");
        if (ps && (ps->t != 'o' || djson_len(ps) > MAX_PARAMS)) { snprintf(err, errcap, "macro %s: params must be an object of at most %d", mc->key, MAX_PARAMS); macros_free(m); return NULL; }
        if (check_steps(m, djson_get(mc, "steps"), mc->key, err, errcap, 0)) { macros_free(m); return NULL; }
    }
    return m;
}

macros_t *macros_load(const char *path, char *err, size_t errcap) {
    FILE *f = fopen(path, "rb"); if (!f) { snprintf(err, errcap, "no macros file at %s", path); return NULL; }
    char *buf = malloc(1 << 20); size_t n = fread(buf, 1, (1 << 20) - 1, f); fclose(f); buf[n] = 0;
    macros_t *m = macros_parse(buf, err, errcap); free(buf); return m;
}
void macros_free(macros_t *m) { if (!m) return; djson_free(m->root); free(m); }
int macros_has(const macros_t *m, const char *name) { return m && djson_get(djson_get(m->root, "macros"), name) != NULL; }
void macros_screen(const macros_t *m, int *w, int *h) { *w = m->w; *h = m->h; }

/* ---- running ---- */
typedef struct {
    const macros_t *m; mcp_t *mcp; macro_vision_fn vision; void *user;
    const char *names[MAX_PARAMS]; double vals[MAX_PARAMS]; const char *svals[MAX_PARAMS]; int np;
    int steps; int sw, sh; char *msg; size_t cap;
} run_t;

static int fail(run_t *r, const char *fmt, const char *a, int i, const char *macro) {
    char lab[200]; snprintf(lab, sizeof lab, fmt, a ? a : "");
    snprintf(r->msg, r->cap, "stopped in %s at step %d: %s", macro, i, lab); return -1;
}

static void subst(run_t *r, const char *in, char *out, size_t cap) {          /* {param} -> its text */
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < cap;) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (e) { for (int i = 0; i < r->np; i++) if (strlen(r->names[i]) == (size_t)(e - p - 1) && !strncmp(r->names[i], p + 1, (size_t)(e - p - 1))) {
                        char t[80]; if (r->svals[i]) snprintf(t, sizeof t, "%s", r->svals[i]); else snprintf(t, sizeof t, "%g", r->vals[i]);
                        size_t l = strlen(t); if (n + l + 1 < cap) { memcpy(out + n, t, l); n += l; } p = e + 1; goto next; } }
        }
        out[n++] = *p++;
    next:;
    }
    out[n] = 0;
}

static int touch(run_t *r, const char *tool, int x, int y, jv *step, const char *macro, int idx, int require_default) {
    char args[160], text[600];
    int hold = (int)djson_num(step, "hold_ms", 0);
    if (hold > 0) snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d,\"%s\":%d,\"screenshot\":\"none\"}", x, y, !strcmp(tool, "long_press") ? "duration_ms" : "hold_ms", hold);
    else snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d,\"screenshot\":\"none\"}", x, y);
    if (x < 0 || y < 0 || x >= r->sw || y >= r->sh) { char c[60]; snprintf(c, sizeof c, "(%d, %d) is off the %dx%d screen", x, y, r->sw, r->sh); return fail(r, "%s", c, idx, macro); }
    if (mcp_call(r->mcp, tool, args, text, sizeof text, NULL) < 0) return fail(r, "%.150s", text, idx, macro);
    jv *rc = djson_get(step, "require_change");
    if ((rc ? rc->num != 0 : require_default) && strstr(text, "did not change")) return fail(r, "the screen did not change after touching (%s)", args, idx, macro);
    return 0;
}

static int run_steps(run_t *r, jv *steps, const char *macro, int depth);

static int run_macro(run_t *r, const char *name, int depth) {
    if (depth > MAX_DEPTH) { snprintf(r->msg, r->cap, "macros nested too deep at %s", name); return -1; }
    jv *mc = djson_get(djson_get(r->m->root, "macros"), name);
    if (!mc) { snprintf(r->msg, r->cap, "no macro called %s", name); return -1; }
    return run_steps(r, djson_get(mc, "steps"), name, depth);
}

static int run_steps(run_t *r, jv *steps, const char *macro, int depth) {
    int i = 0;
    for (jv *s = steps->kid; s; s = s->next) {
        i++;
        if (++r->steps > MAX_STEPS_PER_RUN) return fail(r, "%s", "too many steps in one run", i, macro);
        jv *a;
        if ((a = djson_get(s, "if_param")) && a->t == 's') {   /* only when that text parameter was given */
            int given = 0; for (int q = 0; q < r->np; q++) if (!strcmp(r->names[q], a->str) && r->svals[q] && r->svals[q][0]) given = 1;
            if (!given) continue;
        }
        if ((a = djson_get(s, "skip_if"))) {           /* the screen already looks as this step would leave it: skip it */
            jv *f = djson_get(djson_get(r->m->root, "fingerprints"), a->str), *rg = djson_get(f, "region"); uint8_t want[FP_BYTES], got[FP_BYTES]; char e2[160] = "";
            fp_unhex(djson_str(f, "thumb", ""), want);
            if (fp_capture(r->mcp, (int)djson_at(rg, 0)->num, (int)djson_at(rg, 1)->num, (int)djson_at(rg, 2)->num, (int)djson_at(rg, 3)->num, got, e2, sizeof e2)) return fail(r, "could not check the screen: %.120s", e2, i, macro);
            if (fp_distance(want, got) <= djson_num(f, "tolerance", 12)) continue;
        }
        if ((a = djson_get(s, "tap")) || (a = djson_get(s, "double_tap")) || (a = djson_get(s, "long_press"))) {
            const char *tool = djson_get(s, "tap") ? "tap" : djson_get(s, "double_tap") ? "double_tap" : "long_press";
            double x, y;
            if (eval_jv(djson_at(a, 0), r->names, r->vals, r->np, &x) || eval_jv(djson_at(a, 1), r->names, r->vals, r->np, &y)) return fail(r, "%s", "bad coordinate expression", i, macro);
            if (touch(r, tool, (int)lround(x), (int)lround(y), s, macro, i, 0)) return -1;
        } else if ((a = djson_get(s, "press"))) {
            char name[80], args[160], text[300]; subst(r, a->str, name, sizeof name);
            snprintf(args, sizeof args, "{\"name\":\"%s\",\"screenshot\":\"none\"}", name);
            for (char *c = name; *c; c++) if (*c == '"' || *c == '\\') return fail(r, "%s", "bad button name", i, macro);
            if (mcp_call(r->mcp, "press_button", args, text, sizeof text, NULL) < 0) return fail(r, "%.150s", text, i, macro);
        } else if ((a = djson_get(s, "wait"))) {
            double ms; if (eval_jv(a, r->names, r->vals, r->np, &ms) || ms < 0 || ms > 5000) return fail(r, "%s", "wait must be 0 to 5000 ms", i, macro);
            usleep((useconds_t)(ms * 1000));
        } else if (djson_get(s, "wait_stable") || djson_get(s, "wait_change")) {
            char text[300]; int stable = djson_get(s, "wait_stable") != NULL; jv *t = djson_get(s, stable ? "wait_stable" : "wait_change");
            char args[120]; snprintf(args, sizeof args, "{\"until\":\"%s\",\"timeout_ms\":%d,\"screenshot\":\"none\"}", stable ? "stable" : "change", t && t->t == 'n' ? (int)t->num : 2000);
            if (mcp_call(r->mcp, "wait_for_screen", args, text, sizeof text, NULL) < 0) return fail(r, "%.150s", text, i, macro);
            if (!stable && strstr(text, "did not change")) return fail(r, "%s", "the screen never changed", i, macro);
        } else if ((a = djson_get(s, "scroll"))) {
            double x, y; char args[200], text[300];
            if (eval_jv(djson_at(a, 0), r->names, r->vals, r->np, &x) || eval_jv(djson_at(a, 1), r->names, r->vals, r->np, &y)) return fail(r, "%s", "bad coordinate expression", i, macro);
            const char *dir = djson_str(s, "direction", "down");
            if (strcmp(dir, "up") && strcmp(dir, "down") && strcmp(dir, "left") && strcmp(dir, "right")) return fail(r, "%s", "bad scroll direction", i, macro);
            snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d,\"direction\":\"%s\",\"amount\":%d,\"screenshot\":\"none\"}", (int)lround(x), (int)lround(y), dir, (int)djson_num(s, "amount", 1));
            if (mcp_call(r->mcp, "scroll", args, text, sizeof text, NULL) < 0) return fail(r, "%.150s", text, i, macro);
        } else if ((a = djson_get(s, "find_tap"))) {
            char target[160], hint[200] = "", why[200] = "", text[300]; char *png = NULL; int x = 0, y = 0;
            subst(r, a->str, target, sizeof target); subst(r, djson_str(s, "hint", ""), hint, sizeof hint);
            if (!r->vision) return fail(r, "%s", "no vision available to find things on screen", i, macro);
            int tries = (int)djson_num(s, "tries", 0), rc = -1; jv *sa = djson_get(s, "scroll_at");   /* tries: scroll the list at scroll_at by amount and look again */
            for (int t = 0; t <= tries; t++) {
                if (t > 0) {
                    double sx, sy; if (!sa || eval_jv(djson_at(sa, 0), r->names, r->vals, r->np, &sx) || eval_jv(djson_at(sa, 1), r->names, r->vals, r->np, &sy)) return fail(r, "%s", "tries needs scroll_at", i, macro);
                    char sargs[160]; snprintf(sargs, sizeof sargs, "{\"x\":%d,\"y\":%d,\"direction\":\"down\",\"amount\":%d,\"screenshot\":\"none\"}", (int)lround(sx), (int)lround(sy), (int)djson_num(s, "amount", 400));
                    if (mcp_call(r->mcp, "scroll", sargs, text, sizeof text, NULL) < 0) return fail(r, "%.150s", text, i, macro);
                }
                if (mcp_call(r->mcp, "screenshot", "{\"zoom\":1}", text, sizeof text, &png) < 0 || !png) { free(png); return fail(r, "%s", "could not read the screen", i, macro); }
                rc = r->vision(r->user, target, hint, png, r->sw, r->sh, &x, &y, why, sizeof why); free(png); png = NULL;
                if (rc >= 0) break;
            }
            if (rc < 0) return fail(r, "could not find on screen: %.160s", why[0] ? why : target, i, macro);
            if (touch(r, djson_num(s, "double", 0) != 0 ? "double_tap" : "tap", x, y, s, macro, i, 1)) return -1;      /* a found target must do something, unless the step says otherwise; "double": 1 opens a folder or row */
        } else if ((a = djson_get(s, "find_empty"))) {
            /* the first of `count` cells (centres origin + k*step) that looks like the stored "empty cell" fingerprint, by brightness only */
            jv *o = djson_get(a, "origin"), *st = djson_get(a, "step"), *sz = djson_get(a, "size"); double ox, oy, dx, dy;
            jv *f = djson_get(djson_get(r->m->root, "fingerprints"), djson_str(a, "like", "")); uint8_t want[FP_BYTES], got[FP_BYTES]; fp_unhex(djson_str(f, "thumb", ""), want);
            if (eval_jv(djson_at(o, 0), r->names, r->vals, r->np, &ox) || eval_jv(djson_at(o, 1), r->names, r->vals, r->np, &oy) || eval_jv(djson_at(st, 0), r->names, r->vals, r->np, &dx) || eval_jv(djson_at(st, 1), r->names, r->vals, r->np, &dy)) return fail(r, "%s", "bad coordinate expression", i, macro);
            int cw = (int)djson_at(sz, 0)->num, ch = (int)djson_at(sz, 1)->num, n = (int)djson_num(a, "count", 1), hit = -1; double tol = djson_num(f, "tolerance", 12); char e2[160] = "";
            for (int k = 0; k < n && hit < 0; k++) {
                int cx = (int)lround(ox + k * dx), cy = (int)lround(oy + k * dy), x0 = cx - cw / 2, y0 = cy - ch / 2;
                if (x0 < 0 || y0 < 0 || x0 + cw > r->sw || y0 + ch > r->sh) break;       /* ran off the screen: stop looking */
                if (fp_capture(r->mcp, x0, y0, cw, ch, got, e2, sizeof e2)) return fail(r, "could not check the screen: %.120s", e2, i, macro);
                if (fp_distance(want, got) <= tol) hit = k;
            }
            if (hit < 0) return fail(r, "%s", "no empty slot among the cells checked", i, macro);
            if (touch(r, "tap", (int)lround(ox + hit * dx), (int)lround(oy + hit * dy), s, macro, i, 1)) return -1;
        } else if ((a = djson_get(s, "expect"))) {
            jv *f = djson_get(djson_get(r->m->root, "fingerprints"), a->str); jv *rg = djson_get(f, "region"); uint8_t want[FP_BYTES], got[FP_BYTES];
            double tol = djson_num(f, "tolerance", 12), dist = 1e9; fp_unhex(djson_str(f, "thumb", ""), want);
            long timeout = (long)djson_num(s, "timeout_ms", 2000), waited = 0; char e2[160] = "";
            for (;;) {
                if (fp_capture(r->mcp, (int)djson_at(rg, 0)->num, (int)djson_at(rg, 1)->num, (int)djson_at(rg, 2)->num, (int)djson_at(rg, 3)->num, got, e2, sizeof e2) == 0) { dist = fp_distance(want, got); if (dist <= tol) break; }
                if (waited >= timeout) { char c[200]; if (dist > 1e8) snprintf(c, sizeof c, "could not check the screen: %.120s", e2); else snprintf(c, sizeof c, "the screen does not look like \"%.40s\" (difference %.1f, allowed %.1f)", a->str, dist, tol); return fail(r, "%s", c, i, macro); }
                usleep(150000); waited += 150;
            }
        } else if ((a = djson_get(s, "call"))) {
            if (run_macro(r, a->str, depth + 1)) return -1;
        } else if ((a = djson_get(s, "repeat"))) {
            double n; if (eval_jv(a, r->names, r->vals, r->np, &n) || n < 0 || n > 20) return fail(r, "%s", "repeat must be 0 to 20", i, macro);
            for (int k = 0; k < (int)n; k++) if (run_steps(r, djson_get(s, "steps"), macro, depth + 1)) return -1;
        }
    }
    return 0;
}

int macro_run(const macros_t *m, mcp_t *mcp, const char *name, const char *args_json, macro_vision_fn vision, void *user, char *result, size_t cap) {
    run_t r; memset(&r, 0, sizeof r); r.m = m; r.mcp = mcp; r.vision = vision; r.user = user; r.msg = result; r.cap = cap;
    jv *mc = m ? djson_get(djson_get(m->root, "macros"), name) : NULL;
    if (!mc) { snprintf(result, cap, "no macro called %s", name); return -1; }
    jv *args = djson_parse(args_json, strlen(args_json));
    if (!args || args->t != 'o') { snprintf(result, cap, "bad arguments"); djson_free(args); return -1; }
    jv *ps = djson_get(mc, "params");
    for (jv *p = ps ? ps->kid : NULL; p && r.np < MAX_PARAMS; p = p->next) {
        jv *av = djson_get(args, p->key); const char *type = djson_str(p, "type", "int");
        int opt = djson_get(p, "optional") && djson_get(p, "optional")->t == 'b' && djson_get(p, "optional")->num != 0;
        if (opt && !strcmp(type, "string") && (!av || (av->t == 's' && !av->str[0]))) { r.names[r.np] = p->key; r.svals[r.np] = ""; r.np++; continue; }   /* an optional text left out: steps with if_param on it are skipped */
        if (!av) { snprintf(result, cap, "missing parameter %s", p->key); djson_free(args); return -1; }
        r.names[r.np] = p->key;
        if (!strcmp(type, "string")) { if (av->t != 's' || !av->str[0] || strlen(av->str) > 80) { snprintf(result, cap, "parameter %s must be a short text", p->key); djson_free(args); return -1; } r.svals[r.np] = av->str; }
        else {
            if (av->t != 'n') { snprintf(result, cap, "parameter %s must be a number", p->key); djson_free(args); return -1; }
            double lo = djson_num(p, "min", -1e9), hi = djson_num(p, "max", 1e9);
            if (av->num < lo || av->num > hi) { snprintf(result, cap, "parameter %s must be %g to %g", p->key, lo, hi); djson_free(args); return -1; }
            r.vals[r.np] = av->num;
        }
        r.np++;
    }
    char text[1200];
    int rc = mcp_call(mcp, "get_screen_info", "{}", text, sizeof text, NULL);
    int w = 0, h = 0; char *j = strchr(text, '{');
    if (rc == 0 && j) { char *e = strchr(j, '}'); if (e) { jv *v = djson_parse(j, (size_t)(e - j + 1)); if (v) { w = (int)djson_num(v, "width", 0); h = (int)djson_num(v, "height", 0); djson_free(v); } } }
    if (rc < 0 || w <= 0 || h <= 0) { snprintf(result, cap, "could not read the screen size from the remote addin"); djson_free(args); return -1; }
    if (w != m->w || h != m->h) { snprintf(result, cap, "the macros were recorded for a %dx%d screen, this one is %dx%d", m->w, m->h, w, h); djson_free(args); return -1; }
    r.sw = w; r.sh = h;
    rc = run_macro(&r, name, 0);
    if (rc == 0) snprintf(result, cap, "ran %s (%d steps)", name, r.steps);
    djson_free(args);
    return rc;
}
