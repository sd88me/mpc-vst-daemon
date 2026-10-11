/* Keyboard entry: a USB or Bluetooth keyboard (both are /dev/input/event* devices) types the prompt. While armed, the keyboards
 * are grabbed so MPC does not also act on the keys; disarmed, no device is open. A keyboard that connects after arming is
 * picked up (rescan every second), which is how a Bluetooth one appears. */
#pragma once
#include <stddef.h>

/* ---- text editing, no I/O ---- */
#define KBD_LINE_MAX 200
typedef struct { char text[KBD_LINE_MAX + 1]; int n; int shift; } kbd_line;
enum { KBD_NONE, KBD_CHANGED, KBD_SUBMIT, KBD_CANCEL };
/* One key event (Linux key code, value 0 release / 1 press / 2 repeat), US layout. Enter submits, Esc cancels, Backspace deletes. */
int kbd_key(kbd_line *l, int code, int value);

/* ---- device reader (a thread) ---- */
typedef struct {
    void (*changed)(void *user, const char *text);   /* the line after an edit */
    void (*submit)(void *user, const char *text);    /* Enter: the reader disarms itself */
    void (*cancel)(void *user);                      /* Esc: ditto */
    void (*found)(void *user, int keyboards);        /* how many keyboards are open now (0 until one connects) */
    void *user;
} kbd_cb;

typedef struct kbd kbd_t;
/* dir: where the event devices are ("/dev/input"). test_dir=1 accepts any file named event* there (a FIFO in tests) without the
 * keyboard capability check. */
kbd_t *kbd_start(const char *dir, int test_dir, kbd_cb cb);
void kbd_arm(kbd_t *k, int on);       /* never blocks; callbacks come from the reader thread */
void kbd_stop(kbd_t *k);              /* joins the thread, closes everything */
