/* Developer scripting spike. One VM and one nonblocking loopback client;
 * everything, including socket servicing, runs on the host/game thread. */
#include "lua_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Socket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
typedef int Socket;
#define BAD_SOCKET (-1)
#define close_socket close
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

#define LINE_CAP 131072
#define RESPONSE_CAP 131072
#define CALLBACK_CAP 64
#define LUA_MEMORY_CAP (16u * 1024u * 1024u)
static Socket listener = BAD_SOCKET, peer = BAD_SOCKET;
static lua_State *vm, *script;
static int script_ref = LUA_NOREF, paused, steps;
static uint8_t *ram;
static const uint8_t *rom;
static uint32_t rom_size;
static uint32_t ram_size, input_mask, input_values, current_inputs;
static unsigned long long frame;
static size_t lua_bytes, rx_len, tx_len, tx_sent;
static char rx[LINE_CAP], tx[RESPONSE_CAP], last_error[512], output[4096];
static unsigned next_id;
static struct { int ref, phase; unsigned id; char name[96]; } callbacks[CALLBACK_CAP];
static const char *buttons[] = {"B","Y","Select","Start","Up","Down","Left","Right","A","X","L","R"};

static void *limited_alloc(void *ud, void *ptr, size_t old, size_t size) {
    (void)ud;
    if (!ptr) old = 0;
    if (!size) { free(ptr); lua_bytes -= old; return NULL; }
    if (size > LUA_MEMORY_CAP || lua_bytes - old > LUA_MEMORY_CAP - size) return NULL;
    void *p = realloc(ptr, size);
    if (p) lua_bytes = lua_bytes - old + size;
    return p;
}
static void budget_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    luaL_error(L, "Lua instruction budget exceeded (100000 instructions per call)");
}
static void remember_error(lua_State *L) {
    const char *s = lua_tostring(L, -1);
    snprintf(last_error, sizeof(last_error), "%s", s ? s : "Lua error");
    fprintf(stderr, "[lua] %s\n", last_error);
}
static int protected_call(lua_State *L, int args, int results) {
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, 100000);
    int rc = lua_pcall(L, args, results, 0);
    lua_sethook(L, NULL, 0, 0);
    if (rc != LUA_OK) remember_error(L);
    return rc;
}

/* Domain handling deliberately excludes MMIO: reading RAM never triggers
 * bus side effects. System Bus only implements WRAM and its low-bank mirrors. */
static int selected_bus;
static const char *domain_names[] = {"WRAM", "System Bus", "CARTROM"};
static int domain(lua_State *L, int arg, int mainmemory) {
    if (mainmemory) return 0;
    const char *s = luaL_optstring(L, arg, domain_names[selected_bus]);
    if (!strcmp(s, "WRAM")) return 0;
    if (!strcmp(s, "System Bus")) return 1;
    if (!strcmp(s, "CARTROM")) return 2;
    return luaL_error(L, "unsupported memory domain: %s", s);
}
static uint32_t address(lua_State *L, lua_Integer a, unsigned size, int bus) {
    if (bus == 2) {
        if (!rom || a < 0 || a > rom_size || size > rom_size - (uint32_t)a)
            luaL_error(L, "memory range outside CARTROM");
        return (uint32_t)a;
    }
    if (bus == 1) {
        if (a >= 0x7e0000 && a <= 0x7fffff) a -= 0x7e0000;
        else if (a >= 0 && a <= 0xffffff && (a & 0x7f0000) < 0x400000 && (a & 0xffff) < 0x2000) {
            if (size > 0x2000 - (a & 0xffff)) luaL_error(L, "range crosses WRAM mirror boundary");
            a &= 0x1fff;
        } else luaL_error(L, "System Bus supports WRAM only in this spike");
    }
    if (a < 0 || a > ram_size || size > ram_size - (uint32_t)a)
        luaL_error(L, "memory range outside WRAM");
    return (uint32_t)a;
}
static int mem_access(lua_State *L) {
    int flags = (int)lua_tointeger(L, lua_upvalueindex(1));
    unsigned n = flags & 7;
    int writing = flags & 8, big = flags & 16, sign = flags & 32;
    int bus = domain(L, writing ? 3 : 2, flags & 64);
    if (writing && bus == 2) return luaL_error(L, "CARTROM is read-only");
    uint32_t a = address(L, luaL_checkinteger(L, 1), n, bus), value = 0;
    if (writing) value = (uint32_t)luaL_checkinteger(L, 2);
    for (unsigned i = 0; i < n; ++i) {
        unsigned shift = 8 * (big ? n - 1 - i : i);
        if (writing) ram[a + i] = (uint8_t)(value >> shift);
        else value |= (uint32_t)(bus == 2 ? rom[a + i] : ram[a + i]) << shift;
    }
    if (writing) return 0;
    lua_Integer result = value;
    if (sign && (value & (1u << (n * 8 - 1)))) result -= (lua_Integer)1 << (n * 8);
    lua_pushinteger(L, result);
    return 1;
}
static int mem_range(lua_State *L) {
    int flags = (int)lua_tointeger(L, lua_upvalueindex(1));
    lua_Integer count = luaL_checkinteger(L, 2);
    luaL_argcheck(L, count >= 0 && count <= 4096, 2, "range must be 0..4096 bytes");
    int bus = domain(L, 3, flags & 64);
    uint32_t a = address(L, luaL_checkinteger(L, 1), (unsigned)count, bus);
    lua_createtable(L, (int)count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushinteger(L, bus == 2 ? rom[a + i] : ram[a + i]); lua_rawseti(L, -2, i + (flags & 1));
    }
    return 1;
}
static int mem_domains(lua_State *L) {
    lua_newtable(L);
    lua_pushliteral(L, "WRAM"); lua_rawseti(L, -2, 0);
    lua_pushliteral(L, "System Bus"); lua_rawseti(L, -2, 1);
    lua_pushliteral(L, "CARTROM"); lua_rawseti(L, -2, 2);
    return 1;
}
static int mem_use(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    int valid = 0;
    for (int i = 0; i < 3; ++i) if (!strcmp(s, domain_names[i])) { valid = 1; selected_bus = i; }
    lua_pushboolean(L, valid); return 1;
}
static int mem_name(lua_State *L) { lua_pushstring(L, domain_names[selected_bus]); return 1; }
static int mem_size(lua_State *L) {
    int d = domain(L, 1, 0); lua_pushinteger(L, d == 2 ? rom_size : d == 1 ? 0x1000000 : ram_size); return 1;
}
static int mem_main_size(lua_State *L) { lua_pushinteger(L, ram_size); return 1; }
static int emu_frame(lua_State *L) { lua_pushinteger(L, (lua_Integer)frame); return 1; }
static int emu_system(lua_State *L) { lua_pushliteral(L, "SNES"); return 1; }
static int emu_advance(lua_State *L) {
    if (L != script) return luaL_error(L, "emu.frameadvance requires a TCP run script");
    if (paused) steps = 1;
    return lua_yield(L, 0);
}
static int client_pause(lua_State *L) { (void)L; paused = 1; steps = 0; return 0; }
static int client_unpause(lua_State *L) { (void)L; paused = 0; steps = 0; return 0; }
static int client_paused(lua_State *L) { lua_pushboolean(L, paused); return 1; }
static int console_log(lua_State *L) {
    for (int i = 1; i <= lua_gettop(L); ++i) {
        size_t n; const char *s = luaL_tolstring(L, i, &n);
        size_t used = strlen(output), space = sizeof(output) - used - 1;
        if (n > space) n = space;
        memcpy(output + used, s, n); output[used + n] = 0;
        lua_pop(L, 1);
    }
    size_t used = strlen(output);
    if (used + 1 < sizeof(output)) { output[used] = '\n'; output[used + 1] = 0; }
    return 0;
}
static int joy_set(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int player = (int)luaL_optinteger(L, 2, 0);
    luaL_argcheck(L, player >= 0 && player <= 2, 2, "controller must be 1 or 2");
    for (int p = 1; p <= 2; ++p) {
        if (player && player != p) continue;
        for (int i = 0; i < 12; ++i) {
            char key[32];
            if (player) snprintf(key, sizeof(key), "%s", buttons[i]);
            else snprintf(key, sizeof(key), "P%d %s", p, buttons[i]);
            lua_getfield(L, 1, key);
            if (!lua_isnil(L, -1)) {
                luaL_argcheck(L, lua_isboolean(L, -1), 1, "button values must be booleans");
                uint32_t bit = 1u << ((p - 1) * 12 + i);
                input_mask |= bit;
                if (lua_toboolean(L, -1)) input_values |= bit;
                else input_values &= ~bit;
            }
            lua_pop(L, 1);
        }
    }
    return 0;
}
static int joy_get(lua_State *L) {
    int player = (int)luaL_optinteger(L, 1, 0);
    luaL_argcheck(L, player >= 0 && player <= 2, 1, "controller must be 1 or 2");
    uint32_t value = (current_inputs & ~input_mask) | input_values;
    lua_newtable(L);
    for (int p = 1; p <= 2; ++p) {
        if (player && player != p) continue;
        for (int i = 0; i < 12; ++i) {
            char key[32];
            if (player) snprintf(key, sizeof(key), "%s", buttons[i]);
            else snprintf(key, sizeof(key), "P%d %s", p, buttons[i]);
            lua_pushboolean(L, value & (1u << ((p - 1) * 12 + i))); lua_setfield(L, -2, key);
        }
    }
    return 1;
}
static void remove_callback(int i) {
    if (callbacks[i].id) luaL_unref(vm, LUA_REGISTRYINDEX, callbacks[i].ref);
    memset(&callbacks[i], 0, sizeof(callbacks[i]));
}
static int event_add(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const char *name = luaL_optstring(L, 2, "");
    luaL_argcheck(L, strlen(name) < sizeof(callbacks[0].name), 2, "name too long");
    for (int i = 0; i < CALLBACK_CAP; ++i) if (!callbacks[i].id) {
        callbacks[i].id = ++next_id;
        callbacks[i].phase = (int)lua_tointeger(L, lua_upvalueindex(1));
        snprintf(callbacks[i].name, sizeof(callbacks[i].name), "%s", name);
        lua_pushvalue(L, 1); callbacks[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushfstring(L, "lua-%d", callbacks[i].id); return 1;
    }
    return luaL_error(L, "callback limit reached (64)");
}
static int event_remove(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    int by_name = (int)lua_tointeger(L, lua_upvalueindex(1)), removed = 0;
    for (int i = 0; i < CALLBACK_CAP; ++i) if (callbacks[i].id) {
        char id[40]; snprintf(id, sizeof(id), "lua-%u", callbacks[i].id);
        if (!strcmp(key, by_name ? callbacks[i].name : id)) { remove_callback(i); removed = 1; }
    }
    lua_pushboolean(L, removed); return 1;
}
static void dispatch_callbacks(int phase) {
    unsigned snapshot[CALLBACK_CAP];
    for (int i = 0; i < CALLBACK_CAP; ++i) snapshot[i] = callbacks[i].id;
    for (int i = 0; i < CALLBACK_CAP; ++i) {
        if (!snapshot[i] || callbacks[i].id != snapshot[i] || callbacks[i].phase != phase) continue;
        lua_rawgeti(vm, LUA_REGISTRYINDEX, callbacks[i].ref);
        if (protected_call(vm, 0, 0) != LUA_OK) {
            lua_pop(vm, 1);
            if (callbacks[i].id == snapshot[i]) remove_callback(i);
        }
    }
}
static void function(lua_State *L, const char *name, lua_CFunction fn, int flags) {
    lua_pushinteger(L, flags); lua_pushcclosure(L, fn, 1); lua_setfield(L, -2, name);
}
static void memory_library(int main) {
    lua_newtable(vm);
    for (int n = 1; n <= 4; ++n) for (int sign = 0; sign <= 1; ++sign)
        for (int big = 0; big <= (n > 1); ++big) for (int wr = 0; wr <= 1; ++wr) {
            char name[48];
            if (n == 1) snprintf(name, sizeof(name), "%s_%c8", wr ? "write" : "read", sign ? 's' : 'u');
            else snprintf(name, sizeof(name), "%s_%c%d_%s", wr ? "write" : "read", sign ? 's' : 'u', n*8, big ? "be" : "le");
            function(vm, name, mem_access, n | wr*8 | big*16 | sign*32 | main*64);
        }
    function(vm, "readbyte", mem_access, 1 | main*64);
    function(vm, "writebyte", mem_access, 9 | main*64);
    function(vm, "readbyterange", mem_range, main*64);
    function(vm, "read_bytes_as_array", mem_range, 1 | main*64);
    if (!main) {
        function(vm, "getmemorydomainlist", mem_domains, 0);
        function(vm, "usememorydomain", mem_use, 0);
        function(vm, "getcurrentmemorydomain", mem_name, 0);
        function(vm, "getmemorydomainsize", mem_size, 0);
    } else function(vm, "getsize", mem_main_size, 0);
    lua_setglobal(vm, main ? "mainmemory" : "memory");
}
static int create_vm(void) {
    vm = lua_newstate(limited_alloc, NULL);
    if (!vm) return -1;
    const luaL_Reg libs[] = {{LUA_GNAME,luaopen_base},{LUA_TABLIBNAME,luaopen_table},
        {LUA_STRLIBNAME,luaopen_string},{LUA_MATHLIBNAME,luaopen_math},{LUA_UTF8LIBNAME,luaopen_utf8},{NULL,NULL}};
    for (const luaL_Reg *lib = libs; lib->name; ++lib) {
        luaL_requiref(vm, lib->name, lib->func, 1); lua_pop(vm, 1);
    }
    /* No blocking file/process/module APIs. pcall/xpcall are omitted so scripts
     * cannot catch and continually swallow the instruction-budget exception. */
    const char *removed[] = {"dofile","loadfile","pcall","xpcall",NULL};
    for (int i = 0; removed[i]; ++i) { lua_pushnil(vm); lua_setglobal(vm, removed[i]); }
    memory_library(0); memory_library(1);
    lua_newtable(vm);
    function(vm, "framecount", emu_frame, 0); function(vm, "getsystemid", emu_system, 0);
    function(vm, "frameadvance", emu_advance, 0); lua_setglobal(vm, "emu");
    lua_newtable(vm);
    function(vm, "pause", client_pause, 0); function(vm, "unpause", client_unpause, 0);
    function(vm, "ispaused", client_paused, 0); lua_setglobal(vm, "client");
    lua_newtable(vm); function(vm, "log", console_log, 0); lua_setglobal(vm, "console");
    lua_pushcfunction(vm, console_log); lua_setglobal(vm, "print");
    lua_newtable(vm); function(vm, "set", joy_set, 0); function(vm, "get", joy_get, 0); lua_setglobal(vm, "joypad");
    lua_newtable(vm);
    function(vm, "onframestart", event_add, 0); function(vm, "onframeend", event_add, 1);
    function(vm, "unregisterbyid", event_remove, 0); function(vm, "unregisterbyname", event_remove, 1);
    lua_setglobal(vm, "event");
    return 0;
}
static void stop_script(void) {
    if (script_ref != LUA_NOREF) luaL_unref(vm, LUA_REGISTRYINDEX, script_ref);
    script_ref = LUA_NOREF; script = NULL;
}
static int resume_script(void) {
    int results;
    lua_sethook(script, budget_hook, LUA_MASKCOUNT, 100000);
    int rc = lua_resume(script, vm, 0, &results);
    lua_sethook(script, NULL, 0, 0);
    if (rc != LUA_OK && rc != LUA_YIELD) remember_error(script);
    lua_settop(script, 0);
    if (rc != LUA_YIELD) stop_script();
    return rc == LUA_OK || rc == LUA_YIELD;
}

static void append(const char *s) {
    size_t n = strlen(s);
    if (n > sizeof(tx) - tx_len - 1) n = sizeof(tx) - tx_len - 1;
    memcpy(tx + tx_len, s, n); tx_len += n; tx[tx_len] = 0;
}
static void json_string(const char *s, size_t n) {
    append("\"");
    for (size_t i = 0; i < n && tx_len + 16 < sizeof(tx); ++i) {
        unsigned char c = (unsigned char)s[i]; char escaped[8];
        /* Escape all high bytes too: output is valid JSON even for binary Lua strings. */
        if (c < 32 || c >= 127) { snprintf(escaped, sizeof(escaped), "\\u%04x", c); append(escaped); }
        else if (c == '"' || c == '\\') { escaped[0] = '\\'; escaped[1] = c; escaped[2] = 0; append(escaped); }
        else { escaped[0] = c; escaped[1] = 0; append(escaped); }
    }
    append("\"");
}
static void response(int ok, int values) {
    char number[160]; tx_len = tx_sent = 0;
    snprintf(number, sizeof(number), "{\"ok\":%s,\"frame\":%llu,\"paused\":%s,\"running\":%s,\"values\":[", ok ? "true":"false", frame, paused ? "true":"false", script ? "true":"false");
    append(number);
    for (int i = 1; i <= values && i <= 16; ++i) {
        if (i > 1) append(",");
        int kind = lua_type(vm, i);
        if (kind == LUA_TNIL) append("null");
        else if (kind == LUA_TBOOLEAN) append(lua_toboolean(vm, i) ? "true" : "false");
        else {
            size_t n = 0; const char *s = lua_tolstring(vm, i, &n);
            if (!s) { s = lua_typename(vm, kind); n = strlen(s); }
            /* Return numbers as strings to preserve exact 64-bit Lua integers. */
            if (n > 1024) n = 1024;
            json_string(s, n);
        }
    }
    append("],\"output\":"); json_string(output, strlen(output));
    append(",\"error\":"); json_string(last_error, strlen(last_error)); append("}\n");
    lua_settop(vm, 0); output[0] = 0;
}
static int unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void process(char *line) {
    int ok = 1, values = 0;
    lua_settop(vm, 0);
    /* status preserves asynchronous callback/script errors for inspection. */
    if (strcmp(line, "status")) last_error[0] = 0;
    if (!strncmp(line, "eval ", 5) || !strncmp(line, "run ", 4)) {
        int run = line[0] == 'r'; char *source = line + (run ? 4 : 5);
        size_t hex_len = strlen(source), n = hex_len / 2;
        if (hex_len % 2) { ok = 0; snprintf(last_error, sizeof(last_error), "odd hex payload length"); }
        for (size_t i = 0; ok && i < n; ++i) {
            int a = unhex(source[2*i]), b = unhex(source[2*i+1]);
            if (a < 0 || b < 0) { ok = 0; snprintf(last_error, sizeof(last_error), "invalid hex payload"); }
            else source[i] = (char)((a << 4) | b);
        }
        if (ok) {
            if (luaL_loadbufferx(vm, source, n, "tcp", "t") != LUA_OK) { remember_error(vm); ok = 0; }
            else if (run) {
                stop_script();
                script = lua_newthread(vm); script_ref = luaL_ref(vm, LUA_REGISTRYINDEX);
                lua_xmove(vm, script, 1); ok = resume_script();
            } else {
                ok = protected_call(vm, 0, LUA_MULTRET) == LUA_OK;
                if (ok) values = lua_gettop(vm);
            }
        }
    } else if (!strcmp(line, "pause")) { paused = 1; steps = 0; }
    else if (!strcmp(line, "resume")) { paused = 0; steps = 0; }
    else if (!strncmp(line, "step ", 5)) {
        char *end; long n = strtol(line + 5, &end, 10);
        if (*end || end == line + 5 || n < 1 || n > 100000) { ok = 0; snprintf(last_error, sizeof(last_error), "step requires 1..100000 frames"); }
        else { paused = 1; steps = (int)n; }
    } else if (!strcmp(line, "stop")) { stop_script(); input_mask = input_values = 0; }
    else if (!strcmp(line, "reset")) {
        stop_script();
        for (int i = 0; i < CALLBACK_CAP; ++i) remove_callback(i);
        lua_close(vm); vm = NULL; input_mask = input_values = 0; selected_bus = 0; steps = 0;
        if (create_vm()) { lua_bridge_shutdown(); return; }
    } else if (strcmp(line, "status") && strcmp(line, "ping")) {
        ok = 0; snprintf(last_error, sizeof(last_error), "commands: eval HEX, run HEX, status, pause, resume, step N, stop, reset");
    }
    response(ok, values);
}
static int nonblocking(Socket s) {
#ifdef _WIN32
    u_long yes = 1; return ioctlsocket(s, FIONBIO, &yes);
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}
static int would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
static void disconnect(void) {
    if (peer != BAD_SOCKET) close_socket(peer);
    peer = BAD_SOCKET; rx_len = tx_len = tx_sent = 0;
}
int lua_bridge_init(uint8_t *wram, uint32_t size, const uint8_t *cart, uint32_t cart_size, int port) {
    if (!port) return 0;
    if (port < 1 || port > 65535 || !wram || !size || vm) return -1;
#ifdef _WIN32
    WSADATA data; if (WSAStartup(MAKEWORD(2,2), &data)) return -1;
#endif
    ram = wram; ram_size = size; rom = cart; rom_size = cart_size;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons((unsigned short)port);
    if (listener == BAD_SOCKET || nonblocking(listener) || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 1) || create_vm()) {
        lua_bridge_shutdown(); return -1;
    }
    fprintf(stderr, "[lua] BizHawk-style Lua TCP listening on 127.0.0.1:%d\n", port);
    const char *start_paused = getenv("SNESRECOMP_LUA_PAUSED");
    paused = start_paused && !strcmp(start_paused, "1");
    return 0;
}
void lua_bridge_poll(void) {
    if (!vm || listener == BAD_SOCKET) return;
    Socket incoming = accept(listener, NULL, NULL);
    if (incoming != BAD_SOCKET) {
        if (peer != BAD_SOCKET || nonblocking(incoming)) close_socket(incoming);
        else { peer = incoming; rx_len = tx_len = tx_sent = 0; }
    }
    if (peer == BAD_SOCKET) return;
    /* Drain partial replies before executing another command. A slow client
     * cannot block gameplay or create an unbounded queue. */
    if (tx_sent < tx_len) {
        int n = send(peer, tx + tx_sent, (int)(tx_len - tx_sent),
#ifdef _WIN32
            0
#else
            MSG_NOSIGNAL
#endif
        );
        if (n > 0) tx_sent += n;
        else if (n == 0 || !would_block()) disconnect();
        return;
    }
    int n = recv(peer, rx + rx_len, (int)(sizeof(rx) - rx_len - 1), 0);
    if (n > 0) rx_len += n;
    else if (n == 0 || !would_block()) { disconnect(); return; }
    rx[rx_len] = 0;
    char *nl = memchr(rx, '\n', rx_len);
    if (nl) {
        size_t consumed = (size_t)(nl - rx) + 1;
        *nl = 0; if (nl > rx && nl[-1] == '\r') nl[-1] = 0;
        if (memchr(rx, 0, (size_t)(nl - rx) - (nl > rx && nl[-1] == 0 ? 1 : 0))) { disconnect(); return; }
        process(rx);
        if (peer == BAD_SOCKET) return;
        memmove(rx, rx + consumed, rx_len - consumed); rx_len -= consumed;
    } else if (rx_len == sizeof(rx) - 1) disconnect();
}
int lua_bridge_paused(void) { return vm && paused && steps == 0; }
uint32_t lua_bridge_frame_start(uint32_t inputs) {
    if (!vm) return inputs;
    current_inputs = inputs;
    dispatch_callbacks(0);
    current_inputs = (inputs & ~input_mask) | input_values;
    if (input_mask & 0xfff) current_inputs |= 1u << 30;
    if (input_mask & 0xfff000) current_inputs |= 1u << 31;
    return current_inputs;
}
void lua_bridge_frame_end(void) {
    if (!vm) return;
    ++frame;
    if (steps) --steps;
    input_mask = input_values = 0;
    dispatch_callbacks(1);
    if (script) resume_script();
}
void lua_bridge_shutdown(void) {
    disconnect();
    if (listener != BAD_SOCKET) close_socket(listener);
    listener = BAD_SOCKET;
    if (vm) lua_close(vm);
    vm = script = NULL; script_ref = LUA_NOREF;
    memset(callbacks, 0, sizeof(callbacks));
    input_mask = input_values = current_inputs = 0; paused = steps = 0;
    selected_bus = 0; frame = next_id = 0; last_error[0] = output[0] = 0;
#ifdef _WIN32
    WSACleanup();
#endif
}
