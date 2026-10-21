/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* Claude (Anthropic Messages API) behind the Gemini wire format: Daemon builds Gemini requests everywhere, and when Claude is the
 * provider the request is translated here, and Claude's reply translated back into a Gemini reply. */
#pragma once
#include <stddef.h>

#define CLAUDE_NEEDS_AUDIO (-2)   /* the request carries audio, which Claude does not take: send it to Gemini instead */

/* A Gemini generateContent body -> a Messages API body for `model`. Returns the length, -1 if it does not fit or does not parse,
 * CLAUDE_NEEDS_AUDIO if it contains audio. */
int claude_request(const char *gemini_body, const char *model, char *out, size_t cap);
/* A Messages API reply (or error) -> a Gemini reply with the same text and function calls. Returns the length, -1 if it does not fit. */
int claude_response(const char *claude_reply, char *out, size_t cap);
/* The model ids in a GET /v1/models reply. Returns how many. */
int claude_models(const char *json, char names[][64], int max);
