// color_lut.c — see color_lut.h.
//
// Color-science core ported from gbarecomp (src/runtime/color_lut.cpp) via
// JRickey/gba-recomp crates/screen, © Jrickey, MIT OR Apache-2.0.

#include "color_lut.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ── CIE colorimetry (build-time only) ──────────────────────────────
typedef struct { double x, y; } Xy;
typedef struct { Xy red, green, blue, white; } Primaries;
typedef struct { double m[3][3]; } Mat3;

// White point D65 = {0.3127, 0.3290}, inlined in each Primaries below.
static const Primaries kSrgb = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}};
// SMPTE-C / NTSC consumer-CRT phosphors (the standard model — not a
// per-console SNES measurement).
static const Primaries kSmpteC = {{0.630, 0.340}, {0.310, 0.595}, {0.155, 0.070}, {0.3127, 0.3290}};
// A cooler/wider Trinitron-ish set, also standard-derived.
static const Primaries kTrinitron = {{0.621, 0.340}, {0.281, 0.606}, {0.152, 0.067}, {0.3127, 0.3290}};

static void mat_apply(const Mat3* a, const double v[3], double out[3]) {
  for (int i = 0; i < 3; ++i)
    out[i] = a->m[i][0] * v[0] + a->m[i][1] * v[1] + a->m[i][2] * v[2];
}
static Mat3 mat_mul(const Mat3* a, const Mat3* b) {
  Mat3 r; memset(&r, 0, sizeof(r));
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) r.m[i][j] += a->m[i][k] * b->m[k][j];
  return r;
}
static Mat3 mat_inverse(const Mat3* a) {
  Mat3 o;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      int r1 = (j + 1) % 3, r2 = (j + 2) % 3, c1 = (i + 1) % 3, c2 = (i + 2) % 3;
      o.m[i][j] = a->m[r1][c1] * a->m[r2][c2] - a->m[r1][c2] * a->m[r2][c1];
    }
  }
  double det = a->m[0][0] * o.m[0][0] + a->m[0][1] * o.m[1][0] + a->m[0][2] * o.m[2][0];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o.m[i][j] /= det;
  return o;
}
static void xy_to_xyz(Xy c, double out[3]) {
  out[0] = c.x / c.y; out[1] = 1.0; out[2] = (1.0 - c.x - c.y) / c.y;
}
static Mat3 rgb_to_xyz(const Primaries* p) {
  double r[3], g[3], b[3], w[3];
  xy_to_xyz(p->red, r); xy_to_xyz(p->green, g); xy_to_xyz(p->blue, b);
  xy_to_xyz(p->white, w);
  Mat3 m = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
  Mat3 mi = mat_inverse(&m);
  double s[3]; mat_apply(&mi, w, s);
  Mat3 out = m;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out.m[i][j] *= s[j];
  return out;
}
static Mat3 rgb_to_rgb(const Primaries* src, const Primaries* dst) {
  Mat3 to = rgb_to_xyz(src);
  Mat3 dx = rgb_to_xyz(dst);
  Mat3 from = mat_inverse(&dx);
  return mat_mul(&from, &to);  // src.white == dst.white (both D65)
}
static double srgb_oetf(double v) {
  if (v <= 0.0) return 0.0;
  if (v >= 1.0) return 1.0;
  return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}
static uint8_t quant(double v) {
  if (v < 0.0) v = 0.0;
  if (v > 1.0) v = 1.0;
  return (uint8_t)(v * 255.0 + 0.5);
}

// ── State ──────────────────────────────────────────────────────────
static uint32_t* g_lut = NULL;  // 32768 entries, BGR555 -> 0x00RRGGBB
static int g_active = 0;

static void build(const Primaries* panel, double gamma) {
  if (!g_lut) g_lut = (uint32_t*)malloc(32768u * sizeof(uint32_t));
  if (!g_lut) { g_active = 0; return; }
  Mat3 to_disp = rgb_to_rgb(panel, &kSrgb);
  for (int px = 0; px < 32768; ++px) {
    double c[3] = {(px & 31) / 31.0, ((px >> 5) & 31) / 31.0, ((px >> 10) & 31) / 31.0};
    double lin[3];
    for (int i = 0; i < 3; ++i) lin[i] = pow(c[i], gamma);
    double out[3]; mat_apply(&to_disp, lin, out);
    uint8_t r = quant(srgb_oetf(out[0]));
    uint8_t g = quant(srgb_oetf(out[1]));
    uint8_t b = quant(srgb_oetf(out[2]));
    g_lut[px] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
  }
}

int snes_color_lut_setup(void) {
  g_active = 0;
  const char* e = getenv("SNESRECOMP_SCREEN");
  if (!e || strcmp(e, "raw") == 0 || e[0] == '\0') return 0;  // passthrough
  if (strcmp(e, "crt") == 0)            build(&kSmpteC, 2.2);
  else if (strcmp(e, "trinitron") == 0) build(&kTrinitron, 2.2);
  else return 0;  // unknown -> raw
  g_active = (g_lut != NULL);
  return g_active;
}

int snes_color_lut_active(void) { return g_active; }

void snes_color_lut_map(const uint32_t* src, uint32_t* dst, size_t n) {
  if (!g_active || !g_lut) {  // safety: identity
    if (src != dst) memcpy(dst, src, n * sizeof(uint32_t));
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    uint32_t px = src[i];
    // 0x00RRGGBB -> recover 5-bit per channel -> BGR555 index (R low).
    uint32_t idx = ((px >> 19) & 31) | (((px >> 11) & 31) << 5) | (((px >> 3) & 31) << 10);
    dst[i] = (px & 0xff000000u) | g_lut[idx];
  }
}
