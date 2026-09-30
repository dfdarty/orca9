/* render.h — Orca-9's software 3D renderer.
 *
 * Each frame: objects are transformed, lit and projected into a triangle list,
 * sorted back to front (painter's algorithm), then the screen is drawn in
 * eight 480x40 bands. While one band is sent to the LCD by DMA, the next is
 * drawn into the other buffer. Overlays (lines, dots, rectangles, text) are
 * queued per frame and drawn into every band they touch, on top of the 3D. */
#ifndef ORCA9_RENDER_H
#define ORCA9_RENDER_H
#include <stdint.h>
#include <stdbool.h>
#include "meshes.h"

#define SCR_W 480
#define SCR_H 320

typedef struct { float x, y, z; } vec3;

/* A model's placement: position plus yaw (about y), pitch (about x), roll (about z). */
typedef struct { vec3 pos; float yaw, pitch, roll, scale; } xform_t;

/* Camera: position, looking along +z, tilted down by `pitch` radians; focal length in px. */
typedef struct { vec3 pos; float pitch, focal; } camera_t;

void r_init(void);
void r_begin(const camera_t *cam);

/* Queue a mesh. tint (0 = none) mixes the lit colour toward RGB565 `tint` by
 * tint_amt (0..1), for hit flashes. Returns false if it was culled whole. */
bool r_mesh(const mesh_t *m, const xform_t *x, uint16_t tint, float tint_amt);

/* Project a world point; false if it is behind the camera. */
bool r_project(vec3 p, float *sx, float *sy, float *depth);

/* Overlays, in RGB565 (native order), drawn over the 3D in queue order. */
void r_line(float x0, float y0, float x1, float y1, uint16_t c, int thick);
void r_rect(int x, int y, int w, int h, uint16_t c);
void r_frame(int x, int y, int w, int h, uint16_t c);            /* 1 px outline */
void r_text(int x, int y, int scale, uint16_t c, const char *s);
int  r_text_w(int scale, const char *s);
void r_text_c(int cx, int y, int scale, uint16_t c, const char *s); /* centred */
void r_dot(float x, float y, int size, uint16_t c);

/* Draw the queued frame and send it to the LCD. Blocks until the last band is queued. */
void r_end(void);

/* Stars behind everything: speed (world units/s) streams them toward the camera. */
void r_stars_update(float dt, float speed, bool warp);

/* Stats from the last frame. */
typedef struct { int tris, tris_drawn, overlays; uint32_t build_us, draw_us, wait_us; } r_stats_t;
r_stats_t r_stats(void);

static inline uint16_t rgb565(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
#endif
