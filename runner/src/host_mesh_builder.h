/* In-memory builder that serialises a HostMesh model to the N64MESHB v1 blob
 * consumed by host_mesh_load(). Importers (a game's owner-ROM extractor)
 * append textures, materials, triangles, display lists, limbs and poses,
 * then call host_mesh_builder_finish() to get the bytes.
 *
 * Textures and materials are de-duplicated by content so an importer can
 * naively re-add the same texture for every display list that binds it.
 */
#pragma once

#include "host_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HostMeshBuilder HostMeshBuilder;

HostMeshBuilder *host_mesh_builder_create(void);
void host_mesh_builder_destroy(HostMeshBuilder *b);

/* Returns a texture index, or -1 on failure. argb is copied (w*h words). */
int host_mesh_builder_add_texture(HostMeshBuilder *b, uint16_t width,
                                  uint16_t height, uint8_t wrap_s,
                                  uint8_t wrap_t, uint8_t mask_s,
                                  uint8_t mask_t, const uint32_t *argb);

/* Returns a material index (de-duplicated), or -1. */
int host_mesh_builder_add_material(HostMeshBuilder *b, uint32_t flags,
                                   int32_t texture, const uint8_t prim[4],
                                   const uint8_t env[4]);

/* Begin a named display list; triangles added until the next begin or
 * finish belong to it. Returns its index or -1 (duplicate name / full). */
int host_mesh_builder_begin_display_list(HostMeshBuilder *b, const char *name);

/* Append one triangle to the open display list. Returns 0 on failure. */
int host_mesh_builder_add_triangle(HostMeshBuilder *b, uint32_t material,
                                   const HostMeshVertex v[3]);

/* Limbs are indexed in insertion order; parent may reference any limb index
 * (also ones added later) as long as the final tree is acyclic. */
int host_mesh_builder_add_limb(HostMeshBuilder *b, int32_t parent,
                               int32_t display_list, HostMeshVec3 trans,
                               HostMeshVec3 rot_deg);

/* Poses carry limb_count rotations (degrees); the array must have exactly
 * as many entries as limbs added so far at finish time. */
int host_mesh_builder_add_pose(HostMeshBuilder *b, const char *name,
                               HostMeshVec3 root_trans,
                               const HostMeshVec3 *rot_deg, uint32_t count);

void host_mesh_builder_set_model_scale(HostMeshBuilder *b, float scale);

/* Serialise. On success *out (malloc'd, caller frees) / *out_size are set
 * and the function returns 1. On failure returns 0 and sets *error. */
int host_mesh_builder_finish(HostMeshBuilder *b, uint8_t **out,
                             size_t *out_size, const char **error);

/* Counts, for diagnostics. */
uint32_t host_mesh_builder_texture_count(const HostMeshBuilder *b);
uint32_t host_mesh_builder_material_count(const HostMeshBuilder *b);
uint32_t host_mesh_builder_triangle_count(const HostMeshBuilder *b);
uint32_t host_mesh_builder_display_list_count(const HostMeshBuilder *b);
uint32_t host_mesh_builder_limb_count(const HostMeshBuilder *b);

#ifdef __cplusplus
}
#endif
