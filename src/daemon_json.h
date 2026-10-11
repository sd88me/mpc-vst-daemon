/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* A small JSON reader and writer: just enough for model replies, Gemini envelopes and the commander protocol. */
#pragma once
#include <stddef.h>

typedef struct jv { char t; double num; char *str; struct jv *kid, *next; char *key; } jv;   /* t: o a s n b 0 (object array string number bool null) */

jv *djson_parse(const char *text, size_t len);                 /* NULL if it is not one complete JSON value; free with djson_free */
void djson_free(jv *v);
jv *djson_get(jv *obj, const char *key);                        /* NULL if absent or obj is not an object */
jv *djson_at(jv *arr, int i);                                   /* NULL if absent */
double djson_num(jv *obj, const char *key, double def);
const char *djson_str(jv *obj, const char *key, const char *def);
int djson_len(jv *arr);
/* Serialize v (compact) into buf; returns the length, or -1 if cap is too small. Strings are escaped. */
int djson_write(const jv *v, char *buf, size_t cap);
