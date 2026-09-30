/* Orca-9: Pod Commander — a 3D space shooter for the FREE-WILi 2.
 *
 * First playable: title, hangar (pick one of four orca hulls), tilt steering,
 * barrel roll, asteroids, Stinger enemies, one Guardian Angel manta, waves,
 * FTL jump and each hull's special. Steering is the BMI323 accelerometer;
 * buttons come from the board manager's link (uartkbd). HOME held 5 s exits.
 *
 * Controls in flight:
 *   tilt        steer           D-pad < >   barrel roll      D-pad ^ v  boost / brake
 *   OK (hold)   fire            CANCEL      special          PAGE       re-centre tilt
 *   BLUE        FTL jump when the FTL bar is full
 * Touch: drag anywhere to steer (a virtual stick from where the finger lands);
 * tapping a key label at the bottom does what that key does.
 */
#include "fw2.h"
#include "platform/diag.h"
#include "sensors/bmi323.h"
#include "input/ft6336.h"
#include "pico/time.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "render.h"
#include "meshes.h"

#define PI 3.14159265f
#define TAU 6.28318531f

/* ------------------------------------------------------------------ colours (RGB565) */
#define C_WHITE   0xFFFF
#define C_TEXT    0xE73C   /* soft white */
#define C_MUTED   0x8D17   /* blue-grey */
#define C_CYAN    0x473F
#define C_GOLD    0xFEAB
#define C_MAGENTA 0xFA34
#define C_ORANGE  0xFCC7
#define C_RED     0xF8A6
#define C_DIM     0x18E5
#define C_PURPLE  0xB47F
/* the front-panel key colours (AGENTS.md), native order */
static const uint16_t KEY_COL[5] = { 0xD69A, 0xFF06, 0x1200, 0x00F8, 0x8007 };
static const uint16_t KEY_TXT[5] = { 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF };

/* ------------------------------------------------------------------ hulls */
enum { W_RAIL, W_LASER, W_MISSILE, W_SPREAD };
typedef struct {
    const char *name, *cls;
    const mesh_t *mesh;
    int st[5];                  /* armour, weapons, speed, FTL, handling (out of 10) */
    float hp, lat;              /* hit points; lateral speed (units/s) */
    int weapon;
    float fire_iv, dmg;         /* seconds between shots; damage per bolt */
    float roll_time, roll_cd;   /* roll length; seconds to regain a charge */
    int roll_charges;
    float sp_cd, sp_dur;        /* special: recharge and duration */
    const char *sp_name, *weak, *roll_name;
    float hitbox;
} hull_t;

static const hull_t HULLS[4] = {
    { "APEX", "GUNSHIP", &MESH_APEX, {6, 6, 6, 5, 6}, 100, 20, W_RAIL, 0.13f, 1.0f,
      0.50f, 2.0f, 1, 30, 5.0f, "OVERDRIVE", "RAILGUN OVERHEATS", "STANDARD", 2.0f },
    { "BLACKFISH", "INTERCEPTOR", &MESH_BLACKFISH, {3, 5, 9, 8, 9}, 50, 30, W_LASER, 0.08f, 0.6f,
      0.35f, 1.2f, 2, 20, 3.0f, "GHOST BURN", "GLASS HULL", "QUICK", 1.8f },
    { "MATRIARCH", "HEAVY CARRIER", &MESH_MATRIARCH, {9, 7, 3, 5, 3}, 150, 12, W_MISSILE, 0.38f, 3.0f,
      0.90f, 4.0f, 1, 40, 6.0f, "POD CALL", "BIG SLOW TARGET", "HEAVY", 2.8f },
    { "TIDEBREAKER", "BRAWLER", &MESH_TIDEBREAKER, {7, 9, 5, 4, 5}, 120, 18, W_SPREAD, 0.24f, 1.3f,
      0.60f, 2.5f, 1, 15, 1.2f, "BREACH", "SHORT REACH", "BREACH ROLL", 2.2f },
};

/* ------------------------------------------------------------------ world */
#define MAX_ENTS   40
#define MAX_SHOTS  96
#define MAX_PARTS  160
#define SPAWN_Z    230.0f
#define BOUND_X    16.0f
#define BOUND_Y    9.0f

enum { E_ROCK, E_STINGER };
typedef struct {
    bool on;
    int kind;
    vec3 p, v;
    float yaw, pitch, roll, spin_y, spin_x;
    float hp, r, scale, t, fire_t, flash, life;
    float hold_z, cx, cy, phase;
    const mesh_t *mesh;
} ent_t;

typedef struct {
    bool on, enemy;
    int type;                    /* W_* for the player's; -1 for enemy bolts */
    vec3 p, v;
    float dmg, life;
    int target;
    uint16_t col;
} shot_t;

typedef struct { bool on; vec3 p, v; float life, max; uint16_t col; } part_t;

static ent_t ents[MAX_ENTS];
static shot_t shots[MAX_SHOTS];
static part_t parts[MAX_PARTS];

/* ------------------------------------------------------------------ game state */
enum { ST_TITLE, ST_HANGAR, ST_CALIB, ST_PLAY, ST_OVER };
static int state = ST_TITLE;
static float state_t;
static int hull_i;
static const hull_t *H;

static struct {
    float x, y, vx, vy, hp;
    bool rolling; int roll_dir; float roll_t; int charges; float charge_t; bool perfect_done;
    float fire_t, heat; bool overheated; int gun_side;
    float sp_cd, sp_on;
    float ftl, invuln, dmg_boost, hit_flash;
    float manta_a, manta_cd, flak_cd;
    float bank;
    int score, wave; float wave_t; int wave_phase; float wave_banner;
    float warp_t;
    float rock_t, sting_t;
    char banner[32]; float banner_t; uint16_t banner_col;
} P;

static float world_speed;
static float tilt_p0, tilt_r0, tilt_p, tilt_r;
static float calib_sum_p, calib_sum_r; static int calib_n;
static bool have_imu, have_touch;
static bool touch_down, touch_steer; static int touch_x0, touch_y0; static float touch_sx, touch_sy;
static bool held[UARTKBD_BTN_COUNT];
static uint32_t rng = 12345;

static float frand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng & 0xFFFFFF) / 16777216.0f; }
static float frange(float a, float b) { return a + (b - a) * frand(); }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float len3(vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static vec3 v3(float x, float y, float z) { vec3 r = { x, y, z }; return r; }
static vec3 vsub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 player_pos(void) { return v3(P.x, P.y, 0.0f); }

static void banner(const char *s, uint16_t col, float t) {
    snprintf(P.banner, sizeof P.banner, "%s", s);
    P.banner_col = col; P.banner_t = t;
}

/* ------------------------------------------------------------------ input */
static void read_tilt(void) {
    if (!have_imu) return;
    bmi323_reading_t m;
    if (!bmi323_read(&m) || !m.valid) return;
    /* Same convention as the emulator's pitch/roll: pitch from x, roll from y/z.
     * Real-board axis mounting still to be confirmed on hardware. */
    float ax = clampf(m.ax, -1.0f, 1.0f);
    tilt_p = asinf(-ax) * (180.0f / PI);
    tilt_r = atan2f(m.ay, m.az) * (180.0f / PI);
}

/* ------------------------------------------------------------------ spawning */
static ent_t *ent_new(void) {
    for (int i = 0; i < MAX_ENTS; i++) if (!ents[i].on) { memset(&ents[i], 0, sizeof ents[i]); ents[i].on = true; return &ents[i]; }
    return NULL;
}
static shot_t *shot_new(void) {
    for (int i = 0; i < MAX_SHOTS; i++) if (!shots[i].on) { memset(&shots[i], 0, sizeof shots[i]); shots[i].on = true; return &shots[i]; }
    return NULL;
}
static void burst(vec3 p, int n, uint16_t c1, uint16_t c2, float speed) {
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < MAX_PARTS; i++) {
            if (parts[i].on) continue;
            part_t *q = &parts[i];
            q->on = true; q->p = p;
            q->v = v3(frange(-1, 1) * speed, frange(-1, 1) * speed, frange(-1, 1) * speed - world_speed * 0.3f);
            q->max = q->life = frange(0.35f, 0.8f);
            q->col = frand() < 0.5f ? c1 : c2;
            break;
        }
    }
}

static void spawn_rock(void) {
    ent_t *e = ent_new(); if (!e) return;
    static const mesh_t *R[3] = { &MESH_ROCK0, &MESH_ROCK1, &MESH_ROCK2 };
    e->kind = E_ROCK; e->mesh = R[(int)(frand() * 3) % 3];
    e->scale = frange(1.6f, 4.2f);
    e->r = e->mesh->radius * e->scale * 0.9f;
    e->hp = e->scale * 1.5f;
    /* bias toward the player's lane so rocks are a threat */
    e->p = v3(clampf(P.x + frange(-14, 14), -BOUND_X - 4, BOUND_X + 4), clampf(P.y + frange(-8, 8), -BOUND_Y - 3, BOUND_Y + 3), SPAWN_Z);
    e->v = v3(frange(-1.5f, 1.5f), frange(-1, 1), 0);
    e->spin_y = frange(-1.5f, 1.5f); e->spin_x = frange(-1.2f, 1.2f);
}

static int count_kind(int k) { int n = 0; for (int i = 0; i < MAX_ENTS; i++) if (ents[i].on && ents[i].kind == k) n++; return n; }

static void spawn_stinger(void) {
    ent_t *e = ent_new(); if (!e) return;
    e->kind = E_STINGER; e->mesh = &MESH_STINGER; e->scale = 2.0f;
    e->r = 4.0f; e->hp = 3.0f + P.wave * 0.5f;
    e->cx = frange(-12, 12); e->cy = frange(-5, 7);
    e->p = v3(e->cx, e->cy + 10, SPAWN_Z);
    e->hold_z = frange(45, 80);
    e->phase = frange(0, TAU);
    e->fire_t = frange(1.2f, 2.4f);
    e->life = frange(11, 16);
}

/* ------------------------------------------------------------------ player actions */
static void fire_bolt(float ox, float oy, float oz, float vx, float vz, int type, float dmg, uint16_t col) {
    shot_t *s = shot_new(); if (!s) return;
    s->type = type; s->enemy = false;
    s->p = v3(P.x + ox, P.y + oy, oz);
    s->v = v3(vx, 0, vz);
    s->dmg = dmg * (P.dmg_boost > 0 ? 1.5f : 1.0f);
    s->life = type == W_SPREAD ? 0.42f : 1.2f;     /* Tidebreaker's jaw cannons fade fast */
    s->target = -1;
    s->col = col;
}

static void player_fire(float dt) {
    P.fire_t -= dt;
    if (!P.overheated && P.heat > 0) P.heat = fmaxf(0, P.heat - dt * 0.45f);
    if (P.overheated) { P.heat -= dt * 0.35f; if (P.heat <= 0.25f) { P.overheated = false; } }
    if (!held[UARTKBD_BTN_OK] || P.rolling || P.fire_t > 0 || P.warp_t > 0) return;
    bool od = hull_i == 0 && P.sp_on > 0;   /* Apex overdrive */
    float iv = H->fire_iv * (od ? 0.5f : 1.0f);
    P.fire_t = iv;
    switch (H->weapon) {
    case W_RAIL:
        if (P.overheated) return;
        P.gun_side ^= 1;
        fire_bolt(P.gun_side ? 0.16f : -0.16f, -0.28f, 2.8f, 0, 280, W_RAIL, H->dmg, C_ORANGE);
        if (od) { fire_bolt(1.55f, -0.6f, 0.7f, 0, 280, W_RAIL, H->dmg, C_GOLD); fire_bolt(-1.55f, -0.6f, 0.7f, 0, 280, W_RAIL, H->dmg, C_GOLD); }
        else { P.heat += 0.055f; if (P.heat >= 1.0f) { P.overheated = true; banner("OVERHEAT", C_RED, 1.0f); DIAG("orca9: overheat\n"); } }
        break;
    case W_LASER:
        P.gun_side ^= 1;
        fire_bolt(P.gun_side ? 1.9f : -1.9f, -0.28f, 0.2f, 0, 320, W_LASER, H->dmg, C_CYAN);
        break;
    case W_MISSILE:
        P.gun_side ^= 1;
        fire_bolt(P.gun_side ? 1.0f : -1.0f, 0.2f, 0.4f, P.gun_side ? 6.0f : -6.0f, 120, W_MISSILE, H->dmg, C_MAGENTA);
        break;
    case W_SPREAD:
        for (int k = -1; k <= 1; k++) fire_bolt(k * 0.3f, -0.45f, 2.8f, k * 45.0f, 230, W_SPREAD, H->dmg, C_GOLD);
        break;
    }
}

static void start_roll(int dir) {
    if (P.rolling || P.charges <= 0 || P.warp_t > 0) return;
    P.rolling = true; P.roll_dir = dir; P.roll_t = 0; P.charges--; P.perfect_done = false;
    DIAG("orca9: roll %s charges=%d\n", dir < 0 ? "left" : "right", P.charges);
}

static void start_special(void) {
    if (P.sp_cd > 0 || P.sp_on > 0) return;
    P.sp_on = H->sp_dur; P.sp_cd = H->sp_cd;
    banner(H->sp_name, C_GOLD, 1.2f);
    DIAG("orca9: special %s\n", H->sp_name);
}

static bool player_invulnerable(bool is_bolt, bool *perfect_window) {
    *perfect_window = false;
    if (P.rolling) {
        float u = P.roll_t / H->roll_time;
        if (u > 0.15f && u < 0.85f) { *perfect_window = true; return true; }
    }
    if (P.warp_t > 0 || P.invuln > 0) return true;
    if (P.sp_on > 0) {
        if (hull_i == 2 || hull_i == 3) return true;          /* pod ring / breach */
        if (hull_i == 1 && is_bolt) return true;               /* ghost burn: fire misses */
    }
    return false;
}

static void damage_player(float d) {
    P.hp -= d;
    P.invuln = 0.6f;
    P.hit_flash = 0.35f;
    DIAG("orca9: hit dmg=%d hp=%d\n", (int)d, (int)P.hp);
    if (P.hp <= 0) {
        burst(player_pos(), 40, C_ORANGE, C_WHITE, 14);
        state = ST_OVER; state_t = 0;
        DIAG("orca9: game over score=%d wave=%d\n", P.score, P.wave);
    }
}

/* A hit that the roll dodged in its invulnerable middle: reward it once per roll. */
static void perfect_roll(void) {
    if (P.perfect_done) return;
    P.perfect_done = true;
    P.charges = H->roll_charges;
    P.charge_t = 0;
    P.dmg_boost = 2.0f;
    banner("PERFECT ROLL", C_CYAN, 1.2f);
    DIAG("orca9: perfect roll\n");
}

static void kill_ent(ent_t *e) {
    e->on = false;
    if (e->kind == E_ROCK) { P.score += 10; burst(e->p, 12, 0x9CD3, 0x6A69, 6 + e->scale); }
    else { P.score += 50; burst(e->p, 22, C_MAGENTA, C_ORANGE, 12); }
}

static void new_game(void) {
    memset(&P, 0, sizeof P);
    memset(ents, 0, sizeof ents); memset(shots, 0, sizeof shots); memset(parts, 0, sizeof parts);
    H = &HULLS[hull_i];
    P.hp = H->hp; P.charges = H->roll_charges; P.wave = 1; P.wave_phase = 0;
    P.rock_t = 1.0f; P.sting_t = 3.0f;
    banner("WAVE 1", C_TEXT, 2.0f);
    DIAG("orca9: launch hull=%s\n", H->name);
}

/* ------------------------------------------------------------------ update */
static void update_play(float dt) {
    /* timers */
    if (P.sp_cd > 0) P.sp_cd -= dt;
    if (P.sp_on > 0) P.sp_on -= dt;
    if (P.invuln > 0) P.invuln -= dt;
    if (P.dmg_boost > 0) P.dmg_boost -= dt;
    if (P.hit_flash > 0) P.hit_flash -= dt;
    if (P.banner_t > 0) P.banner_t -= dt;
    if (P.charges < H->roll_charges) { P.charge_t += dt; if (P.charge_t >= H->roll_cd) { P.charges++; P.charge_t = 0; } }

    /* speed */
    float base = 45.0f + 6.0f * H->st[2];
    float mul = held[UARTKBD_BTN_NAV_UP] ? 1.6f : held[UARTKBD_BTN_NAV_DOWN] ? 0.6f : 1.0f;
    if (hull_i == 1 && P.sp_on > 0) mul *= 2.0f;
    if (P.warp_t > 0) mul = 6.0f;
    world_speed += (base * mul - world_speed) * fminf(1.0f, dt * 3.0f);

    /* steering */
    float dr = tilt_r - tilt_r0, dp = tilt_p - tilt_p0;
    float sx = fabsf(dr) < 2.5f ? 0 : clampf((dr - copysignf(2.5f, dr)) / 20.0f, -1, 1);
    float sy = fabsf(dp) < 2.5f ? 0 : clampf((dp - copysignf(2.5f, dp)) / 20.0f, -1, 1);
    if (touch_steer) { sx = touch_sx; sy = touch_sy; }
    float k = fminf(1.0f, dt * (2.0f + H->st[4] * 0.9f));
    P.vx += (sx * H->lat - P.vx) * k;
    P.vy += (sy * H->lat * 0.7f - P.vy) * k;
    P.x += P.vx * dt; P.y += P.vy * dt;

    /* barrel roll: a full turn while sliding ~7 units sideways */
    float roll_angle = 0;
    if (P.rolling) {
        P.roll_t += dt;
        float u = P.roll_t / H->roll_time;
        if (u >= 1.0f) { P.rolling = false; u = 1.0f; }
        float ease = u * u * (3 - 2 * u);
        roll_angle = -P.roll_dir * TAU * ease;
        P.x += P.roll_dir * (7.0f / H->roll_time) * dt * sinf(PI * u) * (PI / 2);
    }
    P.x = clampf(P.x, -BOUND_X, BOUND_X);
    P.y = clampf(P.y, -BOUND_Y, BOUND_Y);
    P.bank = -P.vx * 0.035f + roll_angle;

    player_fire(dt);

    /* FTL */
    if (P.ftl < 1.0f) P.ftl = fminf(1.0f, P.ftl + dt * (0.012f + 0.006f * H->st[3]));
    if (P.warp_t > 0) {
        P.warp_t -= dt;
        if (P.warp_t <= 0) { P.wave_phase = 2; P.wave_banner = 0; }
    }

    /* waves */
    P.wave_t += dt;
    if (P.wave_phase == 0) {
        P.rock_t -= dt; P.sting_t -= dt;
        if (P.rock_t <= 0) { spawn_rock(); P.rock_t = fmaxf(0.4f, 1.25f - 0.08f * P.wave) * frange(0.6f, 1.3f); }
        int max_st = P.wave + 1 < 6 ? P.wave + 1 : 6;
        if (P.sting_t <= 0) { if (count_kind(E_STINGER) < max_st) spawn_stinger(); P.sting_t = fmaxf(1.4f, 3.6f - 0.25f * P.wave); }
        if (P.wave_t > 35.0f) { P.wave_phase = 1; P.wave_t = 0; }
    } else if (P.wave_phase == 1) {
        if (count_kind(E_STINGER) == 0 || P.wave_t > 8.0f) { P.wave_phase = 2; P.wave_banner = 0; }
    } else if (P.wave_phase == 2) {
        if (P.wave_banner == 0) {
            char b[32]; snprintf(b, sizeof b, "WAVE %d CLEAR", P.wave);
            banner(b, C_GOLD, 2.5f);
            P.hp = fminf(H->hp, P.hp + H->hp * 0.15f);
            DIAG("orca9: wave %d clear score=%d\n", P.wave, P.score);
        }
        P.wave_banner += dt;
        if (P.wave_banner > 2.8f) {
            P.wave++; P.wave_phase = 0; P.wave_t = 0;
            char b[24]; snprintf(b, sizeof b, "WAVE %d", P.wave);
            banner(b, C_TEXT, 2.0f);
        }
    }

    vec3 pp = player_pos();

    /* entities */
    for (int i = 0; i < MAX_ENTS; i++) {
        ent_t *e = &ents[i];
        if (!e->on) continue;
        e->t += dt;
        if (e->flash > 0) e->flash -= dt;
        if (e->kind == E_ROCK) {
            e->p.x += e->v.x * dt; e->p.y += e->v.y * dt; e->p.z -= world_speed * dt;
            e->yaw += e->spin_y * dt; e->pitch += e->spin_x * dt;
        } else {
            /* Stinger: fly in, hold station ahead of you and weave, then dive past */
            float weave_x = e->cx + sinf(e->t * 1.3f + e->phase) * 8.0f;
            float weave_y = e->cy + cosf(e->t * 0.9f + e->phase) * 3.5f;
            if (e->t < e->life) {
                float dz = e->hold_z - e->p.z;
                e->p.z += dz * fminf(1.0f, dt * 1.2f);
                if (e->p.z > e->hold_z + 2) e->p.z -= world_speed * 0.6f * dt;
            } else {
                e->p.z -= (world_speed + 40.0f) * dt;
                weave_x = pp.x + (e->cx > 0 ? 6 : -6);
            }
            float ox = e->p.x;
            e->p.x += (weave_x - e->p.x) * fminf(1.0f, dt * 1.5f);
            e->p.y += (weave_y - e->p.y) * fminf(1.0f, dt * 1.5f);
            e->yaw = PI; e->roll = clampf((e->p.x - ox) / dt * 0.08f, -0.8f, 0.8f); e->pitch = -0.75f;   /* dips its nose so you see its back */
            e->fire_t -= dt;
            if (e->fire_t <= 0 && e->p.z > 25 && e->t < e->life && P.warp_t <= 0) {
                e->fire_t = frange(1.5f, 2.6f) - fminf(0.8f, P.wave * 0.08f);
                shot_t *s = shot_new();
                if (s) {
                    s->enemy = true; s->type = -1; s->p = e->p; s->dmg = 12; s->life = 4.0f; s->col = C_MAGENTA;
                    vec3 d = vsub(pp, e->p); float l = len3(d);
                    float sp = 55.0f;
                    /* aim where you are now, compensating for the world's scroll */
                    s->v = v3(d.x / l * sp, d.y / l * sp, d.z / l * sp);
                }
            }
        }
        if (e->p.z < -25.0f) { e->on = false; continue; }

        /* collide with the player */
        vec3 d = vsub(e->p, pp);
        float hit_r = e->r + H->hitbox;
        if (fabsf(d.z) < hit_r && len3(d) < hit_r && state == ST_PLAY) {
            bool pw;
            if (hull_i == 3 && (P.rolling || P.sp_on > 0)) {     /* Tidebreaker rams */
                e->hp -= P.sp_on > 0 ? 30.0f : 8.0f; e->flash = 0.15f;
                if (e->hp <= 0) kill_ent(e);
                else e->p.z += 6.0f;
                continue;
            }
            if (player_invulnerable(false, &pw)) { if (pw) perfect_roll(); continue; }
            damage_player(e->kind == E_ROCK ? 20.0f : 25.0f);
            kill_ent(e);
            if (state != ST_PLAY) return;
        }
    }

    /* Tidebreaker's breach: smashes whatever is just ahead */
    if (hull_i == 3 && P.sp_on > 0) {
        for (int i = 0; i < MAX_ENTS; i++) {
            ent_t *e = &ents[i];
            if (!e->on) continue;
            if (e->p.z > -2 && e->p.z < 20 && fabsf(e->p.x - P.x) < 6 + e->r && fabsf(e->p.y - P.y) < 5 + e->r) { e->hp -= 30.0f * dt * 4; e->flash = 0.1f; if (e->hp <= 0) kill_ent(e); }
        }
    }

    /* shots */
    for (int i = 0; i < MAX_SHOTS; i++) {
        shot_t *s = &shots[i];
        if (!s->on) continue;
        s->life -= dt;
        if (s->life <= 0) { s->on = false; continue; }
        if (s->type == W_MISSILE) {
            /* home on the nearest thing ahead */
            if (s->target < 0 || !ents[s->target].on) {
                float best = 1e9f; s->target = -1;
                for (int j = 0; j < MAX_ENTS; j++) {
                    if (!ents[j].on || ents[j].p.z < s->p.z) continue;
                    float dd = len3(vsub(ents[j].p, s->p)) + (ents[j].kind == E_STINGER ? -40 : 0);
                    if (dd < best) { best = dd; s->target = j; }
                }
            }
            float sp = fminf(260.0f, len3(s->v) + 300.0f * dt);
            vec3 want = s->v;
            if (s->target >= 0) want = vsub(ents[s->target].p, s->p);
            float lw = len3(want); if (lw < 1e-3f) lw = 1;
            float t = fminf(1.0f, dt * 5.0f);
            vec3 nv = v3(s->v.x + (want.x / lw * sp - s->v.x) * t, s->v.y + (want.y / lw * sp - s->v.y) * t, s->v.z + (want.z / lw * sp - s->v.z) * t);
            s->v = nv;
            if (frand() < 0.5f) burst(s->p, 1, C_MUTED, C_DIM, 1);
        }
        s->p.x += s->v.x * dt; s->p.y += s->v.y * dt;
        s->p.z += s->v.z * dt - (s->enemy ? world_speed * 0.25f * dt : 0);
        if (s->p.z > SPAWN_Z + 30 || s->p.z < -20) { s->on = false; continue; }
        if (s->enemy) {
            vec3 d = vsub(s->p, pp);
            if (len3(d) < H->hitbox + 0.6f) {
                bool pw;
                s->on = false;
                if (player_invulnerable(true, &pw)) { if (pw) perfect_roll(); continue; }
                damage_player(s->dmg);
                burst(s->p, 8, C_MAGENTA, C_WHITE, 6);
                if (state != ST_PLAY) return;
            }
        } else {
            for (int j = 0; j < MAX_ENTS; j++) {
                ent_t *e = &ents[j];
                if (!e->on) continue;
                vec3 d = vsub(e->p, s->p);
                if (fabsf(d.z) > e->r + 4.0f) continue;
                if (len3(d) < e->r + (s->type == W_LASER ? 0.8f : 0.5f)) {
                    e->hp -= s->dmg; e->flash = 0.12f; s->on = false;
                    burst(s->p, 4, s->col, C_WHITE, 5);
                    if (e->hp <= 0) kill_ent(e);
                    break;
                }
            }
        }
    }

    /* Guardian Angel manta: shoots down the nearest incoming bolt */
    P.manta_a += dt * 2.2f;
    P.manta_cd -= dt;
    float manta_iv = hull_i == 2 ? 0.8f : 1.6f;    /* Matriarch relaunches twice as fast */
    if (P.manta_cd <= 0) {
        int best = -1; float bd = 30.0f;
        for (int i = 0; i < MAX_SHOTS; i++) {
            if (!shots[i].on || !shots[i].enemy) continue;
            float d = len3(vsub(shots[i].p, pp));
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0) {
            burst(shots[best].p, 8, C_GOLD, C_WHITE, 5);
            shots[best].on = false;
            P.manta_cd = manta_iv;
            DIAG("orca9: manta intercept\n");
        }
    }
    /* Matriarch's belly flak does the same, on its own timer */
    if (hull_i == 2) {
        P.flak_cd -= dt;
        if (P.flak_cd <= 0) {
            int best = -1; float bd = 22.0f;
            for (int i = 0; i < MAX_SHOTS; i++) {
                if (!shots[i].on || !shots[i].enemy) continue;
                float d = len3(vsub(shots[i].p, pp));
                if (d < bd) { bd = d; best = i; }
            }
            if (best >= 0) { burst(shots[best].p, 6, C_ORANGE, C_WHITE, 4); shots[best].on = false; P.flak_cd = 1.3f; }
        }
    }

    /* particles */
    for (int i = 0; i < MAX_PARTS; i++) {
        part_t *q = &parts[i];
        if (!q->on) continue;
        q->life -= dt;
        if (q->life <= 0) { q->on = false; continue; }
        q->p.x += q->v.x * dt; q->p.y += q->v.y * dt; q->p.z += q->v.z * dt - world_speed * 0.5f * dt;
    }
}

/* ------------------------------------------------------------------ drawing */
static camera_t play_camera(void) {
    camera_t c = { v3(P.x * 0.6f, P.y * 0.6f + 3.4f, -11.5f), 0.14f, 300.0f };
    if (hull_i == 3 && P.sp_on > 0) c.pos.z += 2.5f;          /* breach lunge */
    return c;
}

static void key_slot(int i, const char *label) {
    int x = i * 96;
    r_rect(x, 298, 93, 22, KEY_COL[i]);
    r_text_c(x + 46, 305, 1, KEY_TXT[i], label);
}

static void bar(int x, int y, int w, float f, uint16_t col) {
    r_frame(x, y, w, 7, C_DIM);
    int fw = (int)((w - 4) * clampf(f, 0, 1));
    if (fw > 0) r_rect(x + 2, y + 2, fw, 3, col);
}

static void draw_world(void) {
    for (int i = 0; i < MAX_ENTS; i++) {
        ent_t *e = &ents[i];
        if (!e->on) continue;
        xform_t x = { e->p, e->yaw, e->pitch, e->roll, e->scale };
        r_mesh(e->mesh, &x, C_WHITE, e->flash > 0 ? 0.7f : 0.0f);
    }
    for (int i = 0; i < MAX_SHOTS; i++) {
        shot_t *s = &shots[i];
        if (!s->on) continue;
        float x0, y0, x1, y1, d;
        float trail = s->type == W_LASER ? 0.05f : s->enemy ? 0.06f : 0.025f;
        vec3 tail = v3(s->p.x - s->v.x * trail, s->p.y - s->v.y * trail, s->p.z - s->v.z * trail);
        if (!r_project(s->p, &x0, &y0, &d) || !r_project(tail, &x1, &y1, NULL)) continue;
        int th = d < 40 ? 3 : 2;
        if (s->enemy) { r_dot(x0, y0, d < 50 ? 6 : 4, C_MAGENTA); r_dot(x0, y0, 2, C_WHITE); }
        else r_line(x1, y1, x0, y0, s->col, th);
    }
    for (int i = 0; i < MAX_PARTS; i++) {
        part_t *q = &parts[i];
        if (!q->on) continue;
        float x, y, d;
        if (!r_project(q->p, &x, &y, &d)) continue;
        r_dot(x, y, d < 40 ? 3 : 2, q->col);
    }
}

static void draw_player(void) {
    bool blink = (P.invuln > 0 && !P.rolling) && ((int)(P.invuln * 20) & 1);
    xform_t x = { player_pos(), 0.0f, -P.vy * 0.03f, P.bank, 1.0f };
    if (!blink) r_mesh(H->mesh, &x, C_RED, P.hit_flash > 0 ? P.hit_flash * 1.8f : 0);
    /* Guardian Angel */
    float a = P.manta_a;
    int n = (hull_i == 2 && P.sp_on > 0) ? 9 : 1;        /* Pod Call: the whole pod */
    for (int k = 0; k < n; k++) {
        float aa = a + k * TAU / n;
        xform_t m;
        if (n > 1) {   /* Pod Call: a shield ring around the ship, facing the camera */
            /* stood up so their backs face the camera, noses along the ring */
            m = (xform_t){ v3(P.x + cosf(aa) * 6.0f, P.y + 0.6f + sinf(aa) * 4.2f, 1.5f), aa + PI, -PI / 2 + 0.3f, 0.0f, 0.8f };
        } else {
            m = (xform_t){ v3(P.x + cosf(aa) * 4.8f, P.y + 1.6f + sinf(aa * 2) * 0.6f, sinf(aa) * 2.4f), -aa, 0.0f, 0.35f, 1.0f };
        }
        r_mesh(&MESH_MANTA, &m, 0, 0);
    }
}

static void draw_hud(void) {
    char b[40];
    /* left column */
    r_text(8, 6, 1, C_MUTED, "ARMOUR");
    bar(8, 16, 104, P.hp / H->hp, P.hp / H->hp < 0.3f ? C_RED : C_TEXT);
    r_text(8, 28, 1, C_MUTED, "FTL");
    bar(8, 38, 104, P.ftl, P.ftl >= 1 ? C_CYAN : C_GOLD);
    r_text(8, 50, 1, C_MUTED, "ROLL");
    for (int i = 0; i < H->roll_charges; i++) r_rect(40 + i * 12, 50, 9, 7, i < P.charges ? C_CYAN : C_DIM);
    snprintf(b, sizeof b, "%s", H->sp_name);
    r_text(8, 62, 1, P.sp_cd <= 0 ? C_GOLD : C_MUTED, b);
    bar(8, 72, 104, P.sp_on > 0 ? P.sp_on / H->sp_dur : 1.0f - P.sp_cd / H->sp_cd, P.sp_on > 0 ? C_GOLD : C_PURPLE);
    if (H->weapon == W_RAIL) { r_text(8, 84, 1, P.overheated ? C_RED : C_MUTED, "HEAT"); bar(40, 84, 72, P.heat, P.overheated ? C_RED : C_ORANGE); }
    /* right column */
    r_text(SCR_W - 8 - r_text_w(1, H->name), 6, 1, C_TEXT, H->name);
    snprintf(b, sizeof b, "WAVE %d", P.wave);
    r_text(SCR_W - 8 - r_text_w(1, b), 18, 1, C_MUTED, b);
    snprintf(b, sizeof b, "%06d", P.score);
    r_text(SCR_W - 8 - r_text_w(2, b), 30, 2, C_TEXT, b);
    /* crosshair, projected ahead of the nose */
    float cx, cy;
    if (r_project(v3(P.x, P.y, 70.0f), &cx, &cy, NULL)) {
        uint16_t c = P.rolling ? C_DIM : C_CYAN;
        r_line(cx - 10, cy, cx - 4, cy, c, 1); r_line(cx + 4, cy, cx + 10, cy, c, 1);
        r_line(cx, cy - 10, cx, cy - 4, c, 1); r_line(cx, cy + 4, cx, cy + 10, c, 1);
    }
    /* radar: what is ahead, seen from above */
    int rx = 396, ry = 232, rw = 76, rh = 58;
    r_rect(rx, ry, rw, rh, 0x0843);
    r_frame(rx, ry, rw, rh, 0x2A0A);
    for (int i = 0; i < MAX_ENTS; i++) {
        ent_t *e = &ents[i];
        if (!e->on || e->p.z < 0 || e->p.z > SPAWN_Z) continue;
        int px = rx + rw / 2 + (int)((e->p.x - P.x) / 26.0f * (rw / 2 - 3));
        int py = ry + rh - 4 - (int)(e->p.z / SPAWN_Z * (rh - 8));
        if (px < rx + 2 || px > rx + rw - 3) continue;
        r_rect(px - 1, py - 1, e->kind == E_STINGER ? 3 : 2, e->kind == E_STINGER ? 3 : 2, e->kind == E_STINGER ? C_MAGENTA : C_MUTED);
    }
    r_rect(rx + rw / 2 - 1, ry + rh - 5, 3, 3, C_WHITE);
    /* banner */
    if (P.banner_t > 0) r_text_c(SCR_W / 2, 96, 2, P.banner_col, P.banner);
    if (P.ftl >= 1.0f && P.warp_t <= 0) key_slot(3, "FTL JUMP");
    if (P.hit_flash > 0) { r_frame(0, 0, SCR_W, SCR_H, C_RED); r_frame(1, 1, SCR_W - 2, SCR_H - 2, C_RED); }
}

static void draw_title(float t) {
    camera_t c = { v3(0, 1.5f, -14.0f), 0.08f, 300.0f };
    r_begin(&c);
    xform_t x = { v3(0, -1.2f, 2.0f), t * 0.6f, -0.25f, 0.0f, 1.25f };
    r_mesh(&MESH_APEX, &x, 0, 0);
    xform_t m = { v3(cosf(t * 1.6f) * 6.5f, 1.8f, 2 + sinf(t * 1.6f) * 4.0f), -t * 1.6f, 0.0f, 0.35f, 1.1f };
    r_mesh(&MESH_MANTA, &m, 0, 0);
    r_text_c(SCR_W / 2, 26, 5, C_TEXT, "ORCA-9");
    r_text_c(SCR_W / 2, 70, 2, C_CYAN, "POD COMMANDER");
    if (((int)(t * 2)) & 1) r_text_c(SCR_W / 2, 262, 1, C_MUTED, "PRESS OK");
    key_slot(2, "START");
    r_end();
}

static void draw_hangar(float t) {
    camera_t c = { v3(0, 1.8f, -15.0f), 0.1f, 300.0f };
    r_begin(&c);
    const hull_t *h = &HULLS[hull_i];
    xform_t x = { v3(-6.8f, -0.6f, 0.0f), 0.9f + t * 0.5f, -0.2f, 0.0f, 1.05f };
    r_mesh(h->mesh, &x, 0, 0);
    char b[40];
    snprintf(b, sizeof b, "SELECT HULL  %d/4", hull_i + 1);
    r_text(10, 10, 1, C_CYAN, b);
    r_text(288, 30, 3, C_TEXT, h->name);
    r_text(290, 58, 1, C_MUTED, h->cls);
    static const char *lab[5] = { "ARMOUR", "WEAPONS", "SPEED", "FTL", "HANDLING" };
    static const uint16_t col[5] = { C_TEXT, C_ORANGE, C_CYAN, C_GOLD, C_PURPLE };
    for (int i = 0; i < 5; i++) {
        int y = 80 + i * 17;
        r_text(290, y, 1, C_MUTED, lab[i]);
        for (int k = 0; k < 10; k++) r_rect(366 + k * 10, y, 8, 8, k < h->st[i] ? col[i] : C_DIM);
    }
    r_text(290, 172, 1, C_GOLD, "SPECIAL");   r_text(290, 184, 1, C_TEXT, h->sp_name);
    r_text(290, 204, 1, C_MAGENTA, "WEAKNESS"); r_text(290, 216, 1, C_TEXT, h->weak);
    r_text(290, 236, 1, C_CYAN, "ROLL");       r_text(290, 248, 1, C_TEXT, h->roll_name);
    key_slot(0, "BACK"); key_slot(1, "< PREV"); key_slot(2, "LAUNCH"); key_slot(3, "NEXT >");
    r_end();
}

static void draw_play(void) {
    camera_t c = play_camera();
    r_begin(&c);
    draw_world();
    draw_player();
    draw_hud();
    if (state == ST_CALIB) {
        r_text_c(SCR_W / 2, 140, 2, C_TEXT, "HOLD STEADY");
        r_text_c(SCR_W / 2, 164, 1, C_MUTED, "CENTRING TILT CONTROLS");
    }
    r_end();
}

static void draw_over(float t) {
    camera_t c = play_camera();
    r_begin(&c);
    draw_world();
    r_text_c(SCR_W / 2, 100, 3, C_RED, "HULL LOST");
    char b[40];
    snprintf(b, sizeof b, "SCORE %d   WAVE %d", P.score, P.wave);
    r_text_c(SCR_W / 2, 140, 2, C_TEXT, b);
    if (((int)(t * 2)) & 1) r_text_c(SCR_W / 2, 180, 1, C_MUTED, "PRESS OK");
    key_slot(2, "HANGAR");
    r_end();
}

/* ------------------------------------------------------------------ main */
static void on_press(uartkbd_btn_t btn) {
    switch (state) {
    case ST_TITLE:
        if (btn == UARTKBD_BTN_OK || btn == UARTKBD_BTN_GREEN || btn == UARTKBD_BTN_NAV_CENTER) { state = ST_HANGAR; state_t = 0; DIAG("orca9: hangar\n"); }
        break;
    case ST_HANGAR:
        if (btn == UARTKBD_BTN_NAV_LEFT || btn == UARTKBD_BTN_YELLOW) { hull_i = (hull_i + 3) % 4; DIAG("orca9: select %s\n", HULLS[hull_i].name); }
        if (btn == UARTKBD_BTN_NAV_RIGHT || btn == UARTKBD_BTN_BLUE) { hull_i = (hull_i + 1) % 4; DIAG("orca9: select %s\n", HULLS[hull_i].name); }
        if (btn == UARTKBD_BTN_GREY || btn == UARTKBD_BTN_CANCEL) { state = ST_TITLE; state_t = 0; }
        if (btn == UARTKBD_BTN_OK || btn == UARTKBD_BTN_GREEN || btn == UARTKBD_BTN_NAV_CENTER) {
            new_game();
            state = ST_CALIB; state_t = 0; calib_sum_p = calib_sum_r = 0; calib_n = 0;
        }
        break;
    case ST_PLAY:
        if (btn == UARTKBD_BTN_NAV_LEFT) start_roll(-1);
        if (btn == UARTKBD_BTN_NAV_RIGHT) start_roll(1);
        if (btn == UARTKBD_BTN_CANCEL) start_special();
        if (btn == UARTKBD_BTN_PAGE) { tilt_p0 = tilt_p; tilt_r0 = tilt_r; banner("TILT CENTRED", C_CYAN, 1.0f); }
        if (btn == UARTKBD_BTN_BLUE && P.ftl >= 1.0f && P.warp_t <= 0) {
            P.ftl = 0; P.warp_t = 2.0f; P.score += 500;
            for (int i = 0; i < MAX_ENTS; i++) ents[i].on = false;
            for (int i = 0; i < MAX_SHOTS; i++) shots[i].on = false;
            banner("FTL JUMP", C_CYAN, 2.0f);
            DIAG("orca9: ftl jump\n");
        }
        break;
    case ST_OVER:
        if (state_t > 1.0f && (btn == UARTKBD_BTN_OK || btn == UARTKBD_BTN_GREEN || btn == UARTKBD_BTN_NAV_CENTER)) { state = ST_HANGAR; state_t = 0; }
        break;
    }
}

/* The five key labels along the bottom are touch targets too (AGENTS.md). */
static const uartkbd_btn_t SLOT_BTN[5] = { UARTKBD_BTN_GREY, UARTKBD_BTN_YELLOW, UARTKBD_BTN_GREEN, UARTKBD_BTN_BLUE, UARTKBD_BTN_RED };

static void poll_touch(void) {
    if (!have_touch) return;
    uint16_t x, y;
    bool down = ft6336_poll(&x, &y);
    if (down && !touch_down) {
        touch_x0 = x; touch_y0 = y;
        touch_steer = false;
        if (y >= 294) on_press(SLOT_BTN[x / 96 < 5 ? x / 96 : 4]);          /* a key label */
        else if (state == ST_PLAY || state == ST_CALIB) touch_steer = true;  /* a virtual stick */
        else if (state == ST_TITLE || state == ST_OVER) on_press(UARTKBD_BTN_OK);
    }
    if (down && touch_steer) {
        touch_sx = clampf((x - touch_x0) / 60.0f, -1, 1);
        touch_sy = clampf(-(y - touch_y0) / 60.0f, -1, 1);
    }
    if (!down) { touch_steer = false; touch_sx = touch_sy = 0; }
    touch_down = down;
}

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    st7796_fill_screen(0x0000);
    board_backlight_set(1);
    have_imu = bmi323_init();
    have_touch = ft6336_init();
    r_init();
    rng ^= time_us_32();
    DIAG("orca9: ready imu=%d touch=%d\n", have_imu, have_touch);

    uint64_t last = time_us_64();
    uint32_t fps_t0 = time_us_32(); int frames = 0;
    for (;;) {
        fw2_app_recovery_task();
        uartkbd_event_t ev;
        while (uartkbd_next_event(&ev)) {
            if (ev.btn < UARTKBD_BTN_COUNT) held[ev.btn] = ev.pressed;
            if (ev.pressed) on_press(ev.btn);
        }
        uint64_t now = time_us_64();
        float dt = (now - last) * 1e-6f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        state_t += dt;
        read_tilt();
        poll_touch();

        switch (state) {
        case ST_TITLE:
            r_stars_update(dt, 40.0f, false);
            draw_title(state_t);
            break;
        case ST_HANGAR:
            r_stars_update(dt, 15.0f, false);
            draw_hangar(state_t);
            break;
        case ST_CALIB:
            calib_sum_p += tilt_p; calib_sum_r += tilt_r; calib_n++;
            r_stars_update(dt, 40.0f, false);
            if (state_t > 0.8f) {
                tilt_p0 = calib_n ? calib_sum_p / calib_n : 0;
                tilt_r0 = calib_n ? calib_sum_r / calib_n : 0;
                state = ST_PLAY; state_t = 0;
                DIAG("orca9: play tilt0=%d,%d\n", (int)tilt_p0, (int)tilt_r0);
            }
            draw_play();
            break;
        case ST_PLAY:
            update_play(dt);
            r_stars_update(dt, world_speed, P.warp_t > 0 || (hull_i == 1 && P.sp_on > 0) || held[UARTKBD_BTN_NAV_UP]);
            if (state == ST_PLAY) draw_play(); else draw_over(0);
            break;
        case ST_OVER: {
            /* let the explosion play out */
            for (int i = 0; i < MAX_PARTS; i++) if (parts[i].on) { parts[i].life -= dt; if (parts[i].life <= 0) parts[i].on = false; parts[i].p.x += parts[i].v.x * dt; parts[i].p.y += parts[i].v.y * dt; parts[i].p.z += parts[i].v.z * dt; }
            world_speed *= 1.0f - fminf(1.0f, dt);
            r_stars_update(dt, world_speed, false);
            draw_over(state_t);
            break;
        }
        }

        frames++;
        if (time_us_32() - fps_t0 >= 2000000u) {
            r_stats_t s = r_stats();
            DIAG("orca9: fps=%d tris=%d/%d ops=%d build=%uus draw=%uus wait=%uus\n", frames / 2, s.tris_drawn, s.tris, s.overlays,
                 (unsigned)s.build_us, (unsigned)s.draw_us, (unsigned)s.wait_us);
            frames = 0; fps_t0 = time_us_32();
        }
    }
}
