#pragma once
#include "types.h"

// Framework-generic SpcPlayer interface. Per-game implementations allocate
// a larger struct with this one as its first member, install the callbacks,
// and return it via their own *_Create function. The framework drives the
// player via the function pointers only — it never names a per-game type.

typedef struct Dsp Dsp;
typedef struct SpcPlayer SpcPlayer;

typedef void SpcPlayer_Initialize_Func(SpcPlayer *p);
typedef void SpcPlayer_Upload_Func(SpcPlayer *p, const uint8_t *data);

struct SpcPlayer {
  Dsp *dsp;
  uint8 input_ports[4];
  uint8 port_to_snes[4];

  SpcPlayer_Initialize_Func *initialize;
  SpcPlayer_Upload_Func *upload;
};

extern SpcPlayer *g_spc_player;
