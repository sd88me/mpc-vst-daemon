/* Tap macros: recorded sequences of touches and button presses that drive MPC's own screens through the remote addin, for the things
 * the commander cannot do (select a track, load a plugin onto it). Coordinates are device- and layout-specific, so they live in a data
 * file recorded on the device (macros.json), never in the code. See docs/MACROS.md for the format.
 *
 * Safeguards: the file names the screen size it was recorded for and refuses to run on another; a touch marked require_change stops the
 * macro if the screen did not change (a stale coordinate); nesting and total steps are limited; the vision step only taps where the
 * model found the named thing, and only on a screenshot taken a moment before. */
#pragma once
#include "daemon_mcp.h"
#include <stddef.h>

typedef struct macros macros_t;
macros_t *macros_load(const char *path, char *err, size_t errcap);    /* NULL + err if absent or invalid */
macros_t *macros_parse(const char *json, char *err, size_t errcap);
void macros_free(macros_t *m);
int macros_has(const macros_t *m, const char *name);
void macros_screen(const macros_t *m, int *w, int *h);

/* Find `target` on the screenshot (PNG, base64) and give its centre in screen pixels. 0 found, -1 not found (why goes in why). */
typedef int (*macro_vision_fn)(void *user, const char *target, const char *hint, const char *png_b64, int screen_w, int screen_h,
                               int *x, int *y, char *why, size_t whycap);

/* Run macro `name` with args (a JSON object of its parameters). result gets a one-line summary or the reason it stopped.
 * Returns 0 if every step ran, -1 otherwise. */
int macro_run(const macros_t *m, mcp_t *mcp, const char *name, const char *args_json, macro_vision_fn vision, void *user,
              char *result, size_t cap);
