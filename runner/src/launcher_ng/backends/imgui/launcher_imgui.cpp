// launcher_imgui.cpp — Dear ImGui (MIT) backend for the next-gen launcher.
//
// Draws the shared LauncherModel with Dear ImGui + SDL3 + OpenGL3, at parity
// with the shipping RmlUi MMX launcher (box art, controller art, and all
// panels). Icons are drawn as vector primitives rather than font glyphs so they
// stay crisp at any DPI and don't depend on the text font's glyph coverage.
// Demonstrates the two hard requirements:
//   (1) DPI: fonts re-rasterize at (logical size * display_scale); style
//       re-scales on display-scale change -> crisp at 125/150/175% + monitors.
//   (2) Live resize: immediate mode redraws every frame; a logical-width
//       breakpoint switches the dashboard between two columns and one column.

#include "launcher_backend.h"
#include "launcher_gl.h"
#include "launcher_input.h"
#include "launcher_files.h"
#include "launcher_debug.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>   // glViewport/glClear/glClearColor (GL 1.1)

#include <string>

extern "C" const char* launcher_backend_name(void) { return "Dear ImGui"; }

namespace {

float  g_scale = 1.0f;
float  px(float logical) { return logical * g_scale; }
ImVec4 col(const LngColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }
const LauncherTheme* g_th = nullptr;

LauncherTexture g_boxart, g_pad, g_brand;
ImTextureID tid(const LauncherTexture& t) { return (ImTextureID)(intptr_t)t.id; }

LauncherPad g_pads[LNG_MAX_PADS];   // live gamepad list (repolled every frame)
int         g_pad_count = 0;

SDL_Window* g_window = nullptr;      // for parenting the native file dialog
char        g_pick_buf[512] = {};    // ROM picker result
bool        g_pick_done = false;

// ---- DPI: rebuild fonts + re-derive style from an unscaled baseline ----------
void apply_scale(const LauncherTheme& th, float scale, const char* font_path) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;
    const float body = th.font_body * scale;
    bool loaded = false;
    if (font_path && font_path[0])
        loaded = io.Fonts->AddFontFromFileTTF(font_path, body, &cfg) != nullptr;
    if (!loaded) { cfg.SizePixels = body; io.Fonts->AddFontDefault(&cfg); }
    io.Fonts->Build();
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();

    ImGuiStyle style; ImGui::StyleColorsDark(&style);
    style.WindowRounding = th.radius_lg; style.ChildRounding = th.radius_lg;
    style.FrameRounding  = th.radius_sm; style.GrabRounding  = th.radius_sm;
    style.WindowPadding  = ImVec2(th.spacing_lg, th.spacing_lg);
    style.FramePadding   = ImVec2(th.spacing_md, th.spacing_sm);
    style.ItemSpacing    = ImVec2(th.spacing_md, th.spacing_sm);
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;   // controls get a visible outline
    style.Colors[ImGuiCol_WindowBg]        = col(th.background);
    style.Colors[ImGuiCol_ChildBg]         = col(th.panel);
    style.Colors[ImGuiCol_PopupBg]         = col(th.panel);
    style.Colors[ImGuiCol_Border]          = col(th.border);
    style.Colors[ImGuiCol_FrameBg]         = col(th.control);
    style.Colors[ImGuiCol_FrameBgHovered]  = col(th.control_hovered);
    style.Colors[ImGuiCol_FrameBgActive]   = col(th.control_hovered);
    style.Colors[ImGuiCol_Button]          = col(th.control);
    style.Colors[ImGuiCol_ButtonHovered]   = col(th.control_hovered);
    style.Colors[ImGuiCol_ButtonActive]    = col(th.accent);
    style.Colors[ImGuiCol_Header]          = col(th.control_hovered);
    style.Colors[ImGuiCol_HeaderHovered]   = col(th.control_hovered);
    style.Colors[ImGuiCol_HeaderActive]    = col(th.accent);
    style.Colors[ImGuiCol_CheckMark]       = col(th.accent);
    style.Colors[ImGuiCol_Text]            = col(th.text);
    style.Colors[ImGuiCol_TextDisabled]    = col(th.text_muted);
    style.Colors[ImGuiCol_Separator]       = col(th.border);
    style.Colors[ImGuiCol_ScrollbarBg]     = col(th.panel);
    style.Colors[ImGuiCol_ScrollbarGrab]   = col(th.border);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = col(th.control_hovered);
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
}

// ---- primitive icons (crisp at any DPI, no font dependency) -------------------
void draw_check(const LngColor& c) {   // green check, advances cursor like text
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight(), y = p.y + s * 0.5f;
    ImU32 u = ImGui::GetColorU32(col(c));
    dl->AddLine(ImVec2(p.x + s*0.15f, y), ImVec2(p.x + s*0.40f, y + s*0.28f), u, px(2.0f));
    dl->AddLine(ImVec2(p.x + s*0.40f, y + s*0.28f), ImVec2(p.x + s*0.85f, y - s*0.28f), u, px(2.0f));
    ImGui::Dummy(ImVec2(s, s)); ImGui::SameLine(0, px(6));
}
void draw_dot(bool on, const LngColor& good, const LngColor& off) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight(), r = px(5.0f);
    ImVec2 c(p.x + r, p.y + s * 0.5f);
    if (on) dl->AddCircleFilled(c, r, ImGui::GetColorU32(col(good)));
    else    dl->AddCircle(c, r, ImGui::GetColorU32(col(off)), 0, px(1.5f));
    ImGui::Dummy(ImVec2(r * 2, s)); ImGui::SameLine(0, px(8));
}
bool play_button(const char* label, ImVec2 size, const LngColor& bg, const LngColor& fg) {
    ImGui::PushStyleColor(ImGuiCol_Button, col(bg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(bg));
    ImGui::PushStyleColor(ImGuiCol_Text, col(fg));
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clk = ImGui::Button(label, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cy = p.y + size.y * 0.5f, x = p.x + px(22);
    ImU32 u = ImGui::GetColorU32(col(fg));
    dl->AddTriangleFilled(ImVec2(x, cy - px(7)), ImVec2(x, cy + px(7)), ImVec2(x + px(12), cy), u);
    ImGui::PopStyleColor(3);
    return clk;
}

// Draw a texture fit inside a logical box, preserving aspect.
void image_fit(const LauncherTexture& t, float box_w, float box_h) {
    if (!t.id || t.w <= 0 || t.h <= 0) { ImGui::Dummy(ImVec2(px(box_w), px(box_h))); return; }
    float bw = px(box_w), bh = px(box_h);
    float s = (bw / t.w < bh / t.h) ? bw / (float)t.w : bh / (float)t.h;
    ImGui::Image(tid(t), ImVec2(t.w * s, t.h * s));
}

void eyebrow(const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(g_th->accent));
    ImGui::TextUnformatted(s); ImGui::PopStyleColor(); ImGui::Spacing();
}
// A card: filled + bordered. Hugs its content by default; `fill_h` stretches it
// to the remaining height (used by the dashboard columns so the layout doesn't
// leave a big empty gap under short cards).
bool begin_panel(const char* id, float logical_w = 0.0f, bool fill_h = false) {
    ImGuiChildFlags flags = ImGuiChildFlags_Borders;
    if (!fill_h) flags |= ImGuiChildFlags_AutoResizeY;
    return ImGui::BeginChild(id, ImVec2(px(logical_w), 0.0f), flags);
}
void end_panel() { ImGui::EndChild(); }

// A layout container: no fill, no border. Without this a nested child inherits
// ChildBg and paints a large panel-coloured rectangle behind the real cards,
// which reads as "dead space".
bool begin_container(const char* id, ImVec2 size, ImGuiChildFlags flags = ImGuiChildFlags_None) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    return ImGui::BeginChild(id, size, flags);
}
void end_container() { ImGui::EndChild(); ImGui::PopStyleColor(); }

// Key/value row, drawn full width: muted label column, value, and an optional
// right-aligned badge. No wrapping — the row owns the whole panel width, so
// long values (CRC/SHA) have room instead of being clipped or char-wrapped.
void kv(const char* k, const char* v, const LauncherTheme& th,
        const char* badge = nullptr, bool good = true) {
    const float x0 = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::TextUnformatted(k); ImGui::PopStyleColor();
    ImGui::SameLine(x0 + px(84.0f));
    ImGui::TextUnformatted(v);
    if (badge) {
        char b[24]; snprintf(b, sizeof(b), "[%s]", badge);
        const float bw = ImGui::CalcTextSize(b).x;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        ImGui::PushStyleColor(ImGuiCol_Text, col(good ? th.good : th.warn));
        ImGui::TextUnformatted(b); ImGui::PopStyleColor();
    }
}
void stepper(const char* id, int value, const char* suffix, int* out_delta) {
    ImGui::PushID(id);
    if (ImGui::Button("-", ImVec2(px(32), 0))) *out_delta = -5;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d%s", value, suffix);
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(px(32), 0))) *out_delta = +5;
    ImGui::PopID();
}

// "Label ......... [control]" row: label baseline-aligned to the control.
void row_label(const char* text, const LauncherTheme& th) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(th.text_muted), "%s", text);
    ImGui::SameLine(px(170.0f));
}

// ---- views -----------------------------------------------------------------
void draw_game_panel(LauncherModel* m, const LauncherTheme& th, bool fill_h = false) {
    if (!begin_panel("game", 0, fill_h)) { end_panel(); return; }
    eyebrow("GAME");
    // Art + identity side by side...
    image_fit(g_boxart, 150, 190);
    ImGui::SameLine();
    ImGui::BeginGroup();
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextUnformatted(m->game_name);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextColored(col(th.text_muted), "%s", m->region);
        ImGui::Spacing();
        if (m->rom_present) { draw_check(th.good); ImGui::TextColored(col(th.good), "ROM loaded"); }
        else                { ImGui::TextColored(col(th.warn), "No ROM loaded"); }
        ImGui::Spacing();
        if (ImGui::Button("Change ROM..."))   // native OS picker (Win/mac/Linux)
            launcher_pick_rom(g_window, g_pick_buf, sizeof(g_pick_buf), &g_pick_done);
    ImGui::EndGroup();
    // ...then the ROM details across the panel's full width.
    ImGui::Spacing();
    kv("File",    m->rom_file,    th);
    kv("Size",    m->rom_size,    th);
    kv("Header",  m->rom_header,  th);
    kv("CRC32",   m->rom_crc_str, th, m->crc_match ? "MATCH" : "DIFF", m->crc_match);
    kv("SHA-256", m->rom_sha_str, th, m->sha_match ? "MATCH" : nullptr, m->sha_match);
    end_panel();
}

void draw_controllers_panel(LauncherModel* m, const LauncherTheme& th, bool fill_h = false) {
    if (!begin_panel("controllers", 0, fill_h)) { end_panel(); return; }
    eyebrow("CONTROLLERS");
    for (int p = 0; p < 2; ++p) {
        ImGui::PushID(p);
        image_fit(g_pad, 84, 50);
        ImGui::SameLine();
        ImGui::BeginGroup();
            ImGui::Text("Player %d", p + 1);
            ImGui::SetNextItemWidth(px(200));
            // Real dropdown: None / Keyboard / each connected gamepad by name.
            if (ImGui::BeginCombo("##src", launcher_model_player_src_label(m, p))) {
                if (ImGui::Selectable("None", m->s.player_src[p] == 0))
                    launcher_model_set_source(m, p, 0, 0, nullptr);
                if (ImGui::Selectable("Keyboard", m->s.player_src[p] == 1))
                    launcher_model_set_source(m, p, 1, 0, nullptr);
                for (int i = 0; i < g_pad_count; ++i) {
                    bool sel = m->s.player_src[p] == 2 && m->player_pad_id[p] == g_pads[i].id;
                    if (ImGui::Selectable(g_pads[i].name, sel))
                        launcher_model_set_source(m, p, 2, g_pads[i].id, g_pads[i].name);
                }
                if (g_pad_count == 0) {
                    ImGui::BeginDisabled();
                    ImGui::Selectable("(no gamepad connected)");
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            draw_dot(m->s.player_src[p] != 0, th.good, th.text_muted);
            ImGui::TextColored(m->s.player_src[p] ? col(th.good) : col(th.text_muted),
                               "%s", m->s.player_src[p] ? "connected" : "none");
        ImGui::EndGroup();
        ImGui::SameLine();
        if (ImGui::Button("Configure")) launcher_model_open_config(m, p);
        ImGui::Spacing();
        ImGui::PopID();
    }
    end_panel();
}

void draw_dashboard(LauncherModel* m, const LauncherTheme& th, int logical_w) {
    if (logical_w >= 820) {
        // Two columns, both stretched to the body height.
        begin_container("dash_l", ImVec2(px(460), 0));
        draw_game_panel(m, th, true); end_container();
        ImGui::SameLine();
        begin_container("dash_r", ImVec2(0, 0));
        draw_controllers_panel(m, th, true); end_container();
    } else {
        draw_game_panel(m, th); ImGui::Spacing(); draw_controllers_panel(m, th);
    }
}

void draw_settings(LauncherModel* m, const LauncherTheme& th) {
    if (begin_panel("disp", 0)) {
        eyebrow("DISPLAY");
        row_label("Window scale", th);
        if (ImGui::Button(launcher_model_scale_label(m), ImVec2(px(120), 0)))
            launcher_model_cycle_scale(m);
        row_label("Linear filtering", th);
        bool filter = m->s.linear_filter != 0;
        if (ImGui::Checkbox("##filter", &filter)) launcher_model_toggle_filter(m);
    } end_panel();

    if (m->widescreen_supported) {
        if (begin_panel("ws", 0)) {
            eyebrow("WIDESCREEN");
            bool ws = m->s.widescreen != 0;
            if (ImGui::Checkbox("Widescreen 16:9 (experimental)", &ws))
                launcher_model_toggle_widescreen(m);
        } end_panel();
    }

    if (begin_panel("audio", 0)) {
        eyebrow("AUDIO");
        row_label("Sample rate", th);
        if (ImGui::Button(launcher_model_freq_label(m), ImVec2(px(120), 0)))
            launcher_model_cycle_freq(m);
        row_label("Volume", th);
        int dv = 0; stepper("vol", m->s.volume, "%", &dv);
        if (dv) launcher_model_volume_delta(m, dv);
    } end_panel();

    if (begin_panel("hotkeys", 0)) {
        eyebrow("HOTKEYS");
        // Same responsive grid treatment as the bindings list.
        const float cell_w = px(280.0f);
        int cols = (int)(ImGui::GetContentRegionAvail().x / cell_w);
        cols = cols < 1 ? 1 : (cols > 3 ? 3 : cols);
        if (ImGui::BeginTable("hk", cols)) {
            for (int h = 0; h < LNG_HK_COUNT; ++h) {
                ImGui::TableNextColumn();
                ImGui::PushID(h);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(col(th.text_muted), "%-13s", launcher_hotkey_name((LngHotkey)h));
                ImGui::SameLine(px(120));
                ImGui::Button(m->hotkeys[h], ImVec2(px(130), 0));  // display-only in prototype
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextColored(col(th.text_muted),
                           "Saved to config.ini [KeyMap] (edit wired in production).");
    } end_panel();
}

void draw_controller(LauncherModel* m, const LauncherTheme& th) {
    const int p = m->cfg_player;
    if (begin_panel("cfg_src", 0)) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent));
        ImGui::Text("CONTROLLER - PLAYER %d", p + 1); ImGui::PopStyleColor(); ImGui::Spacing();
        row_label("Input source", th);
        ImGui::SetNextItemWidth(px(200));
        if (ImGui::BeginCombo("##csrc", launcher_model_player_src_label(m, p))) {
            if (ImGui::Selectable("None", m->s.player_src[p] == 0))
                launcher_model_set_source(m, p, 0, 0, nullptr);
            if (ImGui::Selectable("Keyboard", m->s.player_src[p] == 1))
                launcher_model_set_source(m, p, 1, 0, nullptr);
            for (int i = 0; i < g_pad_count; ++i) {
                bool sel = m->s.player_src[p] == 2 && m->player_pad_id[p] == g_pads[i].id;
                if (ImGui::Selectable(g_pads[i].name, sel))
                    launcher_model_set_source(m, p, 2, g_pads[i].id, g_pads[i].name);
            }
            if (g_pad_count == 0) {
                ImGui::BeginDisabled();
                ImGui::Selectable("(no gamepad connected)");
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        row_label("Deadzone", th);
        int dz = 0; stepper("dz", m->s.deadzone[p], "%", &dz);
        if (dz) launcher_model_deadzone_delta(m, p, dz);
    } end_panel();

    if (begin_panel("cfg_binds", 0)) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent));
        ImGui::Text("KEYBOARD BINDINGS - PLAYER %d", p + 1); ImGui::PopStyleColor(); ImGui::Spacing();

        // Responsive grid: fit as many label+chip columns as the width allows
        // (1..4) instead of one tall column with dead space to the right.
        const float cell_w = px(270.0f);
        int cols = (int)(ImGui::GetContentRegionAvail().x / cell_w);
        if (cols < 1) cols = 1;
        if (cols > 4) cols = 4;
        if (ImGui::BeginTable("binds", cols)) {
            for (int b = 0; b < LNG_BTN_COUNT; ++b) {
                ImGui::TableNextColumn();
                ImGui::PushID(b);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(col(th.text_muted), "%-6s", launcher_button_name((LngButton)b));
                ImGui::SameLine(px(70));
                const bool cap = m->capturing && m->capture_btn == (LngButton)b;
                if (cap) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
                if (ImGui::Button(cap ? "[ press a key... ]" : m->binds[p][b], ImVec2(px(160), 0)))
                    launcher_model_begin_capture(m, (LngButton)b);
                if (cap) ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Reset to Defaults")) launcher_model_reset_binds(m);
        if (m->capturing) ImGui::TextColored(col(th.warn), "Listening... (Esc cancels)");
    } end_panel();
}

void draw_footer(LauncherModel* m, const LauncherTheme& th) {
    ImGui::Separator();
    if (m->view == LNG_VIEW_DASHBOARD) {
        bool skip = m->s.skip_launcher != 0;
        if (ImGui::Checkbox("Skip Launcher on Boot", &skip))
            launcher_model_request_skip_toggle(m);
    }
    const float play_w = px(180);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - play_w);
    if (play_button("     PLAY", ImVec2(play_w, px(40)), th.accent, th.accent_text))
        m->action = LNG_ACTION_LAUNCH;
}

void draw_skip_modal(LauncherModel* m) {
    if (m->skip_modal_open) ImGui::OpenPopup("Skip the launcher on boot?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Skip the launcher on boot?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("The launcher will no longer appear - the game boots straight in. "
                           "Run with \"--launcher\" or set \"SkipLauncher = 0\" in config.ini "
                           "to bring it back.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            launcher_model_skip_cancel(m); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip on Boot", ImVec2(px(140), 0))) {
            launcher_model_skip_confirm(m); ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_ui(LauncherModel* m, const LauncherTheme& th, int logical_w, int logical_h) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Titlebar: brand mark + game name + subtitle ......... [Settings|Back]
    image_fit(g_brand, 40, 30); ImGui::SameLine();
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted(m->game_name);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    ImGui::TextColored(col(th.text_muted), "  |  Super Nintendo Launcher");
    {   // right-aligned nav button
        const char* label = (m->view == LNG_VIEW_DASHBOARD) ? "Settings" : "< Back";
        const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        if (ImGui::Button(label)) {
            launcher_model_set_view(m, m->view == LNG_VIEW_DASHBOARD
                                        ? LNG_VIEW_SETTINGS : LNG_VIEW_DASHBOARD);
        }
    }
    ImGui::Spacing();

    // Body: fixed-height child that scrolls when content overflows, so nothing
    // is ever clipped out of reach. The footer below stays fixed (in the fold).
    const float footer_h = px(60.0f);
    float body_h = ImGui::GetContentRegionAvail().y - footer_h;
    if (body_h < px(80.0f)) body_h = px(80.0f);
    begin_container("body", ImVec2(0, body_h));
    switch (m->view) {
        case LNG_VIEW_DASHBOARD:  draw_dashboard(m, th, logical_w); break;
        case LNG_VIEW_SETTINGS:   draw_settings(m, th);             break;
        case LNG_VIEW_CONTROLLER: draw_controller(m, th);           break;
    }
    end_container();

    draw_footer(m, th);
    draw_skip_modal(m);
    ImGui::End();
    (void)logical_h;
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

std::string asset(const char* rel) {
    const char* base = SDL_GetBasePath();
    return std::string(base ? base : "") + rel;
}

} // namespace

extern "C" LngAction launcher_backend_run(LauncherPlatform* p,
                                          LauncherModel* m,
                                          const LauncherTheme* th) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;

    g_th = th;
    ImGui_ImplSDL3_InitForOpenGL(p->window, p->gl);
    ImGui_ImplOpenGL3_Init("#version 330");

    g_window = p->window;
    g_boxart = launcher_texture_load(asset("assets/img/boxart.tga").c_str());
    // snes_pad.tga is 24-bit (no alpha) with a flat backdrop baked in -> key it
    // out so the pad art sits transparently on the panel.
    g_pad    = launcher_texture_load_colorkey(asset("assets/img/snes_pad.tga").c_str(), 24);
    g_brand  = launcher_texture_load(asset("assets/img/brand_mark.tga").c_str());

    std::string font_path = asset("assets/fonts/LatoLatin-Regular.ttf");
    float applied_scale = 0.0f;
    launcher_debug_init();

    long smoke_frames = 0, frame = 0;
    if (const char* sf = SDL_getenv("LNG_SMOKE_FRAMES")) smoke_frames = SDL_atoi(sf);

    while (m->action == LNG_ACTION_NONE && !p->should_quit) {
        if (smoke_frames > 0 && ++frame > smoke_frames) { m->action = LNG_ACTION_QUIT; break; }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) p->should_quit = true;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) p->should_quit = true;
            if (try_capture(m, ev)) continue;
            ImGui_ImplSDL3_ProcessEvent(&ev);
        }

        launcher_platform_refresh_metrics(p);
        g_scale = p->display_scale;
        if (applied_scale != p->display_scale) {
            apply_scale(*th, p->display_scale, font_path.c_str());
            applied_scale = p->display_scale;
        }

        // Re-poll connected gamepads every frame so hot-plugged pads (e.g. a
        // DualSense powered on after launch) appear without a relaunch.
        g_pad_count = launcher_input_poll(g_pads, LNG_MAX_PADS);

        // The native ROM picker fires its callback during event pumping.
        if (g_pick_done) {
            g_pick_done = false;
            launcher_model_set_rom(m, g_pick_buf);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw_ui(m, *th, p->logical_w, p->logical_h);
        ImGui::Render();

        glViewport(0, 0, p->pixel_w, p->pixel_h);
        const LngColor bg = th->background;
        glClearColor(bg.r, bg.g, bg.b, bg.a);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        launcher_debug_step(p, m);   // script/screenshot: after draw, before swap
        launcher_platform_present(p);
    }

    launcher_texture_free(&g_boxart);
    launcher_texture_free(&g_pad);
    launcher_texture_free(&g_brand);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (p->should_quit && m->action == LNG_ACTION_NONE) m->action = LNG_ACTION_QUIT;
    return m->action;
}
