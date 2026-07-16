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
    LngColor background;      // ink — the CRT ground
    LngColor background2;     // slightly lifted ground for the vignette center
    LngColor panel;
    LngColor panel_hovered;
    LngColor control;         // button/input fill — MUST contrast with panel
    LngColor control_hovered;
    LngColor border;          // panel + control outline
    LngColor accent;          // the one bold place: brand + primary CTA
    LngColor accent_dim;      // gradient partner / pressed
    LngColor accent_text;
    LngColor text;
    LngColor text_muted;
    LngColor good;        // verified / connected (phosphor mint)
    LngColor warn;        // unverified / caution (amber)
    LngColor focus_ring;  // gamepad/keyboard focus outline (cyan, sparingly)
    LngColor scanline;    // CRT scanline overlay (very low alpha)

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

// "CRT Console Boot Screen" theme. A cinematic dark retro-console look: a
// violet-biased near-black ground (chosen, not a default grey), ONE bold neon
// accent (electric violet) reserved for brand + primary action, phosphor-mint
// and amber for state only, cyan for focus. Boldness spent in one place; the
// rest kept quiet.
static inline LauncherTheme launcher_theme_default(void) {
    LauncherTheme t;
    t.background      = lng_rgba(0.039f, 0.051f, 0.086f, 1.0f); // #0A0D16 ink
    t.background2     = lng_rgba(0.071f, 0.090f, 0.145f, 1.0f); // #121725 vignette center
    t.panel           = lng_rgba(0.078f, 0.102f, 0.157f, 1.0f); // #141A28 card
    t.panel_hovered   = lng_rgba(0.125f, 0.165f, 0.243f, 1.0f); // #202A3E
    t.control         = lng_rgba(0.106f, 0.137f, 0.208f, 1.0f); // #1B2335 button
    t.control_hovered = lng_rgba(0.145f, 0.188f, 0.278f, 1.0f); // #253047
    t.border          = lng_rgba(0.169f, 0.208f, 0.314f, 1.0f); // #2B3550 hairline
    t.accent          = lng_rgba(0.604f, 0.361f, 1.000f, 1.0f); // #9A5CFF electric violet
    t.accent_dim      = lng_rgba(0.431f, 0.247f, 0.812f, 1.0f); // #6E3FCF gradient/pressed
    t.accent_text     = lng_rgba(1.0f, 1.0f, 1.0f, 1.0f);
    t.text            = lng_rgba(0.925f, 0.933f, 0.965f, 1.0f); // #ECEEF6
    t.text_muted      = lng_rgba(0.529f, 0.565f, 0.659f, 1.0f); // #8790A8
    t.good            = lng_rgba(0.275f, 0.890f, 0.608f, 1.0f); // #46E39B phosphor mint
    t.warn            = lng_rgba(0.961f, 0.698f, 0.235f, 1.0f); // #F5B23C amber
    t.focus_ring      = lng_rgba(0.220f, 0.882f, 0.902f, 1.0f); // #38E1E6 cyan
    t.scanline        = lng_rgba(0.0f, 0.0f, 0.0f, 0.18f);      // CRT scanline

    t.spacing_xs = 4.0f;  t.spacing_sm = 8.0f;
    t.spacing_md = 16.0f; t.spacing_lg = 24.0f;
    t.radius_sm  = 6.0f;  t.radius_lg  = 14.0f;
    t.row_height = 44.0f;                      // large rows: Steam Deck friendly
    t.font_body  = 18.0f; t.font_title = 34.0f; t.font_small = 13.0f;
    t.focus_ring_width = 2.5f;
    return t;
}

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_THEME_H
