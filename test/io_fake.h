#pragma once
#define FAKE_EV 4096
typedef struct { int ch, on, pitch, vel; } fev;
typedef struct { fev ev[FAKE_EV]; int nev, closed, posts, code; char url[300], key[128], body[1500000], resp[100000]; } fake_t;
extern fake_t fake;
