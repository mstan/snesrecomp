/* See host_mesh_builder.h. Mirrors the layout documented in host_mesh.c. */
#include "host_mesh_builder.h"

#include <stdlib.h>
#include <string.h>

typedef struct BTexture {
  uint16_t width, height;
  uint8_t wrap_s, wrap_t, mask_s, mask_t;
  uint32_t pixel_offset; /* in words */
} BTexture;

typedef struct BDisplayList {
  char name[HOST_MESH_NAME_LEN];
  uint32_t first, count;
} BDisplayList;

typedef struct BPose {
  char name[HOST_MESH_NAME_LEN];
  HostMeshVec3 root_trans;
  uint32_t rot_offset; /* in floats */
  uint32_t count;
} BPose;

#define VEC(type, name)                                                        \
  type *name;                                                                  \
  uint32_t name##_count, name##_cap

struct HostMeshBuilder {
  VEC(BTexture, textures);
  VEC(HostMeshMaterial, materials);
  VEC(HostMeshTriangle, triangles);
  VEC(BDisplayList, display_lists);
  VEC(HostMeshLimb, limbs);
  VEC(BPose, poses);
  VEC(uint32_t, pixels);
  VEC(float, pose_floats);
  int open_list;
  float model_scale;
  int failed;
};

static void *grow(void *ptr, uint32_t *cap, uint32_t needed, size_t elem) {
  if (needed <= *cap) return ptr;
  uint32_t ncap = *cap ? *cap : 16;
  while (ncap < needed) ncap *= 2;
  void *n = realloc(ptr, (size_t)ncap * elem);
  if (!n) return NULL;
  *cap = ncap;
  return n;
}

#define PUSH(b, name, type, value)                                             \
  do {                                                                         \
    type *nb = (type *)grow((b)->name, &(b)->name##_cap,                       \
                            (b)->name##_count + 1, sizeof(type));              \
    if (!nb) {                                                                 \
      (b)->failed = 1;                                                         \
      return -1;                                                               \
    }                                                                          \
    (b)->name = nb;                                                            \
    (b)->name[(b)->name##_count++] = (value);                                  \
  } while (0)

HostMeshBuilder *host_mesh_builder_create(void) {
  HostMeshBuilder *b = (HostMeshBuilder *)calloc(1, sizeof(*b));
  if (b) {
    b->open_list = -1;
    b->model_scale = 1.0f;
  }
  return b;
}

void host_mesh_builder_destroy(HostMeshBuilder *b) {
  if (!b) return;
  free(b->textures);
  free(b->materials);
  free(b->triangles);
  free(b->display_lists);
  free(b->limbs);
  free(b->poses);
  free(b->pixels);
  free(b->pose_floats);
  free(b);
}

int host_mesh_builder_add_texture(HostMeshBuilder *b, uint16_t width,
                                  uint16_t height, uint8_t wrap_s,
                                  uint8_t wrap_t, uint8_t mask_s,
                                  uint8_t mask_t, const uint32_t *argb) {
  if (!b || !argb || width == 0 || height == 0 || width > 1024 ||
      height > 1024)
    return -1;
  const uint32_t words = (uint32_t)width * height;
  /* De-duplicate by content. */
  for (uint32_t i = 0; i < b->textures_count; i++) {
    const BTexture *t = &b->textures[i];
    if (t->width == width && t->height == height && t->wrap_s == wrap_s &&
        t->wrap_t == wrap_t && t->mask_s == mask_s && t->mask_t == mask_t &&
        memcmp(b->pixels + t->pixel_offset, argb, words * 4u) == 0)
      return (int)i;
  }
  uint32_t *np = (uint32_t *)grow(b->pixels, &b->pixels_cap,
                                  b->pixels_count + words, sizeof(uint32_t));
  if (!np) {
    b->failed = 1;
    return -1;
  }
  b->pixels = np;
  memcpy(b->pixels + b->pixels_count, argb, words * 4u);
  BTexture t;
  t.width = width;
  t.height = height;
  t.wrap_s = wrap_s;
  t.wrap_t = wrap_t;
  t.mask_s = mask_s;
  t.mask_t = mask_t;
  t.pixel_offset = b->pixels_count;
  b->pixels_count += words;
  PUSH(b, textures, BTexture, t);
  return (int)b->textures_count - 1;
}

int host_mesh_builder_add_material(HostMeshBuilder *b, uint32_t flags,
                                   int32_t texture, const uint8_t prim[4],
                                   const uint8_t env[4]) {
  if (!b) return -1;
  if ((flags & HOST_MESH_MAT_TEXTURE) &&
      (texture < 0 || texture >= (int32_t)b->textures_count))
    return -1;
  if (!(flags & HOST_MESH_MAT_TEXTURE)) texture = -1;
  HostMeshMaterial m;
  memset(&m, 0, sizeof(m));
  m.flags = flags;
  m.texture = texture;
  if (prim) memcpy(m.prim, prim, 4);
  else memset(m.prim, 0xff, 4);
  if (env) memcpy(m.env, env, 4);
  else memset(m.env, 0xff, 4);
  for (uint32_t i = 0; i < b->materials_count; i++)
    if (memcmp(&b->materials[i], &m, sizeof(m)) == 0) return (int)i;
  PUSH(b, materials, HostMeshMaterial, m);
  return (int)b->materials_count - 1;
}

int host_mesh_builder_begin_display_list(HostMeshBuilder *b, const char *name) {
  if (!b || !name || !name[0]) return -1;
  for (uint32_t i = 0; i < b->display_lists_count; i++)
    if (strncmp(b->display_lists[i].name, name, HOST_MESH_NAME_LEN) == 0)
      return -1;
  BDisplayList d;
  memset(&d, 0, sizeof(d));
  strncpy(d.name, name, HOST_MESH_NAME_LEN - 1);
  d.first = b->triangles_count;
  d.count = 0;
  PUSH(b, display_lists, BDisplayList, d);
  b->open_list = (int)b->display_lists_count - 1;
  return b->open_list;
}

int host_mesh_builder_add_triangle(HostMeshBuilder *b, uint32_t material,
                                   const HostMeshVertex v[3]) {
  if (!b || !v || b->open_list < 0 || material >= b->materials_count)
    return 0;
  HostMeshTriangle t;
  t.material = material;
  memcpy(t.v, v, sizeof(t.v));
  HostMeshTriangle *nb = (HostMeshTriangle *)grow(
      b->triangles, &b->triangles_cap, b->triangles_count + 1, sizeof(t));
  if (!nb) {
    b->failed = 1;
    return 0;
  }
  b->triangles = nb;
  b->triangles[b->triangles_count++] = t;
  b->display_lists[b->open_list].count++;
  return 1;
}

int host_mesh_builder_add_limb(HostMeshBuilder *b, int32_t parent,
                               int32_t display_list, HostMeshVec3 trans,
                               HostMeshVec3 rot_deg) {
  if (!b) return -1;
  if (display_list < -1 || display_list >= (int32_t)b->display_lists_count)
    return -1;
  HostMeshLimb l;
  l.parent = parent;
  l.display_list = display_list;
  l.trans = trans;
  l.rot_deg = rot_deg;
  PUSH(b, limbs, HostMeshLimb, l);
  return (int)b->limbs_count - 1;
}

int host_mesh_builder_add_pose(HostMeshBuilder *b, const char *name,
                               HostMeshVec3 root_trans,
                               const HostMeshVec3 *rot_deg, uint32_t count) {
  if (!b || !name || !rot_deg || count == 0) return -1;
  for (uint32_t i = 0; i < b->poses_count; i++)
    if (strncmp(b->poses[i].name, name, HOST_MESH_NAME_LEN) == 0) return -1;
  float *nf = (float *)grow(b->pose_floats, &b->pose_floats_cap,
                            b->pose_floats_count + count * 3u, sizeof(float));
  if (!nf) {
    b->failed = 1;
    return -1;
  }
  b->pose_floats = nf;
  BPose p;
  memset(&p, 0, sizeof(p));
  strncpy(p.name, name, HOST_MESH_NAME_LEN - 1);
  p.root_trans = root_trans;
  p.rot_offset = b->pose_floats_count;
  p.count = count;
  for (uint32_t i = 0; i < count; i++) {
    b->pose_floats[b->pose_floats_count++] = rot_deg[i].x;
    b->pose_floats[b->pose_floats_count++] = rot_deg[i].y;
    b->pose_floats[b->pose_floats_count++] = rot_deg[i].z;
  }
  PUSH(b, poses, BPose, p);
  return (int)b->poses_count - 1;
}

void host_mesh_builder_set_model_scale(HostMeshBuilder *b, float scale) {
  if (b && scale > 0.0f) b->model_scale = scale;
}

/* ---- serialisation ------------------------------------------------------ */
typedef struct Out {
  uint8_t *data;
  size_t size, cap;
  int failed;
} Out;

static void out_bytes(Out *o, const void *p, size_t n) {
  if (o->failed) return;
  if (o->size + n > o->cap) {
    size_t ncap = o->cap ? o->cap : 4096;
    while (ncap < o->size + n) ncap *= 2;
    uint8_t *nd = (uint8_t *)realloc(o->data, ncap);
    if (!nd) {
      o->failed = 1;
      return;
    }
    o->data = nd;
    o->cap = ncap;
  }
  memcpy(o->data + o->size, p, n);
  o->size += n;
}
static void out_u32(Out *o, uint32_t v) {
  uint8_t p[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  out_bytes(o, p, 4);
}
static void out_u16(Out *o, uint16_t v) {
  uint8_t p[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  out_bytes(o, p, 2);
}
static void out_f32(Out *o, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  out_u32(o, u);
}
static void out_i32(Out *o, int32_t v) { out_u32(o, (uint32_t)v); }

enum {
  kHeaderSize = 84,
  kTextureRecordSize = 12,
  kMaterialRecordSize = 16,
  kVertexRecordSize = 28,
  kTriangleRecordSize = 4 + 3 * kVertexRecordSize,
  kDisplayListRecordSize = HOST_MESH_NAME_LEN + 8,
  kLimbRecordSize = 32,
  kPoseRecordSize = HOST_MESH_NAME_LEN + 16,
};

int host_mesh_builder_finish(HostMeshBuilder *b, uint8_t **out,
                             size_t *out_size, const char **error) {
  if (error) *error = NULL;
  if (!b || !out || !out_size) {
    if (error) *error = "builder: bad arguments";
    return 0;
  }
  if (b->failed) {
    if (error) *error = "builder: out of memory during construction";
    return 0;
  }
  if (b->triangles_count == 0 || b->display_lists_count == 0 ||
      b->limbs_count == 0 || b->materials_count == 0) {
    if (error) *error = "builder: empty mesh";
    return 0;
  }
  for (uint32_t i = 0; i < b->limbs_count; i++) {
    if (b->limbs[i].parent < -1 ||
        b->limbs[i].parent >= (int32_t)b->limbs_count ||
        b->limbs[i].parent == (int32_t)i) {
      if (error) *error = "builder: limb parent out of range";
      return 0;
    }
  }
  for (uint32_t i = 0; i < b->poses_count; i++) {
    if (b->poses[i].count != b->limbs_count) {
      if (error) *error = "builder: pose limb count mismatch";
      return 0;
    }
  }
  const uint32_t off_tex = kHeaderSize;
  const uint32_t off_mat = off_tex + b->textures_count * kTextureRecordSize;
  const uint32_t off_tri = off_mat + b->materials_count * kMaterialRecordSize;
  const uint32_t off_dl = off_tri + b->triangles_count * kTriangleRecordSize;
  const uint32_t off_limb =
      off_dl + b->display_lists_count * kDisplayListRecordSize;
  const uint32_t off_pose = off_limb + b->limbs_count * kLimbRecordSize;
  const uint32_t off_pix = off_pose + b->poses_count * kPoseRecordSize;
  const uint32_t size_pix = b->pixels_count * 4u;
  const uint32_t off_posedata = off_pix + size_pix;
  const uint32_t size_posedata = b->pose_floats_count * 4u;
  const uint32_t total = off_posedata + size_posedata;

  Out o;
  memset(&o, 0, sizeof(o));
  out_bytes(&o, HOST_MESH_MAGIC, 8);
  out_u32(&o, HOST_MESH_FORMAT_VERSION);
  out_u32(&o, b->textures_count);
  out_u32(&o, b->materials_count);
  out_u32(&o, b->triangles_count);
  out_u32(&o, b->display_lists_count);
  out_u32(&o, b->limbs_count);
  out_u32(&o, b->poses_count);
  out_f32(&o, b->model_scale);
  out_u32(&o, off_tex);
  out_u32(&o, off_mat);
  out_u32(&o, off_tri);
  out_u32(&o, off_dl);
  out_u32(&o, off_limb);
  out_u32(&o, off_pose);
  out_u32(&o, off_pix);
  out_u32(&o, size_pix);
  out_u32(&o, off_posedata);
  out_u32(&o, size_posedata);
  out_u32(&o, total);
  for (uint32_t i = 0; i < b->textures_count; i++) {
    const BTexture *t = &b->textures[i];
    out_u16(&o, t->width);
    out_u16(&o, t->height);
    uint8_t w[4] = {t->wrap_s, t->wrap_t, t->mask_s, t->mask_t};
    out_bytes(&o, w, 4);
    out_u32(&o, t->pixel_offset);
  }
  for (uint32_t i = 0; i < b->materials_count; i++) {
    const HostMeshMaterial *m = &b->materials[i];
    out_u32(&o, m->flags);
    out_i32(&o, m->texture);
    out_bytes(&o, m->prim, 4);
    out_bytes(&o, m->env, 4);
  }
  for (uint32_t i = 0; i < b->triangles_count; i++) {
    const HostMeshTriangle *t = &b->triangles[i];
    out_u32(&o, t->material);
    for (int k = 0; k < 3; k++) {
      const HostMeshVertex *v = &t->v[k];
      out_f32(&o, v->x);
      out_f32(&o, v->y);
      out_f32(&o, v->z);
      out_f32(&o, v->u);
      out_f32(&o, v->v);
      uint8_t n[4] = {(uint8_t)v->nx, (uint8_t)v->ny, (uint8_t)v->nz, 0};
      out_bytes(&o, n, 4);
      out_bytes(&o, v->color, 4);
    }
  }
  for (uint32_t i = 0; i < b->display_lists_count; i++) {
    const BDisplayList *d = &b->display_lists[i];
    out_bytes(&o, d->name, HOST_MESH_NAME_LEN);
    out_u32(&o, d->first);
    out_u32(&o, d->count);
  }
  for (uint32_t i = 0; i < b->limbs_count; i++) {
    const HostMeshLimb *l = &b->limbs[i];
    out_i32(&o, l->parent);
    out_i32(&o, l->display_list);
    out_f32(&o, l->trans.x);
    out_f32(&o, l->trans.y);
    out_f32(&o, l->trans.z);
    out_f32(&o, l->rot_deg.x);
    out_f32(&o, l->rot_deg.y);
    out_f32(&o, l->rot_deg.z);
  }
  for (uint32_t i = 0; i < b->poses_count; i++) {
    const BPose *p = &b->poses[i];
    out_bytes(&o, p->name, HOST_MESH_NAME_LEN);
    out_f32(&o, p->root_trans.x);
    out_f32(&o, p->root_trans.y);
    out_f32(&o, p->root_trans.z);
    out_u32(&o, p->rot_offset);
  }
  for (uint32_t i = 0; i < b->pixels_count; i++) out_u32(&o, b->pixels[i]);
  for (uint32_t i = 0; i < b->pose_floats_count; i++)
    out_f32(&o, b->pose_floats[i]);
  if (o.failed || o.size != total) {
    free(o.data);
    if (error) *error = o.failed ? "builder: out of memory" : "builder: size";
    return 0;
  }
  *out = o.data;
  *out_size = o.size;
  return 1;
}

uint32_t host_mesh_builder_texture_count(const HostMeshBuilder *b) {
  return b ? b->textures_count : 0;
}
uint32_t host_mesh_builder_material_count(const HostMeshBuilder *b) {
  return b ? b->materials_count : 0;
}
uint32_t host_mesh_builder_triangle_count(const HostMeshBuilder *b) {
  return b ? b->triangles_count : 0;
}
uint32_t host_mesh_builder_display_list_count(const HostMeshBuilder *b) {
  return b ? b->display_lists_count : 0;
}
uint32_t host_mesh_builder_limb_count(const HostMeshBuilder *b) {
  return b ? b->limbs_count : 0;
}
