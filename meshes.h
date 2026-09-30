/* meshes.h — flat-shaded meshes (generated into meshes.c by tools/genmesh.js). */
#ifndef ORCA9_MESHES_H
#define ORCA9_MESHES_H
#include <stdint.h>

typedef struct {
    uint16_t i[3];      /* vertex indices */
    float    n[3];      /* unit normal, model space */
    uint8_t  r, g, b;   /* base colour */
    uint8_t  emissive;  /* 1 = glows: drawn at full colour, no lighting */
} mesh_face_t;

typedef struct {
    const float (*v)[3];
    const mesh_face_t *f;
    uint16_t nv, nf;
    float radius;       /* bounding sphere, model space */
} mesh_t;

extern const mesh_t MESH_APEX, MESH_BLACKFISH, MESH_MATRIARCH, MESH_TIDEBREAKER;
extern const mesh_t MESH_MANTA, MESH_STINGER, MESH_ROCK0, MESH_ROCK1, MESH_ROCK2;
#endif
