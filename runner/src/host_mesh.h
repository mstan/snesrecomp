/* Host-side N64-style mesh presentation for trusted, statically linked mods.
 *
 * A HostMesh is an immutable model decoded from the "N64MESHB" blob format:
 * a limb tree (translate + Z/Y/X rotation per limb, as the N64 skeleton
 * drawers compose it), named display lists made of textured triangles,
 * ARGB8888 textures and named static poses. Games that import a character
 * or vehicle from another title bake the owner's assets into this blob at
 * run time (never into the repository) and draw it with host_mesh_draw().
 *
 * The rasteriser is a deterministic CPU renderer: per-draw float Z-buffer,
 * perspective-correct texture mapping, directional + ambient lighting on
 * vertex normals, prim/env colour, alpha blending and optional integer
 * supersampling. Projection is supplied by the caller so a game can reuse
 * the exact projection of the scene it composites into (for example Star
 * Fox's Super FX "vanish + coord * 256 / z" model). Nothing here knows about
 * the SNES, WRAM or the PPU; the caller hands it a BGRA8888 target.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_MESH_MAGIC "N64MESHB"
#define HOST_MESH_FORMAT_VERSION 1u
#define HOST_MESH_NAME_LEN 32u

/* Material flags. The importer folds the N64 colour combiner and geometry
 * mode into this small vocabulary. */
enum {
  HOST_MESH_MAT_TEXTURE = 1u << 0,      /* sample texture (else white) */
  HOST_MESH_MAT_SHADE = 1u << 1,        /* multiply by shade (lit or vertex) */
  HOST_MESH_MAT_LIGHTING = 1u << 2,     /* shade from normals (else colours) */
  HOST_MESH_MAT_PRIM_COLOR = 1u << 3,   /* multiply rgb by prim colour */
  HOST_MESH_MAT_PRIM_ALPHA = 1u << 4,   /* multiply alpha by prim alpha */
  HOST_MESH_MAT_ENV_COLOR = 1u << 5,    /* multiply rgb by env colour */
  HOST_MESH_MAT_ENV_ALPHA = 1u << 6,    /* multiply alpha by env alpha */
  HOST_MESH_MAT_ALPHA_BLEND = 1u << 7,  /* blend over destination */
  HOST_MESH_MAT_ALPHA_TEST = 1u << 8,   /* discard alpha < 128 */
  HOST_MESH_MAT_CULL_BACK = 1u << 9,
  HOST_MESH_MAT_CULL_FRONT = 1u << 10,
  HOST_MESH_MAT_BILINEAR = 1u << 11,    /* bilinear texel filter */
  HOST_MESH_MAT_NO_ZWRITE = 1u << 12,   /* decal / effect: test but no write */
  HOST_MESH_MAT_NO_ZTEST = 1u << 13,    /* always on top (billboard glow) */
  HOST_MESH_MAT_ADDITIVE = 1u << 14,    /* additive blend instead of lerp */
};

/* Texture wrap modes per axis. Masks emulate the N64 tile mask (power of two
 * repeat) so that coordinates beyond the image repeat or mirror correctly. */
enum {
  HOST_MESH_WRAP_REPEAT = 0,
  HOST_MESH_WRAP_MIRROR = 1,
  HOST_MESH_WRAP_CLAMP = 2,
};

typedef struct HostMeshVec3 {
  float x, y, z;
} HostMeshVec3;

typedef struct HostMeshTexture {
  uint16_t width;
  uint16_t height;
  uint8_t wrap_s;
  uint8_t wrap_t;
  uint8_t mask_s;       /* repeat period = 1 << mask (0 = width) */
  uint8_t mask_t;
  const uint32_t *argb; /* width * height, points into the blob */
} HostMeshTexture;

typedef struct HostMeshMaterial {
  uint32_t flags;
  int32_t texture;      /* index or -1 */
  uint8_t prim[4];      /* rgba */
  uint8_t env[4];       /* rgba */
} HostMeshMaterial;

typedef struct HostMeshVertex {
  float x, y, z;
  float u, v;           /* texel units */
  int8_t nx, ny, nz;    /* normal (when lighting) */
  uint8_t color[4];     /* rgba (when not lighting) */
} HostMeshVertex;

typedef struct HostMeshTriangle {
  uint32_t material;
  HostMeshVertex v[3];
} HostMeshTriangle;

typedef struct HostMeshDisplayList {
  char name[HOST_MESH_NAME_LEN];
  uint32_t first_triangle;
  uint32_t triangle_count;
} HostMeshDisplayList;

typedef struct HostMeshLimb {
  int32_t parent;       /* -1 for the root; parents precede children */
  int32_t display_list; /* index or -1 */
  HostMeshVec3 trans;   /* model units */
  HostMeshVec3 rot_deg; /* bind rotation, degrees, applied Z then Y then X */
} HostMeshLimb;

typedef struct HostMeshPose {
  char name[HOST_MESH_NAME_LEN];
  HostMeshVec3 root_trans;
  const HostMeshVec3 *rot_deg; /* limb_count entries */
} HostMeshPose;

typedef struct HostMesh {
  uint32_t texture_count;
  uint32_t material_count;
  uint32_t triangle_count;
  uint32_t display_list_count;
  uint32_t limb_count;
  uint32_t pose_count;
  const HostMeshTexture *textures;
  const HostMeshMaterial *materials;
  const HostMeshTriangle *triangles;
  const HostMeshDisplayList *display_lists;
  const HostMeshLimb *limbs;
  const HostMeshPose *poses;
  float model_scale;    /* informational: source units per model unit */
  /* Bind-pose bounding box in model space (all limbs, all display lists
   * referenced by limbs). */
  HostMeshVec3 bounds_min;
  HostMeshVec3 bounds_max;
} HostMesh;

/* Parse and validate a blob. On success returns an immutable mesh that owns
 * a copy of the data; release with host_mesh_free(). Every count, offset and
 * index is bounds-checked; malformed input yields NULL and a message in
 * *error (static storage) when error is non-NULL. */
HostMesh *host_mesh_load(const void *blob, size_t size, const char **error);
void host_mesh_free(HostMesh *mesh);

int host_mesh_find_display_list(const HostMesh *mesh, const char *name);
int host_mesh_find_pose(const HostMesh *mesh, const char *name);

/* Compute the bounding box of one display list in its own (limb-local)
 * space. Returns 0 when the list is empty or invalid. */
int host_mesh_display_list_bounds(const HostMesh *mesh, int display_list,
                                  HostMeshVec3 *out_min, HostMeshVec3 *out_max);

/* Caller-supplied projection from camera space (x right, y up, z into the
 * screen, positive in front of the eye) to target pixels. near_z is the
 * camera-space depth used for near-plane clipping. */
typedef struct HostMeshProjection {
  void (*project)(void *ctx, float cx, float cy, float cz, float *sx, float *sy);
  void *ctx;
  float near_z;
} HostMeshProjection;

/* Per-limb override, mirroring the N64 OverrideLimbDraw callback: it may
 * swap the display list (set *display_list to another index or -1 to hide),
 * and adjust the limb's translation / rotation (degrees) before the limb and
 * its children are placed. Return 0 to skip drawing this limb (children are
 * still placed with the adjusted transform). */
typedef struct HostMeshLimbOverride {
  int (*fn)(void *ctx, int limb, int *display_list, HostMeshVec3 *trans,
            HostMeshVec3 *rot_deg);
  void *ctx;
} HostMeshLimbOverride;

typedef struct HostMeshDrawStats {
  uint32_t limbs_drawn;
  uint32_t triangles_submitted;
  uint32_t triangles_rasterised;
  uint32_t pixels_written;
  int32_t bbox_min_x, bbox_min_y, bbox_max_x, bbox_max_y; /* target pixels */
} HostMeshDrawStats;

typedef struct HostMeshDrawParams {
  const HostMesh *mesh;
  int pose;                        /* pose index or -1 for bind pose */
  /* Model -> camera transform, row-major 3x4: cam = M * [x y z 1]. */
  float model_to_camera[12];
  HostMeshProjection projection;
  HostMeshLimbOverride override;   /* fn may be NULL */
  /* Target BGRA8888 buffer (alpha ignored on read, written 0xff). */
  uint8_t *target;
  size_t target_pitch;
  int target_width;
  int target_height;
  /* Optional clip rectangle in target pixels (inclusive-exclusive);
   * zero width disables. */
  int clip_x, clip_y, clip_w, clip_h;
  int supersample;                 /* 1..4 */
  /* Winding: by default a triangle counter-clockwise in camera space
   * (x right, y up) is front-facing; set to flip for mirrored sources. */
  int flip_winding;
  /* Lighting in camera space; light_dir points from surface toward light. */
  float light_dir[3];
  float ambient;                   /* 0..1 */
  float diffuse;                   /* 0..1 */
  /* Global tint multiplied into every fragment (rgb 0..1) and alpha scale. */
  float tint[3];
  float alpha_scale;
  /* When set, pixels whose destination is fully transparent (all zero) are
   * only written at coverage >= 0.5 and left fully opaque; partially covered
   * pixels over already-drawn content are blended. This suits scratch
   * layers composited with a "non-zero pixel" rule. */
  int transparent_black_target;
  HostMeshDrawStats *stats;        /* may be NULL */
} HostMeshDrawParams;

/* Fill params with defaults (identity transform, supersample 1, no light). */
void host_mesh_draw_params_init(HostMeshDrawParams *params);

/* Render the mesh. Returns the number of pixels written to the target. */
uint32_t host_mesh_draw(const HostMeshDrawParams *params);

/* Compute the camera-space origin of one limb after pose + override, using
 * the same placement rules as host_mesh_draw. Returns 0 if the limb index is
 * invalid. Useful for anchoring effects (engine glow, muzzle flash). */
int host_mesh_limb_origin(const HostMeshDrawParams *params, int limb,
                          float out_cam[3]);

/* Helpers to build the model_to_camera matrix: M = T * R * S where R is
 * built from a 3x3 row-major rotation. */
void host_mesh_matrix_compose(float out[12], const float rotation3x3[9],
                              float scale, const float translation[3]);

#ifdef __cplusplus
}
#endif
