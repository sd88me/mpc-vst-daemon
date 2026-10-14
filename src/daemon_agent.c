/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#define _GNU_SOURCE
#include "daemon_agent.h"
#include <strings.h>
#include "daemon_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *READ_ONLY[] = { "get_project", "list_plugins", "get_plugin_params" };
static const char *MUTATING[] = { "set_param", "transport", "play_part", "select_track", "load_efx", "load_instrument", "import_clip" };
int agent_tool_is_read_only(const char *n) { for (int i = 0; i < 3; i++) if (!strcmp(n, READ_ONLY[i])) return 1; return 0; }
int agent_tool_is_ui(const char *n) { return !strcmp(n, "select_track") || !strcmp(n, "load_efx") || !strcmp(n, "load_instrument") || !strcmp(n, "import_clip"); }
int agent_tool_known(const char *n) { if (agent_tool_is_read_only(n)) return 1; for (size_t i = 0; i < sizeof MUTATING / sizeof *MUTATING; i++) if (!strcmp(n, MUTATING[i])) return 1; return 0; }

/* ---- history ---- */
void agent_hist_init(ahist_t *h) { memset(h, 0, sizeof *h); }
void agent_hist_free(ahist_t *h) { for (int i = 0; i < h->n; i++) free(h->item[i]); h->n = 0; }
static int hist_add(ahist_t *h, const char *s) { if (h->n >= AGENT_MAX_HISTORY) return -1; h->item[h->n++] = strdup(s); return 0; }

static char *jstr_alloc(const char *s) {            /* the JSON text of a string, malloc'd */
    jv v = { 's', 0, (char *)s, NULL, NULL, NULL }; size_t cap = strlen(s) * 6 + 8; char *o = malloc(cap);
    if (djson_write(&v, o, cap) < 0) { free(o); return NULL; } return o;
}

int agent_hist_user(ahist_t *h, const char *text, const unsigned char *wav, size_t wav_len) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *t = jstr_alloc(text); if (!t) return -1;
    size_t cap = strlen(t) + (wav ? wav_len * 4 / 3 + 16 : 0) + 160, n = 0; char *o = malloc(cap);
    n += (size_t)snprintf(o + n, cap - n, "{\"role\":\"user\",\"parts\":[");
    if (wav) {
        n += (size_t)snprintf(o + n, cap - n, "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"");
        for (size_t i = 0; i < wav_len; i += 3) {
            unsigned v = (unsigned)wav[i] << 16 | (i + 1 < wav_len ? (unsigned)wav[i + 1] << 8 : 0) | (i + 2 < wav_len ? wav[i + 2] : 0);
            o[n++] = b64[v >> 18 & 63]; o[n++] = b64[v >> 12 & 63]; o[n++] = i + 1 < wav_len ? b64[v >> 6 & 63] : '='; o[n++] = i + 2 < wav_len ? b64[v & 63] : '=';
        }
        n += (size_t)snprintf(o + n, cap - n, "\"}},");
    }
    n += (size_t)snprintf(o + n, cap - n, "{\"text\":%s}]}", t);
    int r = hist_add(h, o); free(o); free(t); return r;
}

int agent_hist_model(ahist_t *h, const aturn_t *t) { return t->raw ? hist_add(h, t->raw) : -1; }

int agent_hist_results(ahist_t *h, const aturn_t *t, const char *const *results) {
    size_t cap = 256; for (int i = 0; i < t->n; i++) cap += (results && results[i] ? strlen(results[i]) : 16) * 6 + 160;
    char *o = malloc(cap); size_t n = 0;
    n += (size_t)snprintf(o + n, cap - n, "{\"role\":\"user\",\"parts\":[");
    for (int i = 0; i < t->n; i++) {
        char *name = jstr_alloc(t->calls[i].name), *res = jstr_alloc(results && results[i] ? results[i] : "not run");
        n += (size_t)snprintf(o + n, cap - n, "%s{\"functionResponse\":{\"name\":%s,\"response\":{\"result\":%s}}}", i ? "," : "", name, res);
        free(name); free(res);
    }
    n += (size_t)snprintf(o + n, cap - n, "]}");
    int r = hist_add(h, o); free(o); return r;
}

/* ---- request ---- */
static const char *TOOLS =
 "[{\"functionDeclarations\":["
 "{\"name\":\"get_project\",\"description\":\"The MPC project as last saved: tempo, key, the current sequence, and every track with its name, kind and plugin. Read-only.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{}}},"
 "{\"name\":\"list_plugins\",\"description\":\"The plugin instances currently loaded in MPC: id, name, vendor and parameter count. Read-only.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{}}},"
 "{\"name\":\"get_plugin_params\",\"description\":\"Parameters of one plugin instance with their index, name, 0-1 value and the text MPC shows. A synth can have hundreds: pass match, words separated by | (for example attack|decay|cutoff|filter|env), to get only those whose name contains one. Read-only.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"id\":{\"type\":\"INTEGER\"},\"match\":{\"type\":\"STRING\"}},\"required\":[\"id\"]}},"
 "{\"name\":\"set_param\",\"description\":\"Set one plugin parameter to a value between 0 and 1. The result reports the text MPC then shows, so a value can be corrected.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"id\":{\"type\":\"INTEGER\"},\"index\":{\"type\":\"INTEGER\"},\"value\":{\"type\":\"NUMBER\"}},\"required\":[\"id\",\"index\",\"value\"]}},"
 "{\"name\":\"transport\",\"description\":\"Press a transport button on the MPC.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"cmd\":{\"type\":\"STRING\",\"enum\":[\"play\",\"stop\",\"continue\",\"record\"]}},\"required\":[\"cmd\"]}},"
 "{\"name\":\"play_part\",\"description\":\"Make a MIDI part and loop it from Daemon's own MIDI port, in time with the MPC transport. Notes use beats from the start of the part.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
   "\"name\":{\"type\":\"STRING\"},\"channel\":{\"type\":\"INTEGER\"},\"notes\":{\"type\":\"ARRAY\",\"description\":\"each note is [pitch, start_time, duration, velocity]\",\"items\":{\"type\":\"ARRAY\",\"items\":{\"type\":\"NUMBER\"}}}},\"required\":[\"notes\"]}}"
 ;
static const char *TOOL_SELECT =
 ",{\"name\":\"select_track\",\"description\":\"Select a track on the MPC's screen by touching it (a recorded sequence of touches). Needed before anything that works on the selected track.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"track\":{\"type\":\"INTEGER\"}},\"required\":[\"track\"]}}";
static const char *TOOL_LOAD =
 ",{\"name\":\"load_efx\",\"description\":\"Load an effect plugin into the first free insert slot of a track's mixer strip, finding it in the MPC's plugin browser by name. Selects the track itself. Not for instruments: use load_instrument for those.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"track\":{\"type\":\"INTEGER\"},\"plugin\":{\"type\":\"STRING\",\"description\":\"the plugin's name as listed in the MPC, e.g. 9W9\"}},\"required\":[\"track\",\"plugin\"]}}";
static const char *TOOL_INSTR =
 ",{\"name\":\"load_instrument\",\"description\":\"Load a plugin as the instrument (the sound source) of a plugin track, finding it in the MPC's plugin browser by name. Replaces the track's current instrument.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"track\":{\"type\":\"INTEGER\"},\"plugin\":{\"type\":\"STRING\",\"description\":\"the instrument's name as listed in the MPC, e.g. Hera\"}},\"required\":[\"track\",\"plugin\"]}}";
static const char *TOOL_IMPORT =
 ",{\"name\":\"import_clip\",\"description\":\"Save the part Daemon has made (after play_part) as a MIDI file and load it into the first empty clip slot of a track in the clip matrix, by touching the screen. The track number is the musician's, counted from 1.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"track\":{\"type\":\"INTEGER\"}},\"required\":[\"track\"]}}";
static const char *TOOLS_END = "]}]";

int agent_build_request(const actx_t *ctx, const ahist_t *h, char *buf, size_t cap) {
    char sys[4200];
    snprintf(sys, sizeof sys,
        "You help a musician run an Akai MPC by voice or text. You can read the project and plugins, set plugin parameters, press transport buttons and make MIDI parts, all through the tools. "
        "Look first when you need facts (get_project, list_plugins, get_plugin_params), then ask for every change in one go: the changes are shown to the user as a plan and only run when they press RUN, so do not ask them to confirm in words. "
        "Parts: each note is an array [pitch, start_time, duration, velocity], positions and lengths are in beats (1.0 = a quarter note), pitches are MIDI note numbers, General MIDI drum notes for beats (36 kick, 38 snare, 42 closed hat, 46 open hat, 39 clap), channel 2 unless told otherwise. "
        "Keep melodic notes in the musician's scale unless they ask otherwise. Plugin values run 0 to 1 and the result of set_param tells you the text MPC shows, so use it to land on musical settings. "
        "Track numbers in get_project start at 0: the musician's track 1 is index 0, so say track numbers to them as index plus one. The screen tools (select_track, load_efx, load_instrument, import_clip) count tracks from 1, as the musician does. Daemon's MIDI channel is set to %d: use it for play_part unless the musician names another channel. import_clip selects the track itself: do not call select_track before it. You cannot rename or delete anything: say so briefly if asked, and do what you can. Loading an effect or an instrument is only possible when load_efx or load_instrument exists; otherwise say you cannot. A sound request (a plucky lead, a warm pad) on a synth: load_instrument if it is not loaded yet, then shape it with get_plugin_params and set_param. Selecting or loading touches the screen and is slow, so do it only when asked. "
        "Finish with one or two plain sentences saying what the plan does. Tempo %.0f BPM, default scale %s, default length %d bars. %s",
        ctx->channel > 0 ? ctx->channel : 1, ctx->bpm, ctx->scale, ctx->bars, ctx->typed_context ? ctx->typed_context : "");
    char *s = jstr_alloc(sys); if (!s) return -1;
    size_t n = 0; int w;
    w = snprintf(buf, cap, "{\"system_instruction\":{\"parts\":[{\"text\":%s}]},\"tools\":%s%s%s%s%s%s,\"contents\":[", s, TOOLS, (ctx->has_ui & 1) ? TOOL_SELECT : "", (ctx->has_ui & 2) ? TOOL_LOAD : "", (ctx->has_ui & 8) ? TOOL_INSTR : "", (ctx->has_ui & 4) ? TOOL_IMPORT : "", TOOLS_END); free(s);
    if (w < 0 || (size_t)w >= cap) return -1;
    n = (size_t)w;
    for (int i = 0; i < h->n; i++) {
        size_t l = strlen(h->item[i]);
        if (n + l + 4 >= cap) return -1;
        if (i) buf[n++] = ',';
        memcpy(buf + n, h->item[i], l); n += l;
    }
    static const char tail[] = "],\"generationConfig\":{\"thinkingConfig\":{\"thinkingLevel\":\"low\"}}}";
    if (n + sizeof tail >= cap) return -1;
    memcpy(buf + n, tail, sizeof tail); n += sizeof tail - 1;
    return (int)n;
}

void agent_turn_free(aturn_t *t) { free(t->raw); t->raw = NULL; }

int agent_parse_turn(const char *response, aturn_t *t) {
    memset(t, 0, sizeof *t);
    jv *root = djson_parse(response, strlen(response));
    if (!root) { snprintf(t->text, sizeof t->text, "reply is not JSON"); return -1; }
    jv *content = djson_get(djson_at(djson_get(root, "candidates"), 0), "content");
    jv *parts = djson_get(content, "parts");
    if (!parts) {
        const char *m = djson_str(djson_get(root, "error"), "message", NULL);
        snprintf(t->text, sizeof t->text, "%s", m ? m : "no reply from the model");
        djson_free(root); return -1;
    }
    size_t tl = 0;
    for (jv *p = parts->kid; p; p = p->next) {
        const char *tx = djson_str(p, "text", NULL);
        if (tx && !djson_get(p, "thought")) { size_t l = strlen(tx); if (tl + l + 2 < sizeof t->text) { if (tl) t->text[tl++] = ' '; memcpy(t->text + tl, tx, l); tl += l; t->text[tl] = 0; } }
        jv *fc = djson_get(p, "functionCall");
        if (fc && t->n < AGENT_MAX_CALLS) {
            acall_t *c = &t->calls[t->n];
            snprintf(c->name, sizeof c->name, "%s", djson_str(fc, "name", ""));
            jv *args = djson_get(fc, "args");
            if (args) { if (djson_write(args, c->args, sizeof c->args) < 0) snprintf(c->args, sizeof c->args, "{}"); } else snprintf(c->args, sizeof c->args, "{}");
            t->n++;
        }
    }
    size_t cap = 400000; t->raw = malloc(cap);
    if (djson_write(content, t->raw, cap) < 0) { free(t->raw); t->raw = NULL; }
    djson_free(root);
    return 0;
}

/* ---- running tools ---- */
static int jerr(char *r, size_t cap, const char *msg) { jv v = { 's', 0, (char *)msg, NULL, NULL, NULL }; char e[300]; djson_write(&v, e, sizeof e); snprintf(r, cap, "{\"error\":%s}", e); return -1; }

int agent_exec(cmdr_t *c, const acall_t *call, agent_play_fn play, agent_ui_fn ui, void *user, char *result, size_t cap) {
    jv *a = djson_parse(call->args, strlen(call->args));
    int rc = 0;
    if (!a) return jerr(result, cap, "bad arguments");
    if (!strcmp(call->name, "get_project")) { if (!c) rc = jerr(result, cap, "no commander connection"); else if (cmdr_project(c, result, cap) < 0) rc = -1; }
    else if (!strcmp(call->name, "list_plugins")) { if (!c) rc = jerr(result, cap, "no commander connection"); else if (cmdr_plugins_summary(c, result, cap) < 0) rc = jerr(result, cap, "too many plugins to list"); }
    else if (!strcmp(call->name, "get_plugin_params")) { if (!c) rc = jerr(result, cap, "no commander connection"); else if (cmdr_plugin_params_match(c, (int)djson_num(a, "id", -1), djson_str(a, "match", ""), result, cap) < 0) rc = jerr(result, cap, "no plugin with that id, or the list is too long: pass match"); }
    else if (!strcmp(call->name, "set_param")) {
        char text[96];
        if (!c) rc = jerr(result, cap, "no commander connection");
        else if (cmdr_set(c, (int)djson_num(a, "id", -1), (int)djson_num(a, "index", -1), djson_num(a, "value", 0), text, sizeof text, 1200) < 0) rc = jerr(result, cap, text[0] ? text : "set failed");
        else { jv t = { 's', 0, text, NULL, NULL, NULL }; char tj[200]; djson_write(&t, tj, sizeof tj); snprintf(result, cap, "{\"ok\":true,\"now_shows\":%s}", tj); }
    }
    else if (!strcmp(call->name, "transport")) {
        if (!c) rc = jerr(result, cap, "no commander connection");
        else if (cmdr_transport(c, djson_str(a, "cmd", "")) < 0) rc = jerr(result, cap, "unknown transport command");
        else snprintf(result, cap, "{\"ok\":true}");
    }
    else if (!strcmp(call->name, "play_part")) {
        char msg[200] = "";
        if (!play) rc = jerr(result, cap, "parts are not available here");
        else if (play(user, call->args, msg, sizeof msg) < 0) rc = jerr(result, cap, msg[0] ? msg : "no usable notes");
        else { jv t = { 's', 0, msg, NULL, NULL, NULL }; char tj[300]; djson_write(&t, tj, sizeof tj); snprintf(result, cap, "{\"ok\":true,\"note\":%s}", tj); }
    }
    else if (agent_tool_is_ui(call->name)) {
        char msg[300] = "";
        if (!ui) rc = jerr(result, cap, "this needs tap macros recorded on the device, and there are none");
        else if (ui(user, call->name, call->args, msg, sizeof msg) < 0) rc = jerr(result, cap, msg[0] ? msg : "the macro failed");
        else { jv t = { 's', 0, msg, NULL, NULL, NULL }; char tj[400]; djson_write(&t, tj, sizeof tj); snprintf(result, cap, "{\"ok\":true,\"note\":%s}", tj); }
    }
    else rc = jerr(result, cap, "unknown tool");
    djson_free(a);
    return rc;
}

int agent_describe(cmdr_t *c, const acall_t *call, char *out, size_t cap) {
    jv *a = djson_parse(call->args, strlen(call->args));
    int id = (int)djson_num(a, "id", -1);
    char plug[80] = "", pname[80] = "";
    if (c && !strcmp(call->name, "set_param")) {
        char *pj = malloc(200000); if (pj && cmdr_plugin_params(c, id, pj, 200000) >= 0) {
            jv *ps = djson_parse(pj, strlen(pj));
            for (jv *p = ps ? ps->kid : NULL; p; p = p->next) if ((int)djson_num(p, "i", -1) == (int)djson_num(a, "index", -2)) snprintf(pname, sizeof pname, "%s", djson_str(p, "name", ""));
            djson_free(ps);
        }
        free(pj);
        char sj[8192]; if (cmdr_plugins_summary(c, sj, sizeof sj) > 0) {
            jv *ss = djson_parse(sj, strlen(sj));
            for (jv *p = ss ? ss->kid : NULL; p; p = p->next) if ((int)djson_num(p, "id", -1) == id) snprintf(plug, sizeof plug, "%s", djson_str(p, "name", ""));
            djson_free(ss);
        }
    }
    if (!strcmp(call->name, "set_param")) snprintf(out, cap, "set %s %s to %.0f%%", plug[0] ? plug : "plugin", pname[0] ? pname : "parameter", djson_num(a, "value", 0) * 100);
    else if (!strcmp(call->name, "transport")) snprintf(out, cap, "press %s", djson_str(a, "cmd", "?"));
    else if (!strcmp(call->name, "select_track")) snprintf(out, cap, "select track %d", (int)djson_num(a, "track", 0));
    else if (!strcmp(call->name, "import_clip")) snprintf(out, cap, "clip into track %d", (int)djson_num(a, "track", 0));
    else if (!strcmp(call->name, "load_instrument")) snprintf(out, cap, "load instrument %s on track %d", djson_str(a, "plugin", "?"), (int)djson_num(a, "track", 0));
    else if (!strcmp(call->name, "load_efx")) snprintf(out, cap, "load %s on track %d", djson_str(a, "plugin", "?"), (int)djson_num(a, "track", 0));
    else if (!strcmp(call->name, "play_part")) snprintf(out, cap, "make a part \"%s\" on channel %d, %d notes", djson_str(a, "name", "daemon"), (int)djson_num(a, "channel", 2), djson_len(djson_get(a, "notes")));
    else snprintf(out, cap, "%s", call->name);
    djson_free(a);
    return 0;
}

/* A step that destroys or overwrites something: 1 and a short reason in out, else 0. Needs the commander's project snapshot (the last
 * saved state, so a track added since is not known and is not flagged). */
int agent_risk(cmdr_t *c, const acall_t *call, char *out, size_t cap) {
    out[0] = 0;
    jv *a = djson_parse(call->args, strlen(call->args));
    if (!strcmp(call->name, "transport") && !strcmp(djson_str(a, "cmd", ""), "record")) snprintf(out, cap, "RECORDS over the track");
    else if (c && !strcmp(call->name, "load_instrument")) {
        int tr = (int)djson_num(a, "track", 0); const char *want = djson_str(a, "plugin", "");
        char *pj = malloc(60000);
        if (pj && cmdr_project(c, pj, 60000) > 0) {
            jv *pr = djson_parse(pj, strlen(pj));
            for (jv *t = pr ? djson_get(pr, "tracks") : NULL, *k = t ? t->kid : NULL; k; k = k->next) if ((int)djson_num(k, "index", -1) == tr) {
                jv *pl = djson_get(k, "plugin"); const char *nm = pl ? djson_str(pl, "name", "") : "";
                if (nm[0] && strcasecmp(nm, want)) snprintf(out, cap, "REPLACES %.40s on track %d", nm, tr);
            }
            djson_free(pr);
        }
        free(pj);
    }
    djson_free(a);
    return out[0] != 0;
}
