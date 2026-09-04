/* Unit tests for runner/src/host_mesh.{c,h}.
 *
 * Builds small N64MESHB blobs in memory, checks the strict parser rejects
 * malformed input, and renders a two-limb textured model through a simple
 * pinhole projection to verify Z-buffering, culling, texture wrap, limb
 * hierarchy, overrides, supersampling and deterministic output.
 */
#include "host_mesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

/* ---- tiny blob writer ------------------------------------------------- */
typedef struct Buf {
  uint8_t *data;
  size_t size, cap;
} Buf;

static void buf_put(Buf *b, const void *p, size_t n) {
  if (b->size + n > b->cap) {
    b->cap = (b->size + n) * 2 + 64;
    b->data = (uint8_t *)realloc(b->data, b->cap);
  }
  memcpy(b->data + b->size, p, n);
  b->size += n;
}
static void put_u32(Buf *b, uint32_t v) {
  uint8_t p[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  buf_put(b, p, 4);
}
static void put_i32(Buf *b, int32_t v) { put_u32(b, (uint32_t)v); }
static void put_u16(Buf *b, uint16_t v) {
  uint8_t p[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  buf_put(b, p, 2);
}
static void put_f32(Buf *b, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  put_u32(b, u);
}
static void put_name(Buf *b, const char *name) {
  char n[HOST_MESH_NAME_LEN];
  memset(n, 0, sizeof(n));
  snprintf(n, sizeof(n), "%s", name);
  buf_put(b, n, sizeof(n));
}
static void put_vertex(Buf *b, float x, float y, float z, float u, float v,
                       int nx, int ny, int nz, uint32_t rgba) {
  put_f32(b, x);
  put_f32(b, y);
  put_f32(b, z);
  put_f32(b, u);
  put_f32(b, v);
  uint8_t n[4] = {(uint8_t)(int8_t)nx, (uint8_t)(int8_t)ny, (uint8_t)(int8_t)nz,
                  0};
  buf_put(b, n, 4);
  uint8_t c[4] = {(uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16),
                  (uint8_t)(rgba >> 8), (uint8_t)rgba};
  buf_put(b, c, 4);
}

typedef struct TestTri {
  uint32_t material;
  float v[3][3];
  float uv[3][2];
  int n[3];
  uint32_t rgba;
} TestTri;

typedef struct TestModel {
  /* textures */
  int tex_count;
  uint16_t tex_w, tex_h;
  uint8_t wrap_s, wrap_t, mask_s, mask_t;
  const uint32_t *tex_pixels;
  /* materials */
  int mat_count;
  uint32_t mat_flags[4];
  int32_t mat_tex[4];
  /* triangles / lists / limbs */
  int tri_count;
  const TestTri *tris;
  int dl_count;
  const char *dl_names[4];
  uint32_t dl_first[4], dl_count_tris[4];
  int limb_count;
  int32_t limb_parent[4], limb_dl[4];
  float limb_trans[4][3], limb_rot[4][3];
  int pose_count;
  const char *pose_name;
  float pose_rot[4][3];
  /* tampering knobs */
  uint32_t total_override;
} TestModel;

static void build_blob(const TestModel *m, Buf *out) {
  Buf body = {0};
  /* Layout after the 84-byte header: tex, mat, tri, dl, limb, pose, pixels,
   * pose data. We write into `body` and patch offsets. */
  const uint32_t hdr = 84;
  uint32_t off[8];
  off[0] = hdr + (uint32_t)body.size;
  for (int i = 0; i < m->tex_count; i++) {
    put_u16(&body, m->tex_w);
    put_u16(&body, m->tex_h);
    uint8_t w[4] = {m->wrap_s, m->wrap_t, m->mask_s, m->mask_t};
    buf_put(&body, w, 4);
    put_u32(&body, (uint32_t)(i * m->tex_w * m->tex_h));
  }
  off[1] = hdr + (uint32_t)body.size;
  for (int i = 0; i < m->mat_count; i++) {
    put_u32(&body, m->mat_flags[i]);
    put_i32(&body, m->mat_tex[i]);
    uint8_t prim[4] = {255, 255, 255, 255}, env[4] = {255, 255, 255, 255};
    buf_put(&body, prim, 4);
    buf_put(&body, env, 4);
  }
  off[2] = hdr + (uint32_t)body.size;
  for (int i = 0; i < m->tri_count; i++) {
    const TestTri *t = &m->tris[i];
    put_u32(&body, t->material);
    for (int k = 0; k < 3; k++)
      put_vertex(&body, t->v[k][0], t->v[k][1], t->v[k][2], t->uv[k][0],
                 t->uv[k][1], t->n[0], t->n[1], t->n[2], t->rgba);
  }
  off[3] = hdr + (uint32_t)body.size;
  for (int i = 0; i < m->dl_count; i++) {
    put_name(&body, m->dl_names[i]);
    put_u32(&body, m->dl_first[i]);
    put_u32(&body, m->dl_count_tris[i]);
  }
  off[4] = hdr + (uint32_t)body.size;
  for (int i = 0; i < m->limb_count; i++) {
    put_i32(&body, m->limb_parent[i]);
    put_i32(&body, m->limb_dl[i]);
    for (int k = 0; k < 3; k++) put_f32(&body, m->limb_trans[i][k]);
    for (int k = 0; k < 3; k++) put_f32(&body, m->limb_rot[i][k]);
  }
  off[5] = hdr + (uint32_t)body.size;
  for (int i = 0; i < m->pose_count; i++) {
    put_name(&body, m->pose_name);
    put_f32(&body, 0.0f);
    put_f32(&body, 0.0f);
    put_f32(&body, 0.0f);
    put_u32(&body, 0);
  }
  off[6] = hdr + (uint32_t)body.size;
  const uint32_t pix_size =
      (uint32_t)(m->tex_count * m->tex_w * m->tex_h * 4);
  for (int i = 0; i < m->tex_count * m->tex_w * m->tex_h; i++)
    put_u32(&body, m->tex_pixels[i % (m->tex_w * m->tex_h)]);
  off[7] = hdr + (uint32_t)body.size;
  const uint32_t pose_size =
      m->pose_count ? (uint32_t)(m->limb_count * 3 * 4) : 0;
  for (int i = 0; i < (m->pose_count ? m->limb_count : 0); i++)
    for (int k = 0; k < 3; k++) put_f32(&body, m->pose_rot[i][k]);
  const uint32_t total = hdr + (uint32_t)body.size;

  buf_put(out, HOST_MESH_MAGIC, 8);
  put_u32(out, HOST_MESH_FORMAT_VERSION);
  put_u32(out, (uint32_t)m->tex_count);
  put_u32(out, (uint32_t)m->mat_count);
  put_u32(out, (uint32_t)m->tri_count);
  put_u32(out, (uint32_t)m->dl_count);
  put_u32(out, (uint32_t)m->limb_count);
  put_u32(out, (uint32_t)m->pose_count);
  put_f32(out, 1.0f);
  for (int i = 0; i < 6; i++) put_u32(out, off[i]);
  put_u32(out, off[6]);
  put_u32(out, pix_size);
  put_u32(out, off[7]);
  put_u32(out, pose_size);
  put_u32(out, m->total_override ? m->total_override : total);
  buf_put(out, body.data, body.size);
  free(body.data);
}

/* ---- projection ------------------------------------------------------- */
typedef struct Pinhole {
  float focal, cx, cy;
} Pinhole;
static void pinhole(void *ctx, float x, float y, float z, float *sx, float *sy) {
  const Pinhole *p = (const Pinhole *)ctx;
  *sx = p->cx + x * p->focal / z;
  *sy = p->cy - y * p->focal / z;
}

/* ---- fixtures ---------------------------------------------------------- */
/* 4x4 checker texture: red/blue. */
static const uint32_t kChecker[16] = {
    0xffff0000u, 0xff0000ffu, 0xffff0000u, 0xff0000ffu,
    0xff0000ffu, 0xffff0000u, 0xff0000ffu, 0xffff0000u,
    0xffff0000u, 0xff0000ffu, 0xffff0000u, 0xff0000ffu,
    0xff0000ffu, 0xffff0000u, 0xff0000ffu, 0xffff0000u,
};

/* Two quads: material 0 = textured green-lit (front, z=10), material 1 =
 * flat white (back, z=20, larger). Front quad CCW seen from -z looking +z.
 * Camera looks down +z, x right, y up: CCW in (x,y) with y up. */
static const TestTri kTris[] = {
    /* front quad (limb 1's list), at local z = 0 */
    {0, {{-2, -2, 0}, {2, -2, 0}, {2, 2, 0}}, {{0, 0}, {8, 0}, {8, 8}},
     {0, 0, -127}, 0xffffffffu},
    {0, {{-2, -2, 0}, {2, 2, 0}, {-2, 2, 0}}, {{0, 0}, {8, 8}, {0, 8}},
     {0, 0, -127}, 0xffffffffu},
    /* back quad (limb 0's list), flat white, at local z = 0 */
    {1, {{-6, -6, 0}, {6, -6, 0}, {6, 6, 0}}, {{0, 0}, {0, 0}, {0, 0}},
     {0, 0, -127}, 0xffffffffu},
    {1, {{-6, -6, 0}, {6, 6, 0}, {-6, 6, 0}}, {{0, 0}, {0, 0}, {0, 0}},
     {0, 0, -127}, 0xffffffffu},
};

static TestModel base_model(void) {
  TestModel m;
  memset(&m, 0, sizeof(m));
  m.tex_count = 1;
  m.tex_w = 4;
  m.tex_h = 4;
  m.wrap_s = HOST_MESH_WRAP_REPEAT;
  m.wrap_t = HOST_MESH_WRAP_REPEAT;
  m.tex_pixels = kChecker;
  m.mat_count = 2;
  m.mat_flags[0] = HOST_MESH_MAT_TEXTURE | HOST_MESH_MAT_SHADE |
                   HOST_MESH_MAT_LIGHTING | HOST_MESH_MAT_CULL_BACK;
  m.mat_tex[0] = 0;
  m.mat_flags[1] = HOST_MESH_MAT_SHADE; /* vertex colour white */
  m.mat_tex[1] = -1;
  m.tri_count = 4;
  m.tris = kTris;
  m.dl_count = 2;
  m.dl_names[0] = "back";
  m.dl_first[0] = 2;
  m.dl_count_tris[0] = 2;
  m.dl_names[1] = "front";
  m.dl_first[1] = 0;
  m.dl_count_tris[1] = 2;
  m.limb_count = 2;
  m.limb_parent[0] = -1;
  m.limb_dl[0] = 0;
  m.limb_trans[0][2] = 20.0f; /* root at z = 20 */
  m.limb_parent[1] = 0;
  m.limb_dl[1] = 1;
  m.limb_trans[1][2] = -10.0f; /* child at z = 10 */
  m.pose_count = 1;
  m.pose_name = "bind";
  return m;
}

static HostMesh *load_model(const TestModel *m, const char **err) {
  Buf b = {0};
  build_blob(m, &b);
  HostMesh *mesh = host_mesh_load(b.data, b.size, err);
  free(b.data);
  return mesh;
}

static uint32_t px(const uint8_t *buf, int pitch, int x, int y) {
  const uint8_t *p = buf + y * pitch + x * 4;
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[1] << 8) | p[0];
}

static uint32_t hash_buf(const uint8_t *buf, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 16777619u;
  return h;
}

/* ---- tests ------------------------------------------------------------- */
static void test_parser(void) {
  const char *err = NULL;
  TestModel m = base_model();
  HostMesh *mesh = load_model(&m, &err);
  CHECK(mesh != NULL);
  if (!mesh) {
    printf("  load error: %s\n", err ? err : "?");
    return;
  }
  CHECK(mesh->limb_count == 2 && mesh->display_list_count == 2 &&
        mesh->triangle_count == 4 && mesh->texture_count == 1);
  CHECK(host_mesh_find_display_list(mesh, "front") == 1);
  CHECK(host_mesh_find_display_list(mesh, "nope") == -1);
  CHECK(host_mesh_find_pose(mesh, "bind") == 0);
  /* Bind bounds: back quad +-6 at z 20, front quad +-2 at z 10. */
  CHECK(fabsf(mesh->bounds_min.x + 6.0f) < 1e-4f &&
        fabsf(mesh->bounds_max.x - 6.0f) < 1e-4f);
  CHECK(fabsf(mesh->bounds_min.z - 10.0f) < 1e-4f &&
        fabsf(mesh->bounds_max.z - 20.0f) < 1e-4f);
  HostMeshVec3 mn, mx;
  CHECK(host_mesh_display_list_bounds(mesh, 1, &mn, &mx) &&
        fabsf(mn.x + 2.0f) < 1e-4f && fabsf(mx.y - 2.0f) < 1e-4f);
  host_mesh_free(mesh);

  /* Rejections. */
  Buf b = {0};
  build_blob(&m, &b);
  CHECK(host_mesh_load(b.data, b.size - 1, &err) == NULL && err);
  b.data[0] = 'X';
  CHECK(host_mesh_load(b.data, b.size, &err) == NULL);
  b.data[0] = 'N';
  free(b.data);

  TestModel bad = base_model();
  bad.limb_parent[1] = 1; /* self parent */
  CHECK(load_model(&bad, &err) == NULL);
  bad = base_model();
  bad.dl_first[1] = 3;
  bad.dl_count_tris[1] = 2; /* overruns triangles */
  CHECK(load_model(&bad, &err) == NULL);
  bad = base_model();
  bad.mat_tex[0] = 5;
  CHECK(load_model(&bad, &err) == NULL);
  bad = base_model();
  bad.total_override = 12345;
  CHECK(load_model(&bad, &err) == NULL);
  bad = base_model();
  bad.tex_w = 0;
  CHECK(load_model(&bad, &err) == NULL);
}

typedef struct OverrideCtx {
  int hide_limb;
  int swap_limb;
  int swap_to;
} OverrideCtx;

static int override_fn(void *ctx, int limb, int *dl, HostMeshVec3 *trans,
                       HostMeshVec3 *rot) {
  (void)trans;
  (void)rot;
  OverrideCtx *o = (OverrideCtx *)ctx;
  if (limb == o->hide_limb) return 0;
  if (limb == o->swap_limb) *dl = o->swap_to;
  return 1;
}

static void test_render(void) {
  const char *err = NULL;
  TestModel m = base_model();
  HostMesh *mesh = load_model(&m, &err);
  CHECK(mesh != NULL);
  if (!mesh) return;

  enum { W = 64, H = 64 };
  uint8_t *target = (uint8_t *)calloc(W * H * 4, 1);
  Pinhole cam = {32.0f, 32.0f, 32.0f};
  HostMeshDrawParams p;
  host_mesh_draw_params_init(&p);
  p.mesh = mesh;
  p.projection.project = pinhole;
  p.projection.ctx = &cam;
  p.projection.near_z = 1.0f;
  p.target = target;
  p.target_pitch = W * 4;
  p.target_width = W;
  p.target_height = H;
  p.light_dir[2] = -1.0f; /* toward camera */
  p.ambient = 0.25f;
  p.diffuse = 0.75f;
  p.transparent_black_target = 1;
  HostMeshDrawStats stats;
  p.stats = &stats;

  uint32_t written = host_mesh_draw(&p);
  if (written == 0)
    printf("  debug: limbs=%u submitted=%u rasterised=%u bbox=%d,%d..%d,%d\n",
           stats.limbs_drawn, stats.triangles_submitted,
           stats.triangles_rasterised, stats.bbox_min_x, stats.bbox_min_y,
           stats.bbox_max_x, stats.bbox_max_y);
  CHECK(written > 0);
  CHECK(stats.limbs_drawn == 2 && stats.triangles_submitted == 4);
  /* Back quad projects to +-6*32/20 = +-9.6 px; front to +-2*32/10 = +-6.4. */
  CHECK(stats.bbox_min_x >= 21 && stats.bbox_max_x <= 42);
  /* Centre pixel is the textured front quad: lit red/blue checker, never
   * white. Corner (32+8, 32) is the white back quad. */
  uint32_t centre = px(target, W * 4, 32, 32);
  uint32_t side = px(target, W * 4, 40, 32);
  CHECK((centre & 0x00ffffffu) != 0x00ffffffu && (centre >> 24) == 0xff);
  CHECK((side & 0x00ffffffu) == 0x00ffffffu);
  /* Texture: u/v span 8 texels across 4px-period checker -> red and blue
   * both present inside the front quad. */
  int saw_red = 0, saw_blue = 0;
  for (int y = 27; y <= 37; y++) {
    for (int x = 27; x <= 37; x++) {
      uint32_t c = px(target, W * 4, x, y);
      uint8_t r = (c >> 16) & 0xff, b = c & 0xff;
      if (r > 100 && b < 30) saw_red = 1;
      if (b > 100 && r < 30) saw_blue = 1;
    }
  }
  CHECK(saw_red && saw_blue);
  /* Lighting: normal faces camera (-z) with light toward -z -> full shade
   * 1.0 so red texels read 255. */
  int max_r = 0;
  for (int y = 27; y <= 37; y++)
    for (int x = 27; x <= 37; x++) {
      int r = (px(target, W * 4, x, y) >> 16) & 0xff;
      if (r > max_r) max_r = r;
    }
  CHECK(max_r == 255);
  const uint32_t h1 = hash_buf(target, W * H * 4);

  /* Determinism. */
  memset(target, 0, W * H * 4);
  host_mesh_draw(&p);
  CHECK(hash_buf(target, W * H * 4) == h1);

  /* Z-buffer: draw the front quad behind by moving the child limb to z=+10
   * (further than the back quad at 20 -> total 30): centre must be white. */
  OverrideCtx oc = {-1, -1, -1};
  p.override.fn = override_fn;
  p.override.ctx = &oc;
  memset(target, 0, W * H * 4);
  {
    TestModel far_m = base_model();
    far_m.limb_trans[1][2] = 10.0f;
    HostMesh *far_mesh = load_model(&far_m, &err);
    CHECK(far_mesh != NULL);
    HostMeshDrawParams pf = p;
    pf.mesh = far_mesh;
    host_mesh_draw(&pf);
    CHECK((px(target, W * 4, 32, 32) & 0x00ffffffu) == 0x00ffffffu);
    host_mesh_free(far_mesh);
  }

  /* Override: hide limb 1 -> centre white. */
  oc.hide_limb = 1;
  memset(target, 0, W * H * 4);
  host_mesh_draw(&p);
  CHECK(stats.limbs_drawn == 1);
  CHECK((px(target, W * 4, 32, 32) & 0x00ffffffu) == 0x00ffffffu);

  /* Override: swap limb 1 to the "back" list -> two large white quads. */
  oc.hide_limb = -1;
  oc.swap_limb = 1;
  oc.swap_to = 0;
  memset(target, 0, W * H * 4);
  host_mesh_draw(&p);
  CHECK((px(target, W * 4, 32, 32) & 0x00ffffffu) == 0x00ffffffu);
  CHECK(stats.bbox_max_x >= 45); /* back list at z=10 spans +-19 px */
  p.override.fn = NULL;

  /* Back-face culling: rotate child limb 180 deg about Y via a pose so the
   * textured quad faces away; with CULL_BACK it disappears -> centre white. */
  {
    TestModel rot_m = base_model();
    rot_m.pose_rot[1][1] = 180.0f;
    HostMesh *rot_mesh = load_model(&rot_m, &err);
    CHECK(rot_mesh != NULL);
    HostMeshDrawParams pr = p;
    pr.mesh = rot_mesh;
    pr.pose = 0;
    memset(target, 0, W * H * 4);
    host_mesh_draw(&pr);
    CHECK((px(target, W * 4, 32, 32) & 0x00ffffffu) == 0x00ffffffu);
    /* Flip winding: now the rotated quad counts as front-facing again. */
    pr.flip_winding = 1;
    memset(target, 0, W * H * 4);
    host_mesh_draw(&pr);
    /* The back quad (no cull flag) is drawn either way; textured front quad
     * now shows again but the back quad is also flipped... it has no cull
     * flag so it still draws. Centre must be non-white (checker). */
    CHECK((px(target, W * 4, 32, 32) & 0x00ffffffu) != 0x00ffffffu);
    host_mesh_free(rot_mesh);
  }

  /* Supersampling: edges get intermediate coverage against drawn content.
   * Output remains deterministic and covers the same bbox. */
  p.supersample = 3;
  memset(target, 0, W * H * 4);
  host_mesh_draw(&p);
  const uint32_t h3 = hash_buf(target, W * H * 4);
  memset(target, 0, W * H * 4);
  host_mesh_draw(&p);
  CHECK(hash_buf(target, W * H * 4) == h3);
  CHECK(h3 != h1);
  CHECK(stats.bbox_min_x >= 21 && stats.bbox_max_x <= 42);
  p.supersample = 1;

  /* Limb origin: child sits at camera z = 10 on the axis. */
  float origin[3];
  CHECK(host_mesh_limb_origin(&p, 1, origin) && fabsf(origin[2] - 10.0f) < 1e-4f &&
        fabsf(origin[0]) < 1e-4f);

  /* Clip rectangle restricts output. */
  p.clip_x = 0;
  p.clip_y = 0;
  p.clip_w = 32;
  p.clip_h = 64;
  memset(target, 0, W * H * 4);
  host_mesh_draw(&p);
  CHECK(stats.bbox_max_x <= 31);
  CHECK(px(target, W * 4, 33, 32) == 0);

  /* Near plane: pull the whole model to straddle z=near; must not crash and
   * must still draw something. */
  p.clip_w = 0;
  p.model_to_camera[11] = -9.5f; /* root now at z=10.5, child at 0.5 */
  memset(target, 0, W * H * 4);
  CHECK(host_mesh_draw(&p) > 0);

  free(target);
  host_mesh_free(mesh);
}

static void test_texture_wrap(void) {
  /* Mirror + clamp behaviours through a single fullscreen quad. */
  const char *err = NULL;
  TestModel m = base_model();
  m.wrap_s = HOST_MESH_WRAP_CLAMP;
  m.wrap_t = HOST_MESH_WRAP_CLAMP;
  HostMesh *mesh = load_model(&m, &err);
  CHECK(mesh != NULL);
  if (!mesh) return;
  enum { W = 32, H = 32 };
  uint8_t *target = (uint8_t *)calloc(W * H * 4, 1);
  Pinhole cam = {16.0f, 16.0f, 16.0f};
  HostMeshDrawParams p;
  host_mesh_draw_params_init(&p);
  p.mesh = mesh;
  p.projection.project = pinhole;
  p.projection.ctx = &cam;
  p.target = target;
  p.target_pitch = W * 4;
  p.target_width = W;
  p.target_height = H;
  p.ambient = 1.0f;
  OverrideCtx oc = {0, -1, -1}; /* hide back quad */
  p.override.fn = override_fn;
  p.override.ctx = &oc;
  host_mesh_draw(&p);
  /* With clamp, u in 4..8 clamps to the last column: right half of the quad
   * is a single colour column pattern (rows alternate). Sample two pixels
   * in the right half on the same row: identical. */
  /* Quad spans x 12.8..19.2 px; u = 8*(x-12.8)/6.4: pixel 17 -> u 5.9,
   * pixel 18 -> u 7.1. Repeat would give columns 1 and 3; clamp gives 3,3. */
  uint32_t a = px(target, W * 4, 17, 16), b = px(target, W * 4, 18, 16);
  CHECK(a != 0 && a == b);
  free(target);
  host_mesh_free(mesh);
}

int main(void) {
  test_parser();
  test_render();
  test_texture_wrap();
  if (g_failures) {
    printf("host_mesh tests: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("host_mesh tests passed\n");
  return 0;
}
