/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* daemon_claude.c: Gemini requests become Messages API requests, Claude replies become Gemini replies. */
#include "../src/daemon_claude.h"
#include "../src/daemon_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static char out[200000];
    /* an agent round: system, tools, user text, a model turn with two calls, their results */
    const char *g = "{\"system_instruction\":{\"parts\":[{\"text\":\"You are Daemon.\"}]},"
        "\"tools\":[{\"functionDeclarations\":[{\"name\":\"get_project\",\"description\":\"tracks\",\"parameters\":{\"type\":\"OBJECT\",\"properties\":{\"n\":{\"type\":\"INTEGER\"}}}},{\"name\":\"play\"}]}],"
        "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"add reverb\"}]},"
        "{\"role\":\"model\",\"parts\":[{\"text\":\"\"},{\"functionCall\":{\"name\":\"get_project\",\"args\":{\"n\":1}}},{\"functionCall\":{\"name\":\"play\",\"args\":{}}}]},"
        "{\"role\":\"user\",\"parts\":[{\"functionResponse\":{\"name\":\"get_project\",\"response\":{\"result\":\"{\\\"tracks\\\":[]}\"}}},{\"functionResponse\":{\"name\":\"play\",\"response\":{\"result\":\"ok\"}}}]}],"
        "\"generationConfig\":{\"thinkingConfig\":{\"thinkingLevel\":\"low\"}}}";
    int n = claude_request(g, "claude-x", out, sizeof out);
    CHECK(n > 0, "converted (%d)", n);
    jv *r = djson_parse(out, (size_t)n);
    CHECK(r != NULL, "valid JSON: %s", out);
    CHECK(!strcmp(djson_str(r, "model", ""), "claude-x") && djson_num(r, "max_tokens", 0) > 0 && !strcmp(djson_str(r, "system", ""), "You are Daemon."), "model, max_tokens, system: %.200s", out);
    jv *tools = djson_get(r, "tools");
    CHECK(djson_len(tools) == 2 && !strcmp(djson_str(djson_get(djson_at(tools, 0), "input_schema"), "type", ""), "object")
          && !strcmp(djson_str(djson_get(djson_get(djson_get(djson_at(tools, 0), "input_schema"), "properties"), "n"), "type", ""), "integer")
          && !strcmp(djson_str(djson_get(djson_at(tools, 1), "input_schema"), "type", ""), "object"), "tools with lower-case schemas: %.300s", out);
    jv *msgs = djson_get(r, "messages");
    CHECK(djson_len(msgs) == 3, "three messages (%d)", djson_len(msgs));
    jv *a = djson_get(djson_at(msgs, 1), "content"), *u = djson_get(djson_at(msgs, 2), "content");
    CHECK(!strcmp(djson_str(djson_at(msgs, 1), "role", ""), "assistant") && djson_len(a) == 2, "empty text dropped, two tool_use: %.300s", out);
    CHECK(!strcmp(djson_str(djson_at(a, 0), "type", ""), "tool_use") && !strcmp(djson_str(djson_at(a, 1), "id", "a"), djson_str(djson_at(u, 1), "tool_use_id", "b"))
          && !strcmp(djson_str(djson_at(a, 0), "id", "a"), djson_str(djson_at(u, 0), "tool_use_id", "b")) && strcmp(djson_str(djson_at(a, 0), "id", ""), djson_str(djson_at(a, 1), "id", "")), "results answer their calls by id");
    CHECK(strstr(djson_str(djson_at(u, 0), "content", ""), "tracks") != NULL, "result content carried");
    djson_free(r);

    /* audio is not for Claude; an image is */
    CHECK(claude_request("{\"contents\":[{\"role\":\"user\",\"parts\":[{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"AAA\"}},{\"text\":\"hi\"}]}]}", "m", out, sizeof out) == CLAUDE_NEEDS_AUDIO, "audio refused");
    n = claude_request("{\"contents\":[{\"role\":\"user\",\"parts\":[{\"inline_data\":{\"mime_type\":\"image/png\",\"data\":\"iVBO\"}},{\"text\":\"where\"}]}]}", "m", out, sizeof out);
    CHECK(n > 0 && strstr(out, "\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"iVBO\"}"), "image block: %s", out);
    CHECK(claude_request("{\"contents\":[]}", "m", out, 40) == -1, "too small a buffer fails");

    /* replies */
    n = claude_response("{\"content\":[{\"type\":\"text\",\"text\":\"Adding it.\"},{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"load_efx\",\"input\":{\"track\":5}}],\"stop_reason\":\"tool_use\"}", out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Adding it.\"},{\"functionCall\":{\"name\":\"load_efx\",\"args\":{\"track\":5}}}]}}]}"), "tool reply: %s", out);
    claude_response("{\"content\":[{\"type\":\"text\",\"text\":\"```json\\n{\\\"a\\\":1}\\n```\"}]}", out, sizeof out);
    CHECK(strstr(out, "{\\\"a\\\":1}") && !strstr(out, "```"), "code fence removed: %s", out);
    claude_response("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}", out, sizeof out);
    CHECK(!strcmp(out, "{\"error\":{\"message\":\"Overloaded\"}}"), "error kept: %s", out);
    char names[4][64];
    CHECK(claude_models("{\"data\":[{\"id\":\"claude-a\"},{\"id\":\"other\"},{\"id\":\"claude-b\"}]}", names, 4) == 2 && !strcmp(names[1], "claude-b"), "model ids");
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
