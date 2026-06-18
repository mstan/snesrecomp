/* snesref — minimal SDL2 libretro frontend: a known-good SNES interpreter
 * with recomp debugging instrumentation, used as the differential oracle for
 * the recompiler. Loads a libretro SNES core, plays a ROM with reliable SDL
 * keyboard input, and logs per-frame WRAM changes (same JSON shape as the
 * recomp debug_server's wram_writes_at) to mmx_trace.jsonl.
 *
 *   snesref.exe <core.dll> <rom.sfc>
 *
 * Keys (match the recomp keybinds): arrows=D-pad, Z=B(jump), X=A, A=Y(fire),
 *   S=X, C=L, V=R, Enter=Start, RShift=Select.
 *   F2 = save state -> mmx_state.bin    F4 = load state
 *   F5 = clear mmx_trace.jsonl (start a fresh capture)   Esc = quit
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "libretro.h"

// ---- core function pointers ----
static HMODULE g_core;
#define LR(sym) static decltype(&sym) p_##sym;
LR(retro_init) LR(retro_deinit) LR(retro_api_version)
LR(retro_get_system_info) LR(retro_get_system_av_info)
LR(retro_set_environment) LR(retro_set_video_refresh)
LR(retro_set_audio_sample) LR(retro_set_audio_sample_batch)
LR(retro_set_input_poll) LR(retro_set_input_state)
LR(retro_set_controller_port_device)
LR(retro_load_game) LR(retro_unload_game) LR(retro_run)
LR(retro_serialize_size) LR(retro_serialize) LR(retro_unserialize)
LR(retro_get_memory_data) LR(retro_get_memory_size)
#undef LR

template<class T> static void bind(T& fn, const char* name) {
    fn = (T)GetProcAddress(g_core, name);
    if (!fn) { fprintf(stderr, "missing core symbol: %s\n", name); exit(2); }
}

// ---- video state ----
static SDL_Window*   g_win;
static SDL_Renderer* g_ren;
static SDL_Texture*  g_tex;
static int g_tex_w = 0, g_tex_h = 0;
static retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static SDL_GameController* g_pad = nullptr;

static void open_first_pad() {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) { printf("[controller: %s]\n", SDL_GameControllerName(g_pad)); fflush(stdout); return; }
        }
    }
}

// ---- WRAM trace ----
// Retargeted 2026-06-02 for the Rangda Bangda blue-eye coordinate-space bug:
// capture the eye AI's CE9A target globals $0BAD/$0BB0 (X's position used as
// the fly target) plus the whole $0E00-$1FFF object table region so the
// flying-eye slot can be located by its marching X position regardless of
// which slot snes9x picks. Goal: read hardware eye [D+0x05] (eye X) and
// $0BAD at the launch frame and compare to the recomp (eye X ~83 screen vs
// $0BAD 5143 level). See MegamanXRecomp/ISSUES.md.
// Per-frame changed-byte trace over low WRAM ($0000-$1FFF: zero page, stack,
// and game-logic state — where first divergences live). Widened from the old
// MMX-specific region for general recomp-vs-oracle first-divergence diffing.
// On the first tick the whole region is emitted as a baseline so the diff tool
// can reconstruct absolute state at any frame. Output file is
// SNESREF_TRACE_FILE (default snesref_trace.jsonl).
#define WRAM_LO 0x00000
#define WRAM_HI 0x01fff
static FILE* g_log;
static uint8_t g_prev[WRAM_HI - WRAM_LO + 1];
static bool    g_primed = false;
static uint32_t g_frame = 0;

static const char* trace_path() {
    const char* p = getenv("SNESREF_TRACE_FILE");
    return (p && p[0]) ? p : "snesref_trace.jsonl";
}
static void emit(int addr, uint8_t o, uint8_t n) {
    if (!g_log) { g_log = fopen(trace_path(), "a"); if (!g_log) return; }
    fprintf(g_log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
            g_frame, addr, o, n);
}

static void trace_tick() {
    uint8_t* ram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || sz <= WRAM_HI) return;
    if (!g_primed) {
        for (int a=WRAM_LO;a<=WRAM_HI;a++){ g_prev[a-WRAM_LO]=ram[a]; emit(a,0,ram[a]); }
        g_primed=true; return;
    }
    for (int a=WRAM_LO;a<=WRAM_HI;a++){ uint8_t v=ram[a]; if(v!=g_prev[a-WRAM_LO]){ emit(a,g_prev[a-WRAM_LO],v); g_prev[a-WRAM_LO]=v; } }
    if (g_log && (g_frame % 30)==0) fflush(g_log);
}

static void clear_trace() {
    if (g_log) { fclose(g_log); g_log=nullptr; }
    FILE* f=fopen(trace_path(),"w"); if(f) fclose(f);
    g_primed=false;
    printf("[trace cleared]\n"); fflush(stdout);
}

// ---- libretro callbacks ----
static bool cb_environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(const retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: if(data) *(bool*)data=false; return true;
        default: return false;
    }
}
static void ensure_texture(unsigned w, unsigned h) {
    if ((int)w==g_tex_w && (int)h==g_tex_h && g_tex) return;
    if (g_tex) SDL_DestroyTexture(g_tex);
    Uint32 sf = (g_fmt==RETRO_PIXEL_FORMAT_XRGB8888) ? SDL_PIXELFORMAT_ARGB8888
              : (g_fmt==RETRO_PIXEL_FORMAT_RGB565)   ? SDL_PIXELFORMAT_RGB565
              :                                        SDL_PIXELFORMAT_ARGB1555;
    g_tex = SDL_CreateTexture(g_ren, sf, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_tex_w=w; g_tex_h=h;
}
static void cb_video(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (data && w && h) {
        ensure_texture(w,h);
        SDL_UpdateTexture(g_tex, nullptr, data, (int)pitch);
    }
    SDL_RenderClear(g_ren);
    if (g_tex) SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
    SDL_RenderPresent(g_ren);
}
static void  cb_audio_sample(int16_t, int16_t) {}
static size_t cb_audio_batch(const int16_t*, size_t frames) { return frames; }
static void  cb_input_poll(void) {}

static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port!=0 || device!=RETRO_DEVICE_JOYPAD) return 0;
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    SDL_Scancode sc; SDL_GameControllerButton gb;
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_B:      sc=SDL_SCANCODE_Z;      gb=SDL_CONTROLLER_BUTTON_A; break;             /* jump (PS5 cross) */
        case RETRO_DEVICE_ID_JOYPAD_Y:      sc=SDL_SCANCODE_A;      gb=SDL_CONTROLLER_BUTTON_X; break;             /* fire (PS5 square) */
        case RETRO_DEVICE_ID_JOYPAD_A:      sc=SDL_SCANCODE_X;      gb=SDL_CONTROLLER_BUTTON_B; break;             /* PS5 circle */
        case RETRO_DEVICE_ID_JOYPAD_X:      sc=SDL_SCANCODE_S;      gb=SDL_CONTROLLER_BUTTON_Y; break;             /* PS5 triangle */
        case RETRO_DEVICE_ID_JOYPAD_L:      sc=SDL_SCANCODE_C;      gb=SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
        case RETRO_DEVICE_ID_JOYPAD_R:      sc=SDL_SCANCODE_V;      gb=SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
        case RETRO_DEVICE_ID_JOYPAD_START:  sc=SDL_SCANCODE_RETURN; gb=SDL_CONTROLLER_BUTTON_START; break;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: sc=SDL_SCANCODE_RSHIFT; gb=SDL_CONTROLLER_BUTTON_BACK; break;
        case RETRO_DEVICE_ID_JOYPAD_UP:     sc=SDL_SCANCODE_UP;     gb=SDL_CONTROLLER_BUTTON_DPAD_UP; break;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   sc=SDL_SCANCODE_DOWN;   gb=SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   sc=SDL_SCANCODE_LEFT;   gb=SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  sc=SDL_SCANCODE_RIGHT;  gb=SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
        default: return 0;
    }
    if (ks[sc]) return 1;
    if (g_pad && SDL_GameControllerGetButton(g_pad, gb)) return 1;
    /* analog left-stick as d-pad fallback */
    if (g_pad) {
        const int DZ = 16000;
        if (id==RETRO_DEVICE_ID_JOYPAD_LEFT  && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX) < -DZ) return 1;
        if (id==RETRO_DEVICE_ID_JOYPAD_RIGHT && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX) >  DZ) return 1;
        if (id==RETRO_DEVICE_ID_JOYPAD_UP    && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY) < -DZ) return 1;
        if (id==RETRO_DEVICE_ID_JOYPAD_DOWN  && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY) >  DZ) return 1;
    }
    return 0;
}

// ---- save state (9 slots): Shift+Fn = save slot n, Fn = load slot n ----
static void slot_path(int slot, char* out, size_t n) { snprintf(out, n, "mmx_state_%d.bin", slot); }

static void save_state(int slot) {
    size_t n = p_retro_serialize_size(); if(!n) return;
    std::vector<uint8_t> buf(n);
    if (p_retro_serialize(buf.data(), n)) {
        char path[64]; slot_path(slot, path, sizeof path);
        FILE* f=fopen(path,"wb"); if(f){ fwrite(buf.data(),1,n,f); fclose(f); printf("[slot %d SAVED %zu bytes]\n",slot,n); fflush(stdout);} }
}
static void load_state(int slot) {
    char path[64]; slot_path(slot, path, sizeof path);
    FILE* f=fopen(path,"rb"); if(!f){ printf("[slot %d empty]\n",slot); fflush(stdout); return; }
    fseek(f,0,SEEK_END); long fn=ftell(f); fseek(f,0,SEEK_SET);
    size_t need = p_retro_serialize_size();        // size the core expects NOW
    if (fn <= 0) { fclose(f); printf("[slot %d bad file]\n",slot); fflush(stdout); return; }
    size_t bn = ((size_t)fn > need) ? (size_t)fn : need;   // never under-size the buffer
    std::vector<uint8_t> buf(bn, 0);
    fread(buf.data(),1,(size_t)fn,f); fclose(f);
    printf("[slot %d load: file=%ld coreNeeds=%zu]\n",slot,fn,need); fflush(stdout);
    if ((size_t)fn != need)
        printf("[warn slot %d: size mismatch file=%ld need=%zu]\n",slot,fn,need);
    bool ok = p_retro_unserialize(buf.data(), need);   // pass the size the core expects
    printf("[slot %d %s]\n",slot, ok?"LOADED":"unserialize returned FALSE"); fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr,"usage: snesref <core.dll> <rom.sfc>\n"); return 1; }
    const char* corePath = argv[1];
    const char* romPath  = argv[2];

    g_core = LoadLibraryA(corePath);
    if (!g_core) { fprintf(stderr,"LoadLibrary failed: %s (err %lu)\n", corePath, GetLastError()); return 2; }
    bind(p_retro_init,"retro_init"); bind(p_retro_deinit,"retro_deinit");
    bind(p_retro_api_version,"retro_api_version");
    bind(p_retro_get_system_info,"retro_get_system_info");
    bind(p_retro_get_system_av_info,"retro_get_system_av_info");
    bind(p_retro_set_environment,"retro_set_environment");
    bind(p_retro_set_video_refresh,"retro_set_video_refresh");
    bind(p_retro_set_audio_sample,"retro_set_audio_sample");
    bind(p_retro_set_audio_sample_batch,"retro_set_audio_sample_batch");
    bind(p_retro_set_input_poll,"retro_set_input_poll");
    bind(p_retro_set_input_state,"retro_set_input_state");
    bind(p_retro_set_controller_port_device,"retro_set_controller_port_device");
    bind(p_retro_load_game,"retro_load_game"); bind(p_retro_unload_game,"retro_unload_game");
    bind(p_retro_run,"retro_run");
    bind(p_retro_serialize_size,"retro_serialize_size");
    bind(p_retro_serialize,"retro_serialize"); bind(p_retro_unserialize,"retro_unserialize");
    bind(p_retro_get_memory_data,"retro_get_memory_data");
    bind(p_retro_get_memory_size,"retro_get_memory_size");

    p_retro_set_environment(cb_environment);
    p_retro_init();

    retro_system_info si; memset(&si,0,sizeof si); p_retro_get_system_info(&si);
    printf("core: %s %s  need_fullpath=%d\n", si.library_name?si.library_name:"?",
           si.library_version?si.library_version:"?", si.need_fullpath);

    retro_game_info gi; memset(&gi,0,sizeof gi); gi.path=romPath;
    std::vector<uint8_t> rom;
    if (!si.need_fullpath) {
        FILE* f=fopen(romPath,"rb"); if(!f){ fprintf(stderr,"cannot open rom %s\n",romPath); return 3; }
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
        rom.resize(n); fread(rom.data(),1,n,f); fclose(f);
        gi.data=rom.data(); gi.size=rom.size();
    }
    p_retro_set_video_refresh(cb_video);
    p_retro_set_audio_sample(cb_audio_sample);
    p_retro_set_audio_sample_batch(cb_audio_batch);
    p_retro_set_input_poll(cb_input_poll);
    p_retro_set_input_state(cb_input_state);
    if (!p_retro_load_game(&gi)) { fprintf(stderr,"retro_load_game failed\n"); return 4; }
    p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    retro_system_av_info av; memset(&av,0,sizeof av); p_retro_get_system_av_info(&av);
    int vw=(int)av.geometry.base_width, vh=(int)av.geometry.base_height;
    if(vw<=0)vw=256; if(vh<=0)vh=224;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 5; }
    open_first_pad();
    g_win = SDL_CreateWindow("snesref (libretro) — Fn load / Shift+Fn save / Backspace clear-trace",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, vw*2, vh*2, SDL_WINDOW_RESIZABLE);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(g_ren, vw, vh);

    printf("RUN. KB: arrows=DPad Z=B(jump) X=A A=Y(fire) S=X C=L V=R Enter=Start RShift=Select\n");
    printf("     Pad: dpad/L-stick, Cross=jump Square=fire Circle=A Triangle=X L1/R1=L/R Start/Select\n");
    printf("     States: Fn=LOAD slot n, Shift+Fn=SAVE slot n (1-9) | Backspace=clear trace | Esc=quit\n");
    fflush(stdout);

    bool running=true;
    Uint64 freq=SDL_GetPerformanceFrequency(), prev=SDL_GetPerformanceCounter();
    const double target = (double)freq / 60.098;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type==SDL_QUIT) running=false;
            else if (e.type==SDL_CONTROLLERDEVICEADDED) open_first_pad();
            else if (e.type==SDL_CONTROLLERDEVICEREMOVED) { if(g_pad){ SDL_GameControllerClose(g_pad); g_pad=nullptr; printf("[controller removed]\n"); fflush(stdout);} open_first_pad(); }
            else if (e.type==SDL_KEYDOWN && e.key.repeat==0) {
                SDL_Scancode s = e.key.keysym.scancode;
                if (s==SDL_SCANCODE_ESCAPE) running=false;
                else if (s==SDL_SCANCODE_BACKSPACE) clear_trace();
                else if (s>=SDL_SCANCODE_F1 && s<=SDL_SCANCODE_F9) {
                    int slot = (int)(s - SDL_SCANCODE_F1) + 1;
                    if (e.key.keysym.mod & KMOD_SHIFT) save_state(slot); else load_state(slot);
                }
            }
        }
        p_retro_run();
        g_frame++;
        trace_tick();
        // headless self-test: MMX_SELFTEST=1 -> save@200, load@400, quit@600
        { static int st=-1; if(st<0){const char*v=getenv("MMX_SELFTEST"); st=(v&&v[0]&&v[0]!='0')?1:0;}
          if(st){ if(g_frame==200){printf("[selftest] saving slot9 @f200\n");fflush(stdout);save_state(9);}
                  else if(g_frame==400){printf("[selftest] loading slot9 @f400\n");fflush(stdout);load_state(9);}
                  else if(g_frame==410){printf("[selftest] SURVIVED load, still running @f410\n");fflush(stdout);}
                  else if(g_frame>=600){running=false;} } }
        // headless deterministic capture: SNESREF_FRAMES=N -> run N frames, no
        // input (attract/boot is self-driving), then quit. For diff captures.
        { static long fr=-2; if(fr==-2){const char*v=getenv("SNESREF_FRAMES"); fr=(v&&v[0])?atol(v):-1;}
          if(fr>0 && (long)g_frame>=fr){ if(g_log)fflush(g_log); running=false; } }
        // 60fps cap
        for (;;) {
            Uint64 now=SDL_GetPerformanceCounter();
            double el=(double)(now-prev);
            if (el>=target) { prev=now; break; }
            double rem_ms=(target-el)*1000.0/(double)freq;
            if (rem_ms>1.5) SDL_Delay((Uint32)(rem_ms-1.0)); // else busy-spin
        }
    }
    if (g_log) fflush(g_log);
    p_retro_unload_game(); p_retro_deinit();
    SDL_Quit(); FreeLibrary(g_core);
    return 0;
}
