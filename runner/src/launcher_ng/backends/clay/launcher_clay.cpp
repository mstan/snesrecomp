// launcher_clay.cpp — Clay (zlib) backend for the next-gen launcher.
//
// Same shared LauncherModel / theme / platform as the ImGui backend, at MMX
// parity. Clay supplies the constraint layout; everything else here — widgets,
// dropdown, grids, hit-testing, scrolling glue — is hand-built, because Clay
// ships layout only. Rendering + text come from clay_renderer_gl.
//
// Clay works in PHYSICAL PIXELS: we feed it pixel dimensions and pixel font
// sizes (logical * display_scale), so text bakes crisply at the target DPI and
// layout reflows on live resize.

#include "launcher_backend.h"
#include "launcher_gl.h"
#include "launcher_input.h"
#include "launcher_files.h"
#include "launcher_debug.h"
#include "backends/clay/clay_renderer_gl.h"

#include "third_party/clay.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" const char* launcher_backend_name(void) { return "Clay"; }

namespace {

float g_scale = 1.0f;
float px(float logical) { return logical * g_scale; }
bool  g_pressed = false;                 // left-click edge this frame
const LauncherTheme* g_th = nullptr;
LauncherTexture g_boxart, g_pad, g_brand;

LauncherPad g_pads[LNG_MAX_PADS];
int         g_pad_count = 0;
int         g_open_dropdown = -1;        // player index whose dropdown is open

SDL_Window* g_window = nullptr;
char        g_pick_buf[512] = {};
bool        g_pick_done = false;

Clay_Color CC(const LngColor& c) {
    Clay_Color k; k.r = c.r*255; k.g = c.g*255; k.b = c.b*255; k.a = c.a*255; return k;
}
Clay_String CS(const char* s) {
    Clay_String r; r.isStaticallyAllocated = false;
    r.length = (int32_t)strlen(s); r.chars = s; return r;
}
// Per-frame string arena (Clay stores pointers into EndLayout/render).
char g_pool[16384]; int g_pool_off = 0;
void pool_reset() { g_pool_off = 0; }
Clay_String FS(const char* fmt, ...) {
    char* dst = g_pool + g_pool_off;
    int avail = (int)sizeof(g_pool) - g_pool_off;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(dst, avail, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= avail) { Clay_String e = {false,0,""}; return e; }
    g_pool_off += n + 1;
    Clay_String r; r.isStaticallyAllocated = false; r.length = n; r.chars = dst; return r;
}

uint16_t font_base(uint16_t fid) {
    return fid == LNG_FONT_TITLE ? 30 : (fid == LNG_FONT_SMALL ? 14 : 18);
}
void text_(Clay_String s, uint16_t fid, const LngColor& c) {
    CLAY_TEXT(s, CLAY_TEXT_CONFIG({
        .textColor = CC(c),
        .fontId    = fid,
        .fontSize  = (uint16_t)px((float)font_base(fid)),
    }));
}
Clay_BorderWidth bw1() { Clay_BorderWidth w{}; w.left=w.right=w.top=w.bottom=1; return w; }

// Flexible spacer that eats remaining space on the main axis (Clay's
// right-align idiom — there is no SameLine/right-align helper).
void spacer() {
    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } } }) {}
}

Clay_LayoutConfig row(uint16_t gap, Clay_LayoutAlignmentX ax = CLAY_ALIGN_X_LEFT,
                      Clay_LayoutAlignmentY ay = CLAY_ALIGN_Y_CENTER) {
    Clay_LayoutConfig l = {};
    l.layoutDirection = CLAY_LEFT_TO_RIGHT; l.childGap = gap;
    l.childAlignment.x = ax; l.childAlignment.y = ay;
    l.sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) };
    return l;
}

// ---- widgets (all hand-built: Clay has none) --------------------------------

bool button_(const char* id, Clay_String label, float min_w, bool accent = false) {
    const LauncherTheme& th = *g_th;
    bool clicked = false;
    CLAY({
        .id = Clay_GetElementId(CS(id)),
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(px(min_w)), CLAY_SIZING_FIXED(px(34)) },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = accent ? CC(th.accent)
                                  : (Clay_Hovered() ? CC(th.control_hovered) : CC(th.control)),
        .cornerRadius = CLAY_CORNER_RADIUS(px(th.radius_sm)),
        .border = { .color = CC(accent ? th.accent : th.border), .width = bw1() },
    }) {
        if (Clay_Hovered() && g_pressed) clicked = true;
        text_(label, LNG_FONT_BODY, accent ? th.accent_text : th.text);
    }
    return clicked;
}

void dot(bool on) {
    const LauncherTheme& th = *g_th;
    CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(px(9)), CLAY_SIZING_FIXED(px(9)) } },
           .backgroundColor = on ? CC(th.good) : CC(th.text_muted),
           .cornerRadius = CLAY_CORNER_RADIUS(px(4.5f)) }) {}
}

void image_(const LauncherTexture& t, float box_w, float box_h) {
    if (!t.id) {
        CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(px(box_w)), CLAY_SIZING_FIXED(px(box_h)) } } }) {}
        return;
    }
    float bw = px(box_w), bh = px(box_h);
    float s = (bw / t.w < bh / t.h) ? bw / (float)t.w : bh / (float)t.h;
    CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(t.w * s), CLAY_SIZING_FIXED(t.h * s) } },
           .image = { .imageData = (void*)(size_t)t.id } }) {}
}

// "label ....... value [BADGE]" — full-width row with right-aligned badge.
void kv(const char* k, const char* v, const char* badge = nullptr, bool good = true) {
    const LauncherTheme& th = *g_th;
    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                       .childGap = 0, .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER },
                       .layoutDirection = CLAY_LEFT_TO_RIGHT } }) {
        CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(px(84)), CLAY_SIZING_FIT(0) } } }) {
            text_(CS(k), LNG_FONT_SMALL, th.text_muted);
        }
        text_(CS(v), LNG_FONT_SMALL, th.text);
        if (badge) {
            spacer();
            text_(FS("[%s]", badge), LNG_FONT_SMALL, good ? th.good : th.warn);
        }
    }
}

void stepper(const char* id, int value, int* out_delta) {
    char a[32], b[32];
    snprintf(a, sizeof(a), "%s_d", id); snprintf(b, sizeof(b), "%s_u", id);
    CLAY({ .layout = row((uint16_t)px(8)) }) {
        if (button_(a, CLAY_STRING("-"), 34)) *out_delta = -5;
        CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(px(56)), CLAY_SIZING_FIT(0) },
                           .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } } }) {
            text_(FS("%d%%", value), LNG_FONT_BODY, g_th->text);
        }
        if (button_(b, CLAY_STRING("+"), 34)) *out_delta = +5;
    }
}

// "Label            [control]" row.
void row_label(const char* label) {
    CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(px(150)), CLAY_SIZING_FIT(0) },
                       .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } } }) {
        text_(CS(label), LNG_FONT_BODY, g_th->text_muted);
    }
}

// Card: filled + bordered. fill_h stretches it to the remaining height.
#define PANEL_BEGIN(idstr, fill_h)                                                       \
    CLAY({ .id = CLAY_ID(idstr),                                                          \
           .layout = { .sizing = { CLAY_SIZING_GROW(0),                                   \
                                   (fill_h) ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0) }, \
                       .padding = CLAY_PADDING_ALL((uint16_t)px(g_th->spacing_md)),       \
                       .childGap = (uint16_t)px(g_th->spacing_sm),                        \
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },                           \
           .backgroundColor = CC(g_th->panel),                                            \
           .cornerRadius = CLAY_CORNER_RADIUS(px(g_th->radius_lg)),                       \
           .border = { .color = CC(g_th->border), .width = bw1() } })

// The source dropdown: anchor + floating option list (hand-built).
void source_dropdown(LauncherModel* m, int player) {
    const LauncherTheme& th = *g_th;
    char anchor_id[32]; snprintf(anchor_id, sizeof(anchor_id), "srcdd%d", player);
    const bool open = (g_open_dropdown == player);

    CLAY({
        .id = Clay_GetElementId(CS(anchor_id)),
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(px(200)), CLAY_SIZING_FIXED(px(34)) },
            .padding = { (uint16_t)px(10), (uint16_t)px(10), 0, 0 },
            .childGap = (uint16_t)px(6),
            .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = Clay_Hovered() ? CC(th.control_hovered) : CC(th.control),
        .cornerRadius = CLAY_CORNER_RADIUS(px(th.radius_sm)),
        .border = { .color = CC(open ? th.focus_ring : th.border), .width = bw1() },
    }) {
        if (Clay_Hovered() && g_pressed) g_open_dropdown = open ? -1 : player;
        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) } } }) {
            text_(CS(launcher_model_player_src_label(m, player)), LNG_FONT_BODY, th.text);
        }
        text_(open ? CLAY_STRING("^") : CLAY_STRING("v"), LNG_FONT_SMALL, th.text_muted);

        if (open) {
            CLAY({
                .layout = { .sizing = { CLAY_SIZING_FIXED(px(200)), CLAY_SIZING_FIT(0) },
                            .padding = CLAY_PADDING_ALL((uint16_t)px(4)),
                            .childGap = (uint16_t)px(2),
                            .layoutDirection = CLAY_TOP_TO_BOTTOM },
                .backgroundColor = CC(th.panel),
                .cornerRadius = CLAY_CORNER_RADIUS(px(th.radius_sm)),
                .floating = { .offset = { 0, px(4) },
                              .attachPoints = { CLAY_ATTACH_POINT_LEFT_TOP,
                                                CLAY_ATTACH_POINT_LEFT_BOTTOM },
                              .attachTo = CLAY_ATTACH_TO_PARENT },
                .border = { .color = CC(th.focus_ring), .width = bw1() },
            }) {
                struct Opt { const char* label; int kind; uint32_t id; };
                Opt opts[2 + LNG_MAX_PADS];
                int n = 0;
                opts[n++] = { "None", 0, 0 };
                opts[n++] = { "Keyboard", 1, 0 };
                for (int i = 0; i < g_pad_count && n < (int)(sizeof(opts)/sizeof(opts[0])); ++i)
                    opts[n++] = { g_pads[i].name, 2, g_pads[i].id };

                for (int i = 0; i < n; ++i) {
                    const bool sel = (m->s.player_src[player] == opts[i].kind) &&
                                     (opts[i].kind != 2 || m->player_pad_id[player] == opts[i].id);
                    CLAY({
                        .id = Clay_GetElementIdWithIndex(CS(anchor_id), (uint32_t)(i + 1)),
                        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(px(30)) },
                                    .padding = { (uint16_t)px(8), (uint16_t)px(8), 0, 0 },
                                    .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
                        .backgroundColor = Clay_Hovered() ? CC(th.control_hovered)
                                                          : (sel ? CC(th.accent) : CC(th.panel)),
                        .cornerRadius = CLAY_CORNER_RADIUS(px(4)),
                    }) {
                        if (Clay_Hovered() && g_pressed) {
                            launcher_model_set_source(m, player, opts[i].kind,
                                                      opts[i].id, opts[i].label);
                            g_open_dropdown = -1;
                        }
                        text_(CS(opts[i].label), LNG_FONT_SMALL, sel ? th.accent_text : th.text);
                    }
                }
                if (g_pad_count == 0) {
                    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(px(26)) },
                                       .padding = { (uint16_t)px(8), (uint16_t)px(8), 0, 0 },
                                       .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } } }) {
                        text_(CLAY_STRING("(no gamepad connected)"), LNG_FONT_SMALL, th.text_muted);
                    }
                }
            }
        }
    }
}

// ---- views ------------------------------------------------------------------

void game_panel(LauncherModel* m, bool fill_h) {
    const LauncherTheme& th = *g_th;
    PANEL_BEGIN("game", fill_h) {
        text_(CLAY_STRING("GAME"), LNG_FONT_SMALL, th.accent);
        // art + identity
        CLAY({ .layout = row((uint16_t)px(th.spacing_md), CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP) }) {
            image_(g_boxart, 150, 190);
            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                               .childGap = (uint16_t)px(4),
                               .layoutDirection = CLAY_TOP_TO_BOTTOM } }) {
                text_(CS(m->game_name), LNG_FONT_TITLE, th.text);
                text_(CS(m->region), LNG_FONT_BODY, th.text_muted);
                text_(m->rom_present ? CLAY_STRING("ROM loaded") : CLAY_STRING("No ROM loaded"),
                      LNG_FONT_SMALL, m->rom_present ? th.good : th.warn);
                if (button_("change_rom", CLAY_STRING("Change ROM..."), 150))
                    launcher_pick_rom(g_window, g_pick_buf, sizeof(g_pick_buf), &g_pick_done);
            }
        }
        // details, full width
        kv("File",    m->rom_file);
        kv("Size",    m->rom_size);
        kv("Header",  m->rom_header);
        kv("CRC32",   m->rom_crc_str, m->crc_match ? "MATCH" : "DIFF", m->crc_match);
        kv("SHA-256", m->rom_sha_str, m->sha_match ? "MATCH" : NULL,   m->sha_match);
    }
}

void controllers_panel(LauncherModel* m, bool fill_h) {
    const LauncherTheme& th = *g_th;
    PANEL_BEGIN("controllers", fill_h) {
        text_(CLAY_STRING("CONTROLLERS"), LNG_FONT_SMALL, th.accent);
        for (int p = 0; p < 2; ++p) {
            CLAY({ .layout = row((uint16_t)px(th.spacing_md), CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP) }) {
                image_(g_pad, 84, 50);
                CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                                   .childGap = (uint16_t)px(4),
                                   .layoutDirection = CLAY_TOP_TO_BOTTOM } }) {
                    text_(FS("Player %d", p + 1), LNG_FONT_BODY, th.text);
                    source_dropdown(m, p);
                    CLAY({ .layout = row((uint16_t)px(6)) }) {
                        dot(m->s.player_src[p] != 0);
                        text_(m->s.player_src[p] ? CLAY_STRING("connected") : CLAY_STRING("none"),
                              LNG_FONT_SMALL, m->s.player_src[p] ? th.good : th.text_muted);
                    }
                }
                char cb[24]; snprintf(cb, sizeof(cb), "cfg%d", p);
                if (button_(cb, CLAY_STRING("Configure"), 110)) launcher_model_open_config(m, p);
            }
        }
    }
}

void view_dashboard(LauncherModel* m, int logical_w) {
    const LauncherTheme& th = *g_th;
    const bool wide = logical_w >= 820;
    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), wide ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0) },
                       .childGap = (uint16_t)px(th.spacing_md),
                       .layoutDirection = wide ? CLAY_LEFT_TO_RIGHT : CLAY_TOP_TO_BOTTOM } }) {
        if (wide) {
            CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(px(460)), CLAY_SIZING_GROW(0) } } }) {
                game_panel(m, true);
            }
            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {
                controllers_panel(m, true);
            }
        } else {
            game_panel(m, false);
            controllers_panel(m, false);
        }
    }
}

void view_settings(LauncherModel* m) {
    const LauncherTheme& th = *g_th;
    PANEL_BEGIN("disp", false) {
        text_(CLAY_STRING("DISPLAY"), LNG_FONT_SMALL, th.accent);
        CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
            row_label("Window scale");
            if (button_("scale", CS(launcher_model_scale_label(m)), 110))
                launcher_model_cycle_scale(m);
        }
        CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
            row_label("Linear filtering");
            if (button_("filter", m->s.linear_filter ? CLAY_STRING("On") : CLAY_STRING("Off"), 110))
                launcher_model_toggle_filter(m);
        }
    }
    if (m->widescreen_supported) {
        PANEL_BEGIN("ws", false) {
            text_(CLAY_STRING("WIDESCREEN"), LNG_FONT_SMALL, th.accent);
            CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
                row_label("Widescreen 16:9");
                if (button_("wstog", m->s.widescreen ? CLAY_STRING("On") : CLAY_STRING("Off"), 110))
                    launcher_model_toggle_widescreen(m);
            }
        }
    }
    PANEL_BEGIN("audio", false) {
        text_(CLAY_STRING("AUDIO"), LNG_FONT_SMALL, th.accent);
        CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
            row_label("Sample rate");
            if (button_("freq", CS(launcher_model_freq_label(m)), 130))
                launcher_model_cycle_freq(m);
        }
        CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
            row_label("Volume");
            int dv = 0; stepper("vol", m->s.volume, &dv);
            if (dv) launcher_model_volume_delta(m, dv);
        }
    }
    PANEL_BEGIN("hotkeys", false) {
        text_(CLAY_STRING("HOTKEYS"), LNG_FONT_SMALL, th.accent);
        // Hand-rolled responsive grid (Clay has no table): chunk into rows of N.
        const int cols = 3;
        for (int base = 0; base < LNG_HK_COUNT; base += cols) {
            CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
                for (int i = base; i < base + cols && i < LNG_HK_COUNT; ++i) {
                    CLAY({ .layout = row((uint16_t)px(6)) }) {
                        CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(px(110)), CLAY_SIZING_FIT(0) },
                                           .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } } }) {
                            text_(CS(launcher_hotkey_name((LngHotkey)i)), LNG_FONT_SMALL, th.text_muted);
                        }
                        char id[24]; snprintf(id, sizeof(id), "hk%d", i);
                        button_(id, CS(m->hotkeys[i]), 120);   // display-only in prototype
                    }
                }
            }
        }
        text_(CLAY_STRING("Saved to config.ini [KeyMap] (edit wired in production)."),
              LNG_FONT_SMALL, th.text_muted);
    }
}

void view_controller(LauncherModel* m) {
    const LauncherTheme& th = *g_th;
    const int p = m->cfg_player;
    PANEL_BEGIN("cfg_src", false) {
        text_(FS("CONTROLLER - PLAYER %d", p + 1), LNG_FONT_SMALL, th.accent);
        CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
            row_label("Input source");
            source_dropdown(m, p);
        }
        CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
            row_label("Deadzone");
            int dz = 0; stepper("dz", m->s.deadzone[p], &dz);
            if (dz) launcher_model_deadzone_delta(m, p, dz);
        }
    }
    PANEL_BEGIN("cfg_binds", false) {
        text_(FS("KEYBOARD BINDINGS - PLAYER %d", p + 1), LNG_FONT_SMALL, th.accent);
        const int cols = 3;   // hand-rolled grid again
        for (int base = 0; base < LNG_BTN_COUNT; base += cols) {
            CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
                for (int b = base; b < base + cols && b < LNG_BTN_COUNT; ++b) {
                    CLAY({ .layout = row((uint16_t)px(6)) }) {
                        CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(px(56)), CLAY_SIZING_FIT(0) },
                                           .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } } }) {
                            text_(CS(launcher_button_name((LngButton)b)), LNG_FONT_SMALL, th.text_muted);
                        }
                        const bool cap = m->capturing && m->capture_btn == (LngButton)b;
                        char id[24]; snprintf(id, sizeof(id), "bind%d", b);
                        if (button_(id, cap ? CLAY_STRING("[ press a key... ]") : CS(m->binds[p][b]),
                                    150, cap))
                            launcher_model_begin_capture(m, (LngButton)b);
                    }
                }
            }
        }
        if (button_("resetbinds", CLAY_STRING("Reset to Defaults"), 180))
            launcher_model_reset_binds(m);
        if (m->capturing)
            text_(CLAY_STRING("Listening... (Esc cancels)"), LNG_FONT_SMALL, th.warn);
    }
}

void build_ui(LauncherModel* m, int logical_w) {
    const LauncherTheme& th = *g_th;
    CLAY({ .id = CLAY_ID("root"),
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                       .padding = CLAY_PADDING_ALL((uint16_t)px(th.spacing_lg)),
                       .childGap = (uint16_t)px(th.spacing_md),
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = CC(th.background) }) {

        // Titlebar: brand + name + subtitle ............... [Settings|Back]
        CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
            image_(g_brand, 40, 30);
            text_(CS(m->game_name), LNG_FONT_TITLE, th.text);
            text_(CLAY_STRING("|  Super Nintendo Launcher"), LNG_FONT_BODY, th.text_muted);
            spacer();
            if (m->view == LNG_VIEW_DASHBOARD) {
                if (button_("nav", CLAY_STRING("Settings"), 110))
                    launcher_model_set_view(m, LNG_VIEW_SETTINGS);
            } else {
                if (button_("nav", CLAY_STRING("< Back"), 110))
                    launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
            }
        }

        // Body — vertical scroll container; footer below stays in the fold.
        CLAY({ .id = CLAY_ID("body"),
               .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                           .childGap = (uint16_t)px(th.spacing_md),
                           .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
            switch (m->view) {
                case LNG_VIEW_DASHBOARD:  view_dashboard(m, logical_w); break;
                case LNG_VIEW_SETTINGS:   view_settings(m);             break;
                case LNG_VIEW_CONTROLLER: view_controller(m);           break;
            }
        }

        // Footer
        CLAY({ .layout = row((uint16_t)px(th.spacing_sm)) }) {
            if (m->view == LNG_VIEW_DASHBOARD) {
                if (button_("skip", m->s.skip_launcher ? CLAY_STRING("Skip on Boot: On")
                                                       : CLAY_STRING("Skip on Boot: Off"), 190))
                    launcher_model_request_skip_toggle(m);
            }
            spacer();
            CLAY({ .id = CLAY_ID("play"),
                   .layout = { .sizing = { CLAY_SIZING_FIXED(px(180)), CLAY_SIZING_FIXED(px(44)) },
                               .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = Clay_Hovered() ? CC(th.focus_ring) : CC(th.accent),
                   .cornerRadius = CLAY_CORNER_RADIUS(px(th.radius_sm)) }) {
                if (Clay_Hovered() && g_pressed) m->action = LNG_ACTION_LAUNCH;
                text_(CLAY_STRING("PLAY"), LNG_FONT_BODY, th.accent_text);
            }
        }

        // Skip-confirm modal
        if (m->skip_modal_open) {
            CLAY({ .id = CLAY_ID("modal"),
                   .layout = { .sizing = { CLAY_SIZING_FIXED(px(470)), CLAY_SIZING_FIT(0) },
                               .padding = CLAY_PADDING_ALL((uint16_t)px(th.spacing_lg)),
                               .childGap = (uint16_t)px(th.spacing_md),
                               .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = CC(th.panel),
                   .cornerRadius = CLAY_CORNER_RADIUS(px(th.radius_lg)),
                   .floating = { .attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                                                   CLAY_ATTACH_POINT_CENTER_CENTER },
                                 .attachTo = CLAY_ATTACH_TO_ROOT },
                   .border = { .color = CC(th.border), .width = bw1() } }) {
                text_(CLAY_STRING("Skip the launcher on boot?"), LNG_FONT_BODY, th.text);
                text_(CLAY_STRING("The game boots straight in. Run with --launcher or set "
                                  "SkipLauncher = 0 in config.ini to bring it back."),
                      LNG_FONT_SMALL, th.text_muted);
                CLAY({ .layout = row((uint16_t)px(th.spacing_sm), CLAY_ALIGN_X_RIGHT) }) {
                    spacer();
                    if (button_("skip_cancel", CLAY_STRING("Cancel"), 110))
                        launcher_model_skip_cancel(m);
                    if (button_("skip_ok", CLAY_STRING("Skip on Boot"), 150, true))
                        launcher_model_skip_confirm(m);
                }
            }
        }
    }
}

bool try_capture(LauncherModel* m, const SDL_Event& ev) {
    if (!m->capturing) return false;
    if (ev.type == SDL_EVENT_KEY_DOWN) {
        if (ev.key.key == SDLK_ESCAPE) { launcher_model_cancel_capture(m); return true; }
        launcher_model_accept_capture(m, SDL_GetKeyName(ev.key.key));
        return true;
    }
    if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const char* n = SDL_GetGamepadStringForButton((SDL_GamepadButton)ev.gbutton.button);
        launcher_model_accept_capture(m, n ? n : "Pad");
        return true;
    }
    return false;
}

void clay_error(Clay_ErrorData e) {
    fprintf(stderr, "[clay] %.*s\n", (int)e.errorText.length, e.errorText.chars);
}

} // namespace

extern "C" LngAction launcher_backend_run(LauncherPlatform* p,
                                          LauncherModel* m,
                                          const LauncherTheme* th) {
    g_th = th;
    g_window = p->window;
    if (!clay_gl_init()) return LNG_ACTION_QUIT;

    const char* base = SDL_GetBasePath();
    std::string bp = base ? base : "";
    g_boxart = launcher_texture_load((bp + "assets/img/boxart.tga").c_str());
    g_pad    = launcher_texture_load_colorkey((bp + "assets/img/snes_pad.tga").c_str(), 24);
    g_brand  = launcher_texture_load((bp + "assets/img/brand_mark.tga").c_str());
    std::string font_path = bp + "assets/fonts/LatoLatin-Regular.ttf";

    uint32_t mem = Clay_MinMemorySize();
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem, malloc(mem));
    Clay_Dimensions dims0{ (float)p->pixel_w, (float)p->pixel_h };
    Clay_ErrorHandler eh{ clay_error, NULL };
    Clay_Initialize(arena, dims0, eh);
    Clay_SetMeasureTextFunction(clay_gl_measure_text, NULL);

    float applied_scale = 0.0f;
    Uint64 last_ticks = 0;
    launcher_debug_init();

    long smoke_frames = 0, frame = 0;
    if (const char* sf = SDL_getenv("LNG_SMOKE_FRAMES")) smoke_frames = SDL_atoi(sf);

    while (m->action == LNG_ACTION_NONE && !p->should_quit) {
        if (smoke_frames > 0 && ++frame > smoke_frames) { m->action = LNG_ACTION_QUIT; break; }

        g_pressed = false;
        float wheel_y = 0.0f;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) p->should_quit = true;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) p->should_quit = true;
            if (try_capture(m, ev)) continue;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT)
                g_pressed = true;
            if (ev.type == SDL_EVENT_MOUSE_WHEEL) wheel_y += ev.wheel.y;
        }

        launcher_platform_refresh_metrics(p);
        g_scale = p->display_scale;
        if (applied_scale != p->display_scale) {
            clay_gl_rebake_fonts(font_path.c_str(), px(18), px(30), px(14));
            applied_scale = p->display_scale;
        }

        g_pad_count = launcher_input_poll(g_pads, LNG_MAX_PADS);
        if (g_pick_done) { g_pick_done = false; launcher_model_set_rom(m, g_pick_buf); }

        float mx = 0, my = 0; SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
        float rx = p->logical_w ? (float)p->pixel_w / p->logical_w : 1.0f;
        float ry = p->logical_h ? (float)p->pixel_h / p->logical_h : 1.0f;
        Clay_Vector2 ptr{ mx * rx, my * ry };
        Clay_SetPointerState(ptr, (mb & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0);
        Clay_Dimensions dims{ (float)p->pixel_w, (float)p->pixel_h };
        Clay_SetLayoutDimensions(dims);

        Uint64 now = SDL_GetTicks();
        float dt = last_ticks ? (float)(now - last_ticks) / 1000.0f : 0.016f;
        last_ticks = now;
        Clay_Vector2 sdelta{ 0.0f, wheel_y * 3.0f };
        Clay_UpdateScrollContainers(true, sdelta, dt);

        pool_reset();
        Clay_BeginLayout();
        build_ui(m, p->logical_w);
        Clay_RenderCommandArray cmds = Clay_EndLayout();

        const LngColor bg = th->background;
        glClearColor(bg.r, bg.g, bg.b, bg.a);
        glClear(GL_COLOR_BUFFER_BIT);
        clay_gl_render(cmds, p->pixel_w, p->pixel_h);
        launcher_debug_step(p, m);
        launcher_platform_present(p);
    }

    launcher_texture_free(&g_boxart);
    launcher_texture_free(&g_pad);
    launcher_texture_free(&g_brand);
    clay_gl_shutdown();
    free(arena.memory);

    if (p->should_quit && m->action == LNG_ACTION_NONE) m->action = LNG_ACTION_QUIT;
    return m->action;
}
