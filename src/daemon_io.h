/* What the engine needs from the outside world: a MIDI port and an HTTPS POST. io_device.c is the real one (libasound and
 * libcurl loaded at run time); test/io_fake.c records and answers from a script, so the engine runs offline. */
#pragma once
#include <stddef.h>

typedef struct {
    void *(*midi_open)(const char *client, const char *port);          /* NULL if there is no sequencer */
    void (*midi_send)(void *h, int ch, int on, int pitch, int vel);    /* ch 0-15 */
    void (*midi_close)(void *h);
    /* POST JSON; key goes in x-goog-api-key. Returns the HTTP status (0 = no reply at all) and fills resp (NUL-terminated). */
    int (*http_post)(const char *url, const char *key, const char *body, size_t len, char *resp, size_t cap, int timeout_s);
} daemon_io_t;

const daemon_io_t *daemon_io(void);
