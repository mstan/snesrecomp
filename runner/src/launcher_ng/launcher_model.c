// launcher_model.c — implementation of the game-agnostic launcher view-model.
//
// Pure logic: no SDL, no OpenGL, no UI toolkit. Safe to compile as C and link
// into any game or either prototype backend.

#include "launcher_model.h"

#include "crc32.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int kFreqTable[] = { 32000, 44100, 48000 };
static const int kFreqCount   = (int)(sizeof(kFreqTable) / sizeof(kFreqTable[0]));

static const char* kButtonNames[LNG_BTN_COUNT] = {
    "Up", "Down", "Left", "Right", "A", "B", "X", "Y",
    "L", "R", "Start", "Select"
};
// Player 1 keyboard defaults (Player 2 defaults unbound, mirroring the RML note).
static const char* kP1Defaults[LNG_BTN_COUNT] = {
    "Up", "Down", "Left", "Right", "X", "Z", "S", "A", "D", "C", "Enter", "RShift"
};
// Display labels for the 11 engine hotkeys (order == LngHotkey == [KeyMap] keys).
static const char* kHotkeyNames[LNG_HK_COUNT] = {
    "Fullscreen", "Reset", "Pause", "Pause (dimmed)", "Fast-forward",
    "Window bigger", "Window smaller", "Volume up", "Volume down",
    "FPS readout", "Toggle renderer"
};
static const char* kViewNames[3] = { "Dashboard", "Settings", "Controller" };
static const char* kSrcNames[3]  = { "None", "Keyboard", "Gamepad" };

static void safe_copy(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void launcher_model_init(LauncherModel* m,
                         const SnesLauncherCSettings* io,
                         const SnesLauncherCGameInfo* game,
                         const char* initial_rom) {
    memset(m, 0, sizeof(*m));

    if (game) {
        m->game_name            = game->name ? game->name : "Unknown Game";
        m->region               = game->region ? game->region : "";
        m->widescreen_supported = game->widescreen_supported != 0;
        m->msu1_supported       = game->msu1_supported != 0;
        m->saves_supported      = game->sram_path != NULL;
        m->sram_path            = game->sram_path;
        /* 0 = unset (caller predates the field) -> assume 2 players. */
        m->player_count         = game->num_players ? clampi(game->num_players, 1, 2) : 2;
        m->expected_crc         = game->expected_crc;
        m->has_expected_crc     = game->has_expected_crc;
        m->known_sha256         = game->known_sha256;
        m->num_known_sha256     = game->num_known_sha256;
    } else {
        m->game_name    = "Unknown Game";
        m->region       = "";
        m->player_count = 2;
    }

    if (io) m->s = *io;

    // Real ROM read + CRC/SHA verification (computes rom_size, crc_match,
    // sha_match). No synthesized/faked facts.
    launcher_model_set_rom(m, initial_rom);

    m->view      = LNG_VIEW_DASHBOARD;
    m->action    = LNG_ACTION_NONE;
    m->cfg_player = 0;

    // Placeholder display until launcher_binds_load() fills real values from
    // keybinds.ini / config.ini [KeyMap].
    for (int b = 0; b < LNG_BTN_COUNT; ++b) {
        safe_copy(m->binds[0][b], sizeof(m->binds[0][b]), kP1Defaults[b]);
        safe_copy(m->binds[1][b], sizeof(m->binds[1][b]), "(unbound)");
    }
    for (int h = 0; h < LNG_HK_COUNT; ++h)
        m->hotkeys[h][0] = '\0';
}

void launcher_model_commit(const LauncherModel* m, SnesLauncherCSettings* io) {
    if (io) *io = m->s;
}

void launcher_model_set_rom(LauncherModel* m, const char* path) {
    m->rom_present = path && path[0] != '\0';
    safe_copy(m->rom_full, sizeof(m->rom_full), m->rom_present ? path : "");

    // Display just the basename (handles both / and \ separators).
    const char* base = m->rom_full;
    for (const char* q = m->rom_full; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    safe_copy(m->rom_file, sizeof(m->rom_file), m->rom_present ? base : "(none)");

    /* Read the ROM once: real size, and real CRC32 + SHA-256 over the cartridge
     * body (SMC copier header stripped) compared against the expected
     * fingerprint. No faking — a wrong/corrupt ROM fails verification. */
    m->rom_size[0] = '\0';
    m->crc_match = false;
    m->sha_match = false;
    if (m->rom_present) {
        FILE* f = fopen(m->rom_full, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0) {
                snprintf(m->rom_size, sizeof(m->rom_size), "%.2f MB (%ld Mbit)",
                         (double)n / (1024.0 * 1024.0), (long)((n * 8) / (1024 * 1024)));
                uint8_t* buf = (uint8_t*)malloc((size_t)n);
                if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                    /* SMC copier header is present when (size % 1024 == 512). */
                    size_t hdr  = ((size_t)n % 1024 == 512) ? 512 : 0;
                    const uint8_t* body = buf + hdr;
                    size_t blen = (size_t)n - hdr;
                    uint32_t crc = crc32_compute(body, blen);
                    uint8_t  sha[32];
                    sha256_compute(body, blen, sha);
                    m->crc_match = m->has_expected_crc && crc == m->expected_crc;
                    for (size_t k = 0; k < m->num_known_sha256; ++k)
                        if (memcmp(sha, m->known_sha256[k], 32) == 0) { m->sha_match = true; break; }
                }
                free(buf);
            }
            fclose(f);
        }
    }
    if (!m->rom_size[0]) safe_copy(m->rom_size, sizeof(m->rom_size), "--");
}

const char* launcher_model_rom_path(const LauncherModel* m) {
    return m->rom_full;
}

bool launcher_model_rom_verified(const LauncherModel* m) {
    if (!m->rom_present) return false;
    const int has_crc = m->has_expected_crc;
    const int has_sha = m->num_known_sha256 > 0;
    if (!has_crc && !has_sha) return false;   // no fingerprint => can't vouch
    if (has_crc && !m->crc_match) return false;
    if (has_sha && !m->sha_match) return false;
    return true;
}

void launcher_model_set_view(LauncherModel* m, LngView v) {
    m->view = v;
}

void launcher_model_open_config(LauncherModel* m, int player) {
    m->cfg_player = clampi(player, 0, 1);
    m->view = LNG_VIEW_CONTROLLER;
}

void launcher_model_cycle_scale(LauncherModel* m) {
    m->s.window_scale = (m->s.window_scale >= 6) ? 1 : m->s.window_scale + 1;
    if (m->s.window_scale < 1) m->s.window_scale = 1;
}

void launcher_model_toggle_filter(LauncherModel* m) {
    m->s.linear_filter = !m->s.linear_filter;
}

void launcher_model_toggle_widescreen(LauncherModel* m) {
    if (!m->widescreen_supported) return;   // gated: no-op when unsupported
    m->s.widescreen = !m->s.widescreen;
}

void launcher_model_cycle_freq(LauncherModel* m) {
    int idx = 0;
    for (int i = 0; i < kFreqCount; ++i)
        if (kFreqTable[i] == m->s.audio_freq) { idx = i; break; }
    m->s.audio_freq = kFreqTable[(idx + 1) % kFreqCount];
}

void launcher_model_volume_delta(LauncherModel* m, int delta) {
    m->s.volume = clampi(m->s.volume + delta, 0, 100);
}

void launcher_model_cycle_player_src(LauncherModel* m, int player) {
    player = clampi(player, 0, 1);
    m->s.player_src[player] = (m->s.player_src[player] + 1) % 3;  // None/Kbd/Pad
}

void launcher_model_deadzone_delta(LauncherModel* m, int player, int delta) {
    player = clampi(player, 0, 1);
    m->s.deadzone[player] = clampi(m->s.deadzone[player] + delta, 0, 100);
}

void launcher_model_set_source(LauncherModel* m, int player, int kind,
                               uint32_t pad_id, const char* pad_name) {
    player = clampi(player, 0, 1);
    m->s.player_src[player] = clampi(kind, 0, 2);
    if (kind == 2) {
        m->player_pad_id[player] = pad_id;
        safe_copy(m->player_pad_name[player], sizeof(m->player_pad_name[player]),
                  pad_name ? pad_name : "Gamepad");
    } else {
        m->player_pad_id[player] = 0;
        m->player_pad_name[player][0] = '\0';
    }
}

void launcher_model_request_skip_toggle(LauncherModel* m) {
    if (!m->s.skip_launcher) {
        m->skip_modal_open = true;    // enabling: confirm first
    } else {
        m->s.skip_launcher = 0;       // disabling: immediate
    }
}

void launcher_model_skip_confirm(LauncherModel* m) {
    m->s.skip_launcher = 1;
    m->skip_modal_open = false;
}

void launcher_model_skip_cancel(LauncherModel* m) {
    m->skip_modal_open = false;
}

void launcher_model_begin_capture(LauncherModel* m, LngButton b) {
    if (b < 0 || b >= LNG_BTN_COUNT) return;
    m->hk_capturing = false;
    m->capturing    = true;
    m->capture_btn  = b;
}
void launcher_model_cancel_capture(LauncherModel* m) { m->capturing = false; }

void launcher_model_begin_hk_capture(LauncherModel* m, LngHotkey h) {
    if (h < 0 || h >= LNG_HK_COUNT) return;
    m->capturing    = false;
    m->hk_capturing = true;
    m->capture_hk   = h;
}
void launcher_model_cancel_hk_capture(LauncherModel* m) { m->hk_capturing = false; }

const char* launcher_model_scale_label(const LauncherModel* m) {
    static char buf[8];
    int s = m->s.window_scale < 1 ? 1 : m->s.window_scale;
    snprintf(buf, sizeof(buf), "%dx", s);
    return buf;
}

const char* launcher_model_freq_label(const LauncherModel* m) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d Hz", m->s.audio_freq);
    return buf;
}

const char* launcher_model_player_src_label(const LauncherModel* m, int player) {
    player = clampi(player, 0, 1);
    int src = clampi(m->s.player_src[player], 0, 2);
    if (src == 2 && m->player_pad_name[player][0])   // show the actual device name
        return m->player_pad_name[player];
    return kSrcNames[src];
}

const char* launcher_button_name(LngButton b) {
    if (b < 0 || b >= LNG_BTN_COUNT) return "?";
    return kButtonNames[b];
}

const char* launcher_hotkey_name(LngHotkey h) {
    if (h < 0 || h >= LNG_HK_COUNT) return "?";
    return kHotkeyNames[h];
}

const char* launcher_view_name(LngView v) {
    if (v < 0 || v > LNG_VIEW_CONTROLLER) return "?";
    return kViewNames[v];
}
