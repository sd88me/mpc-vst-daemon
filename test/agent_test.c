#include "../src/daemon_agent.h"
#include "../src/daemon_core.h"
#include "../src/daemon_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* a Gemini turn: a thought, a sentence, a read-only call and two changing ones (with a signature that must survive the echo) */
static const char *TURN =
 "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":["
 "{\"text\":\"thinking...\",\"thought\":true},"
 "{\"text\":\"Warming up the filter.\"},"
 "{\"functionCall\":{\"name\":\"get_plugin_params\",\"args\":{\"id\":5}},\"thoughtSignature\":\"SIG==\"},"
 "{\"functionCall\":{\"name\":\"set_param\",\"args\":{\"id\":5,\"index\":0,\"value\":0.35}}},"
 "{\"functionCall\":{\"name\":\"play_part\",\"args\":{\"name\":\"bass\",\"channel\":3,\"notes\":[{\"pitch\":36,\"start_time\":0,\"duration\":0.5,\"velocity\":100},{\"pitch\":39,\"start_time\":1,\"duration\":0.5,\"velocity\":90}]}}}"
 "]},\"finishReason\":\"STOP\"}]}";

static int played;
static int play_cb(void *u, const char *args, char *res, size_t cap) {
    (void)u; static dpart_t p; int n = daemon_parse_reply(args, 32, &p);
    if (n <= 0) { snprintf(res, cap, "no usable notes"); return -1; }
    played = n; snprintf(res, cap, "%d notes, %.0f beats", n, p.loop_beats); return 0;
}

int main(void) {
    CHECK(agent_tool_is_read_only("get_project") && agent_tool_is_read_only("list_plugins") && agent_tool_is_read_only("get_plugin_params"), "read-only tools");
    CHECK(!agent_tool_is_read_only("set_param") && !agent_tool_is_read_only("transport") && !agent_tool_is_read_only("play_part") && !agent_tool_is_read_only("rm_rf"), "changing and unknown tools are not read-only");
    CHECK(agent_tool_known("transport") && !agent_tool_known("load_plugin") && !agent_tool_known(""), "known tools");

    static aturn_t t;
    CHECK(agent_parse_turn(TURN, &t) == 0 && t.n == 3, "parse turn: %d calls", t.n);
    CHECK(!strcmp(t.text, "Warming up the filter."), "thought text dropped, text kept: '%s'", t.text);
    CHECK(!strcmp(t.calls[0].name, "get_plugin_params") && !strcmp(t.calls[0].args, "{\"id\":5}"), "call 0 %s %s", t.calls[0].name, t.calls[0].args);
    CHECK(!strcmp(t.calls[1].name, "set_param") && strstr(t.calls[1].args, "0.35"), "call 1 args %s", t.calls[1].args);
    CHECK(t.raw && strstr(t.raw, "\"thoughtSignature\":\"SIG==\"") && strstr(t.raw, "\"thought\":true") && strstr(t.raw, "\"role\":\"model\""), "raw content keeps the signature: %.200s", t.raw ? t.raw : "(null)");
    aturn_t bad;
    CHECK(agent_parse_turn("{\"error\":{\"message\":\"quota\"}}", &bad) == -1 && !strcmp(bad.text, "quota"), "api error '%s'", bad.text);
    CHECK(agent_parse_turn("{\"candidates\":[{\"finishReason\":\"SAFETY\"}]}", &bad) == -1, "blocked turn");
    CHECK(agent_parse_turn("nope", &bad) == -1, "not json");

    /* history and request: valid JSON, echoes the raw turn, answers every call */
    ahist_t h; agent_hist_init(&h);
    static unsigned char wav[300]; for (int i = 0; i < 300; i++) wav[i] = (unsigned char)i;
    CHECK(agent_hist_user(&h, "set up track 1 with \"9w9\"\nplease", wav, sizeof wav) == 0, "user turn");
    CHECK(agent_hist_model(&h, &t) == 0, "model turn");
    const char *res[3] = { "[{\"i\":0}]", NULL, NULL };
    CHECK(agent_hist_results(&h, &t, res) == 0 && h.n == 3, "results turn (%d items)", h.n);
    actx_t ctx = { 124, "F Minor", 4, "" };
    static char req[1 << 20]; int rl = agent_build_request(&ctx, &h, req, sizeof req);
    CHECK(rl > 1000, "request length %d", rl);
    { char tiny[500]; CHECK(agent_build_request(&ctx, &h, tiny, sizeof tiny) == -1, "small request buffer"); }
    FILE *f = fopen("/tmp/daemon_agent_req.json", "w"); fputs(req, f); fclose(f);
    CHECK(strstr(req, "\"functionResponse\"") && strstr(req, "not run") && strstr(req, "SIG==") && strstr(req, "inline_data"), "request pieces");
    agent_hist_free(&h);

    /* running tools against the fake commander */
    int in[2], out[2]; if (pipe(in) || pipe(out)) return 1;
    pid_t pid = fork();
    if (!pid) { dup2(in[0], 0); dup2(out[1], 1); close(in[1]); close(out[0]); execlp("python3", "python3", "-I", "test/fake_commander.py", (char *)NULL); _exit(127); }
    close(in[0]); close(out[1]);
    FILE *so = fdopen(out[0], "r"); char line[512]; if (!fgets(line, sizeof line, so)) { printf("fake commander did not start\n"); return 1; }
    char err[100]; cmdr_t *c = cmdr_open("127.0.0.1", atoi(line), err, sizeof err); CHECK(c != NULL, "open: %s", err); if (!c) return 1;
    static char r[1 << 16];
    CHECK(agent_exec(c, &(acall_t){ "list_plugins", "{}" }, play_cb, NULL, r, sizeof r) == 0 && strstr(r, "Profit-8"), "list_plugins: %s", r);
    CHECK(agent_exec(c, &(acall_t){ "get_plugin_params", "{\"id\":5}" }, play_cb, NULL, r, sizeof r) == 0 && strstr(r, "Cutoff"), "get_plugin_params: %.80s", r);
    CHECK(agent_exec(c, &(acall_t){ "get_plugin_params", "{\"id\":77}" }, play_cb, NULL, r, sizeof r) == -1 && strstr(r, "error"), "unknown plugin: %s", r);
    CHECK(agent_exec(c, &(acall_t){ "get_project", "{}" }, play_cb, NULL, r, sizeof r) == 0 && strstr(r, "F Minor"), "get_project: %.80s", r);
    CHECK(agent_exec(c, &t.calls[1], play_cb, NULL, r, sizeof r) == 0 && !strcmp(r, "{\"ok\":true,\"now_shows\":\"35%\"}"), "set_param: %s", r);
    CHECK(agent_exec(c, &(acall_t){ "set_param", "{\"id\":5,\"index\":42,\"value\":1}" }, play_cb, NULL, r, sizeof r) == -1 && strstr(r, "no such parameter"), "bad parameter: %s", r);
    CHECK(agent_exec(c, &(acall_t){ "transport", "{\"cmd\":\"play\"}" }, play_cb, NULL, r, sizeof r) == 0, "transport: %s", r);
    CHECK(agent_exec(c, &(acall_t){ "transport", "{\"cmd\":\"explode\"}" }, play_cb, NULL, r, sizeof r) == -1, "bad transport: %s", r);
    CHECK(agent_exec(c, &t.calls[2], play_cb, NULL, r, sizeof r) == 0 && played == 2 && strstr(r, "2 notes, 2 beats"), "play_part: %s (played %d)", r, played);
    CHECK(agent_exec(c, &(acall_t){ "play_part", "{\"notes\":[]}" }, play_cb, NULL, r, sizeof r) == -1, "empty part: %s", r);
    CHECK(agent_exec(c, &(acall_t){ "load_plugin", "{}" }, play_cb, NULL, r, sizeof r) == -1 && strstr(r, "unknown tool"), "unknown tool: %s", r);
    CHECK(agent_exec(NULL, &(acall_t){ "set_param", "{\"id\":1,\"index\":0,\"value\":1}" }, play_cb, NULL, r, sizeof r) == -1 && strstr(r, "no commander"), "without commander: %s", r);
    CHECK(agent_exec(c, &(acall_t){ "set_param", "not json" }, play_cb, NULL, r, sizeof r) == -1, "bad arguments");

    char d[200];
    agent_describe(c, &t.calls[1], d, sizeof d); CHECK(!strcmp(d, "set Profit-8 Cutoff to 35%"), "describe set: '%s'", d);
    agent_describe(c, &t.calls[2], d, sizeof d); CHECK(!strcmp(d, "make a part \"bass\" on channel 3, 2 notes"), "describe part: '%s'", d);
    agent_describe(c, &(acall_t){ "transport", "{\"cmd\":\"record\"}" }, d, sizeof d); CHECK(!strcmp(d, "press record"), "describe transport: '%s'", d);

    cmdr_close(c); close(in[1]); fclose(so);
    agent_turn_free(&t);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
