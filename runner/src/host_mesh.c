/* See host_mesh.h. */
#include "host_mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Blob format (all little-endian, 4-byte aligned sections)
 *
 *   char     magic[8]        "N64MESHB"
 *   u32      version         1
 *   u32      texture_count, material_count, triangle_count,
 *            display_list_count, limb_count, pose_count
 *   f32      model_scale
 *   u32      texture_table_offset      -> texture_count records
 *   u32      material_table_offset     -> material_count records
 *   u32      triangle_table_offset     -> triangle_count records
 *   u32      display_list_table_offset -> display_list_count records
 *   u32      limb_table_offset         -> limb_count records
 *   u32      pose_table_offset         -> pose_count records
 *   u32      pixel_data_offset, pixel_data_size (u32 argb pool)
 *   u32      pose_data_offset, pose_data_size  (f32 xyz pool)
 *   u32      total_size
 *
 *   texture record  : u16 w, u16 h, u8 wrap_s, u8 wrap_t, u8 mask_s,
 *                     u8 mask_t, u32 pixel_offset (into pixel pool, in u32)
 *   material record : u32 flags, i32 texture, u8 prim[4], u8 env[4]
 *   triangle record : u32 material, 3 x vertex
 *   vertex          : f32 x y z u v, i8 nx ny nz, u8 pad, u8 rgba[4]
 *   display list    : char name[32], u32 first, u32 count
 *   limb record     : i32 parent, i32 display_list, f32 trans[3], f32 rot[3]
 *   pose record     : char name[32], f32 root_trans[3], u32 rot_offset
 *                     (into pose pool, in floats; limb_count * 3 floats)
 * ---------------------------------------------------------------------- */

enum {
  kHeaderSize = 8 + 4 + 6 * 4 + 4 + 6 * 4 + 4 * 4 + 4,
  kTextureRecordSize = 2 + 2 + 4 + 4,
  kMaterialRecordSize = 4 + 4 + 4 + 4,
  kVertexRecordSize = 5 * 4 + 4 + 4,
  kTriangleRecordSize = 4 + 3 * kVertexRecordSize,
  kDisplayListRecordSize = HOST_MESH_NAME_LEN + 4 + 4,
  kLimbRecordSize = 4 + 4 + 3 * 4 + 3 * 4,
  kPoseRecordSize = HOST_MESH_NAME_LEN + 3 * 4 + 4,
  kMaxCount = 1u << 20,
  kMaxBlobSize = 64u << 20,
};

static int host_mesh_internal_bind_bounds(const HostMesh *mesh, float *mn,
                                          float *mx);

typedef struct HostMeshImpl {
  HostMesh pub;
  uint8_t *blob;
  size_t blob_size;
  HostMeshTexture *textures;
  HostMeshMaterial *materials;
  HostMeshTriangle *triangles;
  HostMeshDisplayList *display_lists;
  HostMeshLimb *limbs;
  HostMeshPose *poses;
  uint32_t *pixels;
  HostMeshVec3 *pose_rot;
  uint32_t *limb_order;
} HostMeshImpl;

static uint32_t rd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static int32_t rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }
static float rd_f32(const uint8_t *p) {
  uint32_t u = rd_u32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

static int section_ok(size_t total, uint32_t offset, uint64_t size) {
  return offset % 4u == 0 && (uint64_t)offset + size <= total;
}

static int finite3(HostMeshVec3 v) {
  return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

HostMesh *host_mesh_load(const void *blob_in, size_t size, const char **error) {
  const char *err = NULL;
  HostMeshImpl *impl = NULL;
  const uint8_t *b;
  if (!blob_in || size < kHeaderSize || size > kMaxBlobSize) {
    err = "host_mesh: blob missing or implausible size";
    goto fail;
  }
  impl = (HostMeshImpl *)calloc(1, sizeof(*impl));
  if (!impl) {
    err = "host_mesh: out of memory";
    goto fail;
  }
  impl->blob = (uint8_t *)malloc(size);
  if (!impl->blob) {
    err = "host_mesh: out of memory";
    goto fail;
  }
  memcpy(impl->blob, blob_in, size);
  impl->blob_size = size;
  b = impl->blob;
  if (memcmp(b, HOST_MESH_MAGIC, 8) != 0) {
    err = "host_mesh: bad magic";
    goto fail;
  }
  if (rd_u32(b + 8) != HOST_MESH_FORMAT_VERSION) {
    err = "host_mesh: unsupported version";
    goto fail;
  }
  const uint32_t counts[6] = {rd_u32(b + 12), rd_u32(b + 16), rd_u32(b + 20),
                              rd_u32(b + 24), rd_u32(b + 28), rd_u32(b + 32)};
  for (int i = 0; i < 6; i++) {
    if (counts[i] > kMaxCount) {
      err = "host_mesh: count out of range";
      goto fail;
    }
  }
  const float model_scale = rd_f32(b + 36);
  const uint32_t off_tex = rd_u32(b + 40), off_mat = rd_u32(b + 44),
                 off_tri = rd_u32(b + 48), off_dl = rd_u32(b + 52),
                 off_limb = rd_u32(b + 56), off_pose = rd_u32(b + 60);
  const uint32_t off_pix = rd_u32(b + 64), size_pix = rd_u32(b + 68);
  const uint32_t off_posedata = rd_u32(b + 72), size_posedata = rd_u32(b + 76);
  const uint32_t total = rd_u32(b + 80);
  if (total != size) {
    err = "host_mesh: total_size mismatch";
    goto fail;
  }
  if (!isfinite(model_scale) || model_scale <= 0.0f) {
    err = "host_mesh: bad model_scale";
    goto fail;
  }
  if (!section_ok(size, off_tex, (uint64_t)counts[0] * kTextureRecordSize) ||
      !section_ok(size, off_mat, (uint64_t)counts[1] * kMaterialRecordSize) ||
      !section_ok(size, off_tri, (uint64_t)counts[2] * kTriangleRecordSize) ||
      !section_ok(size, off_dl, (uint64_t)counts[3] * kDisplayListRecordSize) ||
      !section_ok(size, off_limb, (uint64_t)counts[4] * kLimbRecordSize) ||
      !section_ok(size, off_pose, (uint64_t)counts[5] * kPoseRecordSize) ||
      !section_ok(size, off_pix, size_pix) || size_pix % 4u != 0 ||
      !section_ok(size, off_posedata, size_posedata) ||
      size_posedata % 4u != 0) {
    err = "host_mesh: section out of bounds";
    goto fail;
  }
  if (counts[2] == 0 || counts[3] == 0 || counts[4] == 0 || counts[1] == 0) {
    err = "host_mesh: empty mesh";
    goto fail;
  }

  impl->textures = (HostMeshTexture *)calloc(counts[0] ? counts[0] : 1,
                                             sizeof(HostMeshTexture));
  impl->materials =
      (HostMeshMaterial *)calloc(counts[1], sizeof(HostMeshMaterial));
  impl->triangles =
      (HostMeshTriangle *)calloc(counts[2], sizeof(HostMeshTriangle));
  impl->display_lists =
      (HostMeshDisplayList *)calloc(counts[3], sizeof(HostMeshDisplayList));
  impl->limbs = (HostMeshLimb *)calloc(counts[4], sizeof(HostMeshLimb));
  impl->poses =
      (HostMeshPose *)calloc(counts[5] ? counts[5] : 1, sizeof(HostMeshPose));
  const size_t pixel_words = size_pix / 4u;
  impl->pixels = (uint32_t *)calloc(pixel_words ? pixel_words : 1, 4);
  const size_t pose_floats = size_posedata / 4u;
  impl->pose_rot = (HostMeshVec3 *)calloc(
      pose_floats / 3u ? pose_floats / 3u : 1, sizeof(HostMeshVec3));
  if (!impl->textures || !impl->materials || !impl->triangles ||
      !impl->display_lists || !impl->limbs || !impl->poses || !impl->pixels ||
      !impl->pose_rot) {
    err = "host_mesh: out of memory";
    goto fail;
  }
  for (size_t i = 0; i < pixel_words; i++)
    impl->pixels[i] = rd_u32(b + off_pix + i * 4u);
  for (size_t i = 0; i + 2 < pose_floats; i += 3) {
    impl->pose_rot[i / 3].x = rd_f32(b + off_posedata + i * 4u);
    impl->pose_rot[i / 3].y = rd_f32(b + off_posedata + i * 4u + 4u);
    impl->pose_rot[i / 3].z = rd_f32(b + off_posedata + i * 4u + 8u);
  }

  for (uint32_t i = 0; i < counts[0]; i++) {
    const uint8_t *r = b + off_tex + i * kTextureRecordSize;
    HostMeshTexture *t = &impl->textures[i];
    t->width = rd_u16(r);
    t->height = rd_u16(r + 2);
    t->wrap_s = r[4];
    t->wrap_t = r[5];
    t->mask_s = r[6];
    t->mask_t = r[7];
    const uint32_t pixel_offset = rd_u32(r + 8);
    if (t->width == 0 || t->height == 0 || t->width > 1024 ||
        t->height > 1024 || t->wrap_s > HOST_MESH_WRAP_CLAMP ||
        t->wrap_t > HOST_MESH_WRAP_CLAMP || t->mask_s > 10 || t->mask_t > 10 ||
        (uint64_t)pixel_offset + (uint64_t)t->width * t->height > pixel_words) {
      err = "host_mesh: invalid texture record";
      goto fail;
    }
    t->argb = impl->pixels + pixel_offset;
  }
  for (uint32_t i = 0; i < counts[1]; i++) {
    const uint8_t *r = b + off_mat + i * kMaterialRecordSize;
    HostMeshMaterial *m = &impl->materials[i];
    m->flags = rd_u32(r);
    m->texture = rd_i32(r + 4);
    memcpy(m->prim, r + 8, 4);
    memcpy(m->env, r + 12, 4);
    if (m->texture < -1 || m->texture >= (int32_t)counts[0] ||
        ((m->flags & HOST_MESH_MAT_TEXTURE) && m->texture < 0)) {
      err = "host_mesh: invalid material texture";
      goto fail;
    }
  }
  for (uint32_t i = 0; i < counts[2]; i++) {
    const uint8_t *r = b + off_tri + i * kTriangleRecordSize;
    HostMeshTriangle *t = &impl->triangles[i];
    t->material = rd_u32(r);
    if (t->material >= counts[1]) {
      err = "host_mesh: triangle material out of range";
      goto fail;
    }
    for (int k = 0; k < 3; k++) {
      const uint8_t *vr = r + 4 + k * kVertexRecordSize;
      HostMeshVertex *v = &t->v[k];
      v->x = rd_f32(vr);
      v->y = rd_f32(vr + 4);
      v->z = rd_f32(vr + 8);
      v->u = rd_f32(vr + 12);
      v->v = rd_f32(vr + 16);
      v->nx = (int8_t)vr[20];
      v->ny = (int8_t)vr[21];
      v->nz = (int8_t)vr[22];
      memcpy(v->color, vr + 24, 4);
      if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z) ||
          !isfinite(v->u) || !isfinite(v->v) || fabsf(v->x) > 1e6f ||
          fabsf(v->y) > 1e6f || fabsf(v->z) > 1e6f || fabsf(v->u) > 1e6f ||
          fabsf(v->v) > 1e6f) {
        err = "host_mesh: non-finite vertex";
        goto fail;
      }
    }
  }
  for (uint32_t i = 0; i < counts[3]; i++) {
    const uint8_t *r = b + off_dl + i * kDisplayListRecordSize;
    HostMeshDisplayList *d = &impl->display_lists[i];
    memcpy(d->name, r, HOST_MESH_NAME_LEN);
    d->name[HOST_MESH_NAME_LEN - 1] = 0;
    d->first_triangle = rd_u32(r + HOST_MESH_NAME_LEN);
    d->triangle_count = rd_u32(r + HOST_MESH_NAME_LEN + 4);
    if ((uint64_t)d->first_triangle + d->triangle_count > counts[2]) {
      err = "host_mesh: display list range out of bounds";
      goto fail;
    }
  }
  for (uint32_t i = 0; i < counts[4]; i++) {
    const uint8_t *r = b + off_limb + i * kLimbRecordSize;
    HostMeshLimb *l = &impl->limbs[i];
    l->parent = rd_i32(r);
    l->display_list = rd_i32(r + 4);
    l->trans.x = rd_f32(r + 8);
    l->trans.y = rd_f32(r + 12);
    l->trans.z = rd_f32(r + 16);
    l->rot_deg.x = rd_f32(r + 20);
    l->rot_deg.y = rd_f32(r + 24);
    l->rot_deg.z = rd_f32(r + 28);
    if (l->parent < -1 || l->parent >= (int32_t)counts[4] ||
        l->parent == (int32_t)i || l->display_list < -1 ||
        l->display_list >= (int32_t)counts[3] || !finite3(l->trans) ||
        !finite3(l->rot_deg)) {
      err = "host_mesh: invalid limb record";
      goto fail;
    }
  }
  /* Topological order: repeatedly emit limbs whose parent is emitted. */
  impl->limb_order = (uint32_t *)calloc(counts[4], sizeof(uint32_t));
  if (!impl->limb_order) {
    err = "host_mesh: out of memory";
    goto fail;
  }
  {
    uint8_t *emitted = (uint8_t *)calloc(counts[4], 1);
    if (!emitted) {
      err = "host_mesh: out of memory";
      goto fail;
    }
    uint32_t n = 0, roots = 0;
    while (n < counts[4]) {
      uint32_t progress = 0;
      for (uint32_t i = 0; i < counts[4]; i++) {
        if (emitted[i]) continue;
        const int32_t parent = impl->limbs[i].parent;
        if (parent == -1 || emitted[parent]) {
          if (parent == -1) roots++;
          impl->limb_order[n++] = i;
          emitted[i] = 1;
          progress++;
        }
      }
      if (!progress) break;
    }
    free(emitted);
    if (n != counts[4]) {
      err = "host_mesh: limb parent cycle";
      goto fail;
    }
    if (roots == 0) {
      err = "host_mesh: no root limb";
      goto fail;
    }
  }
  for (uint32_t i = 0; i < counts[5]; i++) {
    const uint8_t *r = b + off_pose + i * kPoseRecordSize;
    HostMeshPose *p = &impl->poses[i];
    memcpy(p->name, r, HOST_MESH_NAME_LEN);
    p->name[HOST_MESH_NAME_LEN - 1] = 0;
    p->root_trans.x = rd_f32(r + HOST_MESH_NAME_LEN);
    p->root_trans.y = rd_f32(r + HOST_MESH_NAME_LEN + 4);
    p->root_trans.z = rd_f32(r + HOST_MESH_NAME_LEN + 8);
    const uint32_t rot_offset = rd_u32(r + HOST_MESH_NAME_LEN + 12);
    if (rot_offset % 3u != 0 ||
        (uint64_t)rot_offset + (uint64_t)counts[4] * 3u > pose_floats ||
        !finite3(p->root_trans)) {
      err = "host_mesh: invalid pose record";
      goto fail;
    }
    p->rot_deg = impl->pose_rot + rot_offset / 3u;
    for (uint32_t k = 0; k < counts[4]; k++) {
      if (!finite3(p->rot_deg[k])) {
        err = "host_mesh: non-finite pose rotation";
        goto fail;
      }
    }
  }

  impl->pub.texture_count = counts[0];
  impl->pub.material_count = counts[1];
  impl->pub.triangle_count = counts[2];
  impl->pub.display_list_count = counts[3];
  impl->pub.limb_count = counts[4];
  impl->pub.pose_count = counts[5];
  impl->pub.textures = impl->textures;
  impl->pub.materials = impl->materials;
  impl->pub.triangles = impl->triangles;
  impl->pub.display_lists = impl->display_lists;
  impl->pub.limbs = impl->limbs;
  impl->pub.poses = impl->poses;
  impl->pub.limb_order = impl->limb_order;
  impl->pub.model_scale = model_scale;

  /* Bind-pose bounds: place every limb and accumulate its display list. */
  {
    float mn[3] = {INFINITY, INFINITY, INFINITY};
    float mx[3] = {-INFINITY, -INFINITY, -INFINITY};
    int any = 0;
    any = host_mesh_internal_bind_bounds(&impl->pub, mn, mx);
    if (!any) {
      mn[0] = mn[1] = mn[2] = 0.0f;
      mx[0] = mx[1] = mx[2] = 0.0f;
    }
    impl->pub.bounds_min.x = mn[0];
    impl->pub.bounds_min.y = mn[1];
    impl->pub.bounds_min.z = mn[2];
    impl->pub.bounds_max.x = mx[0];
    impl->pub.bounds_max.y = mx[1];
    impl->pub.bounds_max.z = mx[2];
  }
  if (error) *error = NULL;
  return &impl->pub;

fail:
  if (error) *error = err;
  if (impl) host_mesh_free(&impl->pub);
  return NULL;
}

void host_mesh_free(HostMesh *mesh) {
  if (!mesh) return;
  HostMeshImpl *impl = (HostMeshImpl *)mesh;
  free(impl->textures);
  free(impl->materials);
  free(impl->triangles);
  free(impl->display_lists);
  free(impl->limbs);
  free(impl->poses);
  free(impl->pixels);
  free(impl->pose_rot);
  free(impl->limb_order);
  free(impl->blob);
  free(impl);
}

int host_mesh_find_display_list(const HostMesh *mesh, const char *name) {
  if (!mesh || !name) return -1;
  for (uint32_t i = 0; i < mesh->display_list_count; i++)
    if (strncmp(mesh->display_lists[i].name, name, HOST_MESH_NAME_LEN) == 0)
      return (int)i;
  return -1;
}

int host_mesh_find_pose(const HostMesh *mesh, const char *name) {
  if (!mesh || !name) return -1;
  for (uint32_t i = 0; i < mesh->pose_count; i++)
    if (strncmp(mesh->poses[i].name, name, HOST_MESH_NAME_LEN) == 0)
      return (int)i;
  return -1;
}

int host_mesh_display_list_bounds(const HostMesh *mesh, int display_list,
                                  HostMeshVec3 *out_min, HostMeshVec3 *out_max) {
  if (!mesh || display_list < 0 ||
      display_list >= (int)mesh->display_list_count)
    return 0;
  const HostMeshDisplayList *d = &mesh->display_lists[display_list];
  if (d->triangle_count == 0) return 0;
  HostMeshVec3 mn = {INFINITY, INFINITY, INFINITY};
  HostMeshVec3 mx = {-INFINITY, -INFINITY, -INFINITY};
  for (uint32_t i = 0; i < d->triangle_count; i++) {
    const HostMeshTriangle *t = &mesh->triangles[d->first_triangle + i];
    for (int k = 0; k < 3; k++) {
      const HostMeshVertex *v = &t->v[k];
      if (v->x < mn.x) mn.x = v->x;
      if (v->y < mn.y) mn.y = v->y;
      if (v->z < mn.z) mn.z = v->z;
      if (v->x > mx.x) mx.x = v->x;
      if (v->y > mx.y) mx.y = v->y;
      if (v->z > mx.z) mx.z = v->z;
    }
  }
  if (out_min) *out_min = mn;
  if (out_max) *out_max = mx;
  return 1;
}

/* ------------------------------------------------------------------------
 * Matrices (row-major 3x4 affine: m[0..3] row x, m[4..7] row y, m[8..11] z)
 * ---------------------------------------------------------------------- */

static void mat_identity(float m[12]) {
  memset(m, 0, 12 * sizeof(float));
  m[0] = m[5] = m[10] = 1.0f;
}

static void mat_mul(float out[12], const float a[12], const float b[12]) {
  float r[12];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 4; j++) {
      float s = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                a[i * 4 + 2] * b[2 * 4 + j];
      if (j == 3) s += a[i * 4 + 3];
      r[i * 4 + j] = s;
    }
  }
  memcpy(out, r, sizeof(r));
}

static void mat_translate(float m[12], float x, float y, float z) {
  float t[12];
  mat_identity(t);
  t[3] = x;
  t[7] = y;
  t[11] = z;
  mat_mul(m, m, t);
}

static void mat_rotate_axis(float m[12], int axis, float degrees) {
  const float rad = degrees * (float)(3.14159265358979323846 / 180.0);
  const float c = cosf(rad), s = sinf(rad);
  float r[12];
  mat_identity(r);
  switch (axis) {
  case 0: /* X */
    r[5] = c;
    r[6] = -s;
    r[9] = s;
    r[10] = c;
    break;
  case 1: /* Y */
    r[0] = c;
    r[2] = s;
    r[8] = -s;
    r[10] = c;
    break;
  default: /* Z */
    r[0] = c;
    r[1] = -s;
    r[4] = s;
    r[5] = c;
    break;
  }
  mat_mul(m, m, r);
}

static void mat_apply(const float m[12], float x, float y, float z,
                      float out[3]) {
  out[0] = m[0] * x + m[1] * y + m[2] * z + m[3];
  out[1] = m[4] * x + m[5] * y + m[6] * z + m[7];
  out[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
}

static void mat_apply_dir(const float m[12], float x, float y, float z,
                          float out[3]) {
  out[0] = m[0] * x + m[1] * y + m[2] * z;
  out[1] = m[4] * x + m[5] * y + m[6] * z;
  out[2] = m[8] * x + m[9] * y + m[10] * z;
}

void host_mesh_matrix_compose(float out[12], const float rotation3x3[9],
                              float scale, const float translation[3]) {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) out[i * 4 + j] = rotation3x3[i * 3 + j] * scale;
    out[i * 4 + 3] = translation ? translation[i] : 0.0f;
  }
}

void host_mesh_draw_params_init(HostMeshDrawParams *params) {
  if (!params) return;
  memset(params, 0, sizeof(*params));
  params->pose = -1;
  mat_identity(params->model_to_camera);
  params->projection.near_z = 1.0f;
  params->supersample = 1;
  params->light_dir[2] = -1.0f;
  params->ambient = 1.0f;
  params->diffuse = 0.0f;
  params->tint[0] = params->tint[1] = params->tint[2] = 1.0f;
  params->alpha_scale = 1.0f;
}

/* ------------------------------------------------------------------------
 * Limb placement
 * ---------------------------------------------------------------------- */

typedef struct PlacedLimb {
  float world[12];   /* limb-local -> camera */
  int display_list;  /* -1 hidden */
  int draw;
} PlacedLimb;

/* Place every limb following the N64 skeleton drawer: each limb applies
 * translate(trans) * rotZ * rotY * rotX relative to its parent. The root's
 * translation comes from the pose (or limb) as the drawer does in mode 1,
 * and the pose supplies per-limb rotations replacing the bind rotation. */
static int place_limbs(const HostMeshDrawParams *params, PlacedLimb *out) {
  const HostMesh *mesh = params->mesh;
  const HostMeshPose *pose =
      (params->pose >= 0 && params->pose < (int)mesh->pose_count)
          ? &mesh->poses[params->pose]
          : NULL;
  for (uint32_t oi = 0; oi < mesh->limb_count; oi++) {
    const uint32_t i = mesh->limb_order[oi];
    const HostMeshLimb *limb = &mesh->limbs[i];
    HostMeshVec3 trans = limb->trans;
    HostMeshVec3 rot = pose ? pose->rot_deg[i] : limb->rot_deg;
    int display_list = limb->display_list;
    int draw = 1;
    if (params->override.fn) {
      draw = params->override.fn(params->override.ctx, (int)i, &display_list,
                                 &trans, &rot);
      if (display_list < -1 || display_list >= (int)mesh->display_list_count)
        display_list = -1;
    }
    float local[12];
    mat_identity(local);
    mat_translate(local, trans.x, trans.y, trans.z);
    mat_rotate_axis(local, 2, rot.z);
    mat_rotate_axis(local, 1, rot.y);
    mat_rotate_axis(local, 0, rot.x);
    if (limb->parent < 0)
      mat_mul(out[i].world, params->model_to_camera, local);
    else
      mat_mul(out[i].world, out[limb->parent].world, local);
    out[i].display_list = display_list;
    out[i].draw = draw && display_list >= 0;
  }
  return 1;
}

static int host_mesh_internal_bind_bounds(const HostMesh *mesh, float *mn,
                                          float *mx) {
  HostMeshDrawParams params;
  host_mesh_draw_params_init(&params);
  params.mesh = mesh;
  PlacedLimb *placed =
      (PlacedLimb *)calloc(mesh->limb_count, sizeof(PlacedLimb));
  if (!placed) return 0;
  place_limbs(&params, placed);
  int any = 0;
  for (uint32_t i = 0; i < mesh->limb_count; i++) {
    if (!placed[i].draw) continue;
    const HostMeshDisplayList *d = &mesh->display_lists[placed[i].display_list];
    for (uint32_t t = 0; t < d->triangle_count; t++) {
      const HostMeshTriangle *tri = &mesh->triangles[d->first_triangle + t];
      for (int k = 0; k < 3; k++) {
        float p[3];
        mat_apply(placed[i].world, tri->v[k].x, tri->v[k].y, tri->v[k].z, p);
        for (int a = 0; a < 3; a++) {
          if (p[a] < mn[a]) mn[a] = p[a];
          if (p[a] > mx[a]) mx[a] = p[a];
        }
        any = 1;
      }
    }
  }
  free(placed);
  return any;
}

int host_mesh_limb_origin(const HostMeshDrawParams *params, int limb,
                          float out_cam[3]) {
  if (!params || !params->mesh || limb < 0 ||
      limb >= (int)params->mesh->limb_count)
    return 0;
  PlacedLimb *placed =
      (PlacedLimb *)calloc(params->mesh->limb_count, sizeof(PlacedLimb));
  if (!placed) return 0;
  place_limbs(params, placed);
  out_cam[0] = placed[limb].world[3];
  out_cam[1] = placed[limb].world[7];
  out_cam[2] = placed[limb].world[11];
  free(placed);
  return 1;
}

/* ------------------------------------------------------------------------
 * Rasteriser
 * ---------------------------------------------------------------------- */

typedef struct RasterVertex {
  float cx, cy, cz;   /* camera space */
  float sx, sy;       /* supersampled target pixels */
  float inv_z;
  float u_z, v_z;     /* u/z, v/z */
  float r_z, g_z, b_z, a_z; /* shade * colour / z */
} RasterVertex;

typedef struct Raster {
  const HostMeshDrawParams *params;
  int ss;
  int ox, oy;        /* target-pixel origin of the scratch region */
  int w, h;          /* scratch size in supersampled pixels */
  float *depth;
  uint32_t *color;   /* 0xAARRGGBB, A = 0 untouched */
  uint8_t *covered;
} Raster;

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static int wrap_coord(int c, int size, int wrap, int mask) {
  const int period = mask ? (1 << mask) : size;
  if (wrap == HOST_MESH_WRAP_CLAMP) {
    if (c < 0) c = 0;
    if (c >= size) c = size - 1;
    return c;
  }
  if (wrap == HOST_MESH_WRAP_MIRROR) {
    int p2 = period * 2;
    int m = c % p2;
    if (m < 0) m += p2;
    if (m >= period) m = p2 - 1 - m;
    c = m;
  } else {
    int m = c % period;
    if (m < 0) m += period;
    c = m;
  }
  if (c >= size) c = size - 1;
  return c;
}

static uint32_t texel(const HostMeshTexture *t, int x, int y) {
  x = wrap_coord(x, t->width, t->wrap_s, t->mask_s);
  y = wrap_coord(y, t->height, t->wrap_t, t->mask_t);
  return t->argb[(size_t)y * t->width + x];
}

static void sample_texture(const HostMeshTexture *t, float u, float v,
                           int bilinear, float out[4]) {
  if (!bilinear) {
    uint32_t p = texel(t, (int)floorf(u), (int)floorf(v));
    out[0] = ((p >> 16) & 0xff) / 255.0f;
    out[1] = ((p >> 8) & 0xff) / 255.0f;
    out[2] = (p & 0xff) / 255.0f;
    out[3] = ((p >> 24) & 0xff) / 255.0f;
    return;
  }
  u -= 0.5f;
  v -= 0.5f;
  const int x0 = (int)floorf(u), y0 = (int)floorf(v);
  const float fx = u - (float)x0, fy = v - (float)y0;
  const uint32_t p00 = texel(t, x0, y0), p10 = texel(t, x0 + 1, y0),
                 p01 = texel(t, x0, y0 + 1), p11 = texel(t, x0 + 1, y0 + 1);
  for (int c = 0; c < 4; c++) {
    const int shift = c == 3 ? 24 : (16 - 8 * c);
    const float a = ((p00 >> shift) & 0xff), b = ((p10 >> shift) & 0xff),
                d = ((p01 >> shift) & 0xff), e = ((p11 >> shift) & 0xff);
    const float top = a + (b - a) * fx, bot = d + (e - d) * fx;
    out[c] = (top + (bot - top) * fy) / 255.0f;
  }
}

static void shade_fragment(const Raster *rs, const HostMeshMaterial *m,
                           const HostMeshTexture *tex, float u, float v,
                           const float shade[4], float out[4]) {
  float rgb[3] = {1.0f, 1.0f, 1.0f};
  float alpha = 1.0f;
  if (tex) {
    float t[4];
    sample_texture(tex, u, v, (m->flags & HOST_MESH_MAT_BILINEAR) != 0, t);
    rgb[0] = t[0];
    rgb[1] = t[1];
    rgb[2] = t[2];
    alpha = t[3];
  }
  if (m->flags & HOST_MESH_MAT_SHADE) {
    rgb[0] *= shade[0];
    rgb[1] *= shade[1];
    rgb[2] *= shade[2];
    alpha *= shade[3];
  }
  if (m->flags & HOST_MESH_MAT_LERP_PRIM_ENV) {
    for (int c = 0; c < 3; c++) {
      const float pr = m->prim[c] / 255.0f, en = m->env[c] / 255.0f;
      rgb[c] = en + (pr - en) * rgb[c];
    }
  } else {
    if (m->flags & HOST_MESH_MAT_PRIM_COLOR) {
      rgb[0] *= m->prim[0] / 255.0f;
      rgb[1] *= m->prim[1] / 255.0f;
      rgb[2] *= m->prim[2] / 255.0f;
    }
    if (m->flags & HOST_MESH_MAT_ENV_COLOR) {
      rgb[0] *= m->env[0] / 255.0f;
      rgb[1] *= m->env[1] / 255.0f;
      rgb[2] *= m->env[2] / 255.0f;
    }
  }
  if (m->flags & HOST_MESH_MAT_PRIM_ALPHA) alpha *= m->prim[3] / 255.0f;
  if (m->flags & HOST_MESH_MAT_ENV_ALPHA) alpha *= m->env[3] / 255.0f;
  const HostMeshDrawParams *p = rs->params;
  out[0] = clampf(rgb[0] * p->tint[0], 0.0f, 1.0f);
  out[1] = clampf(rgb[1] * p->tint[1], 0.0f, 1.0f);
  out[2] = clampf(rgb[2] * p->tint[2], 0.0f, 1.0f);
  out[3] = clampf(alpha * p->alpha_scale, 0.0f, 1.0f);
}

static void write_fragment(Raster *rs, int idx, const HostMeshMaterial *m,
                           const float frag[4]) {
  uint32_t dst = rs->color[idx];
  float dr = ((dst >> 16) & 0xff) / 255.0f, dg = ((dst >> 8) & 0xff) / 255.0f,
        db = (dst & 0xff) / 255.0f;
  const int dst_written = rs->covered[idx] != 0;
  float r, g, b;
  if ((m->flags & HOST_MESH_MAT_ADDITIVE) && dst_written) {
    r = clampf(dr + frag[0] * frag[3], 0.0f, 1.0f);
    g = clampf(dg + frag[1] * frag[3], 0.0f, 1.0f);
    b = clampf(db + frag[2] * frag[3], 0.0f, 1.0f);
  } else if ((m->flags & HOST_MESH_MAT_ALPHA_BLEND) && dst_written) {
    r = dr + (frag[0] - dr) * frag[3];
    g = dg + (frag[1] - dg) * frag[3];
    b = db + (frag[2] - db) * frag[3];
  } else if ((m->flags & (HOST_MESH_MAT_ALPHA_BLEND | HOST_MESH_MAT_ADDITIVE)) &&
             !dst_written) {
    /* Blending over the transparent scratch: keep colour, record coverage as
     * alpha so the final composite can weight it. */
    r = frag[0];
    g = frag[1];
    b = frag[2];
    uint32_t a = (uint32_t)(frag[3] * 255.0f + 0.5f);
    if (a == 0) return;
    rs->color[idx] = (a << 24) | ((uint32_t)(r * 255.0f + 0.5f) << 16) |
                     ((uint32_t)(g * 255.0f + 0.5f) << 8) |
                     (uint32_t)(b * 255.0f + 0.5f);
    rs->covered[idx] = 1;
    return;
  } else {
    r = frag[0];
    g = frag[1];
    b = frag[2];
  }
  rs->color[idx] = 0xff000000u | ((uint32_t)(r * 255.0f + 0.5f) << 16) |
                   ((uint32_t)(g * 255.0f + 0.5f) << 8) |
                   (uint32_t)(b * 255.0f + 0.5f);
  rs->covered[idx] = 1;
}

static float edge(float ax, float ay, float bx, float by, float px, float py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void raster_triangle(Raster *rs, const HostMeshMaterial *m,
                            const RasterVertex *a, const RasterVertex *b,
                            const RasterVertex *c, HostMeshDrawStats *stats) {
  float area = edge(a->sx, a->sy, b->sx, b->sy, c->sx, c->sy);
  if (area == 0.0f || !isfinite(area)) return;
  /* Screen y grows downward, so a counter-clockwise triangle in camera
   * space (front-facing) has negative signed area here. */
  int front = area < 0.0f;
  if (rs->params->flip_winding) front = !front;
  if ((m->flags & HOST_MESH_MAT_CULL_BACK) && !front) return;
  if ((m->flags & HOST_MESH_MAT_CULL_FRONT) && front) return;
  /* Normalise to a positive signed area so the edge functions are positive
   * inside regardless of the source winding. */
  if (area < 0.0f) {
    const RasterVertex *t = b;
    b = c;
    c = t;
    area = -area;
  }
  const float inv_area = 1.0f / area;
  int min_x = (int)floorf(fminf(a->sx, fminf(b->sx, c->sx)));
  int max_x = (int)ceilf(fmaxf(a->sx, fmaxf(b->sx, c->sx)));
  int min_y = (int)floorf(fminf(a->sy, fminf(b->sy, c->sy)));
  int max_y = (int)ceilf(fmaxf(a->sy, fmaxf(b->sy, c->sy)));
  if (min_x < 0) min_x = 0;
  if (min_y < 0) min_y = 0;
  if (max_x > rs->w - 1) max_x = rs->w - 1;
  if (max_y > rs->h - 1) max_y = rs->h - 1;
  if (min_x > max_x || min_y > max_y) return;
  if (stats) stats->triangles_rasterised++;
  const HostMeshTexture *tex =
      (m->flags & HOST_MESH_MAT_TEXTURE) && m->texture >= 0
          ? &rs->params->mesh->textures[m->texture]
          : NULL;
  const int ztest = (m->flags & HOST_MESH_MAT_NO_ZTEST) == 0;
  const int zwrite = (m->flags & HOST_MESH_MAT_NO_ZWRITE) == 0;
  for (int y = min_y; y <= max_y; y++) {
    const float py = (float)y + 0.5f;
    for (int x = min_x; x <= max_x; x++) {
      const float px = (float)x + 0.5f;
      float w0 = edge(b->sx, b->sy, c->sx, c->sy, px, py);
      float w1 = edge(c->sx, c->sy, a->sx, a->sy, px, py);
      float w2 = edge(a->sx, a->sy, b->sx, b->sy, px, py);
      /* Top-left fill rule approximation: accept zero on edges. */
      if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
      w0 *= inv_area;
      w1 *= inv_area;
      w2 *= inv_area;
      const float inv_z = w0 * a->inv_z + w1 * b->inv_z + w2 * c->inv_z;
      if (inv_z <= 0.0f) continue;
      const float z = 1.0f / inv_z;
      const int idx = y * rs->w + x;
      if (ztest && z > rs->depth[idx]) continue;
      const float u = (w0 * a->u_z + w1 * b->u_z + w2 * c->u_z) * z;
      const float v = (w0 * a->v_z + w1 * b->v_z + w2 * c->v_z) * z;
      float shade[4] = {(w0 * a->r_z + w1 * b->r_z + w2 * c->r_z) * z,
                        (w0 * a->g_z + w1 * b->g_z + w2 * c->g_z) * z,
                        (w0 * a->b_z + w1 * b->b_z + w2 * c->b_z) * z,
                        (w0 * a->a_z + w1 * b->a_z + w2 * c->a_z) * z};
      float frag[4];
      shade_fragment(rs, m, tex, u, v, shade, frag);
      if ((m->flags & HOST_MESH_MAT_ALPHA_TEST) && frag[3] < 0.5f) continue;
      if (!(m->flags & (HOST_MESH_MAT_ALPHA_BLEND | HOST_MESH_MAT_ADDITIVE)) &&
          frag[3] <= 0.0f)
        continue;
      write_fragment(rs, idx, m, frag);
      if (zwrite) rs->depth[idx] = z;
    }
  }
}

/* Clip a camera-space triangle against z = near, emitting 0..2 triangles. */
typedef struct ClipVertex {
  float c[3];
  float u, v;
  float shade[4];
} ClipVertex;

static ClipVertex clip_lerp(const ClipVertex *a, const ClipVertex *b, float t) {
  ClipVertex r;
  for (int i = 0; i < 3; i++) r.c[i] = a->c[i] + (b->c[i] - a->c[i]) * t;
  r.u = a->u + (b->u - a->u) * t;
  r.v = a->v + (b->v - a->v) * t;
  for (int i = 0; i < 4; i++)
    r.shade[i] = a->shade[i] + (b->shade[i] - a->shade[i]) * t;
  return r;
}

static int clip_near(const ClipVertex in[3], float near_z, ClipVertex out[4]) {
  int n = 0;
  for (int i = 0; i < 3; i++) {
    const ClipVertex *cur = &in[i];
    const ClipVertex *nxt = &in[(i + 1) % 3];
    const int cur_in = cur->c[2] >= near_z;
    const int nxt_in = nxt->c[2] >= near_z;
    if (cur_in) out[n++] = *cur;
    if (cur_in != nxt_in) {
      const float t = (near_z - cur->c[2]) / (nxt->c[2] - cur->c[2]);
      out[n++] = clip_lerp(cur, nxt, t);
    }
  }
  return n;
}

static void to_raster_vertex(const Raster *rs, const ClipVertex *cv,
                             RasterVertex *out) {
  float sx, sy;
  rs->params->projection.project(rs->params->projection.ctx, cv->c[0], cv->c[1],
                                 cv->c[2], &sx, &sy);
  out->cx = cv->c[0];
  out->cy = cv->c[1];
  out->cz = cv->c[2];
  out->sx = (sx - (float)rs->ox) * (float)rs->ss;
  out->sy = (sy - (float)rs->oy) * (float)rs->ss;
  out->inv_z = 1.0f / cv->c[2];
  out->u_z = cv->u * out->inv_z;
  out->v_z = cv->v * out->inv_z;
  out->r_z = cv->shade[0] * out->inv_z;
  out->g_z = cv->shade[1] * out->inv_z;
  out->b_z = cv->shade[2] * out->inv_z;
  out->a_z = cv->shade[3] * out->inv_z;
}

static void vertex_shade(const HostMeshDrawParams *p, const HostMeshMaterial *m,
                         const float world[12], const HostMeshVertex *v,
                         float out[4]) {
  if (m->flags & HOST_MESH_MAT_LIGHTING) {
    float n[3];
    mat_apply_dir(world, v->nx / 127.0f, v->ny / 127.0f, v->nz / 127.0f, n);
    const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    float ndotl = 0.0f;
    if (len > 1e-6f) {
      ndotl = (n[0] * p->light_dir[0] + n[1] * p->light_dir[1] +
               n[2] * p->light_dir[2]) /
              len;
    }
    if (ndotl < 0.0f) ndotl = 0.0f;
    const float s = clampf(p->ambient + p->diffuse * ndotl, 0.0f, 1.0f);
    out[0] = out[1] = out[2] = s;
    out[3] = v->color[3] / 255.0f;
  } else {
    out[0] = v->color[0] / 255.0f;
    out[1] = v->color[1] / 255.0f;
    out[2] = v->color[2] / 255.0f;
    out[3] = v->color[3] / 255.0f;
  }
}

uint32_t host_mesh_draw(const HostMeshDrawParams *params) {
  if (!params || !params->mesh || !params->target ||
      !params->projection.project || params->target_width <= 0 ||
      params->target_height <= 0 ||
      params->target_pitch < (size_t)params->target_width * 4u)
    return 0;
  const HostMesh *mesh = params->mesh;
  int ss = params->supersample;
  if (ss < 1) ss = 1;
  if (ss > 4) ss = 4;
  HostMeshDrawStats local_stats;
  HostMeshDrawStats *stats = params->stats ? params->stats : &local_stats;
  memset(stats, 0, sizeof(*stats));
  stats->bbox_min_x = stats->bbox_min_y = INT32_MAX;
  stats->bbox_max_x = stats->bbox_max_y = INT32_MIN;

  PlacedLimb *placed = (PlacedLimb *)calloc(mesh->limb_count, sizeof(PlacedLimb));
  if (!placed) return 0;
  place_limbs(params, placed);

  /* Pass 1: transform, near-clip and project every vertex to find the
   * target-space bounding box so the scratch buffers stay small. */
  const float light_len = sqrtf(params->light_dir[0] * params->light_dir[0] +
                                params->light_dir[1] * params->light_dir[1] +
                                params->light_dir[2] * params->light_dir[2]);
  HostMeshDrawParams local_params = *params;
  if (light_len > 1e-6f) {
    local_params.light_dir[0] /= light_len;
    local_params.light_dir[1] /= light_len;
    local_params.light_dir[2] /= light_len;
  }
  const float near_z = params->projection.near_z > 1e-3f
                           ? params->projection.near_z
                           : 1e-3f;
  int clip_x0 = 0, clip_y0 = 0, clip_x1 = params->target_width,
      clip_y1 = params->target_height;
  if (params->clip_w > 0 && params->clip_h > 0) {
    clip_x0 = params->clip_x > 0 ? params->clip_x : 0;
    clip_y0 = params->clip_y > 0 ? params->clip_y : 0;
    if (params->clip_x + params->clip_w < clip_x1)
      clip_x1 = params->clip_x + params->clip_w;
    if (params->clip_y + params->clip_h < clip_y1)
      clip_y1 = params->clip_y + params->clip_h;
  }
  if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
    free(placed);
    return 0;
  }

  float bb_min_x = INFINITY, bb_min_y = INFINITY, bb_max_x = -INFINITY,
        bb_max_y = -INFINITY;
  uint32_t total_tris = 0;
  for (uint32_t i = 0; i < mesh->limb_count; i++) {
    if (!placed[i].draw) continue;
    const HostMeshDisplayList *d = &mesh->display_lists[placed[i].display_list];
    total_tris += d->triangle_count;
    for (uint32_t t = 0; t < d->triangle_count; t++) {
      const HostMeshTriangle *tri = &mesh->triangles[d->first_triangle + t];
      for (int k = 0; k < 3; k++) {
        float c[3];
        mat_apply(placed[i].world, tri->v[k].x, tri->v[k].y, tri->v[k].z, c);
        if (c[2] < near_z) c[2] = near_z; /* conservative */
        float sx, sy;
        params->projection.project(params->projection.ctx, c[0], c[1], c[2],
                                   &sx, &sy);
        if (!isfinite(sx) || !isfinite(sy)) continue;
        if (sx < bb_min_x) bb_min_x = sx;
        if (sx > bb_max_x) bb_max_x = sx;
        if (sy < bb_min_y) bb_min_y = sy;
        if (sy > bb_max_y) bb_max_y = sy;
      }
    }
  }
  const int extra_count =
      params->extra_lists
          ? (params->extra_list_count < HOST_MESH_MAX_EXTRA_LISTS
                 ? params->extra_list_count
                 : HOST_MESH_MAX_EXTRA_LISTS)
          : 0;
  for (int e = 0; e < extra_count; e++) {
    const HostMeshExtraList *x = &params->extra_lists[e];
    if (x->display_list < 0 || x->display_list >= (int)mesh->display_list_count)
      continue;
    const HostMeshDisplayList *d = &mesh->display_lists[x->display_list];
    total_tris += d->triangle_count;
    for (uint32_t t = 0; t < d->triangle_count; t++) {
      const HostMeshTriangle *tri = &mesh->triangles[d->first_triangle + t];
      for (int k = 0; k < 3; k++) {
        float c[3];
        mat_apply(x->model_to_camera, tri->v[k].x, tri->v[k].y, tri->v[k].z, c);
        if (c[2] < near_z) c[2] = near_z;
        float sx, sy;
        params->projection.project(params->projection.ctx, c[0], c[1], c[2],
                                   &sx, &sy);
        if (!isfinite(sx) || !isfinite(sy)) continue;
        if (sx < bb_min_x) bb_min_x = sx;
        if (sx > bb_max_x) bb_max_x = sx;
        if (sy < bb_min_y) bb_min_y = sy;
        if (sy > bb_max_y) bb_max_y = sy;
      }
    }
  }
  stats->triangles_submitted = total_tris;
  if (total_tris == 0 || !isfinite(bb_min_x) || !isfinite(bb_max_x) ||
      !isfinite(bb_min_y) || !isfinite(bb_max_y)) {
    free(placed);
    return 0;
  }
  int ox = (int)floorf(bb_min_x) - 1, oy = (int)floorf(bb_min_y) - 1;
  int ex = (int)ceilf(bb_max_x) + 2, ey = (int)ceilf(bb_max_y) + 2;
  if (ox < clip_x0) ox = clip_x0;
  if (oy < clip_y0) oy = clip_y0;
  if (ex > clip_x1) ex = clip_x1;
  if (ey > clip_y1) ey = clip_y1;
  if (ox >= ex || oy >= ey) {
    free(placed);
    return 0;
  }
  Raster rs;
  memset(&rs, 0, sizeof(rs));
  rs.params = &local_params;
  rs.ss = ss;
  rs.ox = ox;
  rs.oy = oy;
  rs.w = (ex - ox) * ss;
  rs.h = (ey - oy) * ss;
  const size_t cells = (size_t)rs.w * rs.h;
  rs.depth = (float *)malloc(cells * sizeof(float));
  rs.color = (uint32_t *)calloc(cells, sizeof(uint32_t));
  rs.covered = (uint8_t *)calloc(cells, 1);
  if (!rs.depth || !rs.color || !rs.covered) {
    free(rs.depth);
    free(rs.color);
    free(rs.covered);
    free(placed);
    return 0;
  }
  for (size_t i = 0; i < cells; i++) rs.depth[i] = INFINITY;

  /* Pass 2: rasterise limb by limb in skeleton order (Z-buffer resolves
   * overlap; order only matters for blended materials). */
  for (uint32_t i = 0; i < mesh->limb_count; i++) {
    if (!placed[i].draw) continue;
    stats->limbs_drawn++;
    const HostMeshDisplayList *d = &mesh->display_lists[placed[i].display_list];
    for (uint32_t t = 0; t < d->triangle_count; t++) {
      const HostMeshTriangle *tri = &mesh->triangles[d->first_triangle + t];
      const HostMeshMaterial *m = &mesh->materials[tri->material];
      ClipVertex cv[3];
      for (int k = 0; k < 3; k++) {
        mat_apply(placed[i].world, tri->v[k].x, tri->v[k].y, tri->v[k].z,
                  cv[k].c);
        cv[k].u = tri->v[k].u;
        cv[k].v = tri->v[k].v;
        vertex_shade(&local_params, m, placed[i].world, &tri->v[k], cv[k].shade);
      }
      ClipVertex clipped[4];
      const int n = clip_near(cv, near_z, clipped);
      if (n < 3) continue;
      RasterVertex rv[4];
      for (int k = 0; k < n; k++) to_raster_vertex(&rs, &clipped[k], &rv[k]);
      raster_triangle(&rs, m, &rv[0], &rv[1], &rv[2], stats);
      if (n == 4) raster_triangle(&rs, m, &rv[0], &rv[2], &rv[3], stats);
    }
  }

  /* Pass 2b: detached extra lists in the same Z-buffer. */
  for (int e = 0; e < extra_count; e++) {
    const HostMeshExtraList *x = &params->extra_lists[e];
    if (x->display_list < 0 || x->display_list >= (int)mesh->display_list_count)
      continue;
    const HostMeshDisplayList *d = &mesh->display_lists[x->display_list];
    HostMeshDrawParams extra_params = local_params;
    extra_params.alpha_scale = params->alpha_scale * x->alpha_scale;
    Raster xrs = rs;
    xrs.params = &extra_params;
    for (uint32_t t = 0; t < d->triangle_count; t++) {
      const HostMeshTriangle *tri = &mesh->triangles[d->first_triangle + t];
      const HostMeshMaterial *m = &mesh->materials[tri->material];
      ClipVertex cv[3];
      for (int k = 0; k < 3; k++) {
        mat_apply(x->model_to_camera, tri->v[k].x, tri->v[k].y, tri->v[k].z,
                  cv[k].c);
        cv[k].u = tri->v[k].u;
        cv[k].v = tri->v[k].v;
        vertex_shade(&extra_params, m, x->model_to_camera, &tri->v[k],
                     cv[k].shade);
      }
      ClipVertex clipped[4];
      const int n = clip_near(cv, near_z, clipped);
      if (n < 3) continue;
      RasterVertex rv[4];
      for (int k = 0; k < n; k++) to_raster_vertex(&xrs, &clipped[k], &rv[k]);
      raster_triangle(&xrs, m, &rv[0], &rv[1], &rv[2], stats);
      if (n == 4) raster_triangle(&xrs, m, &rv[0], &rv[2], &rv[3], stats);
    }
  }

  /* Pass 3: downsample and composite into the target. */
  uint32_t written = 0;
  const float inv_cells = 1.0f / (float)(ss * ss);
  for (int y = oy; y < ey; y++) {
    uint8_t *row = params->target + (size_t)y * params->target_pitch;
    for (int x = ox; x < ex; x++) {
      float r = 0.0f, g = 0.0f, b = 0.0f, cov = 0.0f;
      int any = 0;
      for (int sy = 0; sy < ss; sy++) {
        for (int sx = 0; sx < ss; sx++) {
          const int idx = ((y - oy) * ss + sy) * rs.w + (x - ox) * ss + sx;
          if (!rs.covered[idx]) continue;
          const uint32_t c = rs.color[idx];
          const float a = ((c >> 24) & 0xff) / 255.0f;
          r += ((c >> 16) & 0xff) / 255.0f * a;
          g += ((c >> 8) & 0xff) / 255.0f * a;
          b += (c & 0xff) / 255.0f * a;
          cov += a;
          any = 1;
        }
      }
      if (!any || cov <= 0.0f) continue;
      r /= cov;
      g /= cov;
      b /= cov;
      cov *= inv_cells;
      uint8_t *px = row + (size_t)x * 4u;
      const int dst_transparent =
          px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0;
      if (params->transparent_black_target && dst_transparent) {
        if (cov < 0.5f) continue;
        px[0] = (uint8_t)(b * 255.0f + 0.5f);
        px[1] = (uint8_t)(g * 255.0f + 0.5f);
        px[2] = (uint8_t)(r * 255.0f + 0.5f);
        px[3] = 0xff;
      } else {
        const float dr = px[2] / 255.0f, dg = px[1] / 255.0f, db = px[0] / 255.0f;
        px[0] = (uint8_t)((db + (b - db) * cov) * 255.0f + 0.5f);
        px[1] = (uint8_t)((dg + (g - dg) * cov) * 255.0f + 0.5f);
        px[2] = (uint8_t)((dr + (r - dr) * cov) * 255.0f + 0.5f);
        px[3] = 0xff;
      }
      written++;
      if (x < stats->bbox_min_x) stats->bbox_min_x = x;
      if (x > stats->bbox_max_x) stats->bbox_max_x = x;
      if (y < stats->bbox_min_y) stats->bbox_min_y = y;
      if (y > stats->bbox_max_y) stats->bbox_max_y = y;
    }
  }
  stats->pixels_written = written;
  free(rs.depth);
  free(rs.color);
  free(rs.covered);
  free(placed);
  return written;
}
