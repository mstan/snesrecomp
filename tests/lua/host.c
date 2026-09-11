/* ROM-free host for exercising the real bridge over TCP. */
#include "lua_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
static uint8_t ram[0x20000], rom[64];
#if SNESRECOMP_ENABLE_LUA
static unsigned resets;
static int command(const char *name, const char *args, char *out, size_t size) {
    if (!strcmp(name, "__reset")) { ++resets; return 1; }
    if (!strcmp(name, "test.echo")) { snprintf(out, size, "%s", args); return 1; }
    if (!strcmp(name, "test.resets")) { snprintf(out, size, "%u", resets); return 1; }
    snprintf(out, size, "unknown test command"); return 0;
}
#endif
int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 0;
    if (lua_bridge_init(ram, sizeof(ram), rom, sizeof(rom), port)) return 1;
    if (!port) { lua_bridge_shutdown(); return 0; }
#if SNESRECOMP_ENABLE_LUA
    lua_bridge_set_game_command_handler(command);
#else
    return 0;
#endif
    for (;;) {
        lua_bridge_poll();
        if (!lua_bridge_paused()) {
            uint32_t inputs = lua_bridge_frame_start(0);
            for (unsigned i = 0; i < 4; ++i) ram[0x100+i] = (uint8_t)(inputs >> (8*i));
            lua_bridge_frame_end();
        }
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL);
#endif
    }
}
