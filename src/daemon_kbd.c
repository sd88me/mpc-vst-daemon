/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#define _GNU_SOURCE
#include "daemon_kbd.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ---- editing ---- */
static char key_char(int code, int shift) {
    static const char lo[] = "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0" "\\zxcvbnm,./";
    static const char hi[] = "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0" "|ZXCVBNM<>?";
    if (code == KEY_SPACE) return ' ';
    if (code < 0 || code >= (int)sizeof lo - 1) return 0;
    return shift ? hi[code] : lo[code];
}

int kbd_key(kbd_line *l, int code, int value) {
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) { l->shift = value != 0; return KBD_NONE; }
    if (value == 0) return KBD_NONE;
    if (code == KEY_ENTER || code == KEY_KPENTER) return l->n ? KBD_SUBMIT : KBD_NONE;
    if (code == KEY_ESC) return KBD_CANCEL;
    if (code == KEY_BACKSPACE) { if (!l->n) return KBD_NONE; l->text[--l->n] = 0; return KBD_CHANGED; }
    char c = key_char(code, l->shift);
    if (!c || l->n >= KBD_LINE_MAX) return KBD_NONE;
    l->text[l->n++] = c; l->text[l->n] = 0;
    return KBD_CHANGED;
}

/* ---- reader ---- */
#define MAXDEV 8
struct kbd {
    char dir[160]; int test_dir; kbd_cb cb;
    pthread_t th; pthread_mutex_t mu; pthread_cond_t cv; int armed, quit;
    int fd[MAXDEV]; char name[MAXDEV][64]; int n; char logpath[300]; char seen[MAXDEV * 4][64]; int nseen;
};

/* One line per device per outcome (not per rescan) in kbd.log, so a keyboard that never types can be diagnosed from the device. */
static void note(kbd_t *k, const char *node, const char *what) {
    if (!k->logpath[0]) return;
    char key[64]; snprintf(key, sizeof key, "%.20s:%.40s", node, what);
    for (int i = 0; i < k->nseen; i++) if (!strcmp(k->seen[i], key)) return;
    if (k->nseen < MAXDEV * 4) snprintf(k->seen[k->nseen++], sizeof k->seen[0], "%s", key);
    FILE *f = fopen(k->logpath, "a"); if (!f) return; fprintf(f, "%s: %s\n", node, what); fclose(f);
}
void kbd_set_log(kbd_t *k, const char *path) { snprintf(k->logpath, sizeof k->logpath, "%s", path); }

#define BITS(n) ((n) / 8 + 1)
static int is_keyboard(int fd) {
    unsigned char ev[BITS(EV_MAX)] = {0}, key[BITS(KEY_MAX)] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 || !(ev[EV_KEY / 8] & (1 << (EV_KEY % 8)))) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0) return 0;
    return (key[KEY_A / 8] & (1 << (KEY_A % 8))) && (key[KEY_ENTER / 8] & (1 << (KEY_ENTER % 8)));   /* letters and Enter: not a mouse or a power button */
}

static void scan(kbd_t *k) {
    DIR *d = opendir(k->dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && k->n < MAXDEV) {
        if (strncmp(e->d_name, "event", 5) || strlen(e->d_name) >= sizeof k->name[0]) continue;
        int dup = 0; for (int i = 0; i < k->n; i++) if (!strcmp(k->name[i], e->d_name)) dup = 1;
        if (dup) continue;
        char path[430]; snprintf(path, sizeof path, "%.160s/%.63s", k->dir, e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        char nm[96] = "?"; if (!k->test_dir) ioctl(fd, EVIOCGNAME(sizeof nm), nm);
        if (!k->test_dir && !is_keyboard(fd)) { char m[160]; snprintf(m, sizeof m, "\"%.60s\" not a keyboard", nm); note(k, e->d_name, m); close(fd); continue; }
        if (!k->test_dir && ioctl(fd, EVIOCGRAB, 1) < 0) {
            int er = errno; char m[160]; snprintf(m, sizeof m, "\"%.60s\" grab failed: %.40s", nm, strerror(er)); note(k, e->d_name, m);
            if (er == EBUSY) { close(fd); continue; }       /* someone else has it exclusively: no events would reach us */
        } else if (!k->test_dir) { char m[160]; snprintf(m, sizeof m, "\"%.60s\" grabbed", nm); note(k, e->d_name, m); }
        snprintf(k->name[k->n], sizeof k->name[0], "%.63s", e->d_name); k->fd[k->n++] = fd;
        if (k->cb.found) k->cb.found(k->cb.user, k->n);
    }
    closedir(d);
}

static void close_all(kbd_t *k) {
    for (int i = 0; i < k->n; i++) { if (!k->test_dir) ioctl(k->fd[i], EVIOCGRAB, 0); close(k->fd[i]); }
    k->n = 0;
}

static int armed(kbd_t *k) { pthread_mutex_lock(&k->mu); int a = k->armed; pthread_mutex_unlock(&k->mu); return a; }

static void *reader(void *arg) {
    kbd_t *k = arg;
    for (;;) {
        pthread_mutex_lock(&k->mu);
        while (!k->armed && !k->quit) pthread_cond_wait(&k->cv, &k->mu);
        int quit = k->quit; pthread_mutex_unlock(&k->mu);
        if (quit) break;
        kbd_line line; memset(&line, 0, sizeof line);
        time_t last_scan = 0;
        while (armed(k)) {
            time_t now = time(NULL);
            if (now != last_scan) { scan(k); last_scan = now; }
            struct pollfd p[MAXDEV]; for (int i = 0; i < k->n; i++) { p[i].fd = k->fd[i]; p[i].events = POLLIN; p[i].revents = 0; }
            int r = poll(p, (nfds_t)k->n, 100);
            if (r <= 0) { if (!k->n) usleep(100000); continue; }
            for (int i = 0; i < k->n; i++) {
                if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {      /* a keyboard went away (Bluetooth out of range) */
                    if (!k->test_dir) ioctl(k->fd[i], EVIOCGRAB, 0);
                    close(k->fd[i]); k->fd[i] = k->fd[k->n - 1]; memcpy(k->name[i], k->name[k->n - 1], sizeof k->name[0]); k->n--;
                    if (k->cb.found) k->cb.found(k->cb.user, k->n);
                    break;
                }
                if (!(p[i].revents & POLLIN)) continue;
                struct input_event ev[16]; ssize_t n = read(k->fd[i], ev, sizeof ev);
                for (ssize_t j = 0; j < n / (ssize_t)sizeof ev[0]; j++) {
                    if (ev[j].type != EV_KEY) continue;
                    int res = kbd_key(&line, ev[j].code, ev[j].value);
                    if (res == KBD_CHANGED && k->cb.changed) k->cb.changed(k->cb.user, line.text);
                    if (res == KBD_SUBMIT || res == KBD_CANCEL) {
                        pthread_mutex_lock(&k->mu); k->armed = 0; pthread_mutex_unlock(&k->mu);   /* release the keyboards before telling the engine */
                        close_all(k);
                        if (res == KBD_SUBMIT && k->cb.submit) k->cb.submit(k->cb.user, line.text);
                        if (res == KBD_CANCEL && k->cb.cancel) k->cb.cancel(k->cb.user);
                        goto disarmed;
                    }
                }
            }
        }
    disarmed:
        close_all(k);
    }
    return NULL;
}

kbd_t *kbd_start(const char *dir, int test_dir, kbd_cb cb) {
    kbd_t *k = calloc(1, sizeof *k);
    snprintf(k->dir, sizeof k->dir, "%s", dir); k->test_dir = test_dir; k->cb = cb;
    pthread_mutex_init(&k->mu, NULL); pthread_cond_init(&k->cv, NULL);
    pthread_create(&k->th, NULL, reader, k);
    return k;
}

void kbd_arm(kbd_t *k, int on) {
    pthread_mutex_lock(&k->mu); k->armed = on; pthread_cond_signal(&k->cv); pthread_mutex_unlock(&k->mu);
}

void kbd_stop(kbd_t *k) {
    if (!k) return;
    pthread_mutex_lock(&k->mu); k->quit = 1; k->armed = 0; pthread_cond_signal(&k->cv); pthread_mutex_unlock(&k->mu);
    pthread_join(k->th, NULL);
    pthread_mutex_destroy(&k->mu); pthread_cond_destroy(&k->cv);
    free(k);
}
