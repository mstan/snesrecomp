// launcher_gui.cpp — shared RmlUi pre-boot launcher (see launcher_gui.h).
//
// Ported in structure from psxrecomp/runtime/launcher/launcher.cpp, adapted to
// the SNES settings model and the mockup's dashboard (GAME / CONTROLLERS /
// SAVES / MSU-1 AUDIO) plus nested Settings and Controller views.

#include "launcher_gui.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include "RmlUi_Renderer_GL3.h"
#include "RmlUi_Platform_SDL.h"

#include <functional>
#include <memory>

#include <SDL.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>

extern "C" {
#include "crc32.h"
#include "sha256.h"
#include "keybinds.h"
}

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  include <shellapi.h>
#  include <shlobj.h>
#  include <objbase.h>
#endif

namespace fs = std::filesystem;

namespace snes_launcher {
namespace {

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------

std::string basename_of(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string human_size(long bytes) {
    char buf[64];
    if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%ld B", bytes);
    return buf;
}

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

std::string sha_short(const uint8_t d[32]) {
    char b[80];
    std::snprintf(b, sizeof(b),
                  "%02X%02X%02X%02X%02X...%02X%02X%02X",
                  d[0], d[1], d[2], d[3], d[4], d[29], d[30], d[31]);
    return b;
}

// Read a whole file. Returns empty vector on failure.
std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return data;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    if (sz > 0) {
        data.resize((size_t)sz);
        if (std::fread(data.data(), 1, (size_t)sz, f) != (size_t)sz) data.clear();
    }
    std::fclose(f);
    return data;
}

// SMC copier header is present when (size % 1024 == 512).
size_t smc_header(size_t sz) { return (sz % 1024 == 512) ? 512 : 0; }

// Best-effort LoROM/HiROM detection from the cartridge header checksum pair.
const char* detect_mapping(const uint8_t* rom, size_t sz) {
    auto valid_at = [&](size_t hdr) -> bool {
        if (hdr + 0x20 > sz) return false;
        uint16_t cks  = (uint16_t)(rom[hdr + 0x1E] | (rom[hdr + 0x1F] << 8));
        uint16_t cmpl = (uint16_t)(rom[hdr + 0x1C] | (rom[hdr + 0x1D] << 8));
        return (uint16_t)(cks ^ cmpl) == 0xFFFF;
    };
    bool lo = valid_at(0x7FC0);
    bool hi = valid_at(0xFFC0);
    if (lo && !hi) return "LoROM";
    if (hi && !lo) return "HiROM";
    return "LoROM";  // SNES default / SMW
}

#ifdef _WIN32
bool pick_file(const char* title, const char* filter, char* out, size_t max_len) {
    OPENFILENAMEA ofn;
    std::memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = (DWORD)max_len;
    ofn.lpstrTitle  = title;
    // OFN_NOCHANGEDIR: keep the dialog from changing the process CWD, which would
    // defeat snesrecomp_anchor_to_exe_dir() and scatter config.ini/saves next to
    // the picked file instead of the exe.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY
              | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) != 0;
}
bool pick_folder(char* out, size_t max_len) {
    BROWSEINFOA bi;
    std::memset(&bi, 0, sizeof(bi));
    bi.lpszTitle = "Select MSU-1 audio folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;
    bool ok = SHGetPathFromIDListA(pidl, out) != 0;
    CoTaskMemFree(pidl);
    (void)max_len;
    return ok;
}
void open_in_explorer(const char* path) {
    ShellExecuteA(nullptr, "open", path, nullptr, nullptr, SW_SHOWNORMAL);
}
#else
bool pick_file(const char*, const char*, char* out, size_t) { out[0] = '\0'; return false; }
bool pick_folder(char* out, size_t) { out[0] = '\0'; return false; }
void open_in_explorer(const char*) {}
#endif

// Apply an IPS patch (classic 8-byte-offset format) from `patch` onto a copy of
// `src`, returning the patched bytes. Returns empty on malformed patch.
std::vector<uint8_t> ips_apply(const std::vector<uint8_t>& src,
                               const std::vector<uint8_t>& patch) {
    std::vector<uint8_t> out = src;
    if (patch.size() < 8 || std::memcmp(patch.data(), "PATCH", 5) != 0)
        return {};
    size_t i = 5;
    auto u24 = [&](void) -> long {
        long v = (patch[i] << 16) | (patch[i + 1] << 8) | patch[i + 2];
        i += 3; return v;
    };
    auto u16 = [&](void) -> long {
        long v = (patch[i] << 8) | patch[i + 1];
        i += 2; return v;
    };
    while (i + 3 <= patch.size()) {
        if (std::memcmp(&patch[i], "EOF", 3) == 0) { i += 3; return out; }
        long off = u24();
        if (i + 2 > patch.size()) return {};
        long len = u16();
        if (len == 0) {  // RLE chunk
            if (i + 3 > patch.size()) return {};
            long rlen = u16();
            uint8_t val = patch[i++];
            if ((size_t)(off + rlen) > out.size()) out.resize(off + rlen);
            for (long k = 0; k < rlen; k++) out[off + k] = val;
        } else {
            if (i + len > patch.size()) return {};
            if ((size_t)(off + len) > out.size()) out.resize(off + len);
            std::memcpy(&out[off], &patch[i], len);
            i += len;
        }
    }
    return out;  // no EOF marker, but consumed cleanly
}

// Count <base>-N.pcm files in dir (MSU pack presence check).
bool msu_pack_present(const std::string& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return false;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        std::string n = e.path().filename().string();
        size_t dot = n.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = n.substr(dot);
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".pcm" || ext == ".msu") return true;
        }
    }
    return false;
}

// One entry in a controller-source dropdown: a stable token + display label.
struct SrcOption { Rml::String value; Rml::String label; };

// ----------------------------------------------------------------------------
// Hotkey ([KeyMap]) model — the Settings → Hotkeys rebind editor.
//
// The system hotkeys live in config.ini's [KeyMap] section and are parsed by
// each game's config.c with SDL *keycode* names ("Ctrl+r", "Tab"); the player
// buttons live in keybinds.ini with SDL *scancode* names. The two capture
// paths below must not be mixed up. The kKeyNameId hotkey set and the code
// defaults are identical across all snesrecomp games, so this table is
// engine-owned; `def` mirrors each game's kDefaultKbdControls entry (shown
// when config.ini has no line for the key; empty = unbound).
// ----------------------------------------------------------------------------

struct HotkeyDef { const char* key; const char* label; const char* def; };
const HotkeyDef kHotkeys[] = {
    { "Fullscreen",     "Toggle fullscreen",       "Alt+Return" },
    { "Reset",          "Reset game",              "Ctrl+R"     },
    { "Pause",          "Pause",                   "Shift+P"    },
    { "PauseDimmed",    "Pause (dimmed)",          "P"          },
    { "Turbo",          "Turbo (fast-forward)",    "Tab"        },
    { "WindowBigger",   "Window bigger",           ""           },
    { "WindowSmaller",  "Window smaller",          ""           },
    { "VolumeUp",       "Volume up",               ""           },
    { "VolumeDown",     "Volume down",             ""           },
    { "DisplayPerf",    "FPS / perf readout",      "F"          },
    { "ToggleRenderer", "Toggle PPU renderer",     "R"          },
};
constexpr int kHotkeyCount = (int)(sizeof(kHotkeys) / sizeof(kHotkeys[0]));

bool ieq(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

// Does this line assign `key` (optionally commented out)? Mirrors config.c's
// CfgLineIsKey so the editor replaces the same lines WriteConfigFile would.
bool line_is_key(const std::string& line, const char* key) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i < line.size() && line[i] == '#') {
        i++;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    }
    size_t klen = std::strlen(key);
    if (line.size() - i < klen) return false;
    for (size_t k = 0; k < klen; k++)
        if (tolower((unsigned char)line[i + k]) != tolower((unsigned char)key[k])) return false;
    i += klen;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    return i < line.size() && line[i] == '=';
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        std::string l = text.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        start = nl + 1;
    }
    return lines;
}

// Read the current [KeyMap] binding strings for every kHotkeys entry from
// `path`. A key with no (uncommented) line keeps its built-in default.
std::vector<std::string> hotkeys_read(const std::string& path) {
    std::vector<std::string> vals;
    for (int i = 0; i < kHotkeyCount; i++) vals.emplace_back(kHotkeys[i].def);

    std::vector<uint8_t> raw = read_file(path);
    if (raw.empty()) return vals;
    std::string text((const char*)raw.data(), raw.size());

    bool in_keymap = false;
    for (const std::string& line : split_lines(text)) {
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos) continue;
        if (line[i] == '#') continue;
        if (line[i] == '[') {
            size_t close = line.find(']', i);
            std::string sec = line.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
            in_keymap = ieq(sec, "KeyMap");
            continue;
        }
        if (!in_keymap) continue;
        size_t eq = line.find('=', i);
        if (eq == std::string::npos) continue;
        std::string k = line.substr(i, eq - i);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        std::string v = line.substr(eq + 1);
        size_t vs = v.find_first_not_of(" \t");
        v = (vs == std::string::npos) ? "" : v.substr(vs);
        size_t ce = v.find('#');                       // strip trailing comment
        if (ce != std::string::npos) v = v.substr(0, ce);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        for (int h = 0; h < kHotkeyCount; h++)
            if (ieq(k, kHotkeys[h].key)) { vals[h] = v; break; }
    }
    return vals;
}

// Surgically set `Key = value` inside [KeyMap] in `path`, preserving every
// other line byte-for-byte. Replaces an existing (possibly commented) line for
// the key, else inserts at the end of the section, else appends a new section.
// An empty value writes "Key = " which config.c parses as "unbound" (and
// suppresses the built-in default for that key).
void hotkeys_write(const std::string& path, const char* key, const std::string& value) {
    std::vector<uint8_t> raw = read_file(path);
    std::string text((const char*)raw.data(), raw.size());
    std::vector<std::string> lines = split_lines(text);

    std::string assign = std::string(key) + " = " + value;

    int keymap_start = -1, keymap_end = -1;   // [start, end) line range of the section body
    for (int i = 0; i < (int)lines.size(); i++) {
        size_t s = lines[i].find_first_not_of(" \t");
        if (s == std::string::npos || lines[i][s] != '[') continue;
        size_t close = lines[i].find(']', s);
        std::string sec = lines[i].substr(s + 1, close == std::string::npos ? std::string::npos : close - s - 1);
        if (keymap_start >= 0) { keymap_end = i; break; }
        if (ieq(sec, "KeyMap")) keymap_start = i + 1;
    }
    if (keymap_start >= 0 && keymap_end < 0) keymap_end = (int)lines.size();

    if (keymap_start < 0) {
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back("[KeyMap]");
        lines.push_back(assign);
    } else {
        int hit = -1;
        for (int i = keymap_start; i < keymap_end; i++)
            if (line_is_key(lines[i], key)) { hit = i; break; }
        if (hit >= 0) {
            lines[hit] = assign;
        } else {
            // Insert before the trailing blank lines of the section so the
            // separation from the next [section] stays intact.
            int at = keymap_end;
            while (at > keymap_start && lines[at - 1].find_first_not_of(" \t") == std::string::npos)
                at--;
            lines.insert(lines.begin() + at, assign);
        }
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "launcher: cannot write %s\n", path.c_str()); return; }
    for (const std::string& l : lines) { std::fwrite(l.data(), 1, l.size(), f); std::fputc('\n', f); }
    std::fclose(f);
}

// Format the SDL modifier state + keycode the way config.c's ParseKeyArray
// reads it back ("Ctrl+Shift+F1"). Keycode names round-trip through
// SDL_GetKeyFromName. Returns "" for a bare modifier press (keep scanning).
std::string hotkey_capture_string(const SDL_Keysym& ks) {
    switch (ks.sym) {
        case SDLK_LSHIFT: case SDLK_RSHIFT:
        case SDLK_LCTRL:  case SDLK_RCTRL:
        case SDLK_LALT:   case SDLK_RALT:
        case SDLK_LGUI:   case SDLK_RGUI:
            return "";
        default: break;
    }
    std::string s;
    if (ks.mod & KMOD_CTRL)  s += "Ctrl+";
    if (ks.mod & KMOD_ALT)   s += "Alt+";
    if (ks.mod & KMOD_SHIFT) s += "Shift+";
    const char* name = SDL_GetKeyName(ks.sym);
    if (!name || !name[0]) return "";
    s += name;
    return s;
}

// ----------------------------------------------------------------------------
// View model — every variable bound to the RML data model.
// ----------------------------------------------------------------------------

struct Model {
    Rml::String view = "dashboard";

    Rml::String game_name, game_region;
    bool has_boxart = false;            // a boxart.tga is bundled beside launcher.rml
    bool widescreen_supported = true;   // hide the Widescreen settings panel when false

    bool rom_loaded = false;
    Rml::String rom_file, rom_size, rom_header, rom_crc, rom_sha;
    bool crc_match = false, sha_match = false;

    bool msu1_supported = false;
    bool msu1_patch_available = false;
    bool msu1_enabled = false;          // streamed audio toggle (default OFF)
    Rml::String msu1_note;              // which patch a pack must match (per-game)
    Rml::String msu1_dir;
    bool msu1_pack_found = false;

    // Names of the SDL game controllers actually plugged in, index 0..N. Used to
    // label the dropdowns with the real device ("DualShock 4 Controller") instead
    // of a generic "Gamepad N".
    std::vector<std::string> pad_names;

    Rml::String p1_src_label = "Keyboard", p2_src_label = "Gamepad";
    Rml::String p1_status = "Enabled", p2_status = "Enabled";
    bool p1_enabled = true, p2_enabled = true;
    // Real <select> dropdowns: per-player option lists + the selected token.
    std::vector<SrcOption> p1_options, p2_options;
    Rml::String p1_src_value = "kbd", p2_src_value = "none";

    // SAVES panel is hidden entirely for games with no battery SRAM
    // (gi.sram_path == NULL, e.g. MMX — a password game).
    bool saves_supported = false;
    bool save_found = false;
    Rml::String save_file = "(none)", save_size = "0 KB";

    // Boot straight to the game next time (dashboard toggle, issue #5).
    bool skip_launcher = false;
    bool show_skip_modal = false;   // confirm dialog shown while enabling skip

    // settings
    Rml::String renderer_label, scale_label, fullscreen_label, freq_label;
    bool aspect = false, filter = false, widescreen = false, widescreen_hud = true;
    bool audio_enabled = true;
    int  volume = 100;

    // controller config
    int cfg_player = 0;
    Rml::String cfg_player_label = "1", cfg_src_label = "Keyboard (SDL2)";
    int cfg_deadzone = 30;

    bool status_ready = false;
};

// Enumerate the connected SDL controllers, returning their human names in
// connection order. Prefers the GameController name, falls back to the raw
// joystick name. SDL's gamecontroller subsystem must already be initialised.
std::vector<std::string> enumerate_pads() {
    std::vector<std::string> names;
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        const char* nm = SDL_IsGameController(i) ? SDL_GameControllerNameForIndex(i)
                                                 : SDL_JoystickNameForIndex(i);
        names.emplace_back(nm && *nm ? nm : "Controller");
    }
    return names;
}

// Build a player's dropdown options: None, Keyboard, and that player's connected
// controller (port p -> pad p) if present. value is the stable token the change
// handler decodes back into an InputSource.
void build_src_options(std::vector<SrcOption>& opts, int player,
                       const std::vector<std::string>& pads) {
    opts.clear();
    opts.push_back({ "none", "None" });
    opts.push_back({ "kbd",  "Keyboard" });
    if (player >= 0 && player < (int)pads.size())
        opts.push_back({ "pad", pads[player] });
}

const char* src_to_value(InputSource s) {
    return s == InputSource::Keyboard ? "kbd"
         : s == InputSource::Gamepad  ? "pad"
         : "none";
}
InputSource value_to_src(const Rml::String& v) {
    if (v == "kbd") return InputSource::Keyboard;
    if (v == "pad") return InputSource::Gamepad;
    return InputSource::None;
}

std::string src_label(InputSource s, int player, const std::vector<std::string>& pads) {
    switch (s) {
        case InputSource::None:     return "None";
        case InputSource::Keyboard: return "Keyboard";
        case InputSource::Gamepad:
            if (player >= 0 && player < (int)pads.size() && !pads[player].empty())
                return pads[player];
            return player == 0 ? "Gamepad 1 (not connected)"
                               : "Gamepad 2 (not connected)";
    }
    return "None";
}

void refresh_settings_labels(Model& m, const SnesLauncherSettings& s) {
    const char* rends[] = { "SDL", "SDL (software)", "OpenGL" };
    m.renderer_label = rends[(s.output_method >= 0 && s.output_method < 3) ? s.output_method : 2];
    char b[32];
    std::snprintf(b, sizeof(b), "%dx", s.window_scale < 1 ? 1 : s.window_scale);
    m.scale_label = b;
    const char* fs[] = { "Off", "Borderless", "Exclusive" };
    m.fullscreen_label = fs[(s.fullscreen >= 0 && s.fullscreen < 3) ? s.fullscreen : 0];
    std::snprintf(b, sizeof(b), "%d Hz", s.audio_freq);
    m.freq_label = b;
    m.aspect = s.ignore_aspect;
    m.filter = s.linear_filter;
    m.widescreen = s.widescreen;
    m.widescreen_hud = s.widescreen_hud;
    m.audio_enabled = s.enable_audio;
    m.volume = s.volume;
    m.p1_enabled = s.player_src[0] != InputSource::None;
    m.p2_enabled = s.player_src[1] != InputSource::None;
    m.p1_src_label = src_label(s.player_src[0], 0, m.pad_names);
    m.p2_src_label = src_label(s.player_src[1], 1, m.pad_names);
    m.p1_src_value = src_to_value(s.player_src[0]);
    m.p2_src_value = src_to_value(s.player_src[1]);
    m.p1_status = m.p1_enabled ? "Enabled" : "Disabled";
    m.p2_status = m.p2_enabled ? "Enabled" : "Disabled";
    m.msu1_enabled = s.msu1_enabled;
    m.msu1_dir = s.msu1_dir[0] ? s.msu1_dir : "(not set)";
    m.msu1_pack_found = msu_pack_present(s.msu1_dir);
}

// Compute and display ROM verification info for `path`.
void load_rom_info(Model& m, const GameInfo& g, const std::string& path) {
    m.rom_loaded = false;
    m.crc_match = m.sha_match = false;
    if (path.empty()) { m.rom_file = "(none)"; m.status_ready = false; return; }

    std::vector<uint8_t> data = read_file(path);
    if (data.empty()) { m.rom_file = "(unreadable)"; m.status_ready = false; return; }

    size_t hdr = smc_header(data.size());
    const uint8_t* body = data.data() + hdr;
    size_t blen = data.size() - hdr;

    uint32_t crc = crc32_compute(body, blen);
    uint8_t sha[32];
    sha256_compute(body, blen, sha);

    m.rom_file = basename_of(path);
    m.rom_size = human_size((long)data.size());
    m.rom_header = detect_mapping(body, blen);
    m.rom_crc = hex32(crc);
    m.rom_sha = sha_short(sha);
    m.crc_match = g.has_expected_crc && crc == g.expected_crc;
    for (size_t k = 0; k < g.num_known_sha256; k++)
        if (std::memcmp(sha, g.known_sha256[k], 32) == 0) { m.sha_match = true; break; }
    m.rom_loaded = true;
    m.status_ready = true;
}

// Populate the SAVES panel from the game's exe-anchored SRAM path. Shows the
// bare filename (the directory is always <exe>/saves) and whether it exists yet.
void refresh_save_info(Model& m, const std::string& sram_path) {
    if (sram_path.empty()) {
        m.save_found = false; m.save_file = "(none)"; m.save_size = "0 KB";
        return;
    }
    m.save_file = basename_of(sram_path);
    std::error_code ec;
    if (fs::exists(sram_path, ec) && !ec) {
        m.save_found = true;
        m.save_size = human_size((long)fs::file_size(sram_path, ec));
    } else {
        m.save_found = false;
        m.save_size = "0 KB";
    }
}

bool load_fonts(const fs::path& assets) {
    bool any = false;
    const char* faces[] = { "fonts/LatoLatin-Regular.ttf", "fonts/LatoLatin-Bold.ttf" };
    for (const char* f : faces) {
        fs::path p = assets / f;
        if (fs::exists(p) && Rml::LoadFontFace(p.generic_string())) any = true;
    }
#ifdef _WIN32
    if (!any) {
        if (Rml::LoadFontFace("C:/Windows/Fonts/segoeui.ttf")) any = true;
    }
    // Register a symbol fallback so glyphs missing from Lato (the check mark,
    // chevron, play triangle, note, warning sign, minus, etc.) render instead of
    // tofu boxes. fallback_face=true means it's only consulted for codepoints the
    // primary faces lack.
    Rml::LoadFontFace("C:/Windows/Fonts/seguisym.ttf", /*fallback_face=*/true);
#endif
    return any;
}

} // namespace

// ----------------------------------------------------------------------------
// run()
// ----------------------------------------------------------------------------

Result run(SDL_Window* window, void* /*gl_context*/,
           SnesLauncherSettings& io, const GameInfo& game,
           const char* assets_dir, const char* initial_rom,
           char* out_rom_path, size_t out_rom_path_len) {

    if (out_rom_path && out_rom_path_len) out_rom_path[0] = '\0';

    Rml::String gl_msg;
    if (!RmlGL3::Initialize(&gl_msg)) {
        std::fprintf(stderr, "launcher: RmlGL3::Initialize failed: %s\n", gl_msg.c_str());
        return Result::Unavailable;
    }

    SystemInterface_SDL system_interface;
    system_interface.SetWindow(window);
    RenderInterface_GL3 render_interface;

    Rml::SetSystemInterface(&system_interface);
    Rml::SetRenderInterface(&render_interface);
    if (!Rml::Initialise()) {
        std::fprintf(stderr, "launcher: Rml::Initialise failed\n");
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    const fs::path assets = assets_dir ? fs::path(assets_dir) : fs::current_path();
    if (!load_fonts(assets))
        std::fprintf(stderr, "launcher: warning — no font face loaded; text will not render\n");

    int win_w = 0, win_h = 0;
    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) { win_w = 1280; win_h = 960; }
    render_interface.SetViewport(win_w, win_h);

    Rml::Context* context = Rml::CreateContext("launcher", Rml::Vector2i(win_w, win_h));
    if (!context) {
        std::fprintf(stderr, "launcher: CreateContext failed\n");
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    // ---- model ----
    Model m;
    m.game_name = game.name ? game.name : "SNES Game";
    m.game_region = game.region ? game.region : "";
    m.msu1_supported = game.msu1_supported;
    m.widescreen_supported = game.widescreen_supported;
    m.msu1_note = game.msu1_note ? game.msu1_note : "";
    // Need the gamecontroller subsystem to read real device names for the
    // controller dropdowns (the shim only guarantees VIDEO is up).
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    m.pad_names = enumerate_pads();
    // Open connected controllers so their button/axis events reach the loop —
    // required to navigate the launcher with a gamepad (issue: controller nav).
    std::vector<SDL_GameController*> open_pads;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i))
            if (SDL_GameController* gc = SDL_GameControllerOpen(i))
                open_pads.push_back(gc);
    // Keyboard is always active (mapped in keybinds.ini), so a "Gamepad" source
    // only makes sense when a controller is actually plugged in. If a seeded
    // Gamepad source has no device at its index (e.g. the games default
    // EnableGamepad=true but nothing is connected), fall back to Keyboard (P1) /
    // None (P2) instead of showing a phantom "Gamepad N (not connected)". A real
    // connected pad keeps Gamepad and shows the device name.
    for (int p = 0; p < 2; p++) {
        if (io.player_src[p] == InputSource::Gamepad && p >= (int)m.pad_names.size())
            io.player_src[p] = (p == 0) ? InputSource::Keyboard : InputSource::None;
    }
    build_src_options(m.p1_options, 0, m.pad_names);
    build_src_options(m.p2_options, 1, m.pad_names);
    // A game ships boxart by dropping boxart.tga next to launcher.rml; shown when present.
    m.has_boxart = fs::exists(assets / "boxart.tga");

    // Default the MSU-1 pack folder to "<exe dir>/msu" (cwd is anchored to the
    // exe dir by main). Created so the user never has to point it anywhere; they
    // can still browse elsewhere. Only for games that support MSU-1 — others
    // (e.g. MMX) hide the whole block and get no stray msu/ folder.
    if (game.msu1_supported && io.msu1_dir[0] == '\0') {
        std::error_code ec;
        fs::path def = fs::current_path(ec) / "msu";
        fs::create_directories(def, ec);
        std::snprintf(io.msu1_dir, sizeof(io.msu1_dir), "%s", def.string().c_str());
    }
    refresh_settings_labels(m, io);

    std::string rom_path = initial_rom ? initial_rom : "";
    std::string vanilla_rom = rom_path;   // pre-patch source (for MSU patching)
    load_rom_info(m, game, rom_path);
    m.msu1_patch_available = game.msu1_supported && game.msu1_patch_path &&
                             m.rom_loaded && m.crc_match;

    // SAVES panel + skip-launcher toggle (issues #3a / #5).
    std::string sram_path = game.sram_path ? game.sram_path : "";
    m.saves_supported = !sram_path.empty();
    refresh_save_info(m, sram_path);
    m.skip_launcher = io.skip_launcher;

    Rml::DataModelConstructor c = context->CreateDataModel("launcher");
    if (!c) {
        std::fprintf(stderr, "launcher: CreateDataModel returned an invalid constructor\n");
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }
    // Register the controller-dropdown option struct + array for data-for.
    if (auto sh = c.RegisterStruct<SrcOption>()) {
        sh.RegisterMember("value", &SrcOption::value);
        sh.RegisterMember("label", &SrcOption::label);
    }
    c.RegisterArray<std::vector<SrcOption>>();

    c.Bind("view", &m.view);
    c.Bind("game_name", &m.game_name);
    c.Bind("game_region", &m.game_region);
    c.Bind("has_boxart", &m.has_boxart);
    c.Bind("widescreen_supported", &m.widescreen_supported);
    c.Bind("rom_loaded", &m.rom_loaded);
    c.Bind("rom_file", &m.rom_file);
    c.Bind("rom_size", &m.rom_size);
    c.Bind("rom_header", &m.rom_header);
    c.Bind("rom_crc", &m.rom_crc);
    c.Bind("rom_sha", &m.rom_sha);
    c.Bind("crc_match", &m.crc_match);
    c.Bind("sha_match", &m.sha_match);
    c.Bind("msu1_supported", &m.msu1_supported);
    c.Bind("msu1_patch_available", &m.msu1_patch_available);
    c.Bind("msu1_enabled", &m.msu1_enabled);
    c.Bind("msu1_note", &m.msu1_note);
    c.Bind("msu1_dir", &m.msu1_dir);
    c.Bind("msu1_pack_found", &m.msu1_pack_found);
    c.Bind("p1_options", &m.p1_options);
    c.Bind("p2_options", &m.p2_options);
    c.Bind("p1_src_value", &m.p1_src_value);
    c.Bind("p2_src_value", &m.p2_src_value);
    c.Bind("p1_src_label", &m.p1_src_label);
    c.Bind("p2_src_label", &m.p2_src_label);
    c.Bind("p1_status", &m.p1_status);
    c.Bind("p2_status", &m.p2_status);
    c.Bind("p1_enabled", &m.p1_enabled);
    c.Bind("p2_enabled", &m.p2_enabled);
    c.Bind("saves_supported", &m.saves_supported);
    c.Bind("save_found", &m.save_found);
    c.Bind("save_file", &m.save_file);
    c.Bind("save_size", &m.save_size);
    c.Bind("skip_launcher", &m.skip_launcher);
    c.Bind("show_skip_modal", &m.show_skip_modal);
    c.Bind("renderer_label", &m.renderer_label);
    c.Bind("scale_label", &m.scale_label);
    c.Bind("fullscreen_label", &m.fullscreen_label);
    c.Bind("freq_label", &m.freq_label);
    c.Bind("aspect", &m.aspect);
    c.Bind("filter", &m.filter);
    c.Bind("widescreen", &m.widescreen);
    c.Bind("widescreen_hud", &m.widescreen_hud);
    c.Bind("audio_enabled", &m.audio_enabled);
    c.Bind("volume", &m.volume);
    c.Bind("cfg_player_label", &m.cfg_player_label);
    c.Bind("cfg_src_label", &m.cfg_src_label);
    c.Bind("cfg_deadzone", &m.cfg_deadzone);
    c.Bind("status_ready", &m.status_ready);

    Rml::DataModelHandle handle = c.GetModelHandle();

    Result result = Result::Quit;
    bool running = true;

    auto dirty_all = [&]() { handle.DirtyAllVariables(); };

    // ---- keybind rebinding (player buttons + hotkeys) ----
    // Player buttons edit keybinds.ini through the engine's keybinds module
    // (scancodes; saved immediately on capture — the game reloads the file via
    // keybinds_init after the launcher returns). Hotkeys surgically edit
    // config.ini's [KeyMap] (keycode names; the game re-applies via
    // ConfigReloadKeyMap after the launcher returns).
    keybinds_init(NULL);   // cwd is exe-anchored; load current bindings now
    const std::string config_path =
        (game.config_path && game.config_path[0]) ? game.config_path : "config.ini";
    std::vector<std::string> hotkey_vals = hotkeys_read(config_path);

    enum class ScanKind { None, PlayerKey, Hotkey };
    ScanKind    scan_kind = ScanKind::None;
    int         scan_index = 0;          // button index (PlayerKey) or hotkey index
    std::string scan_chip_id;

    Rml::ElementDocument* doc = nullptr;             // assigned after LoadDocument
    std::function<void()> build_player_list;         // assigned after doc loads
    std::function<void()> build_hotkey_list;
    // Chip click handlers must never rebuild the list they live in from inside
    // their own dispatch (SetInnerRML would destroy the running listener), so
    // rebuilds are deferred to the main loop via these flags.
    bool rebuild_players_pending = false, rebuild_hotkeys_pending = false;

    auto player_chip_label = [&](int button) -> std::string {
        SDL_Scancode sc = keybinds_get_button(m.cfg_player + 1, button);
        const char* n = (sc != SDL_SCANCODE_UNKNOWN) ? SDL_GetScancodeName(sc) : "";
        return (n && n[0]) ? std::string(n) : std::string("None");
    };
    auto hotkey_chip_label = [&](int h) -> std::string {
        return hotkey_vals[h].empty() ? std::string("None") : hotkey_vals[h];
    };

    // Restore the armed chip's label and disarm. Safe to call when idle.
    auto end_scan = [&]() {
        if (scan_kind == ScanKind::None) return;
        if (doc) {
            if (Rml::Element* e = doc->GetElementById(scan_chip_id)) {
                e->SetInnerRML(scan_kind == ScanKind::PlayerKey
                                   ? player_chip_label(scan_index)
                                   : hotkey_chip_label(scan_index));
                e->SetClass("rb-chip--scan", false);
            }
        }
        scan_kind = ScanKind::None;
        scan_chip_id.clear();
    };

    auto begin_scan = [&](ScanKind kind, int index, const std::string& chip_id) {
        end_scan();   // one scan at a time
        scan_kind = kind;
        scan_index = index;
        scan_chip_id = chip_id;
        if (doc) {
            if (Rml::Element* e = doc->GetElementById(chip_id)) {
                e->SetInnerRML("Press a key...");
                e->SetClass("rb-chip--scan", true);
            }
        }
    };

    // Resolve an armed scan against a keydown (called from the SDL loop, which
    // swallows keyboard events while scanning; Esc cancels).
    auto handle_scan_key = [&](const SDL_KeyboardEvent& ke) {
        if (ke.keysym.sym == SDLK_ESCAPE) { end_scan(); return; }
        if (scan_kind == ScanKind::PlayerKey) {
            SDL_Scancode sc = ke.keysym.scancode;
            // Steal: a key already bound to another button (either player)
            // moves here instead of silently double-firing.
            for (int p = 1; p <= 2; p++)
                for (int b = 0; b < keybinds_button_count(); b++)
                    if (keybinds_get_button(p, b) == sc &&
                        !(p == m.cfg_player + 1 && b == scan_index))
                        keybinds_set_button(p, b, SDL_SCANCODE_UNKNOWN);
            keybinds_set_button(m.cfg_player + 1, scan_index, sc);
            keybinds_save();
            end_scan();
            rebuild_players_pending = true;   // stolen chips refresh too
        } else if (scan_kind == ScanKind::Hotkey) {
            std::string s = hotkey_capture_string(ke.keysym);
            if (s.empty()) return;   // bare modifier — keep scanning
            for (int h = 0; h < kHotkeyCount; h++)   // steal duplicates
                if (h != scan_index && ieq(hotkey_vals[h], s.c_str())) {
                    hotkey_vals[h].clear();
                    hotkeys_write(config_path, kHotkeys[h].key, "");
                }
            hotkey_vals[scan_index] = s;
            hotkeys_write(config_path, kHotkeys[scan_index].key, s);
            end_scan();
            rebuild_hotkeys_pending = true;
        }
    };

    // ---- navigation ----
    c.BindEventCallback("show_dashboard", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        end_scan();
        m.view = "dashboard"; dirty_all();
    });
    c.BindEventCallback("show_settings", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        end_scan();
        m.view = "settings"; dirty_all();
    });
    c.BindEventCallback("msu1_settings", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        end_scan();
        m.view = "settings"; dirty_all();
    });
    auto open_cfg = [&](int p) {
        end_scan();
        m.cfg_player = p;
        m.cfg_player_label = p == 0 ? "1" : "2";
        m.cfg_src_label = src_label(io.player_src[p], p, m.pad_names);
        m.cfg_deadzone = io.deadzone[p];
        rebuild_players_pending = true;   // chips show this player's bindings
        m.view = "controller"; dirty_all();
    };
    c.BindEventCallback("config_p1", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { open_cfg(0); });
    c.BindEventCallback("config_p2", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { open_cfg(1); });
    // Reset the shown player's keyboard bindings to the built-in defaults
    // (Player 2's default is all-unbound) and persist.
    c.BindEventCallback("rebind_reset", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        end_scan();
        keybinds_reset_player(m.cfg_player + 1);
        keybinds_save();
        rebuild_players_pending = true;
    });

    // ---- ROM ----
    c.BindEventCallback("change_rom", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        char buf[1024];
        if (pick_file("Select SNES ROM", "SNES ROMs (*.sfc;*.smc)\0*.sfc;*.smc\0All Files (*.*)\0*.*\0", buf, sizeof(buf))) {
            rom_path = buf;
            vanilla_rom = buf;
            load_rom_info(m, game, rom_path);
            m.msu1_patch_available = game.msu1_supported && game.msu1_patch_path && m.rom_loaded && m.crc_match;
            dirty_all();
        }
    });
    // ---- MSU-1 ----
    auto do_patch = [&]() {
        if (vanilla_rom.empty() || !game.msu1_patch_path) return;
        std::vector<uint8_t> src = read_file(vanilla_rom);
        std::vector<uint8_t> pat = read_file(game.msu1_patch_path);
        std::vector<uint8_t> out = ips_apply(src, pat);
        if (out.empty()) { std::fprintf(stderr, "launcher: IPS patch failed\n"); return; }
        fs::path vp(vanilla_rom);
        fs::path target = vp.parent_path() / (vp.stem().string() + ".msu1" + vp.extension().string());
        FILE* f = std::fopen(target.string().c_str(), "wb");
        if (!f) { std::fprintf(stderr, "launcher: cannot write %s\n", target.string().c_str()); return; }
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        rom_path = target.string();
        load_rom_info(m, game, rom_path);
        m.msu1_patch_available = false;  // now patched
        dirty_all();
        std::fprintf(stderr, "launcher: wrote MSU-1 patched ROM: %s\n", rom_path.c_str());
    };
    c.BindEventCallback("patch_rom", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { do_patch(); });
    c.BindEventCallback("skip_patch", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m.msu1_patch_available = false; dirty_all();
    });
    c.BindEventCallback("toggle_msu1", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.msu1_enabled = !io.msu1_enabled; m.msu1_enabled = io.msu1_enabled; dirty_all();
    });
    c.BindEventCallback("msu1_browse", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (!io.msu1_enabled) return;   // control is dimmed until MSU-1 is enabled
        char buf[1024];
        if (pick_folder(buf, sizeof(buf))) {
            std::snprintf(io.msu1_dir, sizeof(io.msu1_dir), "%s", buf);
            m.msu1_dir = io.msu1_dir[0] ? io.msu1_dir : "(not set)";
            m.msu1_pack_found = msu_pack_present(io.msu1_dir);
            dirty_all();
        }
    });
    c.BindEventCallback("msu1_open", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (!io.msu1_enabled) return;   // control is dimmed until MSU-1 is enabled
        if (io.msu1_dir[0]) open_in_explorer(io.msu1_dir);
    });

    // ---- saves (import/clear the game's SRAM .srm) ----
    // Import: pick a .srm/.sav, back up any existing save to <name>.srm.bak, then
    // copy the chosen file into place (issue #3a). Clear: back up, then delete.
    c.BindEventCallback("save_import", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (sram_path.empty()) return;
        char buf[1024];
        if (!pick_file("Import SRAM Save",
                       "SNES saves (*.srm;*.sav)\0*.srm;*.sav\0All Files (*.*)\0*.*\0",
                       buf, sizeof(buf)))
            return;
        std::error_code ec;
        fs::path dst(sram_path);
        fs::create_directories(dst.parent_path(), ec);
        ec.clear();
        if (fs::exists(dst, ec))
            fs::copy_file(dst, fs::path(sram_path + ".bak"),
                          fs::copy_options::overwrite_existing, ec);
        ec.clear();
        fs::copy_file(fs::path(buf), dst, fs::copy_options::overwrite_existing, ec);
        if (ec) std::fprintf(stderr, "launcher: import save failed: %s\n", ec.message().c_str());
        refresh_save_info(m, sram_path);
        dirty_all();
    });
    c.BindEventCallback("save_clear", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (sram_path.empty()) return;
        std::error_code ec;
        fs::path dst(sram_path);
        if (fs::exists(dst, ec)) {
            fs::copy_file(dst, fs::path(sram_path + ".bak"),
                          fs::copy_options::overwrite_existing, ec);
            ec.clear();
            fs::remove(dst, ec);
            if (ec) std::fprintf(stderr, "launcher: clear save failed: %s\n", ec.message().c_str());
        }
        refresh_save_info(m, sram_path);
        dirty_all();
    });
    // ---- skip launcher (boot straight to the game on boot, issue #5) ----
    // Enabling pops a confirm modal (it changes how the user reaches the
    // launcher); disabling is harmless and takes effect immediately.
    c.BindEventCallback("toggle_skip_launcher", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (io.skip_launcher) {
            io.skip_launcher = false; m.skip_launcher = false;
        } else {
            m.show_skip_modal = true;   // ask before turning it on
        }
        dirty_all();
    });
    c.BindEventCallback("skip_modal_confirm", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.skip_launcher = true; m.skip_launcher = true; m.show_skip_modal = false; dirty_all();
    });
    c.BindEventCallback("skip_modal_cancel", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m.show_skip_modal = false; dirty_all();   // leave skip_launcher off
    });

    // ---- display settings ----
    c.BindEventCallback("cycle_renderer", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.output_method = (io.output_method + 1) % 3; refresh_settings_labels(m, io); dirty_all();
    });
    c.BindEventCallback("cycle_scale", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.window_scale = io.window_scale >= 6 ? 1 : io.window_scale + 1; refresh_settings_labels(m, io); dirty_all();
    });
    c.BindEventCallback("cycle_fullscreen", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.fullscreen = (io.fullscreen + 1) % 3; refresh_settings_labels(m, io); dirty_all();
    });
    c.BindEventCallback("toggle_aspect", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.ignore_aspect = !io.ignore_aspect; m.aspect = io.ignore_aspect; dirty_all();
    });
    c.BindEventCallback("toggle_filter", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.linear_filter = !io.linear_filter; m.filter = io.linear_filter; dirty_all();
    });

    // ---- widescreen ----
    c.BindEventCallback("toggle_widescreen", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.widescreen = !io.widescreen; m.widescreen = io.widescreen; dirty_all();
    });
    c.BindEventCallback("toggle_widescreen_hud", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.widescreen_hud = !io.widescreen_hud; m.widescreen_hud = io.widescreen_hud; dirty_all();
    });

    // ---- audio (always on; MSU-1 is the mode toggle, not an on/off) ----
    c.BindEventCallback("cycle_freq", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        // 32040 = native S-DSP rate and the config default; it MUST be in the
        // cycle or stepping away from it makes the default unreachable (issue #3).
        int rates[] = { 32040, 32000, 44100, 48000 };
        int n = (int)(sizeof(rates) / sizeof(rates[0]));
        int idx = 0;
        for (int i = 0; i < n; i++) if (rates[i] == io.audio_freq) idx = i;
        io.audio_freq = rates[(idx + 1) % n]; refresh_settings_labels(m, io); dirty_all();
    });
    c.BindEventCallback("vol_up", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.volume = io.volume >= 100 ? 100 : io.volume + 5; m.volume = io.volume; dirty_all();
    });
    c.BindEventCallback("vol_down", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.volume = io.volume <= 0 ? 0 : io.volume - 5; m.volume = io.volume; dirty_all();
    });

    // ---- controller source dropdowns (<select>) + config ----
    // The <select>'s data-value binding keeps m.pN_src_value current; the change
    // handler reads it and updates the live InputSource + status dot.
    auto src_changed = [&](int p) {
        io.player_src[p] = value_to_src(p == 0 ? m.p1_src_value : m.p2_src_value);
        refresh_settings_labels(m, io); dirty_all();
    };
    c.BindEventCallback("p1_src_changed", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { src_changed(0); });
    c.BindEventCallback("p2_src_changed", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { src_changed(1); });
    c.BindEventCallback("cfg_cycle_src", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        int p = m.cfg_player;
        bool has_pad = p < (int)m.pad_names.size();
        int v = (int)io.player_src[p];
        do { v = (v + 1) % 3; }
        while (v == (int)InputSource::Gamepad && !has_pad);
        io.player_src[p] = (InputSource)v;
        m.cfg_src_label = src_label(io.player_src[p], p, m.pad_names);
        refresh_settings_labels(m, io); dirty_all();
    });
    c.BindEventCallback("cfg_dz_up", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        int p = m.cfg_player; io.deadzone[p] = io.deadzone[p] >= 100 ? 100 : io.deadzone[p] + 5;
        m.cfg_deadzone = io.deadzone[p]; dirty_all();
    });
    c.BindEventCallback("cfg_dz_down", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        int p = m.cfg_player; io.deadzone[p] = io.deadzone[p] <= 0 ? 0 : io.deadzone[p] - 5;
        m.cfg_deadzone = io.deadzone[p]; dirty_all();
    });

    // ---- play / quit ----
    c.BindEventCallback("play", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (rom_path.empty()) {
            char buf[1024];
            if (pick_file("Select SNES ROM", "SNES ROMs (*.sfc;*.smc)\0*.sfc;*.smc\0All Files (*.*)\0*.*\0", buf, sizeof(buf))) {
                rom_path = buf; vanilla_rom = buf; load_rom_info(m, game, rom_path);
            } else { return; }
        }
        if (out_rom_path && out_rom_path_len)
            std::snprintf(out_rom_path, out_rom_path_len, "%s", rom_path.c_str());
        result = Result::Launch;
        running = false;
    });

    doc = context->LoadDocument((assets / "launcher.rml").generic_string());
    if (!doc) {
        std::fprintf(stderr, "launcher: failed to load launcher.rml — booting without launcher\n");
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }
    doc->Show();

    // ---- rebind chip lists (built programmatically, PSR-style) ----
    // data-if only hides views, so the list containers exist from load; chips
    // are (re)generated with SetInnerRML and re-wired each rebuild.
    struct ClickListener : Rml::EventListener {
        std::function<void()> on_click;
        void ProcessEvent(Rml::Event&) override { if (on_click) on_click(); }
    };
    std::vector<std::unique_ptr<ClickListener>> player_chip_listeners;
    std::vector<std::unique_ptr<ClickListener>> hotkey_chip_listeners;

    build_player_list = [&]() {
        Rml::Element* list = doc->GetElementById("rebind-list");
        if (!list) return;
        const int n = keybinds_button_count();
        std::string rml;
        for (int b = 0; b < n; b += 2) {   // two (label, chip) pairs per row
            rml += "<div class=\"rb-row\">";
            for (int k = b; k < b + 2 && k < n; k++) {
                std::string pretty = keybinds_button_name(k);
                if (!pretty.empty()) pretty[0] = (char)toupper((unsigned char)pretty[0]);
                rml += "<span class=\"rb-label\">" + pretty + "</span>";
                rml += "<button class=\"rb-chip\" id=\"kb-";
                rml += keybinds_button_name(k);
                rml += "\">" + player_chip_label(k) + "</button>";
            }
            rml += "</div>";
        }
        list->SetInnerRML(rml);   // destroys any previous chips first...
        player_chip_listeners.clear();   // ...so dropping their listeners is safe
        for (int k = 0; k < n; k++) {
            const std::string id = std::string("kb-") + keybinds_button_name(k);
            if (Rml::Element* e = doc->GetElementById(id)) {
                auto lis = std::make_unique<ClickListener>();
                lis->on_click = [&, k, id]() { begin_scan(ScanKind::PlayerKey, k, id); };
                e->AddEventListener(Rml::EventId::Click, lis.get());
                player_chip_listeners.push_back(std::move(lis));
            }
        }
    };

    build_hotkey_list = [&]() {
        Rml::Element* list = doc->GetElementById("hotkey-list");
        if (!list) return;
        std::string rml;
        for (int h = 0; h < kHotkeyCount; h++) {
            rml += "<div class=\"rb-row\">";
            rml += "<span class=\"rb-label rb-label--wide\">";
            rml += kHotkeys[h].label;
            rml += "</span>";
            rml += "<button class=\"rb-chip\" id=\"hk-";
            rml += kHotkeys[h].key;
            rml += "\">" + hotkey_chip_label(h) + "</button>";
            rml += "<button class=\"rb-x\" id=\"hkx-";
            rml += kHotkeys[h].key;
            rml += "\">&#10005;</button>";
            rml += "</div>";
        }
        list->SetInnerRML(rml);
        hotkey_chip_listeners.clear();
        for (int h = 0; h < kHotkeyCount; h++) {
            const std::string cid = std::string("hk-") + kHotkeys[h].key;
            const std::string xid = std::string("hkx-") + kHotkeys[h].key;
            if (Rml::Element* e = doc->GetElementById(cid)) {
                auto lis = std::make_unique<ClickListener>();
                lis->on_click = [&, h, cid]() { begin_scan(ScanKind::Hotkey, h, cid); };
                e->AddEventListener(Rml::EventId::Click, lis.get());
                hotkey_chip_listeners.push_back(std::move(lis));
            }
            if (Rml::Element* e = doc->GetElementById(xid)) {
                auto lis = std::make_unique<ClickListener>();
                lis->on_click = [&, h]() {
                    end_scan();
                    hotkey_vals[h].clear();
                    hotkeys_write(config_path, kHotkeys[h].key, "");
                    rebuild_hotkeys_pending = true;   // deferred: we're inside a chip's dispatch
                };
                e->AddEventListener(Rml::EventId::Click, lis.get());
                hotkey_chip_listeners.push_back(std::move(lis));
            }
        }
    };

    build_player_list();
    build_hotkey_list();

    // ---- populate the controller <select> dropdowns programmatically ----
    // RmlUi builds a select's options at parse time, so data-for can't generate
    // them; we add them by hand and listen for the change event. (None / Keyboard
    // / the connected controller, per build_src_options above.)
    struct SelListener : Rml::EventListener {
        std::function<void()> on_change;
        void ProcessEvent(Rml::Event&) override { if (on_change) on_change(); }
    };
    std::vector<std::unique_ptr<SelListener>> sel_listeners;
    auto setup_select = [&](const char* id, int p) {
        auto* sel = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(doc->GetElementById(id));
        if (!sel) return;
        sel->RemoveAll();
        const std::vector<SrcOption>& opts = (p == 0) ? m.p1_options : m.p2_options;
        Rml::String cur = src_to_value(io.player_src[p]);
        int selected = 0;
        for (int i = 0; i < (int)opts.size(); i++) {
            sel->Add(opts[i].label, opts[i].value);
            if (opts[i].value == cur) selected = i;
        }
        sel->SetSelection(selected);
        auto lis = std::make_unique<SelListener>();
        lis->on_change = [&, sel, p]() {
            io.player_src[p] = value_to_src(sel->GetValue());
            refresh_settings_labels(m, io);
            dirty_all();
        };
        sel->AddEventListener(Rml::EventId::Change, lis.get());
        sel_listeners.push_back(std::move(lis));
    };
    setup_select("p1src", 0);
    setup_select("p2src", 1);

    // Seed focus on PLAY so a gamepad/keyboard user always has a visible focus
    // ring and can confirm (A / Enter) or navigate (D-pad / Tab) from there.
    if (auto* pb = doc->GetElementById("play")) pb->Focus();

    // ---- gamepad navigation ----
    // RmlUi moves focus on Tab (Shift+Tab reverses) and emulates a click on the
    // focused control on Enter/Space. We translate the pad to those: D-pad /
    // left-stick = move focus, A = activate, B = back to dashboard, Start = PLAY.
    auto nav_back = [&]() { if (m.view != "dashboard") { m.view = "dashboard"; dirty_all(); } };
    auto pad_move = [&](int dir) {
        context->ProcessKeyDown(Rml::Input::KI_TAB,
                                dir < 0 ? (int)Rml::Input::KM_SHIFT : 0);
    };
    int pad_zone_x = 0, pad_zone_y = 0;   // edge-trigger state for the left stick
    auto handle_pad = [&](const SDL_Event& e) {
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: pad_move(+1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP:
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  pad_move(-1); break;
                case SDL_CONTROLLER_BUTTON_A:
                    context->ProcessKeyDown(Rml::Input::KI_RETURN, 0); break;
                case SDL_CONTROLLER_BUTTON_B: nav_back(); break;
                case SDL_CONTROLLER_BUTTON_START:
                    if (auto* pb = doc->GetElementById("play")) pb->Click(); break;
                default: break;
            }
        } else if (e.type == SDL_CONTROLLERAXISMOTION) {
            const int TH = 18000;   // ~55% deflection; edge-triggered, one move per push
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                int z = e.caxis.value > TH ? 1 : e.caxis.value < -TH ? -1 : 0;
                if (z != pad_zone_y) { pad_zone_y = z; if (z) pad_move(z); }
            } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                int z = e.caxis.value > TH ? 1 : e.caxis.value < -TH ? -1 : 0;
                if (z != pad_zone_x) { pad_zone_x = z; if (z) pad_move(z); }
            }
        }
    };

    // ---- main loop ----
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { result = Result::Quit; running = false; }
            else if (scan_kind != ScanKind::None &&
                     (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP ||
                      ev.type == SDL_TEXTINPUT ||
                      ev.type == SDL_CONTROLLERBUTTONDOWN ||
                      ev.type == SDL_CONTROLLERAXISMOTION)) {
                // A rebind scan is armed: swallow keyboard input (the next
                // keydown resolves it; Esc cancels) and park controller nav so
                // a pad press can't activate other controls mid-scan. Mouse
                // stays live (clicking another chip re-arms cleanly).
                if (ev.type == SDL_KEYDOWN) handle_scan_key(ev.key);
            }
            else if (ev.type == SDL_WINDOWEVENT &&
                     (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                      ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
                SDL_GL_GetDrawableSize(window, &win_w, &win_h);
                render_interface.SetViewport(win_w, win_h);
                context->SetDimensions(Rml::Vector2i(win_w, win_h));
                RmlSDL::InputEventHandler(context, ev);
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN ||
                       ev.type == SDL_CONTROLLERAXISMOTION) {
                handle_pad(ev);
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                if (SDL_GameController* gc = SDL_GameControllerOpen(ev.cdevice.which))
                    open_pads.push_back(gc);
            } else {
                RmlSDL::InputEventHandler(context, ev);
            }
        }

        // Deferred chip-list rebuilds (set from chip handlers / scan capture).
        if (rebuild_players_pending) { rebuild_players_pending = false; build_player_list(); }
        if (rebuild_hotkeys_pending) { rebuild_hotkeys_pending = false; build_hotkey_list(); }

        context->Update();

        render_interface.Clear();
        render_interface.BeginFrame();
        context->Render();
        render_interface.EndFrame();
        SDL_GL_SwapWindow(window);
        SDL_Delay(8);
    }

    for (SDL_GameController* gc : open_pads) SDL_GameControllerClose(gc);

    Rml::Shutdown();
    RmlGL3::Shutdown();
    return result;
}

} // namespace snes_launcher

// ----------------------------------------------------------------------------
// C entry point (launcher_capi.h) — owns the launcher window/GL context.
// ----------------------------------------------------------------------------

#include "launcher_capi.h"

extern "C" int snes_launcher_run_window(const char* window_title,
                                        SnesLauncherCSettings* io,
                                        const SnesLauncherCGameInfo* game,
                                        const char* assets_dir,
                                        const char* initial_rom,
                                        char* out_rom_path,
                                        size_t out_rom_path_len) {
    using namespace snes_launcher;
    if (!io || !game) return 2;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "launcher: SDL video init failed: %s\n", SDL_GetError());
        return 2;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* win = SDL_CreateWindow(
        window_title ? window_title : "Super Nintendo Launcher",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960,  // 4:3
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::fprintf(stderr, "launcher: window creation failed: %s\n", SDL_GetError());
        return 2;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        std::fprintf(stderr, "launcher: GL context creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        return 2;
    }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);

    // map C settings -> C++
    SnesLauncherSettings s;
    s.output_method = io->output_method;
    s.window_scale  = io->window_scale;
    s.fullscreen    = io->fullscreen;
    s.ignore_aspect = io->ignore_aspect != 0;
    s.linear_filter = io->linear_filter != 0;
    s.widescreen    = io->widescreen != 0;
    s.widescreen_hud= io->widescreen_hud != 0;
    s.enable_audio  = io->enable_audio != 0;
    s.audio_freq    = io->audio_freq;
    s.volume        = io->volume;
    s.player_src[0] = (InputSource)io->player_src[0];
    s.player_src[1] = (InputSource)io->player_src[1];
    s.deadzone[0]   = io->deadzone[0];
    s.deadzone[1]   = io->deadzone[1];
    s.skip_launcher = io->skip_launcher != 0;
    s.msu1_enabled  = io->msu1_enabled != 0;
    std::snprintf(s.msu1_dir, sizeof(s.msu1_dir), "%s", io->msu1_dir);

    GameInfo g;
    g.name = game->name;
    g.region = game->region;
    g.expected_crc = game->expected_crc;
    g.has_expected_crc = game->has_expected_crc != 0;
    g.known_sha256 = game->known_sha256;
    g.num_known_sha256 = game->num_known_sha256;
    g.widescreen_supported = game->widescreen_supported != 0;
    g.msu1_supported = game->msu1_supported != 0;
    g.msu1_note = game->msu1_note;
    g.msu1_patch_path = game->msu1_patch_path;
    g.sram_path = game->sram_path;
    g.config_path = game->config_path;

    Result r = run(win, ctx, s, g, assets_dir, initial_rom,
                   out_rom_path, out_rom_path_len);

    // map back
    io->output_method = s.output_method;
    io->window_scale  = s.window_scale;
    io->fullscreen    = s.fullscreen;
    io->ignore_aspect = s.ignore_aspect;
    io->linear_filter = s.linear_filter;
    io->widescreen    = s.widescreen;
    io->widescreen_hud= s.widescreen_hud;
    io->enable_audio  = s.enable_audio;
    io->audio_freq    = s.audio_freq;
    io->volume        = s.volume;
    io->player_src[0] = (int)s.player_src[0];
    io->player_src[1] = (int)s.player_src[1];
    io->deadzone[0]   = s.deadzone[0];
    io->deadzone[1]   = s.deadzone[1];
    io->skip_launcher = s.skip_launcher;
    io->msu1_enabled  = s.msu1_enabled;
    std::snprintf(io->msu1_dir, sizeof(io->msu1_dir), "%s", s.msu1_dir);

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_GL_ResetAttributes();

    return r == Result::Launch ? 0 : (r == Result::Quit ? 1 : 2);
}
