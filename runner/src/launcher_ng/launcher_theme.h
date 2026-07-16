// launcher_theme.h — shared design tokens for the launcher.
//
// One source of truth for color / spacing / radius / type, expressed in LOGICAL
// units. Each backend multiplies the pixel-affecting values by the platform
// display_scale so the look is identical at 100%, 125%, 150%, 175% and across
// backends. Keeping tokens here (not baked into a backend) is what lets a
// future toolkit swap reuse the same visual language.

#ifndef LAUNCHER_NG_THEME_H
#define LAUNCHER_NG_THEME_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float r, g, b, a; } LngColor;

typedef struct {
    LngColor background;
    LngColor panel;
    LngColor panel_hovered;
    LngColor control;         // button/input fill — MUST contrast with panel
    LngColor control_hovered;
    LngColor border;          // panel + control outline
    LngColor accent;
    LngColor accent_text;
    LngColor text;
    LngColor text_muted;
    LngColor good;        // verified / success
    LngColor warn;        // unverified / caution
    LngColor focus_ring;  // gamepad/keyboard focus outline

    // logical dimensions (unscaled)
    float spacing_xs, spacing_sm, spacing_md, spacing_lg;
    float radius_sm, radius_lg;
    float row_height;
    float font_body, font_title, font_small;
    float focus_ring_width;
} LauncherTheme;

static inline LngColor lng_rgba(float r, float g, float b, float a) {
    LngColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}

// The default dark theme. Deliberately not the ImGui default gray — a launcher
// must not look like a debug tool.
static inline LauncherTheme launcher_theme_default(void) {
    // Palette mirrors the shipping RmlUi launcher (theme.rcss) so the new UI
    // reads as the same product. Panels sit darker than controls: a button that
    // matches its panel is invisible.
    LauncherTheme t;
    t.background      = lng_rgba(0.043f, 0.055f, 0.078f, 1.0f); // #0b0e14 page
    t.panel           = lng_rgba(0.067f, 0.082f, 0.114f, 1.0f); // #11151d card
    t.panel_hovered   = lng_rgba(0.133f, 0.169f, 0.231f, 1.0f); // #222b3b
    t.control         = lng_rgba(0.102f, 0.129f, 0.180f, 1.0f); // #1a212e button
    t.control_hovered = lng_rgba(0.133f, 0.169f, 0.231f, 1.0f); // #222b3b
    t.border          = lng_rgba(0.169f, 0.204f, 0.275f, 1.0f); // #2b3446
    t.accent          = lng_rgba(0.486f, 0.227f, 0.929f, 1.0f); // #7c3aed
    t.accent_text     = lng_rgba(1.0f, 1.0f, 1.0f, 1.0f);
    t.text            = lng_rgba(0.902f, 0.914f, 0.937f, 1.0f); // #e6e9ef
    t.text_muted      = lng_rgba(0.545f, 0.576f, 0.639f, 1.0f); // #8b93a7
    t.good            = lng_rgba(0.247f, 0.725f, 0.314f, 1.0f); // #3fb950
    t.warn            = lng_rgba(0.941f, 0.533f, 0.243f, 1.0f); // #f0883e
    t.focus_ring      = lng_rgba(0.655f, 0.545f, 0.980f, 1.0f); // #a78bfa

    t.spacing_xs = 4.0f;  t.spacing_sm = 8.0f;
    t.spacing_md = 16.0f; t.spacing_lg = 24.0f;
    t.radius_sm  = 8.0f;  t.radius_lg  = 16.0f;
    t.row_height = 44.0f;                      // large rows: Steam Deck friendly
    t.font_body  = 18.0f; t.font_title = 30.0f; t.font_small = 14.0f;
    t.focus_ring_width = 2.5f;
    return t;
}

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_THEME_H
