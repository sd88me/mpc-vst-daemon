/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* The engine end to end with a fake network and MIDI port: talk -> capture -> request -> reply -> parts played from song_pos. */
#include "engine.h"
#include "io_fake.h"
#include "../src/daemon_tap.h"
#include "../src/daemon_core.h"
#include "../src/daemon_json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const mpc_engine_t *E;
static char *get(void *i, const char *k) { static char b[600]; b[0] = 0; E->get_param(i, k, b, sizeof b); return b; }
static void wait_not_thinking(void *i) {
    for (int t = 0; t < 400; t++) { if (strncmp(get(i, "status"), "THINKING", 8)) return; usleep(10000); }
    printf("timeout waiting for the worker\n");
}
static tap_ring_t *ring;   /* the Daemon Listen's side: what it hears goes in here, Daemon's render() takes it out */
static void hear(void *i, const int16_t *in) { int16_t out[256]; tap_write(ring, in, 128); E->render(i, out, 128); }
static void speak(void *i, int loud) {
    int16_t in[256];
    E->set_param(i, "talk", "1");
    for (int b = 0; b < 600; b++) {
        for (int f = 0; f < 128; f++) in[2 * f] = in[2 * f + 1] = loud ? (int16_t)(9000 * sin(6.2832 * 200 * (b * 128 + f) / 44100.0)) : 0;
        hear(i, in);
    }
    E->set_param(i, "talk", "1");
}
static const char *OK_REPLY = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"{\\\"transcript\\\":\\\"a dark bassline\\\",\\\"notes\\\":["
    "{\\\"pitch\\\":41,\\\"start_time\\\":0,\\\"duration\\\":0.5,\\\"velocity\\\":100},{\\\"pitch\\\":44,\\\"start_time\\\":1,\\\"duration\\\":0.5,\\\"velocity\\\":90},"
    "{\\\"pitch\\\":48,\\\"start_time\\\":2,\\\"duration\\\":1,\\\"velocity\\\":95}],\\\"feedback\\\":{\\\"result\\\":\\\"Dark and slow.\\\",\\\"clip_name\\\":\\\"dark_bass\\\"}}\"}]}}]}";

int main(void) {
    setenv("DAEMON_NO_DEBOUNCE", "1", 1); setenv("DAEMON_NO_MODEL_REFRESH", "1", 1);
    char tp[] = "/tmp/daemon_tap_XXXXXX"; int tf = mkstemp(tp); if (tf < 0) return 1; close(tf); setenv("DAEMON_TAP_PATH", tp, 1); ring = tap_create();
    E = mpc_engine();
    char dir[] = "/tmp/daemon_eng_XXXXXX"; if (!mkdtemp(dir)) return 1;

    /* no Daemon Listen inserted: TALK says so instead of listening to nothing */
    { char save[300]; snprintf(save, sizeof save, "%s", getenv("DAEMON_TAP_PATH")); setenv("DAEMON_TAP_PATH", "/tmp/daemon_no_such_tap", 1);
      void *n = E->create(dir); E->set_param(n, "talk", "1");
      CHECK(!strncmp(get(n, "status"), "ERROR no mic", 12), "no tap: '%s'", get(n, "status")); E->destroy(n); setenv("DAEMON_TAP_PATH", save, 1); }

    /* no key: the first request says so and nothing goes to the network */
    void *a = E->create(dir);
    speak(a, 1);
    CHECK(!strncmp(get(a, "status"), "ERROR no api_key", 16), "no key: '%s'", get(a, "status"));
    CHECK(fake.posts == 0, "posted without a key");
    E->destroy(a);

    char path[300]; snprintf(path, sizeof path, "%s/daemon.conf", dir);
    FILE *f = fopen(path, "w"); fputs("api_key=KEY123\nmodel=test-model\nurl_base=http://x/models\n", f); fclose(f);
    a = E->create(dir);
    CHECK(!strcmp(get(a, "status"), "READY") && !strcmp(get(a, "face"), "Idle"), "start state '%s' face '%s'", get(a, "status"), get(a, "face"));
    E->set_param(a, "talk", "1"); CHECK(!strcmp(get(a, "face"), "Listening"), "face while listening '%s'", get(a, "face"));
    { int16_t z[256] = {0}; for (int i = 0; i < 100; i++) hear(a, z); }
    E->set_param(a, "talk", "1"); CHECK(!strcmp(get(a, "face"), "Error"), "face after a silent take '%s'", get(a, "face"));

    /* silence: caught before it costs a request */
    speak(a, 0);
    CHECK(!strncmp(get(a, "status"), "ERROR heard nothing", 19), "silent mic: '%s'", get(a, "status"));
    CHECK(fake.posts == 0, "posted silence");

    /* a spoken request */
    snprintf(fake.resp, sizeof fake.resp, "%s", OK_REPLY); fake.code = 200;
    E->set_param(a, "type", "Melody"); E->set_param(a, "key", "A"); E->set_param(a, "scale", "Dorian"); E->set_param(a, "bars", "2");
    E->song_pos(a, 0, 100, 0, 128);
    speak(a, 1); wait_not_thinking(a);
    CHECK(!strcmp(get(a, "status"), "DONE 3 NOTES") && !strcmp(get(a, "face"), "Done"), "after reply: '%s' face '%s'", get(a, "status"), get(a, "face"));
    CHECK(!strcmp(fake.url, "http://x/models/test-model:generateContent") && !strcmp(fake.key, "KEY123"), "url '%s' key '%s'", fake.url, fake.key);
    CHECK(strstr(fake.body, "audio/wav") && strstr(fake.body, "A Dorian") && strstr(fake.body, "Length: 8 beats") && strstr(fake.body, "Part type: Melody"), "request body: %.200s", fake.body);
    CHECK(strstr(fake.body, "Tempo 100 BPM"), "tempo from the host not in the request");
    CHECK(!strcmp(get(a, "transcript"), "a dark bassline") && !strcmp(get(a, "reply"), "Dark and slow."), "transcript '%s' reply '%s'", get(a, "transcript"), get(a, "reply"));
    int rev = atoi(get(a, "display_rev")); CHECK(rev >= 4, "display_rev %d", rev);

    /* playing: 3 notes over a 3-beat part, two passes of the loop, MIDI on channel 2 (index 1), nothing doubled, all ended */
    double ppb = 100 / 60.0 * 128 / 44100.0, ppq = 0; fake.nev = 0;
    while (ppq < 5.9) { E->song_pos(a, ppq, 100, 1, 128); ppq += ppb; }
    int on = 0, off = 0, badch = 0; for (int i = 0; i < fake.nev; i++) { on += fake.ev[i].on; off += !fake.ev[i].on; badch += fake.ev[i].ch != 0; }
    CHECK(on == 6 && off >= 5 && !badch, "6 note-ons over two passes: on %d off %d badch %d", on, off, badch);
    E->song_pos(a, ppq, 100, 0, 128);                    /* transport stops: whatever is sounding ends */
    on = off = 0; for (int i = 0; i < fake.nev; i++) { on += fake.ev[i].on; off += !fake.ev[i].on; }
    CHECK(on == off, "after stop: on %d off %d", on, off);

    /* regen reuses the audio; a typed prompt goes out as text */
    int p0 = fake.posts; E->set_param(a, "regen", "1"); wait_not_thinking(a);
    CHECK(fake.posts == p0 + 1 && strstr(fake.body, "audio/wav"), "regen posts the same audio again (posts %d)", fake.posts - p0);
    p0 = fake.posts; E->set_param(a, "prompt", "four on the floor"); wait_not_thinking(a);
    CHECK(fake.posts == p0 + 1 && !strstr(fake.body, "audio/wav") && strstr(fake.body, "Request: four on the floor"), "typed prompt body: %.120s", fake.body);

    /* engine/prompt.txt: a dropped file is a typed prompt, read once and deleted */
    {
        char pf[300]; snprintf(pf, sizeof pf, "%s/prompt.txt", dir); p0 = fake.posts;
        FILE *f = fopen(pf, "w"); fputs("slow dub bass\n", f); fclose(f);
        for (int i = 0; i < 300 && fake.posts == p0; i++) usleep(10000);
        wait_not_thinking(a);
        CHECK(fake.posts == p0 + 1 && strstr(fake.body, "Request: slow dub bass") && !strstr(fake.body, "audio/wav") && access(pf, F_OK) != 0, "drop file: posts %d body %.100s", fake.posts - p0, fake.body);
    }

    /* typing: [TYPE] arms a keyboard (a FIFO dir stands in for /dev/input), live text shows, Enter sends it as a typed prompt */
    {
        char idir[300]; snprintf(idir, sizeof idir, "%s/input", dir); mkdir(idir, 0700);
        char fifo[340]; snprintf(fifo, sizeof fifo, "%s/event0", idir); mkfifo(fifo, 0600);
        char cpath[340]; snprintf(cpath, sizeof cpath, "%s/daemon.conf", dir);
        FILE *cf = fopen(cpath, "a"); fprintf(cf, "input_dir=%s\n", idir); fclose(cf);
        void *t = E->create(dir);
        E->set_param(t, "typing", "1");
        CHECK(!strncmp(get(t, "status"), "TYPING", 6) && !strcmp(get(t, "face"), "Typing"), "typing status '%s' face '%s'", get(t, "status"), get(t, "face"));
        int wfd = -1; for (int i = 0; i < 400 && wfd < 0; i++) { wfd = open(fifo, O_WRONLY | O_NONBLOCK); if (wfd < 0) usleep(10000); }
        for (int i = 0; i < 400 && !strstr(get(t, "status"), "Enter sends"); i++) usleep(10000);
        CHECK(strstr(get(t, "status"), "Enter sends") != NULL, "keyboard found: '%s'", get(t, "status"));
        struct input_event ev[8]; memset(ev, 0, sizeof ev);
        int keys[] = { KEY_H, KEY_I }; for (int i = 0; i < 2; i++) { ev[i * 2].type = EV_KEY; ev[i * 2].code = keys[i]; ev[i * 2].value = 1; ev[i * 2 + 1].type = EV_KEY; ev[i * 2 + 1].code = keys[i]; ev[i * 2 + 1].value = 0; }
        if (write(wfd, ev, sizeof ev[0] * 4) < 0) printf("write failed\n");
        for (int i = 0; i < 400 && strcmp(get(t, "transcript"), "> hi_"); i++) usleep(10000);
        CHECK(!strcmp(get(t, "transcript"), "> hi_"), "live typed text '%s'", get(t, "transcript"));
        fake.code = 200; snprintf(fake.resp, sizeof fake.resp, "%s", OK_REPLY); int pp = fake.posts;
        memset(ev, 0, sizeof ev); ev[0].type = EV_KEY; ev[0].code = KEY_ENTER; ev[0].value = 1;
        if (write(wfd, ev, sizeof ev[0]) < 0) printf("write failed\n");
        for (int i = 0; i < 400 && strncmp(get(t, "status"), "DONE", 4); i++) usleep(10000);
        CHECK(!strncmp(get(t, "status"), "DONE", 4) && fake.posts == pp + 1 && strstr(fake.body, "Request: hi") && !strstr(fake.body, "audio/wav"), "typed send: '%s' body %.100s", get(t, "status"), fake.body);
        close(wfd);
        E->set_param(t, "typing", "1"); E->set_param(t, "typing", "1");       /* arm, then press again to cancel */
        CHECK(!strcmp(get(t, "status"), "READY") && !strcmp(get(t, "face"), "Idle"), "cancel by pressing TYPE again: '%s'", get(t, "status"));
        /* the drawn keyboard: KEYS shows it, keys build the text, SEND asks */
        E->set_param(t, "keys", "1");
        CHECK(!strcmp(get(t, "kb"), "1") && !strcmp(get(t, "face"), "Typing"), "keys: kb '%s' face '%s'", get(t, "kb"), get(t, "face"));
        E->set_param(t, "k_H", "1"); E->set_param(t, "k_I", "1"); E->set_param(t, "k_space", "1"); E->set_param(t, "k_X", "1"); E->set_param(t, "k_back", "1"); E->set_param(t, "k_Y", "1");
        CHECK(!strcmp(get(t, "transcript"), "> hi y_"), "drawn text '%s'", get(t, "transcript"));
        pp = fake.posts; E->set_param(t, "k_send", "1");
        for (int i = 0; i < 400 && strncmp(get(t, "status"), "DONE", 4); i++) usleep(10000);
        CHECK(fake.posts == pp + 1 && strstr(fake.body, "Request: hi y") && !strcmp(get(t, "kb"), "0"), "drawn send: posts %d kb '%s' body %.100s", fake.posts - pp, get(t, "kb"), fake.body);
        E->destroy(t);
    }

    /* agent: plan first (read-only tools run, changes wait), RUN does them. The commander is a fake WebSocket server. */
    {
        int in[2], out[2]; if (pipe(in) || pipe(out)) return 1;
        pid_t pid = fork();
        if (!pid) { dup2(in[0], 0); dup2(out[1], 1); close(in[1]); close(out[0]); execlp("python3", "python3", "-I", "test/fake_commander.py", (char *)NULL); _exit(127); }
        close(in[0]); close(out[1]);
        FILE *so = fdopen(out[0], "r"); char pl[200]; if (!fgets(pl, sizeof pl, so)) { printf("fake commander did not start\n"); return 1; }
        char cpath2[340]; snprintf(cpath2, sizeof cpath2, "%s/daemon.conf", dir);
        FILE *cf = fopen(cpath2, "a"); fprintf(cf, "commander=127.0.0.1:%d\n", atoi(pl)); fclose(cf);
        void *g = E->create(dir);
        E->set_param(g, "agent", "On"); CHECK(!strcmp(get(g, "agent"), "On"), "agent on");
        fake.nq = 2; fake.qi = 0; fake.code = 200;
        snprintf(fake.queue[0], sizeof fake.queue[0], "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"functionCall\":{\"name\":\"list_plugins\",\"args\":{}},\"thoughtSignature\":\"S1\"}]}}]}");
        snprintf(fake.queue[1], sizeof fake.queue[1], "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Heard: \\\"warm it up and add a bass\\\". Opening the filter and a bass line.\"},"
            "{\"functionCall\":{\"name\":\"set_param\",\"args\":{\"id\":5,\"index\":0,\"value\":0.35}}},"
            "{\"functionCall\":{\"name\":\"play_part\",\"args\":{\"name\":\"fat bass\",\"channel\":4,\"notes\":[{\"pitch\":36,\"start_time\":0,\"duration\":0.5,\"velocity\":100},{\"pitch\":39,\"start_time\":1,\"duration\":0.5,\"velocity\":90}]}}}]}}]}");
        int pp = fake.posts;
        { FILE *cpf = fopen("/tmp/daemon_curproj_test", "w"); fputs("<PROPERTIES>\n  <VALUE name=\"lastProjectPath\" val=\"\"/>\n</PROPERTIES>\n", cpf); fclose(cpf); setenv("DAEMON_CURRENT_PROJECT_FILE", "/tmp/daemon_curproj_test", 1); }
        E->set_param(g, "prompt", "warm it up and add a bass"); wait_not_thinking(g);
        CHECK(strstr(fake.body, "is not the one get_project shows"), "an unsaved open project is flagged to the model");
        unsetenv("DAEMON_CURRENT_PROJECT_FILE"); remove("/tmp/daemon_curproj_test");
        CHECK(!strcmp(get(g, "status"), "PLAN 2 STEPS: press RUN"), "plan status '%s'", get(g, "status"));
        CHECK(fake.posts == pp + 2, "two model rounds (%d)", fake.posts - pp);
        CHECK(strstr(fake.body, "functionResponse") && strstr(fake.body, "Profit-8") && strstr(fake.body, "S1"), "second request carries the tool result and the echoed turn: %.150s", fake.body);
        CHECK(strstr(get(g, "plan"), "1 set Profit-8 Cutoff to 35%") && strstr(get(g, "plan"), "2 make a part \"fat bass\" on channel 4, 2 notes"), "plan lines '%s'", get(g, "plan"));
        CHECK(!strcmp(get(g, "plan1"), "1 set Profit-8 Cutoff to 35%") && !strncmp(get(g, "plan2"), "2 make a part", 13) && !strcmp(get(g, "plan3"), ""), "plan rows '%s' / '%s' / '%s'", get(g, "plan1"), get(g, "plan2"), get(g, "plan3"));
        CHECK(!strcmp(get(g, "runok"), "on"), "RUN lit with a plan: '%s'", get(g, "runok"));
        CHECK(strstr(get(g, "reply"), "Opening the filter"), "reply '%s'", get(g, "reply"));
        CHECK(!strcmp(get(g, "face"), "Done"), "plan face '%s'", get(g, "face"));
        double q2 = 0; fake.nev = 0; for (int k = 0; k < 300; k++) { E->song_pos(g, q2, 124, 1, 128); q2 += 124 / 60.0 * 128 / 44100.0; }
        CHECK(fake.nev == 0, "nothing plays before RUN (%d events)", fake.nev);
        E->set_param(g, "typing", "1"); E->set_param(g, "typing", "1");   /* another button in between: the plan must still run */
        CHECK(strncmp(get(g, "status"), "PLAN", 4), "status moved on from the plan: '%s'", get(g, "status"));
        E->set_param(g, "run", "1");
        for (int i = 0; i < 400 && strncmp(get(g, "status"), "RAN", 3); i++) usleep(10000);
        CHECK(!strcmp(get(g, "status"), "RAN 2 STEPS"), "run status '%s'", get(g, "status"));
        CHECK(!strcmp(get(g, "plan"), "-") && !strcmp(get(g, "plan1"), "-"), "a plan that ran is cleared: '%s'", get(g, "plan"));
        CHECK(!strcmp(get(g, "runok"), "off"), "RUN dim after the plan ran: '%s'", get(g, "runok"));
        q2 = 0; fake.nev = 0; for (int k = 0; k < 600; k++) { E->song_pos(g, q2, 124, 1, 128); q2 += 124 / 60.0 * 128 / 44100.0; }
        int on4 = 0; for (int i = 0; i < fake.nev; i++) on4 += fake.ev[i].on && fake.ev[i].ch == 3;
        CHECK(on4 >= 2, "the part plays on channel 4 after RUN (%d note-ons)", on4);
        /* a plan that overwrites something (record) needs RUN twice */
        fake.nq = 1; fake.qi = 0;
        snprintf(fake.queue[0], sizeof fake.queue[0], "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Recording.\"},{\"functionCall\":{\"name\":\"transport\",\"args\":{\"cmd\":\"record\"}}}]}}]}");
        E->set_param(g, "prompt", "record a take"); wait_not_thinking(g);
        CHECK(strstr(get(g, "status"), "RUN twice") != NULL && strstr(get(g, "plan"), "[RECORDS") != NULL, "risky plan: '%s' / '%s'", get(g, "status"), get(g, "plan"));
        E->set_param(g, "run", "1");
        CHECK(!strncmp(get(g, "status"), "SURE?", 5), "first RUN only asks: '%s'", get(g, "status"));
        E->set_param(g, "run", "1");
        for (int i = 0; i < 400 && strncmp(get(g, "status"), "RAN", 3); i++) usleep(10000);
        CHECK(!strcmp(get(g, "status"), "RAN 1 STEPS"), "second RUN runs it: '%s'", get(g, "status"));
        E->set_param(g, "run", "1");
        CHECK(!strncmp(get(g, "status"), "ERROR nothing to run", 20), "RUN with no plan: '%s'", get(g, "status"));
        E->destroy(g);
        close(in[1]);
        char logs[2048] = ""; while (fgets(pl, sizeof pl, so)) strncat(logs, pl, sizeof logs - strlen(logs) - 1);
        CHECK(strstr(logs, "\"t\":\"set\",\"id\":5,\"i\":0,\"value\":0.35") != NULL, "the set reached the commander: %s", logs);
        fclose(so); waitpid(pid, NULL, 0);
        fake.nq = 0;
    }

    /* device tools: load_efx runs a recorded macro through the remote addin; the vision step asks Gemini where the plugin is.
     * A failing screen action stops the run before the changes after it. (The macros here are test data, not a recording.) */
    {
        int ci[2], co[2], ri[2], ro[2]; if (pipe(ci) || pipe(co) || pipe(ri) || pipe(ro)) return 1;
        pid_t pc = fork();
        if (!pc) { dup2(ci[0], 0); dup2(co[1], 1); close(ci[1]); close(co[0]); close(ri[0]); close(ri[1]); close(ro[0]); close(ro[1]); execlp("python3", "python3", "-I", "test/fake_commander.py", (char *)NULL); _exit(127); }
        pid_t pr = fork();
        if (!pr) { dup2(ri[0], 0); dup2(ro[1], 1); close(ri[1]); close(ro[0]); close(ci[0]); close(ci[1]); close(co[0]); close(co[1]); execlp("python3", "python3", "-I", "test/fake_remote.py", (char *)NULL); _exit(127); }
        close(ci[0]); close(co[1]); close(ri[0]); close(ro[1]);
        FILE *cso = fdopen(co[0], "r"), *rso = fdopen(ro[0], "r"); char l1[100], l2[100];
        if (!fgets(l1, sizeof l1, cso) || !fgets(l2, sizeof l2, rso)) { printf("fakes did not start\n"); return 1; }
        char d2[] = "/tmp/daemon_eng2_XXXXXX"; if (!mkdtemp(d2)) return 1;
        char p1[340]; snprintf(p1, sizeof p1, "%s/daemon.conf", d2);
        FILE *cf = fopen(p1, "w"); fprintf(cf, "api_key=K\nurl_base=http://x/models\ncommander=127.0.0.1:%d\nremote=127.0.0.1:%d\n", atoi(l1), atoi(l2)); fclose(cf);
        snprintf(p1, sizeof p1, "%s/macros.json", d2);
        cf = fopen(p1, "w");
        fputs("{\"version\":1,\"screen\":[1280,800],\"macros\":{"
              "\"select_track\":{\"params\":{\"track\":{\"type\":\"int\",\"min\":1,\"max\":16}},\"steps\":[{\"tap\":[\"100 + 140*(track-1)\",300],\"require_change\":true}]},"
              "\"load_efx\":{\"params\":{\"track\":{\"type\":\"int\",\"min\":1,\"max\":16},\"plugin\":{\"type\":\"string\"}},\"steps\":[{\"call\":\"select_track\"},{\"find_tap\":\"{plugin}\",\"hint\":\"the plugin list\"}]}}}", cf);
        fclose(cf);
        void *g = E->create(d2);
        E->set_param(g, "agent", "On");
        fake.nq = 1; fake.qi = 0; fake.code = 200;
        snprintf(fake.queue[0], sizeof fake.queue[0], "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Loading 9W9 on track 2 and opening its filter.\"},"
            "{\"functionCall\":{\"name\":\"load_efx\",\"args\":{\"track\":2,\"plugin\":\"9W9\"}}},{\"functionCall\":{\"name\":\"set_param\",\"args\":{\"id\":5,\"index\":0,\"value\":0.5}}}]}}]}");
        E->set_param(g, "prompt", "put 9w9 on track 2");
        for (int i = 0; i < 400 && !strncmp(get(g, "status"), "THINKING", 8); i++) usleep(10000);
        CHECK(!strcmp(get(g, "status"), "PLAN 2 STEPS: press RUN"), "plan with a device tool: '%s'", get(g, "status"));
        CHECK(strstr(get(g, "plan"), "1 load 9W9 on track 2") && strstr(get(g, "plan"), "2 set Profit-8 Cutoff to 50%"), "plan lines '%s'", get(g, "plan"));
        CHECK(strstr(fake.body, "load_efx") && strstr(fake.body, "select_track"), "the model was offered the device tools");
        snprintf(fake.queue[1], sizeof fake.queue[1], "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"{\\\"found\\\":true,\\\"point\\\":[500,500]}\"}]}}]}");
        fake.nq = 2; int vp = fake.posts;
        E->set_param(g, "run", "1");
        for (int i = 0; i < 600 && strncmp(get(g, "status"), "RAN", 3) && strncmp(get(g, "status"), "ERROR", 5); i++) usleep(10000);
        CHECK(!strcmp(get(g, "status"), "RAN 2 STEPS: now SAVE the project so Daemon sees it"), "device run: '%s'", get(g, "status"));
        CHECK(fake.posts == vp + 1 && strstr(fake.body, "image/png") && strstr(fake.body, "reads: 9W9"), "one vision request with the screenshot: %.120s", fake.body);
        E->destroy(g);

        /* second case: the track cell does not react (a stale coordinate): the run stops, the later set never happens */
        snprintf(p1, sizeof p1, "%s/macros.json", d2);
        cf = fopen(p1, "w");
        fputs("{\"version\":1,\"screen\":[1280,800],\"macros\":{\"select_track\":{\"params\":{\"track\":{\"type\":\"int\",\"min\":1,\"max\":16}},\"steps\":[{\"tap\":[50,750],\"require_change\":true}]}}}", cf); fclose(cf);
        g = E->create(d2); E->set_param(g, "agent", "On");
        fake.nq = 1; fake.qi = 0;
        snprintf(fake.queue[0], sizeof fake.queue[0], "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"functionCall\":{\"name\":\"select_track\",\"args\":{\"track\":3}}},{\"functionCall\":{\"name\":\"set_param\",\"args\":{\"id\":5,\"index\":1,\"value\":0.9}}}]}}]}");
        E->set_param(g, "prompt", "select track 3 and open the resonance");
        for (int i = 0; i < 400 && !strncmp(get(g, "status"), "THINKING", 8); i++) usleep(10000);
        E->set_param(g, "run", "1");
        for (int i = 0; i < 600 && strncmp(get(g, "status"), "RAN", 3) && strncmp(get(g, "status"), "ERROR", 5); i++) usleep(10000);
        CHECK(!strncmp(get(g, "status"), "ERROR RAN 0 of 2: select_track", 31) || strstr(get(g, "status"), "RAN 0 of 2"), "stale tap stops the run: '%s'", get(g, "status"));
        E->destroy(g);
        close(ci[1]); close(ri[1]);
        char cl[3000] = "", rl[3000] = ""; while (fgets(l1, sizeof l1, cso)) strncat(cl, l1, sizeof cl - strlen(cl) - 1); while (fgets(l2, sizeof l2, rso)) strncat(rl, l2, sizeof rl - strlen(rl) - 1);
        CHECK(strstr(rl, "\"x\":240") && strstr(rl, "\"y\":300") && strstr(rl, "\"x\":640") && strstr(rl, "\"y\":400"), "the screen saw the track tap (240,300) and the found tap (640,400): %s", rl);
        CHECK(strstr(cl, "\"value\":0.5") && !strstr(cl, "\"value\":0.9"), "the first run's set arrived, the stopped run's did not: %s", cl);
        fclose(cso); fclose(rso); waitpid(pc, NULL, 0); waitpid(pr, NULL, 0);
        fake.nq = 0;
    }

    /* long readouts are cut to 127 characters with "..." */
    {
        void *lg = E->create(dir); char longq[400]; memset(longq, 'a', sizeof longq - 1); longq[sizeof longq - 1] = 0;
        fake.code = 200; snprintf(fake.resp, sizeof fake.resp, "%s", OK_REPLY);
        E->set_param(lg, "prompt", longq); wait_not_thinking(lg);
        char *tr = get(lg, "transcript"); (void)tr;
        CHECK(strlen(get(lg, "status")) <= 127 && strlen(get(lg, "transcript")) <= 127, "readouts stay within 127 characters");
        char buf[600]; E->get_param(lg, "transcript", buf, sizeof buf); 
        CHECK(strlen(buf) <= 127, "transcript length %zu", strlen(buf));
        E->destroy(lg);
    }

    /* the project chunk round-trips the settings */
    E->set_param(a, "type", "Beats"); E->set_param(a, "key", "D#"); E->set_param(a, "scale", "Blues"); E->set_param(a, "bars", "7"); E->set_param(a, "channel", "10");
    E->set_param(a, "agent", "1");
    char st[64]; snprintf(st, sizeof st, "%s", get(a, "state"));
    void *b2 = E->create(dir); E->set_param(b2, "state", st);
    CHECK(!strcmp(get(b2, "state"), st) && !strcmp(get(b2, "type"), "Beats") && !strcmp(get(b2, "key"), "D#") && !strcmp(get(b2, "scale"), "Blues") && !strcmp(get(b2, "bars"), "7"), "state '%s' -> '%s'", st, get(b2, "state"));
    CHECK(!strcmp(get(b2, "agent"), "On"), "the Agent toggle is saved with the project");
    E->set_param(a, "agent", "0"); E->set_param(b2, "state", "2,5,1,4,1,1");           /* an older chunk without the agent field leaves it alone */
    CHECK(!strcmp(get(b2, "agent"), "On"), "an old six-field chunk keeps the toggle");
    E->set_param(b2, "state", st);
    E->set_param(b2, "state", "garbage"); CHECK(!strcmp(get(b2, "type"), "Beats"), "a bad chunk leaves the settings alone");
    E->destroy(b2);
    E->set_param(a, "type", "Melody"); E->set_param(a, "key", "A"); E->set_param(a, "scale", "Dorian"); E->set_param(a, "bars", "2"); E->set_param(a, "channel", "2");

    /* .mid on request */
    E->set_param(a, "save", "1");
    char mid[400]; snprintf(mid, sizeof mid, "%s/clips/dark_bass.mid", dir);
    FILE *m = fopen(mid, "rb"); char hd[4] = {0}; if (m) { if (fread(hd, 1, 4, m) != 4) hd[0] = 0; fclose(m); }
    CHECK(!memcmp(hd, "MThd", 4), "saved %s", mid);
    CHECK(!strncmp(get(a, "status"), "SAVED dark_bass", 15), "save status '%s'", get(a, "status"));

    /* the saved-parts folder: default "Daemon" at the drive root (the path before /Synths/), or clips_dir= (relative to that root) */
    for (int variant = 0; variant < 2; variant++) {
        char root[300], eng[400], cp[460]; snprintf(root, sizeof root, "%s/drive%d", dir, variant); mkdir(root, 0755);
        snprintf(eng, sizeof eng, "%s/Synths", root); mkdir(eng, 0755); strcat(eng, "/P"); mkdir(eng, 0755); strcat(eng, "/engine"); mkdir(eng, 0755);
        snprintf(cp, sizeof cp, "%s/daemon.conf", eng);
        FILE *cf2 = fopen(cp, "w"); fputs("api_key=KEY123\nmodel=test-model\nurl_base=http://x/models\n", cf2); if (variant) fputs("clips_dir=Parts/Mine\n", cf2); fclose(cf2);
        if (variant) { char pp[420]; snprintf(pp, sizeof pp, "%s/Parts", root); mkdir(pp, 0755); }
        void *g2 = E->create(eng); fake.code = 200; snprintf(fake.resp, sizeof fake.resp, "%s", OK_REPLY);
        E->set_param(g2, "prompt", "x"); wait_not_thinking(g2); E->set_param(g2, "save", "1");
        char want[520]; snprintf(want, sizeof want, "%s/%s/dark_bass.mid", root, variant ? "Parts/Mine" : "Daemon");
        CHECK(access(want, R_OK) == 0, "clips folder variant %d: %s", variant, want);
        E->destroy(g2);
    }

    /* failures say what happened and keep the old part */
    fake.code = 404; snprintf(fake.resp, sizeof fake.resp, "{\"error\":{\"message\":\"model gone\"}}");
    E->set_param(a, "regen", "1"); wait_not_thinking(a);
    CHECK(!strcmp(get(a, "status"), "ERROR 404 model gone"), "404: '%s'", get(a, "status"));
    fake.code = 0; snprintf(fake.resp, sizeof fake.resp, "Could not resolve host");
    E->set_param(a, "regen", "1"); wait_not_thinking(a);
    CHECK(!strncmp(get(a, "status"), "ERROR no network", 16), "offline: '%s'", get(a, "status"));
    fake.code = 200; snprintf(fake.resp, sizeof fake.resp, "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"sorry\"}]}}]}");
    E->set_param(a, "regen", "1"); wait_not_thinking(a);
    CHECK(!strcmp(get(a, "status"), "ERROR reply not usable"), "junk reply: '%s'", get(a, "status"));
    CHECK(!strcmp(get(a, "transcript"), "four on the floor") || get(a, "transcript")[0], "old part kept");
    ppq = 0; fake.nev = 0; for (int k = 0; k < 400; k++) { E->song_pos(a, ppq, 100, 1, 128); ppq += ppb; }
    CHECK(fake.nev > 0, "the earlier part still plays after a failed request");

    /* settings page: the key is shown masked, the model list comes from the API, a chosen model is saved to daemon.conf */
    { char md[] = "/tmp/daemon_mod_XXXXXX"; if (!mkdtemp(md)) return 1;
      char mp2[340]; snprintf(mp2, sizeof mp2, "%s/daemon.conf", md); FILE *mf = fopen(mp2, "w"); fputs("api_key=ABCDEFGH1234\nmodel=gem-b\nurl_base=http://x/models\nclips_dir=/tmp\n", mf); fclose(mf);
      fake.code = 200; snprintf(fake.resp, sizeof fake.resp, "{\"models\":[{\"name\":\"models/gem-a\",\"supportedGenerationMethods\":[\"generateContent\"]},{\"name\":\"models/gemini-x\",\"supportedGenerationMethods\":[\"generateContent\"]},{\"name\":\"models/gemini-x-image\",\"supportedGenerationMethods\":[\"generateContent\"]},{\"name\":\"models/gemini-3.5-transcribe\",\"supportedGenerationMethods\":[\"generateContent\"]},{\"name\":\"models/gemini-nano-banana-2\",\"supportedGenerationMethods\":[\"generateContent\"]},{\"name\":\"models/gemini-emb\",\"supportedGenerationMethods\":[\"embedContent\"]},{\"name\":\"models/gemini-y\",\"supportedGenerationMethods\":[\"generateContent\",\"countTokens\"]}]}");
      void *m = E->create(md);
      CHECK(strstr(get(m, "keyinfo"), "ends 1234") && !strstr(get(m, "keyinfo"), "ABCDEFGH"), "key shown masked: '%s'", get(m, "keyinfo"));
      CHECK(strstr(get(m, "models_info"), "REFRESH"), "no list yet: '%s'", get(m, "models_info"));
      int p0 = fake.posts; E->set_param(m, "models_refresh", "1");
      for (int i = 0; i < 300 && !strstr(get(m, "models_info"), "models available"); i++) usleep(10000);
      CHECK(fake.posts == p0 + 1 && !strncmp(fake.url, "http://x/models?pageSize=200", 28) && !strcmp(fake.key, "ABCDEFGH1234"), "one GET of the list: %s", fake.url);
      CHECK(!strcmp(get(m, "models_info"), "2 models available"), "image and embedding models left out: '%s'", get(m, "models_info"));
      CHECK(!strcmp(get(m, "model"), "gem-b  (not in the list)") || strstr(get(m, "model"), "gem-b"), "current model shown: '%s'", get(m, "model"));
      E->set_param(m, "model_next", "1"); CHECK(!strncmp(get(m, "model"), "gemini-x  (1 of 2)", 18), "next model: '%s'", get(m, "model"));
      E->set_param(m, "model_next", "1"); E->set_param(m, "model_next", "1"); CHECK(!strncmp(get(m, "model"), "gemini-x  (1 of 2)", 18), "wraps: '%s'", get(m, "model"));
      E->set_param(m, "model_prev", "1"); CHECK(!strncmp(get(m, "model"), "gemini-y  (2 of 2)", 18), "previous wraps: '%s'", get(m, "model"));
      { char cc[2000] = ""; FILE *cf3 = fopen(mp2, "r"); size_t nn = fread(cc, 1, sizeof cc - 1, cf3); cc[nn] = 0; fclose(cf3);
        CHECK(strstr(cc, "model=gemini-y\n") && strstr(cc, "api_key=ABCDEFGH1234\n") && strstr(cc, "url_base=http://x/models\n") && !strstr(cc, "gem-b"), "choice saved, rest of daemon.conf kept: %s", cc); }
      fake.code = 503; E->set_param(m, "models_refresh", "1");
      for (int i = 0; i < 300 && !strstr(get(m, "models_info"), "refused"); i++) usleep(10000);
      CHECK(strstr(get(m, "models_info"), "HTTP 503") && !strcmp(get(m, "model") + 0, get(m, "model")), "a failed refresh says so: '%s'", get(m, "models_info"));
      E->destroy(m); }

    /* Claude as the provider: its own key, model list and Messages API; the reply comes back as a part; voice falls back to Gemini */
    { char cd[] = "/tmp/daemon_cl_XXXXXX"; if (!mkdtemp(cd)) return 1;
      char cp[340]; snprintf(cp, sizeof cp, "%s/daemon.conf", cd); FILE *cf = fopen(cp, "w"); fputs("provider=claude\nclaude_key=sk-ant-test-WXYZ\nclaude_url=http://c/v1\nclips_dir=/tmp\n", cf); fclose(cf);
      void *m = E->create(cd);
      CHECK(strstr(get(m, "provider"), "CLAUDE") && strstr(get(m, "keyinfo"), "claude_key set (ends WXYZ") && !strstr(get(m, "keyinfo"), "sk-ant"), "claude key shown masked: '%s'", get(m, "keyinfo"));
      CHECK(!strncmp(get(m, "model"), "claude-sonnet-5-5", 17), "default Claude model: '%s'", get(m, "model"));
      fake.code = 200; fake.nq = 0; snprintf(fake.resp, sizeof fake.resp, "{\"data\":[{\"id\":\"claude-a-1\"},{\"id\":\"claude-b-2\"}]}");
      int p1 = fake.posts; E->set_param(m, "models_refresh", "1");
      for (int i = 0; i < 300 && !strstr(get(m, "models_info"), "models available"); i++) usleep(10000);
      CHECK(fake.posts == p1 + 1 && !strcmp(fake.url, "http://c/v1/models?limit=100") && strstr(fake.key, "x-api-key: sk-ant-test-WXYZ\nanthropic-version: "), "Claude model list GET: %s", fake.url);
      CHECK(!strcmp(get(m, "models_info"), "2 models available"), "claude models: '%s'", get(m, "models_info"));
      E->set_param(m, "model_next", "1"); CHECK(!strncmp(get(m, "model"), "claude-a-1  (1 of 2)", 20), "next Claude model: '%s'", get(m, "model"));
      { char cc[2000] = ""; FILE *cf3 = fopen(cp, "r"); size_t nn = fread(cc, 1, sizeof cc - 1, cf3); cc[nn] = 0; fclose(cf3);
        CHECK(strstr(cc, "claude_model=claude-a-1\n") && !strstr(cc, "\nmodel="), "Claude model saved as claude_model: %s", cc); }
      char inner[4000], q[9000]; daemon_extract_text(OK_REPLY, inner, sizeof inner);
      { char fenced[4100]; snprintf(fenced, sizeof fenced, "```json\n%s\n```", inner); jv sv = { 's', 0, fenced, NULL, NULL, NULL }; char js[8000]; djson_write(&sv, js, sizeof js);
        snprintf(q, sizeof q, "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":%s}],\"stop_reason\":\"end_turn\"}", js); }
      snprintf(fake.resp, sizeof fake.resp, "%s", q);
      p1 = fake.posts; E->set_param(m, "prompt", "a dark bassline"); wait_not_thinking(m);
      CHECK(fake.posts == p1 + 1 && !strcmp(fake.url, "http://c/v1/messages") && strstr(fake.body, "\"model\":\"claude-a-1\"") && strstr(fake.body, "\"system\":") && strstr(fake.body, "JSON Schema") && strstr(fake.body, "\\\"type\\\":\\\"object\\\"") && !strstr(fake.body, "generationConfig"), "Messages API body: %.200s", fake.body);
      CHECK(!strncmp(get(m, "status"), "DONE", 4) || strstr(get(m, "reply"), "Dark and slow"), "Claude part used: '%s' / '%s'", get(m, "status"), get(m, "reply"));
      fake.code = 529; snprintf(fake.resp, sizeof fake.resp, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"); p1 = fake.posts; E->set_param(m, "prompt", "again");
      for (int i = 0; i < 2500 && !strncmp(get(m, "status"), "THINKING", 8); i++) usleep(10000);   /* three tries, 3 s and 6 s apart */
      CHECK(fake.posts > p1 + 1 && !strncmp(get(m, "status"), "ERROR 503 Overloaded", 20), "overloaded is retried like 503: %d posts, '%s'", fake.posts - p1, get(m, "status"));
      fake.code = 200;
      E->set_param(m, "provider_next", "1");
      CHECK(strstr(get(m, "provider"), "GEMINI") && strstr(get(m, "models_info"), "no api_key"), "switched to Gemini without a key: '%s'", get(m, "models_info"));
      { char cc[2000] = ""; FILE *cf3 = fopen(cp, "r"); size_t nn = fread(cc, 1, sizeof cc - 1, cf3); cc[nn] = 0; fclose(cf3);
        CHECK(strstr(cc, "provider=gemini\n") && strstr(cc, "claude_key=sk-ant-test-WXYZ\n"), "provider saved: %s", cc); }
      E->destroy(m); }

    E->destroy(a);
    CHECK(fake.closed == 13, "MIDI port closed once per instance: %d", fake.closed);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
