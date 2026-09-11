#ifndef SNESRECOMP_LUA_BRIDGE_H
#define SNESRECOMP_LUA_BRIDGE_H
#include <stdint.h>
#include <stddef.h>
#if SNESRECOMP_ENABLE_LUA
typedef int (*LuaBridgeGameCommand)(const char *name, const char *args, char *result, size_t capacity);
void lua_bridge_set_game_command_handler(LuaBridgeGameCommand handler);
/* All calls belong to the host/game thread, outside RtlRunFrame. Port 0 disables.
 * Inputs use the runner layout: B,Y,Select,Start,Up,Down,Left,Right,A,X,L,R.
 * frame_end must be called exactly once for every admitted frame. */
int lua_bridge_init(uint8_t *wram, uint32_t size, const uint8_t *rom, uint32_t rom_size, int port);
void lua_bridge_poll(void);
int lua_bridge_paused(void);
uint32_t lua_bridge_frame_start(uint32_t inputs);
void lua_bridge_frame_end(void);
void lua_bridge_shutdown(void);
#else
static inline int lua_bridge_init(uint8_t *r, uint32_t s, const uint8_t *rom, uint32_t rs, int p) { (void)r; (void)s; (void)rom; (void)rs; (void)p; return 0; }
static inline void lua_bridge_poll(void) {}
static inline int lua_bridge_paused(void) { return 0; }
static inline uint32_t lua_bridge_frame_start(uint32_t i) { return i; }
static inline void lua_bridge_frame_end(void) {}
static inline void lua_bridge_shutdown(void) {}
#endif
#endif
