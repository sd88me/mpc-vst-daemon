/* Daemon core: turns a model reply into a checked note list, plays it against the host song position,
 * writes a Standard MIDI File and builds the request body. No MPC, ALSA or network code in here, so the
 * host test runs it on x86. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define DAEMON_MAX_NOTES 2048
#define DAEMON_MAX_PENDING 128

typedef struct { uint8_t pitch, vel; float start, dur; } dnote_t;   /* start, dur in beats */

typedef struct {
    dnote_t notes[DAEMON_MAX_NOTES];
    int n;
    float loop_beats;                 /* whole beats, >= 1 */
    char transcript[256], feedback[512], clip_name[24];
} dpart_t;

/* Parse and check a reply: {"transcript","notes":[{pitch,start_time,duration,velocity}],"feedback":{"result","clip_name"}}.
 * Notes outside 0..127, with a negative start or no length are dropped; velocity is clamped to 1..127; a note is cut at
 * max_beats. Returns the kept note count, or -1 if the text is not a usable reply (no JSON object, or no notes array). */
int daemon_parse_reply(const char *text, float max_beats, dpart_t *out);

/* Player: call daemon_feed once per audio block with the host's ppqPos at the block start and end. */
typedef void (*daemon_emit_fn)(void *user, int on, int pitch, int vel);
typedef struct {
    const dpart_t *part;
    double unwrapped;                 /* song position since the last resync, in beats */
    int primed;
    struct { int pitch; double off; } pend[DAEMON_MAX_PENDING];
    int npend;
} dplayer_t;

void daemon_player_init(dplayer_t *p, const dpart_t *part);
void daemon_player_stop(dplayer_t *p, daemon_emit_fn emit, void *user);   /* note-off for everything sounding */
void daemon_feed(dplayer_t *p, double ppq0, double ppq1, daemon_emit_fn emit, void *user);

/* Standard MIDI File (format 0, 480 PPQ, channel 0-15) into buf; returns bytes written or -1 if cap is too small. */
int daemon_write_smf(const dpart_t *part, int channel, double bpm, uint8_t *buf, size_t cap);

/* Gemini generateContent body. wav16 (16 kHz mono 16-bit WAV file bytes) may be NULL for a typed prompt.
 * Returns bytes written (NUL-terminated) or -1. */
typedef struct { const char *type, *scale, *drums; double bpm; int bars; } dreq_t;
int daemon_build_request(const dreq_t *rq, const char *typed, const uint8_t *wav16, size_t wav_len, char *buf, size_t cap);

/* Gemini envelope: candidates[0].content.parts[*].text joined into out. Returns the length, or -1 with the API's own
 * error.message (or "no text in reply") in out. */
int daemon_extract_text(const char *resp, char *out, size_t cap);

/* Mic capture: 44.1 kHz interleaved int16 stereo in, 16 kHz mono out (linear interpolation of the downmix). */
#define DAEMON_CAP_SECONDS 20
#define DAEMON_CAP_MAX (16000 * DAEMON_CAP_SECONDS)
typedef struct { int16_t pcm[DAEMON_CAP_MAX]; int n; double phase; int16_t prev; } dcap_t;
void daemon_cap_reset(dcap_t *c);
void daemon_cap_push(dcap_t *c, const int16_t *in_lr, int frames);   /* stops growing at DAEMON_CAP_MAX */
int daemon_cap_peak(const dcap_t *c);                                /* 0..32767 over the whole capture */
/* A 16 kHz mono 16-bit WAV file of the capture; returns bytes (44 + 2n), or -1 if cap is too small. */
int daemon_cap_wav(const dcap_t *c, uint8_t *out, size_t cap);

/* Vision step for tap macros: a Gemini request asking where `target` is on a screenshot (PNG, base64). The model answers
 * {found, point:[y, x] in 0..1000, reason}. */
int daemon_build_vision_request(const char *target, const char *hint, const char *png_b64, int w, int h, char *buf, size_t cap);
/* Gemini's answer text -> screen pixels. 0 found; -1 not found or unusable (why says which). */
int daemon_parse_point(const char *text, int w, int h, int *x, int *y, char *why, size_t whycap);
