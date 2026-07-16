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
#include "launcher_binds.h"

#include "launcher_sdlcompat.h"   // pulls the right SDL header + event shim

#include "imgui.h"
#if defined(LNG_SDL3)
  #include "imgui_impl_sdl3.h"
  #define LNG_ImplSDL_InitForOpenGL  ImGui_ImplSDL3_InitForOpenGL
  #define LNG_ImplSDL_NewFrame       ImGui_ImplSDL3_NewFrame
  #define LNG_ImplSDL_ProcessEvent   ImGui_ImplSDL3_ProcessEvent
  #define LNG_ImplSDL_Shutdown       ImGui_ImplSDL3_Shutdown
#else
  #include "imgui_impl_sdl2.h"
  #define LNG_ImplSDL_InitForOpenGL  ImGui_ImplSDL2_InitForOpenGL
  #define LNG_ImplSDL_NewFrame       ImGui_ImplSDL2_NewFrame
  #define LNG_ImplSDL_ProcessEvent   ImGui_ImplSDL2_ProcessEvent
  #define LNG_ImplSDL_Shutdown       ImGui_ImplSDL2_Shutdown
#endif
#include "imgui_impl_opengl3.h"

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

char        g_pick_buf[512] = {};    // ROM picker result

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
    // Gamepad/keyboard focus ring: bright cyan so a Deck user always sees where
    // they are. (NavHighlight is the pre-1.91.4 alias of NavCursor.)
    style.Colors[ImGuiCol_NavCursor]       = col(th.focus_ring);
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
}

// ---- CRT / neon atmosphere (drawn with ImDrawList) ---------------------------
ImU32 imcol(const LngColor& c, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a * a));
}

// Vertical center-bright gradient (CRT ground) + faint scanlines. Drawn on the
// background/foreground draw lists so it sits behind/over the whole UI.
void draw_crt_background(ImVec2 origin, ImVec2 size) {
    const LauncherTheme& th = *g_th;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImU32 ink = imcol(th.background), lift = imcol(th.background2);
    float midY = origin.y + size.y * 0.42f;
    // top: ink -> lift, bottom: lift -> ink  (soft horizontal glow band)
    bg->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, midY),
                                ink, ink, lift, lift);
    bg->AddRectFilledMultiColor(ImVec2(origin.x, midY), ImVec2(origin.x + size.x, origin.y + size.y),
                                lift, lift, ink, ink);
    // a soft violet bloom behind the header (arcade marquee glow)
    bg->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, origin.y + px(90)),
                                imcol(th.accent, 0.10f), imcol(th.accent, 0.10f),
                                imcol(th.accent, 0.0f),  imcol(th.accent, 0.0f));
    // scanlines over everything, very subtle
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    float step = px(3.0f); if (step < 2.0f) step = 2.0f;
    ImU32 sl = imcol(th.scanline);
    for (float y = origin.y; y < origin.y + size.y; y += step)
        fg->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y), sl, 1.0f);
}

// Neon glow: concentric rounded rects fading outward behind [min,max].
void glow_rect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding,
               const LngColor& c, float intensity, int layers = 5) {
    for (int i = layers; i >= 1; --i) {
        float grow = px(2.0f) * i;
        float a = intensity * (0.10f) * (float)(layers - i + 1) / layers;
        dl->AddRectFilled(ImVec2(mn.x - grow, mn.y - grow),
                          ImVec2(mx.x + grow, mx.y + grow),
                          imcol(c, a), rounding + grow);
    }
}

// Filled rounded rect with a vertical gradient (top -> bottom).
void grad_rect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding,
               const LngColor& top, const LngColor& bot) {
    dl->AddRectFilled(mn, mx, imcol(bot), rounding);   // base (rounded)
    // overlay a gradient clipped to the rounded rect via a slightly-inset fill
    dl->PushClipRect(mn, mx, true);
    dl->AddRectFilledMultiColor(mn, mx, imcol(top), imcol(top), imcol(bot), imcol(bot));
    dl->PopClipRect();
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
// The primary neon CTA (PLAY): glow + violet gradient + play triangle. Fully
// custom-drawn over an InvisibleButton so it looks nothing like a stock button.
bool neon_cta(const char* id, const char* label, ImVec2 size) {
    const LauncherTheme& th = *g_th;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clk = ImGui::InvisibleButton(id, size);
    bool hov = ImGui::IsItemHovered();
    bool act = ImGui::IsItemActive();
    ImVec2 mn = p, mx = ImVec2(p.x + size.x, p.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = px(th.radius_sm);

    glow_rect(dl, mn, mx, r, th.accent, hov ? 1.6f : 1.0f, 6);
    LngColor top = hov ? th.accent : th.accent;
    LngColor bot = act ? th.accent_dim : th.accent_dim;
    grad_rect(dl, mn, mx, r, top, bot);
    dl->AddRect(mn, mx, imcol(th.accent, hov ? 0.9f : 0.5f), r, 0, px(1.0f));  // crisp edge

    // centered "▶ label"
    float th_h = ImGui::GetTextLineHeight();
    float tw = ImGui::CalcTextSize(label).x;
    float tri = px(11.0f), gap = px(10.0f);
    float total = tri + gap + tw;
    float cx = p.x + (size.x - total) * 0.5f, cy = p.y + size.y * 0.5f;
    ImU32 fg = imcol(th.accent_text);
    dl->AddTriangleFilled(ImVec2(cx, cy - tri*0.55f), ImVec2(cx, cy + tri*0.55f),
                          ImVec2(cx + tri, cy), fg);
    dl->AddText(ImVec2(cx + tri + gap, cy - th_h*0.5f), fg, label);
    return clk;
}

// Uppercase section eyebrow with letter-spacing + a short accent tick, e.g.
//   ▎ CONTROLLERS   — encodes "this is a section header", arcade panel style.
void eyebrow_tracked(const char* s) {
    const LauncherTheme& th = *g_th;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    // accent tick
    dl->AddRectFilled(ImVec2(p.x, p.y + h*0.12f), ImVec2(p.x + px(3.0f), p.y + h*0.9f),
                      imcol(th.accent), px(1.5f));
    // letter-spaced text
    float x = p.x + px(10.0f);
    ImU32 c = imcol(th.accent);
    char buf[2] = {0,0};
    for (const char* q = s; *q; ++q) {
        buf[0] = *q;
        dl->AddText(ImVec2(x, p.y), c, buf);
        x += ImGui::CalcTextSize(buf).x + px(2.2f);
    }
    ImGui::Dummy(ImVec2(x - p.x, h));
    ImGui::Spacing();
}

// Draw a texture fit inside a logical box, preserving aspect.
void image_fit(const LauncherTexture& t, float box_w, float box_h) {
    if (!t.id || t.w <= 0 || t.h <= 0) { ImGui::Dummy(ImVec2(px(box_w), px(box_h))); return; }
    float bw = px(box_w), bh = px(box_h);
    float s = (bw / t.w < bh / t.h) ? bw / (float)t.w : bh / (float)t.h;
    ImGui::Image(tid(t), ImVec2(t.w * s, t.h * s));
}

void eyebrow(const char* s) { eyebrow_tracked(s); }
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

void state_mark(bool ok, const LauncherTheme& th);   // fwd

// One metadata row inside a 3-column table: label | value | optional check.
// `show_mark` puts a mint check / amber cross in its own column instead of a
// text badge, so it can never crowd the panel edge.
void kv_row(const char* k, const char* v, const LauncherTheme& th,
            bool show_mark, bool ok) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::TextUnformatted(k);
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(v);
    ImGui::TableNextColumn();
    if (show_mark) state_mark(ok, th);
}

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
// Box art, centered, framed. No neon glow — the art is photographic content and
// a violet halo around it reads as a bug, not a design. Glow is reserved for
// the PLAY CTA, where it means "this is the action".
void hero_boxart_centered(const LauncherTexture& t, float box_h, float avail_w) {
    const LauncherTheme& th = *g_th;
    float bh = px(box_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (t.id && t.w > 0 && t.h > 0) {
        float s = bh / (float)t.h;
        float iw = t.w * s, ih = bh;
        if (iw > avail_w) { s = avail_w / (float)t.w; iw = avail_w; ih = t.h * s; }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - iw) * 0.5f);  // center
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 mn = p, mx = ImVec2(p.x + iw, p.y + ih);
        dl->AddImageRounded(tid(t), mn, mx, ImVec2(0,0), ImVec2(1,1),
                            imcol(lng_rgba(1,1,1,1)), px(4.0f));
        dl->AddRect(mn, mx, imcol(th.border), px(4.0f), 0, px(1.0f));
        ImGui::Dummy(ImVec2(iw, ih));
    } else {
        // No box art was supplied for this game — draw a tasteful SNES-cartridge
        // placeholder so the GAME card never shows dead space. Game-agnostic: any
        // title that declares no boxart.tga gets this instead of an empty slot.
        float iw = bh * 0.72f;               // match a box-art portrait aspect
        if (iw > avail_w) iw = avail_w;
        float ih = bh;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - iw) * 0.5f);  // center
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 mn = p, mx = ImVec2(p.x + iw, p.y + ih);
        dl->AddRectFilled(mn, mx, imcol(th.panel_hovered), px(6.0f));
        dl->AddRect(mn, mx, imcol(th.border), px(6.0f), 0, px(1.0f));

        // cartridge body, centered in the slot
        float cw = iw * 0.52f, ch = cw * 1.04f;
        float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
        ImVec2 bmn = ImVec2(cx - cw * 0.5f, cy - ch * 0.5f);
        ImVec2 bmx = ImVec2(cx + cw * 0.5f, cy + ch * 0.5f);
        dl->AddRectFilled(bmn, bmx, imcol(th.accent_dim), cw * 0.10f);
        // top ridges
        for (int i = 0; i < 3; i++) {
            float rx = bmn.x + cw * (0.20f + i * 0.24f);
            dl->AddRectFilled(ImVec2(rx, bmn.y - ch * 0.05f),
                              ImVec2(rx + cw * 0.12f, bmn.y + ch * 0.10f),
                              imcol(th.accent), cw * 0.03f);
        }
        // recessed label window
        dl->AddRectFilled(ImVec2(bmn.x + cw * 0.16f, bmn.y + ch * 0.30f),
                          ImVec2(bmx.x - cw * 0.16f, bmx.y - ch * 0.16f),
                          imcol(th.panel), cw * 0.04f);
        ImGui::Dummy(ImVec2(iw, ih));
    }
}

// A verified/failed state marker: mint check or amber cross. Replaces the
// [MATCH] badge that crowded the panel edge.
void state_mark(bool ok, const LauncherTheme& th) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight();
    ImU32 c = imcol(ok ? th.good : th.warn);
    float y = p.y + s * 0.5f;
    if (ok) {
        dl->AddLine(ImVec2(p.x + s*0.16f, y), ImVec2(p.x + s*0.40f, y + s*0.26f), c, px(2.0f));
        dl->AddLine(ImVec2(p.x + s*0.40f, y + s*0.26f), ImVec2(p.x + s*0.84f, y - s*0.26f), c, px(2.0f));
    } else {
        dl->AddLine(ImVec2(p.x + s*0.22f, y - s*0.24f), ImVec2(p.x + s*0.78f, y + s*0.24f), c, px(2.0f));
        dl->AddLine(ImVec2(p.x + s*0.78f, y - s*0.24f), ImVec2(p.x + s*0.22f, y + s*0.24f), c, px(2.0f));
    }
    ImGui::Dummy(ImVec2(s, s));
}

void draw_game_panel(LauncherModel* m, const LauncherTheme& th, bool fill_h = false) {
    if (!begin_panel("game", 0, fill_h)) { end_panel(); return; }
    eyebrow("GAME");
    const float availw = ImGui::GetContentRegionAvail().x;

    // Box art on top (centered), everything else BELOW it. Height is derived
    // from the space actually left after the metadata + button, so the art is
    // as large as it can be WITHOUT pushing the last row out of the card.
    {
        const float reserve = px(228.0f);   // state line + 4 meta rows + button + pad
        float art_h = ImGui::GetContentRegionAvail().y - reserve;
        if (art_h > px(300.0f)) art_h = px(300.0f);
        if (art_h < px(140.0f)) art_h = px(140.0f);
        hero_boxart_centered(g_boxart, art_h / g_scale, availw);
    }
    ImGui::Dummy(ImVec2(0, px(10)));

    // Region + verification state, centered under the art.
    const bool verified = launcher_model_rom_verified(m);
    {
        const char* line = !m->rom_present ? "No ROM loaded"
                          : verified        ? "ROM verified"
                                            : "ROM not recognized";
        float w = ImGui::GetTextLineHeight() + px(6) + ImGui::CalcTextSize(line).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availw - w) * 0.5f);
        state_mark(verified, th);
        ImGui::SameLine(0, px(6));
        ImGui::TextColored(verified ? col(th.good) : col(th.warn), "%s", line);
    }
    ImGui::Dummy(ImVec2(0, px(10)));

    // Metadata: what a PLAYER needs to know. Raw CRC32/SHA-256 digests are
    // developer noise — the question a user has is "is my ROM good?", so the
    // real fingerprint check is surfaced as one PASS/FAIL row.
    if (ImGui::BeginTable("meta", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, px(76));
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed, px(22));
        kv_row("Region", m->region[0] ? m->region : "SNES", th, false, false);
        kv_row("File",   m->rom_file, th, false, false);
        kv_row("Size",   m->rom_size, th, false, false);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
        ImGui::TextUnformatted("Checksum");
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        ImGui::TextColored(verified ? col(th.good) : col(th.warn), "%s", verified ? "PASS" : "FAIL");
        ImGui::TableNextColumn();
        state_mark(verified, th);
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, px(12)));
    if (ImGui::Button("Change ROM", ImVec2(availw, px(34))))
        if (launcher_pick_rom(g_pick_buf, sizeof(g_pick_buf)))
            launcher_model_set_rom(m, g_pick_buf);
    end_panel();
}

// Each player is its OWN self-contained card ("PLAYER 1" as its eyebrow), not a
// floating column inside one big CONTROLLERS box. A 1-player game shows a
// single card (no wasted width); a 2-player game shows two identical cards side
// by side. Same module, composed per the game's declared player count.
void draw_player_panel(LauncherModel* m, const LauncherTheme& th, int p, float w) {
    char id[24];  snprintf(id, sizeof(id), "player%d", p);
    char eb[16];  snprintf(eb, sizeof(eb), "PLAYER %d", p + 1);

    if (!begin_panel(id, w / g_scale, false)) { end_panel(); return; }
    ImGui::PushID(p);
    eyebrow(eb);

    const float inner = ImGui::GetContentRegionAvail().x;
    const float cw    = inner;   // controls span the card => flush by construction

    // pad art centered in the card
    {
        const float aw = px(96);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - aw) * 0.5f);
        image_fit(g_pad, 96, 57);
    }
    ImGui::Dummy(ImVec2(0, px(6)));

    ImGui::SetNextItemWidth(cw);
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
    ImGui::Dummy(ImVec2(0, px(4)));
    if (ImGui::Button("Configure", ImVec2(cw, px(32)))) launcher_model_open_config(m, p);
    ImGui::Dummy(ImVec2(0, px(6)));
    // status line, centered
    {
        const bool on = m->s.player_src[p] != 0;
        const char* st = on ? "connected" : "not assigned";
        float sw = px(10) + px(8) + ImGui::CalcTextSize(st).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - sw) * 0.5f);
        draw_dot(on, th.good, th.text_muted);
        ImGui::TextColored(on ? col(th.good) : col(th.text_muted), "%s", st);
    }
    ImGui::PopID();
    end_panel();
}

// Lays out the player cards: one card for a 1-player game, two side-by-side
// for a 2-player game. Driven by the model, never hardcoded.
void draw_controllers_row(LauncherModel* m, const LauncherTheme& th) {
    const int   n   = (m->player_count >= 2) ? 2 : 1;
    const float gap = px(th.spacing_md);
    const float availw = ImGui::GetContentRegionAvail().x;
    // A 2P game splits the row; a 1P game gets ONE card of the same size rather
    // than a full-width card with a lone pad floating in it.
    float cardw = (availw - gap) * 0.5f;
    if (n == 1 && cardw < px(300.0f)) cardw = availw;   // narrow window: fill
    for (int p = 0; p < n; ++p) {
        if (p) ImGui::SameLine(0, gap);
        begin_container(p ? "pc1" : "pc0", ImVec2(cardw, 0), ImGuiChildFlags_AutoResizeY);
        draw_player_panel(m, th, p, cardw);
        end_container();
    }
}

// SAVES module — only exists for games with battery SRAM. MMX is a password
// game (sram_path == NULL) so this module is absent entirely; Zelda/SMW get it.
// Availability is data-driven off the game's C-ABI struct, never a game name.
void draw_saves_panel(LauncherModel* m, const LauncherTheme& th) {
    if (!begin_panel("saves", 0)) { end_panel(); return; }
    eyebrow("SAVES");
    const char* p = m->sram_path ? m->sram_path : "";
    // basename for display
    const char* base = p;
    for (const char* q = p; *q; ++q) if (*q == '/' || *q == '\\') base = q + 1;
    // Single compact row: file + the two actions, all on one line so the module
    // stays short enough to stack under CONTROLLERS without clipping.
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("File");
    ImGui::PopStyleColor();
    ImGui::SameLine(px(76));
    ImGui::TextUnformatted(base[0] ? base : "(none yet)");
    const float bw = px(96);
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw*2 - px(th.spacing_sm));
    ImGui::Button("Import", ImVec2(bw, px(30)));
    ImGui::SameLine(0, px(th.spacing_sm));
    ImGui::Button("Clear", ImVec2(bw, px(30)));
    end_panel();
}

// MSU-1 module — only for games that ship/support an MSU-1 pack (SMW, Zelda).
// Enable toggle + music-folder picker + the game's note about which patch.
void draw_msu1_panel(LauncherModel* m, const LauncherTheme& th) {
    if (!begin_panel("msu1", 0)) { end_panel(); return; }
    eyebrow("MSU-1 AUDIO");
    bool on = m->s.msu1_enabled != 0;
    if (ImGui::Checkbox("Enable CD-quality music", &on))
        launcher_model_toggle_msu1(m);

    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Folder");
    ImGui::PopStyleColor();
    ImGui::SameLine(px(76));
    const char* dir = m->s.msu1_dir[0] ? m->s.msu1_dir : "(not set)";
    const float bw = px(90);
    float avail = ImGui::GetContentRegionAvail().x - bw - px(th.spacing_sm);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail);
    ImGui::TextUnformatted(dir);
    ImGui::PopTextWrapPos();
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
    if (ImGui::Button("Browse", ImVec2(bw, px(30)))) {
        char buf[512];
        if (launcher_pick_folder("Select MSU-1 music folder", buf, sizeof(buf)))
            launcher_model_set_msu1_dir(m, buf);
    }
    if (m->msu1_note && m->msu1_note[0]) {
        ImGui::Dummy(ImVec2(0, px(4)));
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
        ImGui::TextWrapped("%s", m->msu1_note);
        ImGui::PopStyleColor();
    }
    end_panel();
}

// The dashboard COMPOSES whichever modules this game supports — it does not
// hardcode a fixed set. GAME is always present; the side column stacks
// CONTROLLERS plus any optional modules (SAVES only when the game has SRAM).
// A different game simply contributes a different module set.
void draw_dashboard(LauncherModel* m, const LauncherTheme& th, int logical_w) {
    if (logical_w >= 820) {
        const float gap = px(th.spacing_md);
        // Art-led left column sized to the box art; side column takes the rest.
        begin_container("dash_l", ImVec2(px(400), 0));
        draw_game_panel(m, th, true);
        end_container();

        ImGui::SameLine(0, gap);
        begin_container("dash_r", ImVec2(0, 0), ImGuiChildFlags_None);
            // One self-contained card per player (1 card for a 1P game, two
            // side-by-side for a 2P game), then any optional modules.
            draw_controllers_row(m, th);
            if (m->saves_supported) {          // module: opt-in per game
                ImGui::Dummy(ImVec2(0, px(th.spacing_md)));
                draw_saves_panel(m, th);
            }
            if (m->msu1_supported) {
                ImGui::Dummy(ImVec2(0, px(th.spacing_md)));
                draw_msu1_panel(m, th);
            }
        end_container();
    } else {
        draw_game_panel(m, th);
        ImGui::Spacing();
        draw_controllers_row(m, th);
        if (m->saves_supported) { ImGui::Spacing(); draw_saves_panel(m, th); }
        if (m->msu1_supported)  { ImGui::Spacing(); draw_msu1_panel(m, th); }
    }
}

void draw_settings(LauncherModel* m, const LauncherTheme& th) {
    // Row 1: DISPLAY | AUDIO share the top band (12-col style grid) instead of
    // two near-empty full-width bars stacked with dead space to the right.
    const float gap  = px(th.spacing_md);
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;

    begin_container("set_l", ImVec2(half, 0), ImGuiChildFlags_AutoResizeY);
    if (begin_panel("disp", 0)) {
        eyebrow("DISPLAY");
        row_label("Window scale", th);
        if (ImGui::Button(launcher_model_scale_label(m), ImVec2(px(120), px(30))))
            launcher_model_cycle_scale(m);
        row_label("Linear filtering", th);
        bool filter = m->s.linear_filter != 0;
        if (ImGui::Checkbox("##filter", &filter)) launcher_model_toggle_filter(m);
        if (m->widescreen_supported) {   // module: only for games that support it
            row_label("Widescreen 16:9", th);
            bool ws = m->s.widescreen != 0;
            if (ImGui::Checkbox("##ws", &ws)) launcher_model_toggle_widescreen(m);
        }
    } end_panel();
    end_container();

    ImGui::SameLine(0, gap);

    begin_container("set_r", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
    if (begin_panel("audio", 0)) {
        eyebrow("AUDIO");
        row_label("Sample rate", th);
        if (ImGui::Button(launcher_model_freq_label(m), ImVec2(px(120), px(30))))
            launcher_model_cycle_freq(m);
        row_label("Volume", th);
        int dv = 0; stepper("vol", m->s.volume, "%", &dv);
        if (dv) launcher_model_volume_delta(m, dv);
    } end_panel();
    end_container();

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
                ImGui::SameLine(px(130));
                const bool cap = m->hk_capturing && m->capture_hk == (LngHotkey)h;
                const char* lbl = cap ? "[ press... ]"
                                : m->hotkeys[h][0] ? m->hotkeys[h] : "(unbound)";
                if (cap) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
                if (ImGui::Button(lbl, ImVec2(px(130), 0)))
                    launcher_model_begin_hk_capture(m, (LngHotkey)h);
                if (cap) ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
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
        if (ImGui::Button("Reset to Defaults")) launcher_binds_reset_player(m, m->cfg_player + 1);
        if (m->capturing) ImGui::TextColored(col(th.warn), "Listening... (Esc cancels)");
    } end_panel();
}

// Footer: a fixed-height band with the neon divider pinned to its TOP and the
// CTA vertically centred inside it. Laid out from an explicit origin (not the
// running cursor) so it is pixel-identical on every view and the CTA's glow
// always has clearance below the divider — Settings has less body content, and
// a cursor-relative footer let the glow ride up into the rule.
void draw_footer(LauncherModel* m, const LauncherTheme& th, float footer_h) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  fullw  = ImGui::GetContentRegionAvail().x;
    const float  play_w = px(210), play_h = px(46);

    // divider at the very top of the band
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        origin, ImVec2(origin.x + fullw, origin.y + px(1.5f)),
        imcol(th.border, 0.2f), imcol(th.accent, 0.7f),
        imcol(th.accent, 0.7f), imcol(th.border, 0.2f));

    // CTA centred in the remaining band height (glow clears the rule on both sides)
    const float band_y = origin.y + px(1.5f);
    const float band_h = footer_h - px(1.5f);
    const float cta_y  = band_y + (band_h - play_h) * 0.5f;

    const ImVec2 win = ImGui::GetWindowPos();
    if (m->view == LNG_VIEW_DASHBOARD) {
        bool skip = m->s.skip_launcher != 0;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, cta_y + (play_h - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::Checkbox("Skip launcher on boot", &skip))
            launcher_model_request_skip_toggle(m);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x + fullw - play_w, cta_y));
    if (neon_cta("##play", "PLAY", ImVec2(play_w, play_h)))
        m->action = LNG_ACTION_LAUNCH;
    ImGui::SetItemDefaultFocus();   // gamepad/keyboard start on the primary action
    (void)win;
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
    // CRT ground + scanlines behind everything.
    draw_crt_background(vp->Pos, vp->Size);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));   // let CRT show
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();

    // ---- Marquee header: brand · GAME TITLE · subtitle .......... [nav] ----
    ImVec2 hp = ImGui::GetCursorScreenPos();
    image_fit(g_brand, 44, 33); ImGui::SameLine(0, px(12));
    ImGui::BeginGroup();
        ImGui::SetWindowFontScale(1.55f);
        ImGui::TextUnformatted(m->game_name);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
        ImGui::TextUnformatted("SUPER NINTENDO");
        ImGui::PopStyleColor();
    ImGui::EndGroup();
    {   // right-aligned nav button
        const char* label = (m->view == LNG_VIEW_DASHBOARD) ? "Settings" : "< Back";
        const float w = px(110.0f);
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6.0f));
        if (ImGui::Button(label, ImVec2(w, px(34)))) {
            launcher_model_set_view(m, m->view == LNG_VIEW_DASHBOARD
                                        ? LNG_VIEW_SETTINGS : LNG_VIEW_DASHBOARD);
        }
    }
    // marquee underline: neon gradient rule under the header
    ImGui::Dummy(ImVec2(0, px(8.0f)));
    {
        ImVec2 u = ImGui::GetCursorScreenPos();
        float fw = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilledMultiColor(u, ImVec2(u.x + fw, u.y + px(2.0f)),
            imcol(th.accent, 0.9f), imcol(th.accent, 0.15f),
            imcol(th.accent, 0.15f), imcol(th.accent, 0.9f));
        glow_rect(dl, u, ImVec2(u.x + fw*0.5f, u.y + px(2.0f)), 0, th.accent, 0.5f, 3);
    }
    ImGui::Dummy(ImVec2(0, px(12.0f)));
    (void)hp;

    // Body: fixed-height child that scrolls when content overflows, so nothing
    // is ever clipped out of reach. The footer below stays fixed (in the fold).
    const float footer_h = px(92.0f);   // divider + clearance + CTA + its glow
    float body_h = ImGui::GetContentRegionAvail().y - footer_h;
    if (body_h < px(80.0f)) body_h = px(80.0f);
    begin_container("body", ImVec2(0, body_h));
    switch (m->view) {
        case LNG_VIEW_DASHBOARD:  draw_dashboard(m, th, logical_w); break;
        case LNG_VIEW_SETTINGS:   draw_settings(m, th);             break;
        case LNG_VIEW_CONTROLLER: draw_controller(m, th);           break;
    }
    end_container();

    draw_footer(m, th, footer_h);
    draw_skip_modal(m);
    ImGui::End();
    (void)logical_h;
}

bool is_modifier_scancode(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL ||
           sc == SDL_SCANCODE_LALT  || sc == SDL_SCANCODE_RALT  ||
           sc == SDL_SCANCODE_LSHIFT|| sc == SDL_SCANCODE_RSHIFT ||
           sc == SDL_SCANCODE_LGUI  || sc == SDL_SCANCODE_RGUI;
}

// Keyboard capture for the rebind editors. Player buttons persist a SCANCODE to
// keybinds.ini; system hotkeys persist a KEYCODE+mods to config.ini [KeyMap].
bool try_capture(LauncherModel* m, const SDL_Event& ev) {
    if (!m->capturing && !m->hk_capturing) return false;
    if (ev.type != SDL_EVENT_KEY_DOWN) return true;   // swallow input while capturing
    if (LNG_EVKEY(ev) == SDLK_ESCAPE) {
        launcher_model_cancel_capture(m);
        launcher_model_cancel_hk_capture(m);
        return true;
    }
    if (m->capturing) {
        launcher_binds_set_button(m, m->cfg_player + 1, m->capture_btn, (int)LNG_EVSCAN(ev));
        launcher_model_cancel_capture(m);
        return true;
    }
    // hotkey capture: wait past a bare modifier press for the real key
    if (is_modifier_scancode((SDL_Scancode)LNG_EVSCAN(ev))) return true;
    launcher_binds_set_hotkey(m, m->capture_hk, (int)LNG_EVKEY(ev), (int)LNG_EVMOD(ev));
    launcher_model_cancel_hk_capture(m);
    return true;
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
    // Test hook: force the focus ring always-on so scripted screenshots can
    // verify nav rendering without a physical pad. Off => normal auto behaviour
    // (ring appears on pad/keyboard, hides on mouse).
    if (const char* nv = SDL_getenv("LNG_NAV_ALWAYS"); nv && nv[0] == '1')
        io.ConfigNavCursorVisibleAlways = true;

    g_th = th;
    LNG_ImplSDL_InitForOpenGL(p->window, p->gl);
    ImGui_ImplOpenGL3_Init("#version 330");

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
            LNG_ImplSDL_ProcessEvent(&ev);
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

        ImGui_ImplOpenGL3_NewFrame();
        LNG_ImplSDL_NewFrame();
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
    LNG_ImplSDL_Shutdown();
    ImGui::DestroyContext();

    if (p->should_quit && m->action == LNG_ACTION_NONE) m->action = LNG_ACTION_QUIT;
    return m->action;
}
