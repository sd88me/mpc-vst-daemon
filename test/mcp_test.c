#include "../src/daemon_mcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main(void) {
    int in[2], out[2]; if (pipe(in) || pipe(out)) return 1;
    pid_t pid = fork();
    if (!pid) { dup2(in[0], 0); dup2(out[1], 1); close(in[1]); close(out[0]); execlp("python3", "python3", "-I", "test/fake_remote.py", (char *)NULL); _exit(127); }
    close(in[0]); close(out[1]);
    FILE *so = fdopen(out[0], "r"); char line[400]; if (!fgets(line, sizeof line, so)) return 1;
    int port = atoi(line); char err[120] = "";
    CHECK(mcp_open("127.0.0.1", 1, err, sizeof err) == NULL && strstr(err, "not reachable"), "closed port '%s'", err);
    mcp_t *m = mcp_open("127.0.0.1", port, err, sizeof err); CHECK(m != NULL, "open: %s", err); if (!m) return 1;
    char text[2000]; char *img = NULL;
    CHECK(mcp_call(m, "get_screen_info", "{}", text, sizeof text, NULL) == 0 && strstr(text, "\"width\":1280") && strstr(text, "origin top left"), "screen info '%.80s'", text);
    CHECK(mcp_call(m, "tap", "{\"x\":100,\"y\":200,\"screenshot\":\"full\"}", text, sizeof text, &img) == 0 && strstr(text, "The screen changed") && img && strlen(img) > 20, "tap with image '%s'", text);
    free(img); img = NULL;
    CHECK(mcp_call(m, "tap", "{\"x\":100,\"y\":200,\"screenshot\":\"none\"}", text, sizeof text, &img) == 0 && img == NULL, "tap without image");
    CHECK(mcp_call(m, "tap", "{\"x\":5,\"y\":750}", text, sizeof text, NULL) == 0 && strstr(text, "did not change"), "unchanged screen is reported in the text: '%s'", text);
    CHECK(mcp_call(m, "press_button", "{\"name\":\"NOPE\"}", text, sizeof text, NULL) == -1 && strstr(text, "No button"), "tool error: '%s'", text);
    CHECK(mcp_call(m, "no_such_tool", "{}", text, sizeof text, NULL) == -1, "unknown tool fails");
    mcp_close(m);
    close(in[1]);
    char log[2000] = ""; while (fgets(line, sizeof line, so)) strncat(log, line, sizeof log - strlen(log) - 1);
    CHECK(strstr(log, "CALL tap {\"screenshot\":\"full\",\"x\":100,\"y\":200}") && strstr(log, "CALL get_screen_info {}"), "server log: %s", log);
    fclose(so); waitpid(pid, NULL, 0);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
