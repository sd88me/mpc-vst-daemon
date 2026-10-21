/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* Gemini <-> Claude translation (see daemon_claude.h). */
#include "daemon_claude.h"
#include "daemon_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct { char *p; size_t n, cap; int bad; } sb_t;
static void put(sb_t *s, const char *t) { size_t l = strlen(t); if (s->bad || s->n + l + 1 > s->cap) { s->bad = 1; return; } memcpy(s->p + s->n, t, l); s->n += l; s->p[s->n] = 0; }
static void putv(sb_t *s, const jv *v) {          /* a JSON value, compact */
    if (s->bad) return;
    int l = djson_write(v, s->p + s->n, s->cap - s->n); if (l < 0) s->bad = 1; else s->n += (size_t)l;
}
static void puts_(sb_t *s, const char *str) { jv v = { 's', 0, (char *)str, NULL, NULL, NULL }; putv(s, &v); }

static void lower_types(jv *v) {                  /* Gemini schemas say "OBJECT", JSON Schema says "object" */
    for (jv *k = v ? v->kid : NULL; k; k = k->next) {
        if (k->key && !strcmp(k->key, "type") && k->t == 's') for (char *c = k->str; *c; c++) *c = (char)tolower((unsigned char)*c);
        lower_types(k);
    }
}

static int has_audio(jv *contents) {
    for (jv *m = contents ? contents->kid : NULL; m; m = m->next)
        for (jv *p = djson_get(m, "parts") ? djson_get(m, "parts")->kid : NULL; p; p = p->next) {
            jv *in = djson_get(p, "inline_data"); if (!in) in = djson_get(p, "inlineData");
            const char *mt = djson_str(in, "mime_type", djson_str(in, "mimeType", ""));
            if (!strncmp(mt, "audio/", 6)) return 1;
        }
    return 0;
}

int claude_request(const char *gb, const char *model, char *out, size_t cap) {
    jv *g = djson_parse(gb, strlen(gb)); if (!g) return -1;
    jv *contents = djson_get(g, "contents");
    if (has_audio(contents)) { djson_free(g); return CLAUDE_NEEDS_AUDIO; }
    sb_t s = { out, 0, cap, 0 }; if (cap) out[0] = 0;
    put(&s, "{\"model\":"); puts_(&s, model); put(&s, ",\"max_tokens\":16000");

    /* system: the Gemini system instruction, plus the reply schema when Gemini was asked for JSON */
    jv *si = djson_get(g, "system_instruction"); if (!si) si = djson_get(g, "systemInstruction");
    jv *gc = djson_get(g, "generationConfig"), *schema = djson_get(gc, "responseSchema");
    size_t syl = 64; for (jv *p = djson_get(si, "parts") ? djson_get(si, "parts")->kid : NULL; p; p = p->next) syl += strlen(djson_str(p, "text", "")) + 1;
    char *sch = NULL; if (schema) { lower_types(schema); sch = malloc(20000); if (djson_write(schema, sch, 20000) < 0) sch[0] = 0; syl += strlen(sch) + 120; }
    char *sys = malloc(syl); sys[0] = 0;
    for (jv *p = djson_get(si, "parts") ? djson_get(si, "parts")->kid : NULL; p; p = p->next) { if (sys[0]) strcat(sys, "\n"); strcat(sys, djson_str(p, "text", "")); }
    if (sch) { strcat(sys, "\n\nReply with only one JSON object (no code fence, no other text) that follows this JSON Schema: "); strcat(sys, sch); }
    if (sys[0]) { put(&s, ",\"system\":"); puts_(&s, sys); }
    free(sys); free(sch);

    /* tools: functionDeclarations -> tools with input_schema */
    int nt = 0;
    for (jv *t = djson_get(g, "tools") ? djson_get(g, "tools")->kid : NULL; t; t = t->next)
        for (jv *f = djson_get(t, "functionDeclarations") ? djson_get(t, "functionDeclarations")->kid : NULL; f; f = f->next) {
            put(&s, nt++ ? "," : ",\"tools\":["); put(&s, "{\"name\":"); puts_(&s, djson_str(f, "name", ""));
            const char *ds = djson_str(f, "description", NULL); if (ds) { put(&s, ",\"description\":"); puts_(&s, ds); }
            jv *ps = djson_get(f, "parameters"); put(&s, ",\"input_schema\":");
            if (ps) { lower_types(ps); putv(&s, ps); } else put(&s, "{\"type\":\"object\",\"properties\":{}}");
            put(&s, "}");
        }
    if (nt) put(&s, "]");

    /* messages: tool calls get ids from their place (message i, call k); the results in the next message answer them in order */
    put(&s, ",\"messages\":[");
    int mi = 0, nm = 0;
    for (jv *m = contents ? contents->kid : NULL; m; m = m->next, mi++) {
        int asst = !strcmp(djson_str(m, "role", "user"), "model");
        size_t mark = s.n; int nb = 0, kc = 0, kr = 0;
        put(&s, nm ? "," : ""); put(&s, asst ? "{\"role\":\"assistant\",\"content\":[" : "{\"role\":\"user\",\"content\":[");
        for (jv *p = djson_get(m, "parts") ? djson_get(m, "parts")->kid : NULL; p; p = p->next) {
            const char *tx = djson_str(p, "text", NULL); jv *fc = djson_get(p, "functionCall"), *fr = djson_get(p, "functionResponse");
            jv *in = djson_get(p, "inline_data"); if (!in) in = djson_get(p, "inlineData");
            char id[32];
            if (djson_get(p, "thought")) continue;
            if (tx && tx[0]) { put(&s, nb++ ? "," : ""); put(&s, "{\"type\":\"text\",\"text\":"); puts_(&s, tx); put(&s, "}"); }
            else if (fc) {
                snprintf(id, sizeof id, "toolu_%d_%d", mi, kc++);
                put(&s, nb++ ? "," : ""); put(&s, "{\"type\":\"tool_use\",\"id\":\""); put(&s, id); put(&s, "\",\"name\":"); puts_(&s, djson_str(fc, "name", ""));
                jv *a = djson_get(fc, "args"); put(&s, ",\"input\":"); if (a) putv(&s, a); else put(&s, "{}"); put(&s, "}");
            } else if (fr) {
                snprintf(id, sizeof id, "toolu_%d_%d", mi - 1, kr++);
                jv *r = djson_get(fr, "response"); char *rt = malloc(400000);
                if (!r || djson_write(r, rt, 400000) < 0) snprintf(rt, 400000, "{}");
                put(&s, nb++ ? "," : ""); put(&s, "{\"type\":\"tool_result\",\"tool_use_id\":\""); put(&s, id); put(&s, "\",\"content\":"); puts_(&s, rt); put(&s, "}");
                free(rt);
            } else if (in) {
                put(&s, nb++ ? "," : ""); put(&s, "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":"); puts_(&s, djson_str(in, "mime_type", djson_str(in, "mimeType", "image/png")));
                put(&s, ",\"data\":"); puts_(&s, djson_str(in, "data", "")); put(&s, "}}");
            }
        }
        if (!nb) { s.n = mark; if (cap) out[s.n] = 0; continue; }   /* Claude refuses empty messages */
        put(&s, "]}"); nm++;
    }
    put(&s, "]}");
    djson_free(g);
    return s.bad ? -1 : (int)s.n;
}

int claude_response(const char *cr, char *out, size_t cap) {
    jv *r = djson_parse(cr, strlen(cr));
    sb_t s = { out, 0, cap, 0 }; if (cap) out[0] = 0;
    jv *content = djson_get(r, "content");
    if (!r || !content || content->t != 'a') {
        const char *m = djson_str(djson_get(r, "error"), "message", r ? "no reply from the model" : "reply is not JSON");
        put(&s, "{\"error\":{\"message\":"); puts_(&s, m); put(&s, "}}"); djson_free(r); return s.bad ? -1 : (int)s.n;
    }
    put(&s, "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[");
    int nb = 0;
    for (jv *b = content->kid; b; b = b->next) {
        const char *ty = djson_str(b, "type", "");
        if (!strcmp(ty, "text")) {
            char *t = strdup(djson_str(b, "text", "")), *a = t;           /* a JSON reply may still come in a code fence */
            if (!strncmp(a, "```", 3)) { a = strchr(a, '\n'); a = a ? a + 1 : t + 3; char *e = strstr(a, "```"); if (e) *e = 0; }
            if (*a) { put(&s, nb++ ? "," : ""); put(&s, "{\"text\":"); puts_(&s, a); put(&s, "}"); }
            free(t);
        } else if (!strcmp(ty, "tool_use")) {
            put(&s, nb++ ? "," : ""); put(&s, "{\"functionCall\":{\"name\":"); puts_(&s, djson_str(b, "name", ""));
            jv *in = djson_get(b, "input"); put(&s, ",\"args\":"); if (in) putv(&s, in); else put(&s, "{}"); put(&s, "}}");
        }
    }
    put(&s, "]}}]}");
    djson_free(r);
    return s.bad ? -1 : (int)s.n;
}

int claude_models(const char *json, char names[][64], int max) {
    jv *r = djson_parse(json, strlen(json)); int n = 0; if (!r) return 0;
    for (jv *m = djson_get(r, "data") ? djson_get(r, "data")->kid : NULL; m && n < max; m = m->next) {
        const char *id = djson_str(m, "id", "");
        if (!strncmp(id, "claude", 6) && strlen(id) < 64) snprintf(names[n++], 64, "%s", id);
    }
    djson_free(r); return n;
}
