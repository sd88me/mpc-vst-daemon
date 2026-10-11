/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* The agent: Gemini function calling over a small set of tools that read and drive the device through the commander addin.
 * Read-only tools run while planning; tools that change anything are only collected into a plan, and run when the user presses RUN.
 * Everything here is pure (strings in, strings out) except agent_exec, which talks to a commander session. */
#pragma once
#include "daemon_cmdr.h"
#include <stddef.h>

#define AGENT_MAX_CALLS 12
#define AGENT_MAX_HISTORY 24
typedef struct { char name[32]; char args[3000]; } acall_t;
typedef struct { char text[1200]; acall_t calls[AGENT_MAX_CALLS]; int n; char *raw; } aturn_t;   /* raw: the model's content object, JSON, to echo back; free with agent_turn_free */
typedef struct { char *item[AGENT_MAX_HISTORY]; int n; } ahist_t;

int agent_tool_is_read_only(const char *name);                  /* 1 get_project, list_plugins, get_plugin_params; 0 the rest and unknown names */
int agent_tool_known(const char *name);

void agent_hist_init(ahist_t *h);
void agent_hist_free(ahist_t *h);
int agent_hist_user(ahist_t *h, const char *text, const unsigned char *wav, size_t wav_len);   /* the first turn; wav may be NULL */
int agent_hist_model(ahist_t *h, const aturn_t *t);                                             /* echoes the model's turn */
/* The results of the calls in t (results[i] is JSON text for t->calls[i]); skipped calls pass NULL and are answered "not run". */
int agent_hist_results(ahist_t *h, const aturn_t *t, const char *const *results);

typedef struct { double bpm; const char *scale; int bars; const char *typed_context; int has_ui; } actx_t;   /* has_ui: select_track and load_plugin are offered */
int agent_build_request(const actx_t *ctx, const ahist_t *h, char *buf, size_t cap);
int agent_parse_turn(const char *response, aturn_t *t);        /* 0 ok; -1: t->text holds the API error or why there is no turn */
void agent_turn_free(aturn_t *t);

/* Run one tool. Read-only tools and set_param/transport use the session; play_part is handed to play (args JSON in, result text out,
 * returns 0 ok). result gets JSON text. Returns 0 if the tool ran, -1 if it failed (result then says why). */
typedef int (*agent_play_fn)(void *user, const char *args_json, char *result, size_t cap);
/* select_track and load_plugin are handed to ui (tool name, args JSON), which runs the matching tap macro. */
typedef int (*agent_ui_fn)(void *user, const char *tool, const char *args_json, char *result, size_t cap);
int agent_tool_is_ui(const char *name);
int agent_exec(cmdr_t *c, const acall_t *call, agent_play_fn play, agent_ui_fn ui, void *user, char *result, size_t cap);
/* One line for the screen: "set Profit-8 Cutoff to 35%" */
int agent_describe(cmdr_t *c, const acall_t *call, char *out, size_t cap);
