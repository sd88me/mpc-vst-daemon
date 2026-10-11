#include "../src/daemon_kbd.h"
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int press(kbd_line *l, int code) { int r = kbd_key(l, code, 1); kbd_key(l, code, 0); return r; }
static void type(kbd_line *l, const char *s) {                  /* the keys that make s on a US keyboard */
    for (; *s; s++) {
        int shift = 0, code = 0;
        char c = *s;
        if (c >= 'A' && c <= 'Z') { shift = 1; c += 32; }
        static const char *rows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" }; static const int base[3] = { KEY_Q, KEY_A, KEY_Z };
        for (int r = 0; r < 3; r++) { const char *p = strchr(rows[r], c); if (p && c) code = base[r] + (int)(p - rows[r]); }
        if (c == ' ') code = KEY_SPACE; else if (c == '/') code = KEY_SLASH; else if (c == ',') code = KEY_COMMA; else if (c == '.') code = KEY_DOT;
        else if (c == '-') code = KEY_MINUS; else if (c >= '1' && c <= '9') code = KEY_1 + (c - '1'); else if (c == '0') code = KEY_0;
        if (c == '?') { shift = 1; code = KEY_SLASH; } if (c == '!') { shift = 1; code = KEY_1; }
        if (shift) kbd_key(l, KEY_LEFTSHIFT, 1);
        press(l, code);
        if (shift) kbd_key(l, KEY_LEFTSHIFT, 0);
    }
}

static char got[300], submitted[300]; static int changes, cancels, nfound = -1, submits;
static void on_changed(void *u, const char *t) { (void)u; snprintf(got, sizeof got, "%s", t); changes++; }
static void on_submit(void *u, const char *t) { (void)u; snprintf(submitted, sizeof submitted, "%s", t); submits++; }
static void on_cancel(void *u) { (void)u; cancels++; }
static void on_found(void *u, int n) { (void)u; nfound = n; }
static void send_key(int fd, int code, int value) { struct input_event e; memset(&e, 0, sizeof e); e.type = EV_KEY; e.code = code; e.value = value; if (write(fd, &e, sizeof e) != sizeof e) printf("write failed\n"); }
static void wait_for(int *flag, int want) { for (int i = 0; i < 300 && *flag != want; i++) usleep(10000); }

int main(void) {
    kbd_line l; memset(&l, 0, sizeof l);
    type(&l, "4 bar chords, Faxing Berlin? 1.5-2!");
    CHECK(!strcmp(l.text, "4 bar chords, Faxing Berlin? 1.5-2!"), "typed '%s'", l.text);
    CHECK(press(&l, KEY_BACKSPACE) == KBD_CHANGED && !strcmp(l.text + strlen(l.text) - 3, "5-2"), "backspace '%s'", l.text);
    CHECK(kbd_key(&l, KEY_A, 2) == KBD_CHANGED, "a held key repeats");
    CHECK(kbd_key(&l, KEY_A, 0) == KBD_NONE && kbd_key(&l, KEY_F5, 1) == KBD_NONE && kbd_key(&l, KEY_LEFTCTRL, 1) == KBD_NONE, "release and unmapped keys do nothing");
    CHECK(press(&l, KEY_ENTER) == KBD_SUBMIT && press(&l, KEY_ESC) == KBD_CANCEL, "enter submits, esc cancels");
    kbd_line e; memset(&e, 0, sizeof e);
    CHECK(press(&e, KEY_ENTER) == KBD_NONE && press(&e, KEY_BACKSPACE) == KBD_NONE, "empty line: enter and backspace do nothing");
    for (int i = 0; i < 300; i++) press(&e, KEY_X);
    CHECK(e.n == KBD_LINE_MAX && strlen(e.text) == KBD_LINE_MAX, "line capped at %d (%d)", KBD_LINE_MAX, e.n);

    /* the reader against a FIFO standing in for /dev/input/event0 */
    char dir[] = "/tmp/daemon_kbd_XXXXXX"; if (!mkdtemp(dir)) return 1;
    char path[200]; snprintf(path, sizeof path, "%s/event0", dir);
    if (mkfifo(path, 0600)) return 1;
    kbd_cb cb = { on_changed, on_submit, on_cancel, on_found, NULL };
    kbd_t *k = kbd_start(dir, 1, cb);
    usleep(200000); CHECK(nfound == -1, "nothing opened while disarmed (%d)", nfound);
    kbd_arm(k, 1);
    int fd = open(path, O_WRONLY | O_NONBLOCK); for (int i = 0; fd < 0 && i < 300; i++) { usleep(10000); fd = open(path, O_WRONLY | O_NONBLOCK); }
    CHECK(fd >= 0, "reader opened the device");
    wait_for(&nfound, 1); CHECK(nfound == 1, "found %d keyboards", nfound);
    send_key(fd, KEY_H, 1); send_key(fd, KEY_H, 0); send_key(fd, KEY_I, 1); send_key(fd, KEY_I, 0);
    for (int i = 0; i < 300 && changes < 2; i++) usleep(10000);
    CHECK(!strcmp(got, "hi"), "live text '%s'", got);
    send_key(fd, KEY_ENTER, 1); send_key(fd, KEY_ENTER, 0);
    wait_for(&submits, 1);
    CHECK(submits == 1 && !strcmp(submitted, "hi"), "submitted '%s' (%d)", submitted, submits);
    close(fd);

    /* armed again: a new line starts empty; Esc cancels; unplugging reports 0 keyboards */
    changes = 0; nfound = -1; kbd_arm(k, 1);
    fd = open(path, O_WRONLY | O_NONBLOCK); for (int i = 0; fd < 0 && i < 300; i++) { usleep(10000); fd = open(path, O_WRONLY | O_NONBLOCK); }
    wait_for(&nfound, 1);
    send_key(fd, KEY_O, 1); send_key(fd, KEY_O, 0); for (int i = 0; i < 300 && changes < 1; i++) usleep(10000);
    CHECK(!strcmp(got, "o"), "second line starts empty: '%s'", got);
    send_key(fd, KEY_ESC, 1); wait_for(&cancels, 1);
    CHECK(cancels == 1 && submits == 1, "esc cancels (%d), no submit", cancels);
    close(fd);
    kbd_arm(k, 1); fd = open(path, O_WRONLY | O_NONBLOCK); for (int i = 0; fd < 0 && i < 300; i++) { usleep(10000); fd = open(path, O_WRONLY | O_NONBLOCK); }
    wait_for(&nfound, 1); close(fd);                     /* the writer goes away: the reader drops the device */
    wait_for(&nfound, 0); CHECK(nfound == 0, "keyboard gone -> %d", nfound);
    kbd_arm(k, 0); kbd_stop(k);
    unlink(path); rmdir(dir);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
