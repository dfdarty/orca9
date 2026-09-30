/* render.c — see render.h. */
#include "render.h"
#include <math.h>
#include <string.h>
#include "fw2.h"
#include "platform/diag.h"
#include "display/font5x7.h"
#include "pico/time.h"

#define BAND_H   40
#define N_BANDS  (SCR_H / BAND_H)
#define MAX_TRIS 1800
#define MAX_OPS  400
#define TEXT_POOL 2048
#define N_STARS  150
#define NEAR_Z   3.0f
#define FAR_Z    420.0f
#define N_BUCKETS 2048

typedef struct {
    float x[3], y[3];
    float depth;
    int16_t ymin, ymax;
    uint16_t col_be;
} tri_t;

enum { OP_LINE, OP_RECT, OP_FRAME, OP_TEXT, OP_DOT };
typedef struct {
    uint8_t type, thick_or_scale;
    uint16_t col_be;
    int16_t ymin, ymax;
    float a, b, c, d;       /* line: x0 y0 x1 y1; rect: x y w h; text: x y; dot: x y size */
    uint16_t text_off;
} op_t;

typedef struct { float x, y, z; } star_t;

static uint16_t bufs[2][SCR_W * BAND_H];
static tri_t tris[MAX_TRIS];
static int n_tris;
static int16_t bucket_head[N_BUCKETS];
static int16_t tri_next[MAX_TRIS];
static int16_t order[MAX_TRIS];
static op_t ops[MAX_OPS];
static int n_ops;
static char text_pool[TEXT_POOL];
static int text_used;
static star_t stars[N_STARS];
static float star_sx[N_STARS], star_sy[N_STARS], star_ex[N_STARS], star_ey[N_STARS];
static uint16_t star_col[N_STARS];
static uint8_t star_size[N_STARS];
static bool star_streak;

static camera_t cam;
static float cam_cp, cam_sp;
static const vec3 LIGHT = { -0.36f, 0.72f, -0.59f };   /* toward the light: above, left, behind the camera */
static r_stats_t stats;
static uint32_t t_build0;
static uint32_t rng = 0x9E3779B9u;


static inline uint16_t swap16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
static float frand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng & 0xFFFFFF) / 16777216.0f; }
static void flush_done(void) {}

static void star_reset(star_t *s, bool far) {
    s->x = (frand() - 0.5f) * 240.0f;
    s->y = (frand() - 0.5f) * 170.0f;
    s->z = far ? 260.0f + frand() * 80.0f : 5.0f + frand() * 330.0f;
}

void r_init(void) {
    for (int i = 0; i < N_STARS; i++) star_reset(&stars[i], false);
}

void r_begin(const camera_t *c) {
    cam = *c;
    cam_cp = cosf(cam.pitch);
    cam_sp = sinf(cam.pitch);
    n_tris = 0;
    n_ops = 0;
    text_used = 0;
    stats.tris = stats.tris_drawn = 0;
    t_build0 = time_us_32();
}

static inline void to_view(float wx, float wy, float wz, float *vx, float *vy, float *vz) {
    float x = wx - cam.pos.x, y = wy - cam.pos.y, z = wz - cam.pos.z;
    *vx = x;
    *vy = y * cam_cp + z * cam_sp;
    *vz = -y * cam_sp + z * cam_cp;
}

bool r_project(vec3 p, float *sx, float *sy, float *depth) {
    float vx, vy, vz;
    to_view(p.x, p.y, p.z, &vx, &vy, &vz);
    if (vz < NEAR_Z) return false;
    float inv = cam.focal / vz;
    *sx = SCR_W * 0.5f + vx * inv;
    *sy = SCR_H * 0.5f - vy * inv;
    if (depth) *depth = vz;
    return true;
}

bool r_mesh(const mesh_t *m, const xform_t *x, uint16_t tint, float tint_amt) {
    static float tv[256][3];   /* view-space vertices */
    static float ts[256][2];   /* screen positions */
    float s = x->scale;
    /* whole-object cull on the bounding sphere */
    float cvx, cvy, cvz;
    to_view(x->pos.x, x->pos.y, x->pos.z, &cvx, &cvy, &cvz);
    float r = m->radius * s;
    if (cvz + r < NEAR_Z || cvz - r > FAR_Z) return false;
    float half_w = cvz * (SCR_W * 0.5f) / cam.focal, half_h = cvz * (SCR_H * 0.5f) / cam.focal;
    if (fabsf(cvx) - r > half_w || fabsf(cvy) - r > half_h) return false;
    if (m->nv > 256) return false;

    /* model rotation M = Rx(pitch) * Ry(yaw) * Rz(roll), as in the concept renderer */
    float cr = cosf(x->roll), sr = sinf(x->roll);
    float cy = cosf(x->yaw), sy = sinf(x->yaw);
    float cp = cosf(x->pitch), sp = sinf(x->pitch);
    float M[3][3] = {
        { cy * cr,                  -cy * sr,                 sy },
        { sp * sy * cr + cp * sr,   -sp * sy * sr + cp * cr,  -sp * cy },
        { -cp * sy * cr + sp * sr,  cp * sy * sr + sp * cr,   cp * cy },
    };
    /* Pre-multiply the camera tilt so one matrix takes model space to view space. */
    float V[3][3];
    for (int j = 0; j < 3; j++) {
        V[0][j] = M[0][j];
        V[1][j] = M[1][j] * cam_cp + M[2][j] * cam_sp;
        V[2][j] = -M[1][j] * cam_sp + M[2][j] * cam_cp;
    }
    bool any_near = false;
    for (int i = 0; i < m->nv; i++) {
        const float *p = m->v[i];
        float px = p[0] * s, py = p[1] * s, pz = p[2] * s;
        float vx = V[0][0] * px + V[0][1] * py + V[0][2] * pz + cvx;
        float vy = V[1][0] * px + V[1][1] * py + V[1][2] * pz + cvy;
        float vz = V[2][0] * px + V[2][1] * py + V[2][2] * pz + cvz;
        tv[i][0] = vx; tv[i][1] = vy; tv[i][2] = vz;
        if (vz < NEAR_Z) { any_near = true; continue; }
        float inv = cam.focal / vz;
        ts[i][0] = SCR_W * 0.5f + vx * inv;
        ts[i][1] = SCR_H * 0.5f - vy * inv;
    }
    int tr = (tint >> 11) << 3, tg = ((tint >> 5) & 63) << 2, tb = (tint & 31) << 3;
    for (int k = 0; k < m->nf; k++) {
        const mesh_face_t *f = &m->f[k];
        const float *a = tv[f->i[0]], *b = tv[f->i[1]], *c = tv[f->i[2]];
        if (any_near && (a[2] < NEAR_Z || b[2] < NEAR_Z || c[2] < NEAR_Z)) continue;
        /* back-face test in view space: camera at the origin */
        float nx = V[0][0] * f->n[0] + V[0][1] * f->n[1] + V[0][2] * f->n[2];
        float ny = V[1][0] * f->n[0] + V[1][1] * f->n[1] + V[1][2] * f->n[2];
        float nz = V[2][0] * f->n[0] + V[2][1] * f->n[1] + V[2][2] * f->n[2];
        if (nx * a[0] + ny * a[1] + nz * a[2] >= 0.0f) continue;
        stats.tris++;
        if (n_tris >= MAX_TRIS) continue;
        int R = f->r, G = f->g, B = f->b;
        if (!f->emissive) {
            /* light in world space: rotate the normal by M only */
            float wx = M[0][0] * f->n[0] + M[0][1] * f->n[1] + M[0][2] * f->n[2];
            float wy = M[1][0] * f->n[0] + M[1][1] * f->n[1] + M[1][2] * f->n[2];
            float wz = M[2][0] * f->n[0] + M[2][1] * f->n[1] + M[2][2] * f->n[2];
            float d = wx * LIGHT.x + wy * LIGHT.y + wz * LIGHT.z;
            float k2 = 0.62f + 1.1f * (d > 0 ? d : 0);
            /* a cool rim so dark hulls read against space */
            float facing = -(nx * a[0] + ny * a[1] + nz * a[2]) / sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            float rim = 1.0f - facing; rim = rim * rim * rim * 0.5f;
            R = (int)(R * k2 + 60 * rim); G = (int)(G * k2 + 90 * rim); B = (int)(B * k2 + 140 * rim);
        }
        if (tint_amt > 0) {
            R += (int)((tr - R) * tint_amt); G += (int)((tg - G) * tint_amt); B += (int)((tb - B) * tint_amt);
        }
        tri_t *t = &tris[n_tris];
        const float *sa = ts[f->i[0]], *sb = ts[f->i[1]], *sc = ts[f->i[2]];
        t->x[0] = sa[0]; t->y[0] = sa[1];
        t->x[1] = sb[0]; t->y[1] = sb[1];
        t->x[2] = sc[0]; t->y[2] = sc[1];
        float ymin = fminf(sa[1], fminf(sb[1], sc[1])), ymax = fmaxf(sa[1], fmaxf(sb[1], sc[1]));
        float xmin = fminf(sa[0], fminf(sb[0], sc[0])), xmax = fmaxf(sa[0], fmaxf(sb[0], sc[0]));
        if (ymax < 0 || ymin >= SCR_H || xmax < 0 || xmin >= SCR_W) continue;
        t->ymin = (int16_t)(ymin < -1 ? -1 : ymin);
        t->ymax = (int16_t)(ymax > SCR_H ? SCR_H : ymax + 1);
        t->depth = (a[2] + b[2] + c[2]) * (1.0f / 3.0f);
        t->col_be = swap16(rgb565(R, G, B));
        n_tris++;
    }
    return true;
}

/* ---------------------------------------------------------------- overlays */

static op_t *op_new(int type, uint16_t c, float ymin, float ymax) {
    if (n_ops >= MAX_OPS) return NULL;
    op_t *o = &ops[n_ops++];
    o->type = (uint8_t)type;
    o->col_be = swap16(c);
    o->ymin = (int16_t)(ymin < -100 ? -100 : ymin > 1000 ? 1000 : ymin);
    o->ymax = (int16_t)(ymax < -100 ? -100 : ymax > 1000 ? 1000 : ymax);
    return o;
}

void r_line(float x0, float y0, float x1, float y1, uint16_t c, int thick) {
    op_t *o = op_new(OP_LINE, c, fminf(y0, y1) - thick, fmaxf(y0, y1) + thick);
    if (!o) return;
    o->a = x0; o->b = y0; o->c = x1; o->d = y1; o->thick_or_scale = (uint8_t)thick;
}
void r_rect(int x, int y, int w, int h, uint16_t c) {
    op_t *o = op_new(OP_RECT, c, (float)y, (float)(y + h));
    if (!o) return;
    o->a = (float)x; o->b = (float)y; o->c = (float)w; o->d = (float)h;
}
void r_frame(int x, int y, int w, int h, uint16_t c) {
    op_t *o = op_new(OP_FRAME, c, (float)y, (float)(y + h));
    if (!o) return;
    o->a = (float)x; o->b = (float)y; o->c = (float)w; o->d = (float)h;
}
void r_dot(float x, float y, int size, uint16_t c) {
    op_t *o = op_new(OP_DOT, c, y - size, y + size);
    if (!o) return;
    o->a = x; o->b = y; o->c = (float)size;
}
int r_text_w(int scale, const char *s) { return (int)strlen(s) * 6 * scale - scale; }
void r_text(int x, int y, int scale, uint16_t c, const char *s) {
    int len = (int)strlen(s);
    if (text_used + len + 1 > TEXT_POOL) return;
    op_t *o = op_new(OP_TEXT, c, (float)y, (float)(y + 8 * scale));
    if (!o) return;
    o->a = (float)x; o->b = (float)y; o->thick_or_scale = (uint8_t)scale;
    o->text_off = (uint16_t)text_used;
    memcpy(text_pool + text_used, s, (size_t)len + 1);
    text_used += len + 1;
}
void r_text_c(int cx, int y, int scale, uint16_t c, const char *s) {
    r_text(cx - r_text_w(scale, s) / 2, y, scale, c, s);
}

/* ---------------------------------------------------------------- stars */

void r_stars_update(float dt, float speed, bool warp) {
    star_streak = warp;
    for (int i = 0; i < N_STARS; i++) {
        star_t *s = &stars[i];
        s->z -= speed * dt;
        if (s->z < 2.0f) star_reset(s, true);
    }
}

static void stars_project(void) {
    float f = cam.focal;
    for (int i = 0; i < N_STARS; i++) {
        star_t *s = &stars[i];
        float inv = f / s->z;
        star_sx[i] = SCR_W * 0.5f + s->x * inv;
        star_sy[i] = SCR_H * 0.5f - s->y * inv;
        float z2 = s->z + (star_streak ? 40.0f : 3.0f);
        float inv2 = f / z2;
        star_ex[i] = SCR_W * 0.5f + s->x * inv2;
        star_ey[i] = SCR_H * 0.5f - s->y * inv2;
        int b = (int)(255.0f * (1.0f - s->z / 340.0f));
        if (b < 40) b = 40;
        star_col[i] = swap16(rgb565(b, b, b + 25));
        star_size[i] = s->z < 60.0f ? 2 : 1;
    }
}

/* ---------------------------------------------------------------- band drawing */

static inline void put(uint16_t *buf, int y0, int x, int y, uint16_t c) {
    if ((unsigned)x < SCR_W && y >= y0 && y < y0 + BAND_H) buf[(y - y0) * SCR_W + x] = c;
}

static void fill_span(uint16_t *row, int x0, int x1, uint16_t c) {
    if (x0 < 0) x0 = 0;
    if (x1 > SCR_W - 1) x1 = SCR_W - 1;
    for (int x = x0; x <= x1; x++) row[x] = c;
}

static void raster_tri(uint16_t *buf, int y0, const tri_t *t) {
    /* sort the corners by y */
    int i0 = 0, i1 = 1, i2 = 2, tmp;
    if (t->y[i1] < t->y[i0]) { tmp = i0; i0 = i1; i1 = tmp; }
    if (t->y[i2] < t->y[i0]) { tmp = i0; i0 = i2; i2 = tmp; }
    if (t->y[i2] < t->y[i1]) { tmp = i1; i1 = i2; i2 = tmp; }
    float xa = t->x[i0], ya = t->y[i0], xb = t->x[i1], yb = t->y[i1], xc = t->x[i2], yc = t->y[i2];
    if (yc - ya < 1e-3f) return;
    int r0 = (int)ceilf(ya - 0.5f), r1 = (int)ceilf(yc - 0.5f) - 1;
    if (r0 < y0) r0 = y0;
    if (r1 > y0 + BAND_H - 1) r1 = y0 + BAND_H - 1;
    float s_ac = (xc - xa) / (yc - ya);
    float s_ab = yb - ya > 1e-4f ? (xb - xa) / (yb - ya) : 0.0f;
    float s_bc = yc - yb > 1e-4f ? (xc - xb) / (yc - yb) : 0.0f;
    for (int row = r0; row <= r1; row++) {
        float yy = row + 0.5f;
        float x1 = xa + s_ac * (yy - ya);
        float x2 = yy < yb ? xa + s_ab * (yy - ya) : xb + s_bc * (yy - yb);
        if (x1 > x2) { float q = x1; x1 = x2; x2 = q; }
        int px0 = (int)ceilf(x1 - 0.5f), px1 = (int)ceilf(x2 - 0.5f) - 1;
        if (px1 < px0) continue;
        fill_span(buf + (row - y0) * SCR_W, px0, px1, t->col_be);
    }
}

static void draw_line_band(uint16_t *buf, int y0, const op_t *o) {
    float x0 = o->a, yA = o->b, x1 = o->c, yB = o->d;
    float dx = x1 - x0, dy = yB - yA;
    int steps = (int)fmaxf(fabsf(dx), fabsf(dy));
    if (steps < 1) steps = 1;
    if (steps > 600) steps = 600;
    int th = o->thick_or_scale;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        int x = (int)(x0 + dx * t), y = (int)(yA + dy * t);
        if (y < y0 - th || y >= y0 + BAND_H + th) continue;
        for (int a = 0; a < th; a++)
            for (int b = 0; b < th; b++) put(buf, y0, x + a, y + b, o->col_be);
    }
}

static void draw_text_band(uint16_t *buf, int y0, const op_t *o) {
    int x = (int)o->a, y = (int)o->b, s = o->thick_or_scale;
    for (const char *p = text_pool + o->text_off; *p; p++, x += 6 * s) {
        unsigned ch = (unsigned char)*p;
        if (ch < FONT5X7_FIRST || ch > FONT5X7_LAST) continue;
        const uint8_t *g = font5x7[ch - FONT5X7_FIRST];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int r = 0; r < 7; r++) {
                if (!(bits & (1u << r))) continue;
                int py = y + r * s;
                if (py + s <= y0 || py >= y0 + BAND_H) continue;
                for (int a = 0; a < s; a++)
                    for (int b = 0; b < s; b++) put(buf, y0, x + col * s + a, py + b, o->col_be);
            }
        }
    }
}

static void draw_ops_band(uint16_t *buf, int y0) {
    for (int i = 0; i < n_ops; i++) {
        const op_t *o = &ops[i];
        if (o->ymax < y0 || o->ymin >= y0 + BAND_H) continue;
        switch (o->type) {
        case OP_LINE: draw_line_band(buf, y0, o); break;
        case OP_TEXT: draw_text_band(buf, y0, o); break;
        case OP_DOT: {
            int s = (int)o->c, x = (int)o->a - s / 2, y = (int)o->b - s / 2;
            for (int a = 0; a < s; a++) for (int b = 0; b < s; b++) put(buf, y0, x + a, y + b, o->col_be);
            break;
        }
        case OP_RECT: {
            int x = (int)o->a, y = (int)o->b, w = (int)o->c, h = (int)o->d;
            int ya = y < y0 ? y0 : y, yb = y + h > y0 + BAND_H ? y0 + BAND_H : y + h;
            for (int yy = ya; yy < yb; yy++) fill_span(buf + (yy - y0) * SCR_W, x, x + w - 1, o->col_be);
            break;
        }
        case OP_FRAME: {
            int x = (int)o->a, y = (int)o->b, w = (int)o->c, h = (int)o->d;
            for (int yy = y; yy < y + h; yy++) {
                if (yy < y0 || yy >= y0 + BAND_H) continue;
                uint16_t *row = buf + (yy - y0) * SCR_W;
                if (yy == y || yy == y + h - 1) fill_span(row, x, x + w - 1, o->col_be);
                else { put(buf, y0, x, yy, o->col_be); put(buf, y0, x + w - 1, yy, o->col_be); }
            }
            break;
        }
        }
    }
}

static void draw_stars_band(uint16_t *buf, int y0) {
    for (int i = 0; i < N_STARS; i++) {
        if (star_streak) {
            op_t o = { .type = OP_LINE, .thick_or_scale = 1, .col_be = star_col[i],
                       .a = star_ex[i], .b = star_ey[i], .c = star_sx[i], .d = star_sy[i] };
            if (fmaxf(o.b, o.d) < y0 || fminf(o.b, o.d) >= y0 + BAND_H) continue;
            draw_line_band(buf, y0, &o);
        } else {
            int x = (int)star_sx[i], y = (int)star_sy[i];
            if (y < y0 - 1 || y >= y0 + BAND_H) continue;
            put(buf, y0, x, y, star_col[i]);
            if (star_size[i] > 1) { put(buf, y0, x + 1, y, star_col[i]); put(buf, y0, x, y + 1, star_col[i]); put(buf, y0, x + 1, y + 1, star_col[i]); }
        }
    }
}

void r_end(void) {
    /* sort back to front: bucket by depth, farthest bucket first */
    for (int i = 0; i < N_BUCKETS; i++) bucket_head[i] = -1;
    for (int i = 0; i < n_tris; i++) {
        int k = (int)(tris[i].depth * (N_BUCKETS / FAR_Z));
        if (k < 0) k = 0;
        if (k >= N_BUCKETS) k = N_BUCKETS - 1;
        tri_next[i] = bucket_head[k];
        bucket_head[k] = (int16_t)i;
    }
    int n = 0;
    for (int k = N_BUCKETS - 1; k >= 0; k--)
        for (int i = bucket_head[k]; i >= 0; i = tri_next[i]) order[n++] = (int16_t)i;
    stars_project();
    stats.tris_drawn = n;
    stats.overlays = n_ops;
    uint32_t t0 = time_us_32();
    stats.build_us = t0 - t_build0;
    stats.wait_us = 0;

    for (int band = 0; band < N_BANDS; band++) {
        uint16_t *buf = bufs[band & 1];
        int y0 = band * BAND_H;
        memset(buf, 0, sizeof bufs[0]);
        draw_stars_band(buf, y0);
        for (int j = 0; j < n; j++) {
            const tri_t *t = &tris[order[j]];
            if (t->ymax < y0 || t->ymin >= y0 + BAND_H) continue;
            raster_tri(buf, y0, t);
        }
        draw_ops_band(buf, y0);
        uint32_t tw = time_us_32();
        st7796_flush_wait();
        stats.wait_us += time_us_32() - tw;
        st7796_flush_async(0, (uint16_t)y0, SCR_W - 1, (uint16_t)(y0 + BAND_H - 1), buf, flush_done);
    }
    stats.draw_us = time_us_32() - t0;

}

r_stats_t r_stats(void) { return stats; }
