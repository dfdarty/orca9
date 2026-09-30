// Orca ship concepts — low-poly meshes + a tiny flat-shaded painter's renderer.
// Geometry is kept deliberately small: every triangle here is one the RP2350B
// would have to transform, cull, sort and fill each frame.
(function (G) {
  'use strict';

  // ---------- colours ----------
  const C = {
    hull: [27, 33, 48],     // orca black, pushed toward navy so it reads in space
    belly: [226, 232, 238], // orca white
    saddle: [104, 114, 132],
    trim: [60, 70, 92],
    gun: [72, 78, 90],
    gunTip: [255, 170, 60],
    glowC: [70, 230, 255],  // cyan: canopy "eye patch", engines
    glowM: [255, 70, 150],  // magenta: missile / heavy weapons
    glowG: [255, 214, 90],  // gold: Guardian Angel drones
  };

  // ---------- mesh builder ----------
  function Mesh() { this.v = []; this.f = []; }
  Mesh.prototype.vert = function (x, y, z) { this.v.push([x, y, z]); return this.v.length - 1; };
  // face: indices, colour, emissive flag, optional "role" tag for colour rules
  Mesh.prototype.tri = function (a, b, c, col, em) { this.f.push({ i: [a, b, c], col, em: !!em }); };
  Mesh.prototype.quad = function (a, b, c, d, col, em) { this.tri(a, b, c, col, em); this.tri(a, c, d, col, em); };

  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
  const mul = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const norm = (a) => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };

  // Lofted body: sections [{z, y, w, h}] from nose (+z) to tail. N sides,
  // rotated half a step so there is a flat top and a flat belly.
  // paint(ctx) returns a colour for a face given its centroid, normal and t (0 nose..1 tail).
  function loft(m, secs, N, paint, noseTip, tailTip) {
    const rings = secs.map((s) => {
      const r = [];
      for (let k = 0; k < N; k++) {
        const a = (k + 0.5) / N * Math.PI * 2;
        r.push(m.vert(s.w * Math.sin(a), s.y + s.h * Math.cos(a), s.z));
      }
      return r;
    });
    const z0 = secs[0].z, z1 = secs[secs.length - 1].z;
    const face = (ids) => {
      const P = ids.map((i) => m.v[i]);
      const c = mul(P.reduce(add), 1 / P.length);
      const n = norm(cross(sub(P[1], P[0]), sub(P[2], P[0])));
      return paint({ c, n, t: (z0 - c[2]) / (z0 - z1) });
    };
    const put = (ids) => { const col = face(ids); const em = col.em; const cc = col.c || col;
      if (ids.length === 3) m.tri(ids[0], ids[1], ids[2], cc, em); else m.quad(ids[0], ids[1], ids[2], ids[3], cc, em); };
    if (noseTip) { const t = m.vert(...noseTip); const r = rings[0];
      for (let k = 0; k < N; k++) put([t, r[(k + 1) % N], r[k]]); }
    for (let j = 0; j < rings.length - 1; j++) {
      const A = rings[j], B = rings[j + 1];
      for (let k = 0; k < N; k++) { const k2 = (k + 1) % N; put([A[k], A[k2], B[k2], B[k]]); }
    }
    const r = rings[rings.length - 1];
    if (tailTip) { const t = m.vert(...tailTip); for (let k = 0; k < N; k++) put([t, r[k], r[(k + 1) % N]]); }
    else { const c = m.vert(0, secs[secs.length - 1].y, z1); for (let k = 0; k < N; k++) put([c, r[k], r[(k + 1) % N]]); }
    return rings;
  }

  // Triangular plate (fin/wing) with thickness: 8 triangles.
  function fin(m, p0, p1, p2, th, colTop, colBot, colEdge) {
    let n = norm(cross(sub(p1, p0), sub(p2, p0)));
    // keep "top" colour on the upward face of horizontal-ish fins
    if (n[1] < -0.2) { const q = p1; p1 = p2; p2 = q; n = mul(n, -1); }
    const o = mul(n, th / 2);
    const a = [add(p0, o), add(p1, o), add(p2, o)].map((p) => m.vert(...p));
    const b = [sub(p0, o), sub(p1, o), sub(p2, o)].map((p) => m.vert(...p));
    m.tri(a[0], a[1], a[2], colTop);
    m.tri(b[0], b[2], b[1], colBot || colTop);
    const e = colEdge || colTop;
    m.quad(a[0], b[0], b[1], a[1], e); m.quad(a[1], b[1], b[2], a[2], e); m.quad(a[2], b[2], b[0], a[0], e);
  }
  const mirrorX = (p) => [-p[0], p[1], p[2]];
  // Fin on side s (+1 right, -1 left): mirroring flips winding, so swap two points.
  function finS(m, s, p0, p1, p2, th, a, b, e) {
    if (s > 0) fin(m, p0, p1, p2, th, a, b, e);
    else fin(m, mirrorX(p0), mirrorX(p2), mirrorX(p1), th, a, b, e);
  }

  // Prism along an axis (barrels, pods, nozzles). sides*2 + caps triangles.
  function prism(m, from, to, r, sides, col, capCol, capEm, r2) {
    const ax = norm(sub(to, from));
    const up = Math.abs(ax[1]) > 0.9 ? [1, 0, 0] : [0, 1, 0];
    const u = norm(cross(ax, up)), v = cross(u, ax);
    const ringAt = (c, rad) => { const ids = [];
      for (let k = 0; k < sides; k++) { const a = (k + 0.5) / sides * Math.PI * 2;
        ids.push(m.vert(...add(c, add(mul(u, Math.cos(a) * rad), mul(v, Math.sin(a) * rad))))); }
      return ids; };
    const A = ringAt(from, r), B = ringAt(to, r2 == null ? r : r2);
    for (let k = 0; k < sides; k++) { const k2 = (k + 1) % sides; m.quad(A[k], B[k], B[k2], A[k2], col); }
    for (let k = 1; k < sides - 1; k++) m.tri(A[0], A[k + 1], A[k], col);
    for (let k = 1; k < sides - 1; k++) m.tri(B[0], B[k], B[k + 1], capCol || col, capEm);
  }

  function box(m, c, s, col, em, frontCol, frontEm) {
    const [x, y, z] = c, [a, b, d] = s.map((q) => q / 2);
    const p = [[-a,-b,-d],[a,-b,-d],[a,b,-d],[-a,b,-d],[-a,-b,d],[a,-b,d],[a,b,d],[-a,b,d]].map((q) => m.vert(x + q[0], y + q[1], z + q[2]));
    m.quad(p[4], p[5], p[6], p[7], frontCol || col, frontEm != null ? frontEm : em); // front (+z)
    m.quad(p[1], p[0], p[3], p[2], col, em);
    m.quad(p[0], p[4], p[7], p[3], col, em); m.quad(p[5], p[1], p[2], p[6], col, em);
    m.quad(p[3], p[7], p[6], p[2], col, em); m.quad(p[0], p[1], p[5], p[4], col, em);
  }

  // Orca paint job for a lofted hull. eye: [t0,t1] canopy band, flank: [t0,t1] white flank.
  function orcaPaint(o) {
    return ({ c, n, t }) => {
      if (n[1] < -0.6 && t < (o.bellyEnd || 0.8)) return C.belly;
      const side = Math.abs(n[0]) > 0.55;
      if (side && n[1] < 0 && t < (o.chin || 0.3)) return C.belly;
      if (side && t > o.eye[0] && t < o.eye[1] && n[1] > 0.1) return { c: C.glowC, em: true };
      if (side && o.flank && t > o.flank[0] && t < o.flank[1] && n[1] < 0) return C.belly;
      if (n[1] > 0.6 && o.saddle && t > o.saddle[0] && t < o.saddle[1]) return C.saddle;
      return C.hull;
    };
  }

  // ---------- ship 1: APEX — balanced gunship ----------
  function apex() {
    const m = new Mesh();
    loft(m, [
      { z: 5.0, y: -0.05, w: 0.72, h: 0.55 },
      { z: 4.1, y: 0.05, w: 1.1, h: 0.9 },
      { z: 3.2, y: 0.1, w: 1.3, h: 1.05 },
      { z: 2.4, y: 0.12, w: 1.4, h: 1.1 },
      { z: 0.2, y: 0.12, w: 1.35, h: 1.05 },
      { z: -1.8, y: 0.15, w: 1.0, h: 0.8 },
      { z: -3.4, y: 0.2, w: 0.6, h: 0.45 },
      { z: -4.6, y: 0.25, w: 0.28, h: 0.24 },
    ], 8, orcaPaint({ eye: [0.18, 0.28], chin: 0.2, flank: [0.5, 0.7], saddle: [0.5, 0.7] }), [0, -0.1, 5.6]);
    // dorsal fin — tall and swept, drone cradle on the tip
    fin(m, [0, 1.0, 1.4], [0, 1.0, -1.2], [0, 3.1, -1.7], 0.18, C.hull);
    prism(m, [0, 3.0, -1.35], [0, 3.0, -2.15], 0.16, 6, C.trim, C.glowG, true);
    // pectoral fins with pulse cannons on the tips
    for (const s of [1, -1]) {
      const P = (p) => (s > 0 ? p : mirrorX(p));
      finS(m, s, [1.0, -0.55, 2.0], [1.0, -0.55, 0.6], [3.1, -1.25, 0.2], 0.14, C.hull, C.belly);
      prism(m, P([3.05, -1.25, 1.4]), P([3.05, -1.25, -0.2]), 0.17, 6, C.gun, C.gunTip, true);
      // twin "tooth" rail barrels under the jaw
      prism(m, P([0.32, -0.55, 5.2]), P([0.32, -0.55, 2.8]), 0.07, 5, C.gun, C.gunTip, true);
      // flukes + ion engines
      finS(m, s, [0.15, 0.25, -4.1], [0.15, 0.25, -4.8], [2.1, 0.15, -5.6], 0.12, C.hull, C.belly);
      prism(m, P([0.55, 0.05, -3.2]), P([0.62, 0.05, -4.3]), 0.26, 6, C.trim, C.glowC, true, 0.3);
    }
    return m;
  }

  // ---------- ship 2: BLACKFISH — interceptor ----------
  function blackfish() {
    const m = new Mesh();
    loft(m, [
      { z: 5.6, y: -0.05, w: 0.5, h: 0.38 },
      { z: 4.7, y: 0.0, w: 0.82, h: 0.6 },
      { z: 3.8, y: 0.05, w: 0.95, h: 0.68 },
      { z: 3.0, y: 0.05, w: 1.0, h: 0.72 },
      { z: 0.0, y: 0.05, w: 0.95, h: 0.66 },
      { z: -2.4, y: 0.08, w: 0.7, h: 0.48 },
      { z: -4.0, y: 0.1, w: 0.4, h: 0.28 },
      { z: -5.2, y: 0.12, w: 0.2, h: 0.16 },
    ], 8, orcaPaint({ eye: [0.18, 0.26], chin: 0.18, flank: [0.4, 0.62], saddle: [0.4, 0.62], bellyEnd: 0.85 }), [0, -0.1, 6.3]);
    fin(m, [0, 0.62, 0.6], [0, 0.62, -1.6], [0, 1.7, -2.4], 0.14, C.hull);
    for (const s of [1, -1]) {
      const P = (p) => (s > 0 ? p : mirrorX(p));
      // big swept delta pectorals
      finS(m, s, [0.8, -0.2, 2.6], [0.8, -0.2, -1.4], [3.9, -0.55, -2.6], 0.12, C.hull, C.belly);
      // wingtip lasers
      prism(m, P([3.75, -0.55, 0.4]), P([3.85, -0.55, -2.6]), 0.09, 5, C.gun, C.glowC, true);
      // flukes, swept hard
      finS(m, s, [0.12, 0.15, -4.6], [0.12, 0.15, -5.4], [1.9, 0.1, -6.6], 0.1, C.hull, C.belly);
      // four engines: two per side, stacked
      prism(m, P([0.5, 0.25, -3.0]), P([0.52, 0.25, -4.4]), 0.2, 6, C.trim, C.glowC, true, 0.22);
      prism(m, P([0.5, -0.25, -3.0]), P([0.52, -0.25, -4.4]), 0.2, 6, C.trim, C.glowC, true, 0.22);
    }
    return m;
  }

  // ---------- ship 3: MATRIARCH — heavy carrier ----------
  function matriarch() {
    const m = new Mesh();
    loft(m, [
      { z: 4.6, y: -0.05, w: 1.1, h: 0.8 },
      { z: 3.8, y: 0.05, w: 1.6, h: 1.2 },
      { z: 2.9, y: 0.1, w: 1.85, h: 1.38 },
      { z: 2.1, y: 0.12, w: 1.95, h: 1.45 },
      { z: -0.6, y: 0.12, w: 1.85, h: 1.38 },
      { z: -2.4, y: 0.15, w: 1.3, h: 1.0 },
      { z: -3.6, y: 0.2, w: 0.8, h: 0.62 },
      { z: -4.6, y: 0.25, w: 0.4, h: 0.35 },
    ], 8, orcaPaint({ eye: [0.18, 0.3], chin: 0.2, flank: [0.45, 0.66], saddle: [0.45, 0.66] }), [0, -0.1, 5.2]);
    // towering dorsal fin = Guardian Angel hangar; glowing launch slots on both faces
    fin(m, [0, 1.35, 1.6], [0, 1.35, -1.6], [0, 4.4, -2.0], 0.34, C.hull);
    for (let i = 0; i < 3; i++) {
      const y = 1.9 + i * 0.7, z = 0.25 - i * 0.55;
      box(m, [0, y, z - 0.35], [0.4, 0.14, 0.7], C.glowG, true);
    }
    for (const s of [1, -1]) {
      const P = (p) => (s > 0 ? p : mirrorX(p));
      finS(m, s, [1.45, -0.8, 1.8], [1.45, -0.8, 0.2], [3.3, -1.5, -0.4], 0.16, C.hull, C.belly);
      // flank missile pods — magenta tube mouths
      box(m, P([1.95, 0.35, 0.4]), [0.55, 0.6, 2.4], C.trim, false, C.glowM, true);
      // flukes + big engines
      finS(m, s, [0.2, 0.3, -4.0], [0.2, 0.3, -4.6], [2.5, 0.2, -5.4], 0.14, C.hull, C.belly);
      prism(m, P([0.8, 0.1, -2.9]), P([0.85, 0.1, -4.3]), 0.38, 6, C.trim, C.glowC, true, 0.42);
    }
    // belly flak turret
    prism(m, [0, -1.4, 1.3], [0, -1.75, 1.3], 0.35, 6, C.trim);
    prism(m, [0, -1.62, 1.3], [0, -1.62, 2.6], 0.08, 5, C.gun, C.gunTip, true);
    return m;
  }

  // ---------- ship 4: TIDEBREAKER — brawler with open jaws ----------
  function tidebreaker() {
    const m = new Mesh();
    // upper skull/body
    loft(m, [
      { z: 5.0, y: 0.3, w: 0.7, h: 0.4 },
      { z: 4.1, y: 0.35, w: 1.15, h: 0.75 },
      { z: 3.2, y: 0.3, w: 1.4, h: 1.0 },
      { z: 2.4, y: 0.25, w: 1.5, h: 1.1 },
      { z: 0.0, y: 0.2, w: 1.42, h: 1.05 },
      { z: -1.9, y: 0.2, w: 1.05, h: 0.8 },
      { z: -3.4, y: 0.22, w: 0.62, h: 0.46 },
      { z: -4.6, y: 0.28, w: 0.3, h: 0.25 },
    ], 8, orcaPaint({ eye: [0.18, 0.3], chin: 0.2, flank: [0.5, 0.7], saddle: [0.5, 0.7] }), [0, 0.25, 5.6]);
    // lower jaw hanging open (separate wedge)
    loft(m, [
      { z: 5.0, y: -0.95, w: 0.35, h: 0.16 },
      { z: 3.4, y: -0.85, w: 0.85, h: 0.26 },
      { z: 2.0, y: -0.7, w: 1.05, h: 0.32 },
    ], 6, () => C.belly, [0, -1.0, 5.7], null);
    // teeth-cannons: short glowing spikes along both jaw lines
    for (const s of [1, -1]) {
      const P = (p) => (s > 0 ? p : mirrorX(p));
      for (let i = 0; i < 3; i++) {
        const z = 4.6 - i * 0.6, x = 0.25 + i * 0.22;
        prism(m, P([x, -0.72, z]), P([x, -0.3, z]), 0.07, 4, C.belly, C.gunTip, true, 0.0);
      }
      finS(m, s, [1.2, -0.6, 2.1], [1.2, -0.6, 0.5], [3.0, -1.4, 0.0], 0.16, C.hull, C.belly);
      // heavy shoulder cannons
      prism(m, P([1.35, 0.9, 2.4]), P([1.35, 0.9, -0.6]), 0.24, 6, C.gun, C.glowM, true);
      prism(m, P([1.35, 0.9, 3.4]), P([1.35, 0.9, 2.4]), 0.11, 5, C.gun, C.gunTip, true);
      finS(m, s, [0.15, 0.3, -4.1], [0.15, 0.3, -4.8], [2.2, 0.2, -5.5], 0.12, C.hull, C.belly);
      prism(m, P([0.6, 0.1, -3.0]), P([0.66, 0.1, -4.2]), 0.28, 6, C.trim, C.glowC, true, 0.32);
    }
    // stubby, thick dorsal — notched brawler fin
    fin(m, [0, 1.1, 1.0], [0, 1.1, -1.2], [0, 2.5, -1.3], 0.22, C.hull);
    // ram spike
    prism(m, [0, 0.25, 5.4], [0, 0.25, 6.6], 0.14, 4, C.trim, C.belly, false, 0.0);
    return m;
  }

  // ---------- Guardian Angel drone: a manta ray ----------
  function drone() {
    const m = new Mesh();
    // Points on the midline (x = 0) and one wing (x > 0); the other wing mirrors.
    const top = { nose: [0, 0.06, 0.62], ridge: [0, 0.22, 0.05], back: [0, 0.1, -0.45] };
    const W = { front: [0.7, 0.1, 0.45], mid: [1.2, 0.03, 0.14], tip: [1.8, -0.1, -0.38],
                rear: [0.7, 0.05, -0.38] };
    const V = (p) => m.vert(...p), Vm = (p) => m.vert(-p[0], p[1], p[2]);
    const n = V(top.nose), r = V(top.ridge), b = V(top.back);
    const belly = V([0, -0.06, 0.02]);
    for (const s of [1, -1]) {
      const P = s > 0 ? V : Vm;
      const f = P(W.front), md = P(W.mid), t = P(W.tip), rr = P(W.rear);
      // orient each face explicitly: belly faces point down, everything else up
      const tri = (a, c, d, col, em) => {
        const ny = cross(sub(m.v[c], m.v[a]), sub(m.v[d], m.v[a]))[1];
        const wantDown = col === C.belly;
        if ((ny < 0) !== wantDown) m.tri(a, d, c, col, em); else m.tri(a, c, d, col, em);
      };
      // dark back
      tri(n, f, r, C.hull); tri(r, f, md, C.hull); tri(r, md, rr, C.hull);
      tri(md, t, rr, C.hull); tri(r, rr, b, C.hull);
      // white belly (real mantas are white underneath)
      tri(n, belly, f, C.belly); tri(f, belly, md, C.belly); tri(md, belly, rr, C.belly);
      tri(md, rr, t, C.belly); tri(rr, belly, b, C.belly);
      // gold glow along the wing's leading edge: the "angel" part
      const e1 = P([W.mid[0], W.mid[1] + 0.02, W.mid[2] + 0.06]), e2 = P([W.tip[0] + 0.05, W.tip[1] + 0.02, W.tip[2] + 0.02]);
      tri(md, e1, e2, C.glowG, true); tri(md, e2, t, C.glowG, true);
      const e0 = P([W.front[0], W.front[1] + 0.02, W.front[2] + 0.06]);
      tri(f, e0, e1, C.glowG, true); tri(f, e1, md, C.glowG, true);
      // gold chevron on the back
      const c0 = P([0.05, 0.2, -0.02]), c1 = P([0.42, 0.13, -0.18]), c2 = P([0.36, 0.14, -0.3]);
      tri(c0, c1, c2, C.glowG, true);
      // cephalic fins: the two forward "horns"
      const h0 = P([0.16, 0.05, 0.55]), h1 = P([0.3, 0.02, 0.5]), h2 = P([0.24, -0.04, 0.86]);
      tri(h0, h1, h2, C.hull); tri(h0, h1, h2, C.belly);
      // glowing eye on the head edge
      const y0 = P([0.3, 0.09, 0.44]), y1 = P([0.4, 0.08, 0.38]), y2 = P([0.33, 0.12, 0.36]);
      tri(y0, y1, y2, C.glowG, true);
    }
    // whip tail
    const t0 = m.vert(0.03, 0.08, -0.45), t1 = m.vert(-0.03, 0.08, -0.45), t2 = m.vert(0, 0.03, -1.7);
    m.tri(t0, t1, t2, C.hull); m.tri(t1, t0, t2, C.hull);
    return m;
  }

  // ---------- renderer ----------
  // opts: yaw, pitch, roll (radians), dist, fov, light, W, H, cx, cy, scale
  function render(ctx, mesh, o) {
    const cy = Math.cos(o.yaw), sy = Math.sin(o.yaw), cp = Math.cos(o.pitch), sp = Math.sin(o.pitch);
    const cr = Math.cos(o.roll || 0), sr = Math.sin(o.roll || 0);
    const off = o.offset || [0, 0, 0];
    const V = mesh.v.map(([x, y, z]) => {
      // roll about z, yaw about y, pitch about x
      let x1 = x * cr - y * sr, y1 = x * sr + y * cr, z1 = z;
      let x2 = x1 * cy + z1 * sy, z2 = -x1 * sy + z1 * cy, y2 = y1;
      let y3 = y2 * cp - z2 * sp, z3 = y2 * sp + z2 * cp;
      return [x2 + off[0], y3 + off[1], z3 + off[2]];
    });
    const f = o.fov, d = o.dist;
    const P = V.map(([x, y, z]) => { const w = d - z; return [o.cx + (x * f) / w, o.cy - (y * f) / w, w]; });
    const L = norm(o.light || [-0.4, 0.7, 0.6]);
    const out = [];
    for (const face of mesh.f) {
      const [a, b, c] = face.i;
      const n = norm(cross(sub(V[b], V[a]), sub(V[c], V[a])));
      // view direction from face to camera at (0,0,d)
      const vc = sub([0, 0, d], V[a]);
      if (dot(n, vc) <= 0) continue;
      let col = face.col;
      if (!face.em) {
        const diff = Math.max(0, dot(n, L));
        const rim = Math.pow(1 - Math.max(0, dot(n, norm(vc))), 3) * 0.55;
        const k = 0.62 + 1.1 * diff;
        col = [col[0] * k + 60 * rim, col[1] * k + 90 * rim, col[2] * k + 140 * rim];
      }
      out.push({ z: (P[a][2] + P[b][2] + P[c][2]) / 3, p: [P[a], P[b], P[c]], col, em: face.em });
    }
    out.sort((q, r) => r.z - q.z);
    for (const t of out) {
      const col = `rgb(${Math.min(255, t.col[0]) | 0},${Math.min(255, t.col[1]) | 0},${Math.min(255, t.col[2]) | 0})`;
      ctx.fillStyle = col; ctx.strokeStyle = col; ctx.lineWidth = o.seam == null ? 0.6 : o.seam;
      ctx.beginPath(); ctx.moveTo(t.p[0][0], t.p[0][1]); ctx.lineTo(t.p[1][0], t.p[1][1]); ctx.lineTo(t.p[2][0], t.p[2][1]); ctx.closePath();
      ctx.fill(); if (ctx.lineWidth > 0) ctx.stroke();
    }
    return out.length;
  }

  // Quantise a canvas region to RGB565, as the ST7796 would show it.
  function to565(ctx, W, H) {
    const img = ctx.getImageData(0, 0, W, H), d = img.data;
    for (let i = 0; i < d.length; i += 4) {
      d[i] = (d[i] >> 3) << 3; d[i + 1] = (d[i + 1] >> 2) << 2; d[i + 2] = (d[i + 2] >> 3) << 3;
    }
    ctx.putImageData(img, 0, 0);
  }

  const SHIPS = {
    apex: { name: 'APEX', cls: 'Gunship', build: apex },
    blackfish: { name: 'BLACKFISH', cls: 'Interceptor', build: blackfish },
    matriarch: { name: 'MATRIARCH', cls: 'Heavy carrier', build: matriarch },
    tidebreaker: { name: 'TIDEBREAKER', cls: 'Brawler', build: tidebreaker },
  };
  for (const k in SHIPS) { SHIPS[k].mesh = SHIPS[k].build(); SHIPS[k].tris = SHIPS[k].mesh.f.length; }
  const DRONE = drone();

  G.OrcaShips = { SHIPS, DRONE, render, to565, C };
})(typeof window !== 'undefined' ? window : globalThis);
