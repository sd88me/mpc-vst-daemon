#pragma once
#define FAKE_EV 4096
typedef struct { int ch, on, pitch, vel; } fev;
typedef struct { fev ev[FAKE_EV]; int nev, closed, posts, code; char url[300], key[128], body[1500000], resp[100000]; char queue[8][8000]; int nq, qi; } fake_t;   /* queue: if nq > 0, replies are taken from it in order */
extern fake_t fake;
