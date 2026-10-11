/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#include "../src/daemon_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* req <prompt>: print the request body. parse <response.json>: pull candidates[0].content.parts[0].text out of the
 * Gemini envelope the cheap way (the plugin will use its real JSON reader), then run daemon_parse_reply on it. */
int main(int argc, char **argv) {
    static char buf[1 << 20]; static dpart_t part;
    if (argc > 2 && !strcmp(argv[1], "req")) {
        dreq_t rq = { "Chords", "F Minor", "gm", 124, 4 };
        if (daemon_build_request(&rq, argv[2], NULL, 0, buf, sizeof buf) < 0) return 1;
        puts(buf); return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "parse")) {
        FILE *f = fopen(argv[2], "rb"); size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
        char *t = strstr(buf, "\"text\": \"");
        if (!t) { printf("no text in response:\n%.600s\n", buf); return 1; }
        t += 9; char *o = malloc(n), *w = o;
        for (; *t && *t != '"'; t++) { if (*t == '\\' && t[1]) { t++; *w++ = *t == 'n' ? '\n' : *t; } else *w++ = *t; }
        *w = 0;
        int k = daemon_parse_reply(o, 16, &part);
        printf("parsed %d notes, loop %.0f beats\nclip: %s\ntranscript: %s\nfeedback: %s\n", k, part.loop_beats, part.clip_name, part.transcript, part.feedback);
        for (int i = 0; i < part.n && i < 12; i++) printf("  %3d  start %5.2f  dur %4.2f  vel %3d\n", part.notes[i].pitch, part.notes[i].start, part.notes[i].dur, part.notes[i].vel);
        return k < 0;
    }
    return 2;
}
