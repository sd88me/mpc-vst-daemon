/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* A minimal MCP client (Streamable HTTP, JSON reply per request) for the remote addin (jacob-sabella/mpc-addin-remote), which
 * listens on 127.0.0.1:6720/mcp inside MPC. One short TCP connection per call. Blocking: worker threads only. */
#pragma once
#include <stddef.h>

typedef struct mcp mcp_t;
mcp_t *mcp_open(const char *host, int port, char *err, size_t errcap);    /* initialize + initialized; NULL and err on failure */
void mcp_close(mcp_t *m);
/* tools/call. text gets the text parts joined with newlines. If image_b64 is non-NULL it receives a malloc'd copy of the first image
 * part's base64 data (NULL if there was none; the caller frees it). Returns 0, or -1 when the call failed or the tool reported an
 * error (text then says why). */
int mcp_call(mcp_t *m, const char *tool, const char *args_json, char *text, size_t cap, char **image_b64);
