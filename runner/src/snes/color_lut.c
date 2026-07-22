// color_lut.c — see color_lut.h.
//
// Screen models aligned with mstan/psxrecomp revision
// d7815862e18ef939e5e6e5c6947f8c29667982d5 (PolyForm Noncommercial
// 1.0.0). Color-science lineage: JRickey/gba-recomp crates/screen,
// © Jrickey, MIT OR Apache-2.0. See third_party/psxrecomp_color_lut/.

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
// A late near-sRGB Trinitron-class tube, matching PSXRecomp's shared model.
static const Primaries kTrinitron = {{0.625, 0.340}, {0.280, 0.595}, {0.155, 0.070}, {0.3127, 0.3290}};

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

typedef struct {
  Primaries primaries;
  double gamma;
  double luminance;
  double black_floor;
} PanelModel;

static int panel_model(int kind, PanelModel* out) {
  switch (kind) {
    case SNES_SCREEN_CRT:
      out->primaries = kSmpteC;
      out->gamma = 2.4;
      out->luminance = 0.92;
      out->black_floor = 0.004;
      return 1;
    case SNES_SCREEN_COMPOSITE:
      out->primaries = kSmpteC;
      out->gamma = 2.5;
      out->luminance = 0.90;
      out->black_floor = 0.012;
      return 1;
    case SNES_SCREEN_TRINITRON:
      out->primaries = kTrinitron;
      out->gamma = 2.35;
      out->luminance = 0.95;
      out->black_floor = 0.002;
      return 1;
    default:
      return 0;
  }
}

// ── State ──────────────────────────────────────────────────────────
static uint32_t* g_lut = NULL;  // 32768 entries, BGR555 -> 0x00RRGGBB
static int g_active = 0;

static int build(int kind) {
  if (!g_lut) g_lut = (uint32_t*)malloc(32768u * sizeof(uint32_t));
  if (!g_lut) { g_active = 0; return 0; }
  PanelModel model;
  if (!panel_model(kind, &model)) return 0;
  Mat3 to_disp = rgb_to_rgb(&model.primaries, &kSrgb);
  for (int px = 0; px < 32768; ++px) {
    double c[3] = {(px & 31) / 31.0, ((px >> 5) & 31) / 31.0, ((px >> 10) & 31) / 31.0};
    double lin[3];
    for (int i = 0; i < 3; ++i) {
      double v = pow(c[i], model.gamma) * model.luminance;
      lin[i] = v > 1.0 ? 1.0 : v;
    }
    double out[3]; mat_apply(&to_disp, lin, out);
    for (int i = 0; i < 3; ++i) {
      double v = out[i] < 0.0 ? 0.0 : (out[i] > 1.0 ? 1.0 : out[i]);
      out[i] = srgb_oetf(model.black_floor +
                         (1.0 - model.black_floor) * v);
    }
    uint8_t r = quant(out[0]);
    uint8_t g = quant(out[1]);
    uint8_t b = quant(out[2]);
    g_lut[px] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
  }
  return 1;
}

int snes_color_lut_kind_from_name(const char* name, int* kind) {
  if (!name || !kind) return 0;
  if (strcmp(name, "raw") == 0) *kind = SNES_SCREEN_RAW;
  else if (strcmp(name, "crt") == 0) *kind = SNES_SCREEN_CRT;
  else if (strcmp(name, "composite") == 0) *kind = SNES_SCREEN_COMPOSITE;
  else if (strcmp(name, "trinitron") == 0) *kind = SNES_SCREEN_TRINITRON;
  else return 0;
  return 1;
}

const char* snes_color_lut_kind_name(int kind) {
  static const char* const names[SNES_SCREEN_KIND_COUNT] = {
      "Raw", "CRT", "Composite", "Trinitron"};
  return kind >= 0 && kind < SNES_SCREEN_KIND_COUNT ? names[kind] : names[0];
}

int snes_color_lut_setup_kind(int kind) {
  g_active = 0;
  if (kind == SNES_SCREEN_RAW) return 0;
  if (kind <= SNES_SCREEN_RAW || kind >= SNES_SCREEN_KIND_COUNT) return -1;
  if (!build(kind)) return -1;
  g_active = 1;
  return 1;
}

int snes_color_lut_setup(void) {
  const char* e = getenv("SNESRECOMP_SCREEN");
  if (!e || strcmp(e, "raw") == 0 || e[0] == '\0') return 0;  // passthrough
  int kind = SNES_SCREEN_RAW;
  if (!snes_color_lut_kind_from_name(e, &kind)) return 0;  // compatibility
  return snes_color_lut_setup_kind(kind) > 0;
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
