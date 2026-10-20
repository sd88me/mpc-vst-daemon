/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#define _GNU_SOURCE
#include "daemon_agent.h"
#include <dirent.h>
#include <glob.h>
#include <ctype.h>
#include <strings.h>
#include "daemon_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *READ_ONLY[] = { "get_project", "list_plugins", "get_plugin_params" };
static const char *MUTATING[] = { "set_param", "transport", "play_part", "select_track", "load_efx", "load_instrument", "load_preset", "import_clip", "add_track" };
int agent_tool_is_read_only(const char *n) { for (int i = 0; i < 3; i++) if (!strcmp(n, READ_ONLY[i])) return 1; return 0; }
int agent_tool_is_ui(const char *n) { return !strcmp(n, "select_track") || !strcmp(n, "load_efx") || !strcmp(n, "load_instrument") || !strcmp(n, "load_preset") || !strcmp(n, "import_clip") || !strcmp(n, "add_track"); }
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
 "{\"name\":\"list_plugins\",\"description\":\"The plugin instances currently loaded in MPC (id, name, vendor, parameter count), and when known the effects and instruments installed on the device (installed_effects, installed_instruments: the names load_efx and load_instrument can pick). Read-only.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{}}},"
 "{\"name\":\"get_plugin_params\",\"description\":\"Parameters of one plugin instance with their index, name, 0-1 value and the text MPC shows. A synth can have hundreds: pass match, words separated by | (for example attack|decay|cutoff|filter|env), to get only those whose name contains one. Read-only.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"id\":{\"type\":\"INTEGER\"},\"match\":{\"type\":\"STRING\"}},\"required\":[\"id\"]}},"
 "{\"name\":\"set_param\",\"description\":\"Set one plugin parameter to a value between 0 and 1. The result reports the text MPC then shows, so a value can be corrected.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"id\":{\"type\":\"INTEGER\"},\"index\":{\"type\":\"INTEGER\"},\"value\":{\"type\":\"NUMBER\"}},\"required\":[\"id\",\"index\",\"value\"]}},"
 "{\"name\":\"transport\",\"description\":\"Press a transport button on the MPC.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"cmd\":{\"type\":\"STRING\",\"enum\":[\"play\",\"stop\",\"continue\",\"record\"]}},\"required\":[\"cmd\"]}},"
 "{\"name\":\"play_part\",\"description\":\"Make a MIDI part and loop it from Daemon's own MIDI port, in time with the MPC transport. Notes use beats from the start of the part.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
   "\"name\":{\"type\":\"STRING\"},\"channel\":{\"type\":\"INTEGER\"},\"notes\":{\"type\":\"ARRAY\",\"description\":\"each note is [pitch, start_time, duration, velocity]\",\"items\":{\"type\":\"ARRAY\",\"items\":{\"type\":\"NUMBER\"}}}},\"required\":[\"notes\"]}}"
 ;
static const char *TOOL_SELECT =
 ",{\"name\":\"select_track\",\"description\":\"Select a track on the MPC's screen by touching it (a recorded sequence of touches). Needed before anything that works on the selected track.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"track\":{\"type\":\"INTEGER\"}},\"required\":[\"track\"]}}";
static const char *TOOL_LOAD =
 ",{\"name\":\"load_efx\",\"description\":\"Load an effect plugin into the first free insert slot of a track's mixer strip, finding it in the MPC's plugin browser by name. Selects the track itself. Not for instruments: use load_instrument for those.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"track\":{\"type\":\"INTEGER\"},\"plugin\":{\"type\":\"STRING\",\"description\":\"the plugin's name exactly as in installed_effects, e.g. AIR Reverb\"},\"group\":{\"type\":\"STRING\",\"description\":\"the plugin picker's type group that lists it: use the group given in installed_effects (Delay/Reverb, Dynamics, EQ/Filter, Harmonic, Modulation, Vocal, Vintage Effects, or VST for third-party plugins)\"}},\"required\":[\"track\",\"plugin\"]}}";
static const char *TOOL_INSTR =
 ",{\"name\":\"load_instrument\",\"description\":\"Load a plugin as the instrument (the sound source) of a plugin track, finding it in the MPC's plugin browser by name. Replaces the track's current instrument.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"track\":{\"type\":\"INTEGER\"},\"plugin\":{\"type\":\"STRING\",\"description\":\"the instrument's name as listed in the MPC, e.g. Hera\"}},\"required\":[\"track\",\"plugin\"]}}";
static const char *TOOL_PRESET =
 ",{\"name\":\"load_preset\",\"description\":\"Choose a preset of the instrument already loaded on a plugin track, finding it by name in the preset list on the MPC's screen. Presets are often inside a folder: give the folder if you know it.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"track\":{\"type\":\"INTEGER\"},\"preset\":{\"type\":\"STRING\",\"description\":\"the preset's name as listed, e.g. Rich Strings\"},\"folder\":{\"type\":\"STRING\",\"description\":\"the folder it is in, e.g. Modern; leave out if it is at the top of the list or you do not know\"}},\"required\":[\"track\",\"preset\"]}}";
static const char *TOOL_IMPORT =
 ",{\"name\":\"import_clip\",\"description\":\"Save the part Daemon has made (after play_part) as a MIDI file and load it into the first empty clip slot of a track in the clip matrix, by touching the screen. The track number is the musician's, counted from 1.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"track\":{\"type\":\"INTEGER\"}},\"required\":[\"track\"]}}";
static const char *TOOL_ADD =
 ",{\"name\":\"add_track\",\"description\":\"Add a new empty Plugin track at the end of the project, by touching the MPC's screen. It becomes the next track number after the last one. Daemon cannot see it in get_project until the musician saves the project.\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{}}}";
static const char *TOOLS_END = "]}]";

int agent_build_request(const actx_t *ctx, const ahist_t *h, char *buf, size_t cap) {
    char sys[8000];
    snprintf(sys, sizeof sys,
        "You help a musician run an Akai MPC by voice or text. You can read the project and plugins, set plugin parameters, press transport buttons and make MIDI parts, all through the tools. "
        "Look first when you need facts (get_project, list_plugins, get_plugin_params), then ask for every change in one go: the changes are shown to the user as a plan and only run when they press RUN, so do not ask them to confirm in words. "
        "Parts: each note is an array [pitch, start_time, duration, velocity], positions and lengths are in beats (1.0 = a quarter note), pitches are MIDI note numbers, General MIDI drum notes for beats (36 kick, 38 snare, 42 closed hat, 46 open hat, 39 clap), channel 2 unless told otherwise. "
        "Keep melodic notes in the musician's scale unless they ask otherwise. Plugin values run 0 to 1 and the result of set_param tells you the text MPC shows, so use it to land on musical settings. "
        "Track numbers in get_project start at 0: the musician's track 1 is index 0, so say track numbers to them as index plus one. The screen tools (select_track, load_efx, load_instrument, load_preset, import_clip) count tracks from 1, as the musician does. Daemon's MIDI channel is set to %d: use it for play_part unless the musician names another channel. import_clip selects the track itself: do not call select_track before it. A track you add is not in get_project until the musician saves the project: count it as the next number after the last track you can see (plus any you already added in this plan), use it with load_instrument in the same plan, and tell the musician to save the project afterwards so you can see it. You cannot rename or delete anything: say so briefly if asked, and do what you can. Loading an effect or an instrument is only possible when load_efx or load_instrument exists; otherwise say you cannot. A sound request (a plucky lead, a warm pad) on a synth: load_instrument if it is not loaded yet, then shape it with get_plugin_params and set_param, or choose a stock preset with load_preset when that exists (its folder and name are only known from the screen: name what you expect, and say if it was not found). Style requests often call for effects: dub or dubby wants a tape or echo delay with feedback plus a reverb, ambient or washed-out wants a big reverb, lo-fi wants tape, filter or bit-crush, huge or wide wants chorus or reverb. When load_efx exists and the style implies it, use list_plugins and pick effects from installed_effects only, never a name that is not in that list. Choose the obvious plugin: for a reverb one whose name says reverb (AIR Reverb is the default), for a delay or echo one whose name says delay or echo (AIR Multitap Delay, Delay Analog), for chorus, compression, tape, filter likewise; prefer the device's own effects (any group but VST) over third-party ones (VST) unless the musician names one. Never use a plugin whose name does not say what it does as a stand-in (a Tap FX is not a delay): if nothing installed fits, say so in your reply instead of guessing. Pass the group the plugin is listed under in installed_effects as load_efx's group, load_efx them on the track that carries the part, then set their mix, time and feedback with get_plugin_params and set_param so the effect is clearly audible but musical. Do not add effects the style does not suggest, and do not add one that is already on the track. Selecting or loading touches the screen and is slow, so do it only when asked or when the style clearly implies it. "
        "Finish with one or two plain sentences saying what the plan does. Tempo %.0f BPM, default scale %s, default length %d bars. %s",
        ctx->channel > 0 ? ctx->channel : 1, ctx->bpm, ctx->scale, ctx->bars, ctx->typed_context ? ctx->typed_context : "");
    char *s = jstr_alloc(sys); if (!s) return -1;
    size_t n = 0; int w;
    w = snprintf(buf, cap, "{\"system_instruction\":{\"parts\":[{\"text\":%s}]},\"tools\":%s%s%s%s%s%s%s%s,\"contents\":[", s, TOOLS, (ctx->has_ui & 1) ? TOOL_SELECT : "", (ctx->has_ui & 2) ? TOOL_LOAD : "", (ctx->has_ui & 8) ? TOOL_INSTR : "", (ctx->has_ui & 16) ? TOOL_PRESET : "", (ctx->has_ui & 4) ? TOOL_IMPORT : "", (ctx->has_ui & 32) ? TOOL_ADD : "", TOOLS_END); free(s);
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

/* The plugins MPC has installed (KNOWNPLUGINS in its settings file), so the model picks names that exist. Writes
 * `"installed_effects":[...],"installed_instruments":[...]` (JSON members, no braces) and returns its length, or 0 when there
 * is no settings file. DAEMON_MPC_SETTINGS overrides the path (tests). */
static void xml_unesc(char *s) {
    static const struct { const char *e; char c; } E[] = { { "&amp;", '&' }, { "&quot;", '"' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&apos;", '\'' } };
    char *w = s;
    for (const char *r = s; *r;) {
        int done = 0;
        if (*r == '&') for (size_t k = 0; k < sizeof E / sizeof *E; k++) { size_t n = strlen(E[k].e); if (!strncmp(r, E[k].e, n)) { *w++ = E[k].c; r += n; done = 1; break; } }
        if (!done) *w++ = *r++;
    }
    *w = 0;
}
static int tag_attr(const char *tag, const char *end, const char *key, char *out, size_t cap) {   /* key="value" inside tag..end */
    char pat[40]; snprintf(pat, sizeof pat, " %s=\"", key);
    const char *p = strstr(tag, pat); if (!p || p >= end) return -1;
    p += strlen(pat); const char *q = memchr(p, '"', (size_t)(end - p)); if (!q || (size_t)(q - p) >= cap) return -1;
    memcpy(out, p, (size_t)(q - p)); out[q - p] = 0; xml_unesc(out); return 0;
}
typedef struct { char name[120], cat[60], group[24]; int inst, factory; } inst_ent;
static int ent_has(const inst_ent *e, int n, const char *name) { for (int i = 0; i < n; i++) if (!strcmp(e[i].name, name)) return 1; return 0; }
/* The picker's type group for a factory effect, from its name (the MPC groups its own effects like this; third-party ones are all under VST). */
static const char *factory_group(const char *name) {
    static const struct { const char *g, *w[8]; } G[] = {
        { "Delay/Reverb", { "reverb", "delay", "echo", NULL } },
        { "Modulation", { "chorus", "flang", "phas", "tremolo", "vibrato", "modul", NULL } },
        { "Dynamics", { "compress", "expander", "limit", "gate", "trigger", NULL } },
        { "Vintage Effects", { "vinyl", "tape", "vintage", NULL } },
        { "EQ/Filter", { "filter", "equal", " eq", "channel strip", NULL } },
        { "Harmonic", { "tube", "amp sim", "drive", "distort", "satur", "exciter", "lo-fi", "lofi", "decimator", "talk box", "freq shift", "flavor", "diode", NULL } },
        { "Vocal", { "vocal", "pitch", "harmoniz", "vocoder", NULL } },
    };
    char low[120]; size_t i = 0; for (; name[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)name[i]); low[i] = 0;
    for (size_t g = 0; g < sizeof G / sizeof *G; g++) for (int w = 0; G[g].w[w]; w++) if (strstr(low, G[g].w[w])) return G[g].g;
    return "";
}
static void scan_factory(const char *dir, int force_kind, inst_ent *e, int *n, int max) {   /* "<vendor> - MPC - <name>" folders; force_kind 0 effect, 1 instrument, -1 decide by name */
    DIR *d = opendir(dir); if (!d) return;
    for (struct dirent *de; (de = readdir(d)) && *n < max;) {
        const char *nm = strstr(de->d_name, " - MPC - "); if (!nm) continue; nm += 9;
        if (!nm[0] || strlen(nm) >= sizeof e->name || ent_has(e, *n, nm)) continue;
        inst_ent *x = &e[(*n)++]; memset(x, 0, sizeof *x); snprintf(x->name, sizeof x->name, "%s", nm); x->factory = 1;
        static const char *INSTR[] = { "Solina", "Mellotron", "Odyssey", "Hype", "Bassline", "Electric", "TubeSynth", "DrumSynth", NULL };
        x->inst = force_kind >= 0 ? force_kind : 0; if (force_kind < 0) for (int i = 0; INSTR[i]; i++) if (!strncmp(nm, INSTR[i], strlen(INSTR[i]))) x->inst = 1;
        snprintf(x->cat, sizeof x->cat, "%s", x->inst ? "Instrument" : "Effect"); snprintf(x->group, sizeof x->group, "%s", x->inst ? "" : factory_group(nm));
    }
    closedir(d);
}
static const struct { const char *name, *group; } FACTORY[] = {      /* the built-in effects (no folder on the drive), as the plugin picker lists them */
    { "AIR Delay", "Delay/Reverb" }, { "AIR Diff Delay", "Delay/Reverb" }, { "AIR Multitap Delay", "Delay/Reverb" }, { "AIR Non-Lin Reverb", "Delay/Reverb" },
    { "AIR Reverb", "Delay/Reverb" }, { "AIR Spring Reverb", "Delay/Reverb" }, { "Delay Analog Sync", "Delay/Reverb" }, { "Delay Analog", "Delay/Reverb" },
    { "Delay HP", "Delay/Reverb" }, { "Delay LP", "Delay/Reverb" }, { "Delay Mono Sync", "Delay/Reverb" }, { "Delay Mono", "Delay/Reverb" },
    { "Delay Multi-Tap", "Delay/Reverb" }, { "Delay Ping Pong", "Delay/Reverb" }, { "Delay Stereo", "Delay/Reverb" }, { "Delay Sync", "Delay/Reverb" },
    { "Delay Tape Sync", "Delay/Reverb" }, { "Reverb In Gate", "Delay/Reverb" }, { "Reverb Large 2", "Delay/Reverb" }, { "Reverb Large", "Delay/Reverb" },
    { "Reverb Medium", "Delay/Reverb" }, { "Reverb Out Gate", "Delay/Reverb" }, { "Reverb Small", "Delay/Reverb" }, { "Sample Delay", "Delay/Reverb" },
    { "AIR Amp Sim", "Harmonic" }, { "AIR Diode Clip", "Harmonic" }, { "AIR Distortion", "Harmonic" }, { "AIR Flavor", "Harmonic" }, { "AIR Freq Shift", "Harmonic" },
    { "AIR Lo-Fi", "Harmonic" }, { "AIR Talk Box", "Harmonic" }, { "AIR Tube Drive", "Harmonic" }, { "Decimator", "Harmonic" }, { "Distortion Amp", "Harmonic" },
    { "Distortion Custom", "Harmonic" } };
static int installed_plugins(char *out, size_t cap) {
    const char *path = getenv("DAEMON_MPC_SETTINGS"); glob_t g; char found[512] = "";
    if (path) snprintf(found, sizeof found, "%s", path);
    else if (glob("/media/*/Settings/MPC/MPC.settings", 0, NULL, &g) == 0) { if (g.gl_pathc) snprintf(found, sizeof found, "%s", g.gl_pathv[0]); globfree(&g); }
    if (!found[0]) return 0;
    FILE *f = fopen(found, "re"); if (!f) return 0;
    size_t cp = 1 << 20, n = 0; char *buf = malloc(cp); if (!buf) { fclose(f); return 0; }
    n = fread(buf, 1, cp - 1, f); fclose(f); buf[n] = 0;
    enum { MAXE = 200 }; inst_ent *E = calloc(MAXE, sizeof *E); int ne = 0; char synths[400] = "";
    const char *k = strstr(buf, "<KNOWNPLUGINS>"), *stop = k ? strstr(k, "</KNOWNPLUGINS>") : NULL;
    for (const char *t = k; k && (t = strstr(t, "<PLUGIN ")) && (!stop || t < stop) && ne < MAXE; t += 8) {
        const char *end = strstr(t, "/>"); if (!end) break;
        char name[120], isi[4], fmt[16], file[400];
        if (tag_attr(t, end, "name", name, sizeof name) || tag_attr(t, end, "isInstrument", isi, sizeof isi)) continue;
        if (tag_attr(t, end, "format", fmt, sizeof fmt) || strcmp(fmt, "VST")) continue;
        if (!synths[0] && !tag_attr(t, end, "file", file, sizeof file)) { char *sy = strstr(file, "/Synths/"); if (sy) { sy[7] = 0; snprintf(synths, sizeof synths, "%s", file); } }
        inst_ent *x = &E[ne++]; memset(x, 0, sizeof *x); snprintf(x->name, sizeof x->name, "%s", name); x->inst = isi[0] == '1';
        if (tag_attr(t, end, "category", x->cat, sizeof x->cat)) x->cat[0] = 0;
        if (!x->inst) snprintf(x->group, sizeof x->group, "VST");
    }
    free(buf);
    for (size_t i = 0; i < sizeof FACTORY / sizeof *FACTORY && ne < MAXE; i++) if (!ent_has(E, ne, FACTORY[i].name)) {
        inst_ent *x = &E[ne++]; memset(x, 0, sizeof *x); snprintf(x->name, sizeof x->name, "%s", FACTORY[i].name); snprintf(x->cat, sizeof x->cat, "Effect"); snprintf(x->group, sizeof x->group, "%s", FACTORY[i].group); x->factory = 1;
    }
    if (synths[0]) {                                       /* the device's own plugins: folders on the drive and the preset folders */
        char root[400], pdir[520]; snprintf(root, sizeof root, "%s", synths); char *sl = strrchr(root, '/'); if (sl) *sl = 0;
        snprintf(pdir, sizeof pdir, "%s/Force Documents/Plugin Presets/Insert Effects", root); scan_factory(pdir, 0, E, &ne, MAXE);
        snprintf(pdir, sizeof pdir, "%s/Force Documents/Plugin Presets/Instruments", root); scan_factory(pdir, 1, E, &ne, MAXE);
        scan_factory(synths, -1, E, &ne, MAXE);
    }
    { const char *fd = getenv("DAEMON_FACTORY_DIR"); scan_factory(fd ? fd : "/usr/share/Akai/Content/Synths", -1, E, &ne, MAXE); }   /* the whole built-in set, with presets inside each folder */
    /* compact for the model: names grouped by the picker's type group (VST = third-party, every other group = the device's own) */
    size_t o = 0; const char *seen[MAXE]; int ns = 0;
    for (int kind = 0; kind < 2; kind++) {                 /* 0 effects, 1 instruments */
        o += (size_t)snprintf(out + o, cap - o, "%s\"installed_%s\":{", kind ? "," : "", kind ? "instruments" : "effects"); int gn = 0; ns = 0;
        for (int i = 0; i < ne; i++) if (E[i].inst == kind) {
            const char *g = kind ? (E[i].factory ? "factory" : "VST") : (E[i].group[0] ? E[i].group : "Other"); int dup = 0;
            for (int q = 0; q < ns; q++) if (!strcmp(seen[q], g)) dup = 1;
            if (dup) continue; seen[ns++] = g;
            o += (size_t)snprintf(out + o, cap - o, "%s\"%s\":[", gn++ ? "," : "", g); int cn = 0;
            for (int j = i; j < ne; j++) if (E[j].inst == kind) {
                const char *gj = kind ? (E[j].factory ? "factory" : "VST") : (E[j].group[0] ? E[j].group : "Other"); if (strcmp(gj, g)) continue;
                char *nj = jstr_alloc(E[j].name); if (!nj) continue;
                if (o + strlen(nj) + 16 >= cap) { free(nj); break; }
                o += (size_t)snprintf(out + o, cap - o, "%s%s", cn++ ? "," : "", nj); free(nj);
            }
            o += (size_t)snprintf(out + o, cap - o, "]");
        }
        o += (size_t)snprintf(out + o, cap - o, "}");
    }
    free(E);
    return (int)o;
}

/* The commander's snapshot cut down to what planning needs: no bus/return/output tracks, colours, file paths or plugin state.
 * Roughly 8 KB becomes under 1 KB, and the result is re-sent with every planning round. Leaves buf as is if it can't parse it. */
static void compact_project(char *buf, size_t cap) {
    jv *p = djson_parse(buf, strlen(buf)); if (!p || djson_get(p, "tracks") == NULL) { djson_free(p); return; }
    char *o = malloc(cap); if (!o) { djson_free(p); return; }
    size_t n = 0; char t1[200], t2[200];
    #define JS(dst, str) do { jv v_ = { 's', 0, (char *)(str), NULL, NULL, NULL }; djson_write(&v_, dst, sizeof dst); } while (0)
    JS(t1, djson_str(p, "name", "")); JS(t2, djson_str(p, "key", ""));
    n += (size_t)snprintf(o + n, cap - n, "{\"project\":%s,\"tempo\":%g,\"key\":%s,\"current_track\":%d", t1, djson_num(p, "tempo", 0), t2, (int)djson_num(p, "current_track", -1));
    jv *sq = djson_get(p, "sequence");
    if (sq && sq->t == 'o') { JS(t1, djson_str(sq, "name", "")); n += (size_t)snprintf(o + n, cap - n, ",\"sequence\":{\"name\":%s,\"bars\":%g}", t1, djson_num(sq, "bars", 0)); }
    n += (size_t)snprintf(o + n, cap - n, ",\"tracks\":[");
    int k = 0;
    for (jv *t = djson_get(p, "tracks")->kid; t && n + 400 < cap; t = t->next) {
        const char *ty = djson_str(t, "type", "");
        if (!strcmp(ty, "submix") || !strcmp(ty, "return") || !strcmp(ty, "output")) continue;
        JS(t1, djson_str(t, "name", ""));
        n += (size_t)snprintf(o + n, cap - n, "%s{\"index\":%d,\"name\":%s,\"type\":\"%s\"", k++ ? "," : "", (int)djson_num(t, "index", 0), t1, ty);
        jv *pl = djson_get(t, "plugin");
        if (pl && pl->t == 'o') { JS(t1, djson_str(pl, "name", "")); n += (size_t)snprintf(o + n, cap - n, ",\"plugin\":%s", t1); if (djson_str(pl, "preset", "")[0]) { JS(t2, djson_str(pl, "preset", "")); n += (size_t)snprintf(o + n, cap - n, ",\"preset\":%s", t2); } }
        jv *b;
        if ((b = djson_get(t, "mute")) && b->t == 'b' && b->num) n += (size_t)snprintf(o + n, cap - n, ",\"mute\":true");
        if ((b = djson_get(t, "solo")) && b->t == 'b' && b->num) n += (size_t)snprintf(o + n, cap - n, ",\"solo\":true");
        n += (size_t)snprintf(o + n, cap - n, "}");
    }
    #undef JS
    snprintf(o + n, cap - n, "]}");
    snprintf(buf, cap, "%s", o); free(o); djson_free(p);
}
static int jerr(char *r, size_t cap, const char *msg) { jv v = { 's', 0, (char *)msg, NULL, NULL, NULL }; char e[300]; djson_write(&v, e, sizeof e); snprintf(r, cap, "{\"error\":%s}", e); return -1; }

int agent_exec(cmdr_t *c, const acall_t *call, agent_play_fn play, agent_ui_fn ui, void *user, char *result, size_t cap) {
    jv *a = djson_parse(call->args, strlen(call->args));
    int rc = 0;
    if (!a) return jerr(result, cap, "bad arguments");
    if (!strcmp(call->name, "get_project")) { if (!c) rc = jerr(result, cap, "no commander connection"); else if (cmdr_project(c, result, cap) < 0) rc = -1; else compact_project(result, cap); }
    else if (!strcmp(call->name, "list_plugins")) { if (!c) rc = jerr(result, cap, "no commander connection"); else if (cmdr_plugins_summary(c, result, cap) < 0) rc = jerr(result, cap, "too many plugins to list");
        else { char *inst = malloc(60000); size_t l = strlen(result);
            if (inst && installed_plugins(inst, 60000) > 0 && l + strlen(inst) + 30 < cap) { char *loaded = strdup(result); if (loaded) { snprintf(result, cap, "{\"loaded\":%s,%s}", loaded, inst); free(loaded); } }
            free(inst); } }
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
    else if (!strcmp(call->name, "add_track")) snprintf(out, cap, "add a Plugin track");
    else if (!strcmp(call->name, "import_clip")) snprintf(out, cap, "clip into track %d", (int)djson_num(a, "track", 0));
    else if (!strcmp(call->name, "load_instrument")) snprintf(out, cap, "load instrument %s on track %d", djson_str(a, "plugin", "?"), (int)djson_num(a, "track", 0));
    else if (!strcmp(call->name, "load_preset")) snprintf(out, cap, "load preset %s%s%s on track %d", djson_str(a, "folder", ""), djson_str(a, "folder", "")[0] ? " / " : "", djson_str(a, "preset", "?"), (int)djson_num(a, "track", 0));
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
