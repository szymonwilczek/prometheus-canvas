/*
 * Multi-scale stroke-based oil painter.
 *
 * Knife renderer (knife.c) rebuilds the image from straight smears.
 * This stage paints the way brush does: strokes are *traced* through the
 * coherent flow field (field.c), bending along contours, and the canvas
 * is built bottom-to-top from three independent passes:
 *
 * 1. UNDERCOAT - broad, flat polygonal slabs that block in color over
 *    the whole canvas.
 *    No curvature: this is the palette-knife ground coat every oil
 *    painting starts from.
 * 2. FORM - medium strokes traced along the flow field.
 *    Each stroke starts at a grid-jittered anchor, carries one load of
 *    paint (the target color under the anchor), and walks the field until
 *    it runs off the canvas, exceeds its length budget, or crosses color
 *    boundary - the moment when painter lifts the brush.
 * 3. DETAIL - dense micro-strokes gated by the visual-importance map:
 *    eyes, hair and fabric edges earn them, flat sky does not.
 *
 * `alignment` parameter blends per-step field following against straight
 * continuation of the stroke's initial direction:
 * 1 = strokes bend fully with the contours,
 * 0 = every stroke is a straight flick
 *
 * VOLUME:
 * Alongside the color buffer every stroke writes parabolic thickness profile
 * into a persistent height field - thick on the stroke spine, feathering to
 * nothing at the rim.
 * Overlaps blend max-accumulate: paint already on the canvas lifts the new
 * stroke, so crossings build physical ridges instead of averaging flat.
 *
 * BRISTLES:
 * 1D hash noise in stroke-local coordinates (lanes across the width, ticks
 * along the drag) modulates both the color (streaks of incompletely mixed
 * pigment) and the height profile (grooves scratched by individual hairs).
 *
 * PAINT PHYSICS (shared with the knife):
 * dry-brush skipping over the pass-start relief, wet-on-wet upstream dragging,
 * contaminating brush reservoir with exponential load depletion, and
 * subtractive Kubelka-Munk mixing for every color blend (pc_mix_paint).
 *
 * Deterministic: all randomness is the integer hash of grid indices.
 */
#include "pc.h"

typedef struct {
  u8 *img;          /* canvas being painted             */
  f32 *height;      /* accumulated paint thickness      */
  const u8 *target; /* what the painting should become  */
  const f32 *hbase; /* relief snapshot at pass start    */
  const f32 *fx, *fy, *aniso, *imp;
  i32 w, h;
  f32 bristle;
  f32 dry;    /* dry-brush skipping threshold     */
  f32 drag;   /* wet-on-wet pigment pickup        */
  f32 vib;    /* subtractive mixing strength      */
  f32 fringe; /* pigment halo at stroke margins   */
} Sbr;

static f32 smoothstep(f32 e0, f32 e1, f32 x) {
  f32 t = pc_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/*
 * Pigment fringe:
 * bristles push the densest pigment particles to the stroke margin.
 * Just inside the AA rim the paint runs darker and slightly more saturated -
 * the organic dark halo that defines every overlapping stroke.
 */
static void apply_fringe(f32 *c, f32 vn, f32 fringe) {
  f32 fr = fringe * smoothstep(0.72f, 0.90f, vn) *
           (1.0f - smoothstep(0.93f, 1.0f, vn));
  if (fr <= 0.0f)
    return;
  f32 l = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
  for (i32 k = 0; k < 3; k++)
    c[k] =
        pc_clampf((l + (c[k] - l) * (1.0f + 0.35f * fr)) * (1.0f - 0.28f * fr),
                  0.0f, 255.0f);
}

/*
 * Deposit `hp` of paint at pixel i with coverage `op`.
 * Max-accumulate: existing paint boosts the incoming stroke by up to 30% of its
 * body, so overlapping strokes ridge up instead of replacing each other, while
 * thin glaze can never dig a hole into thick layer below.
 */
static void deposit(Sbr *s, usize i, f32 hp, f32 thick, f32 op) {
  f32 hcur = s->height[i];
  f32 ridge = hp * (1.0f + 0.30f * pc_minf(hcur / (thick + 1e-3f), 1.0f));
  f32 nh = pc_maxf(hcur, ridge);
  s->height[i] = hcur + (nh - hcur) * op;
}

/* mean target color over a sparse 3x3 sample cross of radius r */
static void sample_color(const u8 *src, i32 w, i32 h, i32 cx, i32 cy, i32 r,
                         f32 *out) {
  f32 sr = 0, sg = 0, sb = 0;
  for (i32 dy = -1; dy <= 1; dy++) {
    for (i32 dx = -1; dx <= 1; dx++) {
      i32 x = pc_clampi(cx + dx * r, 0, w - 1);
      i32 y = pc_clampi(cy + dy * r, 0, h - 1);
      const u8 *p = src + ((usize)y * (usize)w + (usize)x) * 4;
      sr += (f32)p[0];
      sg += (f32)p[1];
      sb += (f32)p[2];
    }
  }
  out[0] = sr * (1.0f / 9.0f);
  out[1] = sg * (1.0f / 9.0f);
  out[2] = sb * (1.0f / 9.0f);
}

static f32 color_dist(const f32 *c, const u8 *p) {
  f32 dr = c[0] - (f32)p[0];
  f32 dg = c[1] - (f32)p[1];
  f32 db = c[2] - (f32)p[2];
  return pc_sqrtf(dr * dr + dg * dg + db * db);
}

/*
 * One capsule segment of a traced stroke:
 * every pixel within `rad` of the segment [a, b] takes the stroke color
 * and parabolic thickness profile - `thick` on the spine, zero at the rim,
 * exactly the cross-section of bead of paint squeezed under a round brush.
 * Half-pixel AA on the rim;
 * `fade` models the brush running out of paint toward the stroke tail.
 *
 * Bristle noise lives in stroke-local coordinates:
 * `lane` is the hair index across the width, `s_base + t*len` the distance
 * dragged along the path, so streaks stay glued to the stroke as it bends.
 *
 * `tail` is the progress along the whole stroke (0 anchor, 1 end):
 * wet-on-wet dragging strengthens toward the tail, where the brush has spent
 * its own load and mostly pushes what it picked up.
 */
static void stamp_segment(Sbr *s, f32 ax, f32 ay, f32 bx, f32 by, f32 rad,
                          const f32 *col, f32 thick, f32 fade, f32 tail,
                          f32 s_base, i32 seed) {
  i32 x0 = pc_maxi(0, (i32)(pc_minf(ax, bx) - rad) - 1);
  i32 x1 = pc_mini(s->w - 1, (i32)(pc_maxf(ax, bx) + rad) + 1);
  i32 y0 = pc_maxi(0, (i32)(pc_minf(ay, by) - rad) - 1);
  i32 y1 = pc_mini(s->h - 1, (i32)(pc_maxf(ay, by) + rad) + 1);

  f32 dx = bx - ax, dy = by - ay;
  f32 len2 = dx * dx + dy * dy;
  f32 len = pc_sqrtf(len2);
  f32 inv_len2 = len2 > 1e-6f ? 1.0f / len2 : 0.0f;
  f32 udx = len > 1e-3f ? dx / len : 1.0f;
  f32 udy = len > 1e-3f ? dy / len : 0.0f;
  f32 inv_rad = 1.0f / rad;

  for (i32 py = y0; py <= y1; py++) {
    for (i32 px = x0; px <= x1; px++) {
      f32 rx = (f32)px - ax, ry = (f32)py - ay;
      f32 t = pc_clampf((rx * dx + ry * dy) * inv_len2, 0.0f, 1.0f);
      f32 ex = rx - t * dx, ey = ry - t * dy;
      f32 d = pc_sqrtf(ex * ex + ey * ey);

      f32 cov = rad - d + 0.5f;
      if (cov <= 0.0f)
        continue;
      f32 alpha = pc_minf(cov, 1.0f) * fade;

      /* bristle noise: one value per hair lane, one per drag tick */
      f32 b = -rx * udy + ry * udx; /* signed across-stroke offset */
      i32 lane = (i32)(b * inv_rad * 3.0f + 16.0f);
      f32 ln = pc_hash2(lane * 37 + seed, seed) - 0.5f;
      f32 tick =
          pc_hash2((i32)((s_base + t * len) * 0.5f), lane * 7 + seed) - 0.5f;
      f32 cmod = 1.0f + s->bristle * (0.28f * ln + 0.14f * tick);
      f32 hmod = 1.0f + s->bristle * (0.45f * ln + 0.25f * tick);

      /* bristled coverage grain: paint never lays down evenly */
      f32 grain = 0.82f + 0.18f * pc_hash2(px + seed, py - seed);
      f32 op = alpha * grain;

      usize i = (usize)py * (usize)s->w + (usize)px;

      /* dry-brush skipping - same physics as the knife:
       * starved brush rides the peaks of the pass-start relief
       * (+ canvas tooth) and leaves the valleys as organic voids */
      if (s->dry > 0.0f) {
        f32 tooth = pc_hash2(px * 3 + 11, py * 3 + 29);
        f32 peak =
            pc_clampf(0.5f + (s->height[i] - s->hbase[i]) * 8.0f, 0.0f, 1.0f);
        peak = pc_clampf(peak + (tooth - 0.5f) * 0.35f, 0.0f, 1.0f);
        f32 dep = smoothstep(s->dry - 0.30f, s->dry + 0.15f, peak);
        op *= 1.0f - s->dry * (1.0f - dep);
      }

      f32 c[3] = {pc_clampf(col[0] * cmod, 0.0f, 255.0f),
                  pc_clampf(col[1] * cmod, 0.0f, 255.0f),
                  pc_clampf(col[2] * cmod, 0.0f, 255.0f)};

      /* wet-on-wet:
       * fold in wet pigment sampled upstream along the flow direction,
       * mixed subtractively */
      if (s->drag > 0.0f) {
        i32 ux = pc_clampi((i32)((f32)px - udx * 3.0f), 0, s->w - 1);
        i32 uy = pc_clampi((i32)((f32)py - udy * 3.0f), 0, s->h - 1);
        const u8 *up = s->img + ((usize)uy * (usize)s->w + (usize)ux) * 4;
        f32 upf[3] = {(f32)up[0], (f32)up[1], (f32)up[2]};
        f32 m = s->drag * 0.45f * (0.25f + 0.75f * tail);
        pc_mix_paint(c, upf, m, s->vib, c);
      }

      /* dark pigment halo just inside the stroke rim */
      apply_fringe(c, d * inv_rad, s->fringe);

      usize o = i * 4;
      u8 *im = s->img;
      f32 cur[3] = {(f32)im[o], (f32)im[o + 1], (f32)im[o + 2]};
      pc_mix_paint(cur, c, op, s->vib, cur);
      im[o] = pc_clamp255(cur[0]);
      im[o + 1] = pc_clamp255(cur[1]);
      im[o + 2] = pc_clamp255(cur[2]);
      im[o + 3] = 255;

      /* parabolic bead cross-section, tail keeps most of its body */
      f32 q = 1.0f - (d * inv_rad) * (d * inv_rad);
      if (q > 0.0f) {
        f32 hp = thick * q * hmod * (0.70f + 0.30f * fade);
        deposit(s, i, hp, thick, op);
      }
    }
  }
}

/* Undercoat slab:
 * One flat oriented rectangle of paint, streaked along the drag direction in
 * hash - jittered lanes like a loaded knife pass.
 * Relief is nearly flat plate, gently thickening toward the lift -
 * off end and doming slightly across the width.
 */
static void stamp_slab(Sbr *s, f32 cx, f32 cy, f32 dx, f32 dy, f32 len, f32 wid,
                       const f32 *col, f32 thick, i32 sd) {
  f32 hl = len * 0.5f, hw = wid * 0.5f;
  f32 reach = hl + hw;
  i32 x0 = pc_maxi(0, (i32)(cx - reach));
  i32 x1 = pc_mini(s->w - 1, (i32)(cx + reach) + 1);
  i32 y0 = pc_maxi(0, (i32)(cy - reach));
  i32 y1 = pc_mini(s->h - 1, (i32)(cy + reach) + 1);

  for (i32 py = y0; py <= y1; py++) {
    for (i32 px = x0; px <= x1; px++) {
      f32 rx = (f32)px - cx, ry = (f32)py - cy;
      f32 a = rx * dx + ry * dy;  /* along the slab  */
      f32 b = -rx * dy + ry * dx; /* across the slab */

      f32 cov = pc_minf(hl - pc_fabsf(a), hw - pc_fabsf(b));
      if (cov <= -0.5f)
        continue;
      f32 alpha = pc_clampf(cov + 0.5f, 0.0f, 1.0f);

      i32 lane = (i32)((b / wid + 0.5f) * 5.0f + 16.0f);
      f32 lane_n = pc_hash2(lane * 31 + sd, sd);
      f32 lane_op = 0.85f + 0.15f * lane_n;
      f32 op = alpha * lane_op;

      f32 u = a / len + 0.5f;    /* 0 where the knife lands, 1 lift-off */
      f32 vn = pc_fabsf(b) / hw; /* 0 center, 1 long edge               */

      usize i = (usize)py * (usize)s->w + (usize)px;

      /* dry-brush skipping over the pass-start relief + canvas tooth */
      if (s->dry > 0.0f) {
        f32 tooth = pc_hash2(px * 3 + 11, py * 3 + 29);
        f32 peak =
            pc_clampf(0.5f + (s->height[i] - s->hbase[i]) * 8.0f, 0.0f, 1.0f);
        peak = pc_clampf(peak + (tooth - 0.5f) * 0.35f, 0.0f, 1.0f);
        f32 dep = smoothstep(s->dry - 0.30f, s->dry + 0.15f, peak);
        op *= 1.0f - s->dry * (1.0f - dep);
      }

      f32 c[3] = {col[0], col[1], col[2]};

      /* wet-on-wet: the slab shears whatever wet layer sits upstream */
      if (s->drag > 0.0f) {
        i32 ux = pc_clampi((i32)((f32)px - dx * 3.0f), 0, s->w - 1);
        i32 uy = pc_clampi((i32)((f32)py - dy * 3.0f), 0, s->h - 1);
        const u8 *up = s->img + ((usize)uy * (usize)s->w + (usize)ux) * 4;
        f32 upf[3] = {(f32)up[0], (f32)up[1], (f32)up[2]};
        f32 m = s->drag * 0.45f * (0.25f + 0.75f * u);
        pc_mix_paint(c, upf, m, s->vib, c);
      }

      /* dark pigment halo along the slab's long edges */
      apply_fringe(c, vn, s->fringe);

      usize o = i * 4;
      u8 *im = s->img;
      f32 cur[3] = {(f32)im[o], (f32)im[o + 1], (f32)im[o + 2]};
      pc_mix_paint(cur, c, op, s->vib, cur);
      im[o] = pc_clamp255(cur[0]);
      im[o + 1] = pc_clamp255(cur[1]);
      im[o + 2] = pc_clamp255(cur[2]);
      im[o + 3] = 255;

      f32 hp = thick * (0.80f + 0.20f * u) * (1.0f - 0.10f * vn * vn) *
               (0.9f + 0.2f * s->bristle * (lane_n - 0.5f));
      deposit(s, i, hp, thick, op);
    }
  }
}

/*
 * Trace one stroke from an anchor and stamp it segment by segment.
 *
 * Path re-reads the flow field every `rad` pixels (with sign continuity,
 * so the stroke never doubles back on itself) and blends the new field
 * direction against straight continuation by `alignment`.
 * Termination: length budget, canvas edge, or the target color under
 * the brush drifting too far from the load of paint the stroke carries.
 *
 * BRUSH STATE:
 * Load is a live reservoir. Every step the brush picks up wet pigment from
 * the canvas under its heel (subtractive contamination, so the original color's
 * contribution decays geometrically), and the deposit opacity depletes
 * exponentially along the path - stroke starts opaque with clean paint and ends
 * thin with whatever the bristles gathered on the way.
 */
static void paint_stroke(Sbr *s, f32 x0, f32 y0, f32 rad, i32 max_pts,
                         f32 alignment, f32 thick, f32 fbx, f32 fby, i32 seed) {
  i32 ix = pc_clampi((i32)x0, 0, s->w - 1);
  i32 iy = pc_clampi((i32)y0, 0, s->h - 1);

  f32 col[3];
  sample_color(s->target, s->w, s->h, ix, iy, pc_maxi(1, (i32)rad), col);

  usize i0 = (usize)iy * (usize)s->w + (usize)ix;
  f32 dx, dy;
  if (s->aniso[i0] < 0.15f) {
    dx = fbx; /* flat region: no contour to follow, take the fallback */
    dy = fby;
  } else {
    dx = s->fx[i0];
    dy = s->fy[i0];
  }

  f32 step = pc_maxf(1.0f, rad);
  f32 px = x0, py = y0;
  f32 dragged = 0.0f; /* along-path distance, anchors the bristle noise */
  f32 res[3] = {col[0], col[1], col[2]}; /* paint actually on the brush */
  i32 pts = 1;

  for (; pts < max_pts; pts++) {
    f32 nx = px + dx * step, ny = py + dy * step;
    if (nx < 0.0f || ny < 0.0f || nx > (f32)(s->w - 1) || ny > (f32)(s->h - 1))
      break;

    i32 qx = (i32)(nx + 0.5f), qy = (i32)(ny + 0.5f);
    usize q = (usize)qy * (usize)s->w + (usize)qx;

    /* paint on the brush no longer matches what should be under it: lift off */
    if (color_dist(col, s->target + q * 4) > 55.0f)
      break;

    /* contamination:
     * pick up wet canvas paint under the heel before this segment goes down */
    if (s->drag > 0.0f) {
      i32 hx = pc_clampi((i32)px, 0, s->w - 1);
      i32 hy = pc_clampi((i32)py, 0, s->h - 1);
      const u8 *cv = s->img + ((usize)hy * (usize)s->w + (usize)hx) * 4;
      f32 cvf[3] = {(f32)cv[0], (f32)cv[1], (f32)cv[2]};
      pc_mix_paint(res, cvf, s->drag * 0.30f, s->vib, res);
    }

    /* direction for the next segment, computed *before* stamping:
     * how hard the path bends here is the hand-pressure signal */
    f32 ndx = dx, ndy = dy;
    {
      f32 vx = s->fx[q], vy = s->fy[q];
      if (vx * dx + vy * dy < 0.0f) {
        vx = -vx;
        vy = -vy;
      }
      if (s->aniso[q] >= 0.15f) {
        f32 bx = dx + (vx - dx) * alignment;
        f32 by = dy + (vy - dy) * alignment;
        f32 bl = pc_sqrtf(bx * bx + by * by);
        if (bl > 1e-4f) {
          ndx = bx / bl;
          ndy = by / bl;
        }
      }
    }
    f32 curv = 1.0f - (dx * ndx + dy * ndy); /* 0 straight, grows w/ turn */

    f32 tail = (f32)pts / (f32)max_pts;
    /* Pressure and taper:
     * brush lands thin (head taper), presses to full width through the body,
     * widens where the path turns hard - the hand slows down and leans in -
     * and thins again as it lifts off at the tail */
    f32 wmul = (0.55f + 0.45f * smoothstep(0.0f, 0.22f, tail)) *
               (1.0f - 0.45f * smoothstep(0.72f, 1.0f, tail)) *
               (1.0f + pc_minf(0.5f, 5.0f * curv));

    /* exponential depletion of the load */
    f32 fade = 0.55f + 0.45f * pc_expf(-2.0f * tail);
    stamp_segment(s, px, py, nx, ny, rad * wmul, res, thick, fade, tail,
                  dragged, seed + pts * 97);
    dragged += step;
    px = nx;
    py = ny;
    dx = ndx;
    dy = ndy;
  }

  /* boundary right at the anchor: still leave one dab of paint */
  if (pts == 1)
    stamp_segment(s, x0, y0, x0, y0, rad, col, thick, 1.0f, 0.0f, 0.0f, seed);
}

/*
 * Paint the canvas in three passes, bottom to top.
 * `undercoat`, `form` and `detail` are per-pass stroke density multipliers;
 * 0 skips the pass entirely.
 * `size` is the undercoat slab size in px; the finer passes derive their brush
 * radii from it.
 * `height` (w*h f32, caller- allocated) receives the accumulated paint
 * thickness for relief shading;
 * `bristle` scales the hair-lane color/height modulation.
 * `dry`/`drag`/`vib` are the shared paint physics: dry-brush skipping
 * threshold, wet-on-wet pickup, and subtractive mixing strength.
 */
void pc_sbr(u8 *img, f32 *height, i32 w, i32 h, i32 size, f32 undercoat,
            f32 form, f32 detail, f32 alignment, f32 bristle, f32 azim, f32 dry,
            f32 drag, f32 vib, f32 fringe) {
  if (size < 8)
    size = 8;
  alignment = pc_clampf(alignment, 0.0f, 1.0f);

  usize n = (usize)w * (usize)h;
  u8 *target = (u8 *)pc_alloc(n * 4);
  f32 *fx = (f32 *)pc_alloc(n * 4);
  f32 *fy = (f32 *)pc_alloc(n * 4);
  f32 *aniso = (f32 *)pc_alloc(n * 4);
  f32 *imp = (f32 *)pc_alloc(n * 4);
  f32 *hbase = (f32 *)pc_alloc(n * 4);
  if (!target || !fx || !fy || !aniso || !imp || !hbase)
    return;

  memcpy(target, img, n * 4);
  memset(height, 0, n * 4);
  memset(hbase, 0, n * 4);
  pc_sbr_field(target, w, h, fx, fy, aniso, imp);

  Sbr s = {img,
           height,
           target,
           hbase,
           fx,
           fy,
           aniso,
           imp,
           w,
           h,
           pc_clampf(bristle, 0.0f, 1.0f),
           pc_clampf(dry, 0.0f, 1.0f),
           pc_clampf(drag, 0.0f, 1.0f),
           pc_clampf(vib, 0.0f, 1.0f),
           pc_clampf(fringe, 0.0f, 1.0f)};

  /* flat-region strokes follow the light,
   * like painter working the sky in one consistent sweep */
  f32 ldx = pc_sinf(azim + PC_HALFPI); /* cos(azim) */
  f32 ldy = -pc_sinf(azim);

  /* pass 1: undercoat - flat slabs cover the whole canvas */
  if (undercoat > 0.01f) {
    f32 dens = pc_clampf(undercoat, 0.25f, 2.0f);
    f32 spacing = (f32)size * 0.85f / dens;
    i32 gw = (i32)((f32)w / spacing) + 2;
    i32 gh = (i32)((f32)h / spacing) + 2;
    for (i32 gy = 0; gy < gh; gy++) {
      for (i32 gx = 0; gx < gw; gx++) {
        f32 j1 = pc_hash2(gx * 3 + 101, gy * 5 + 1);
        f32 j2 = pc_hash2(gx * 7 + 2, gy * 3 + 101);
        f32 cx = ((f32)gx + j1 - 0.5f) * spacing;
        f32 cy = ((f32)gy + j2 - 0.5f) * spacing;
        i32 icx = pc_clampi((i32)cx, 0, w - 1);
        i32 icy = pc_clampi((i32)cy, 0, h - 1);
        usize q = (usize)icy * (usize)w + (usize)icx;

        f32 dx, dy;
        if (aniso[q] < 0.3f) {
          f32 wob = (pc_hash2(gx + 101, gy) - 0.5f) * 0.4f;
          f32 cw = pc_sinf(wob + PC_HALFPI), sw = pc_sinf(wob);
          dx = ldx * cw - ldy * sw;
          dy = ldx * sw + ldy * cw;
        } else {
          dx = fx[q];
          dy = fy[q];
        }

        f32 len = (f32)size * (2.0f + 0.8f * pc_hash2(gx + 11, gy + 101));
        f32 wid = (f32)size * (1.0f + 0.3f * pc_hash2(gx + 23, gy + 7));

        f32 col[3];
        sample_color(target, w, h, icx, icy, size / 2, col);
        f32 thick = 0.25f + 0.10f * pc_hash2(gx * 13 + 101, gy * 17);
        stamp_slab(&s, cx, cy, dx, dy, len, wid, col, thick,
                   gx * 131 + gy * 197);
      }
    }
  }

  /* pass 2: form - medium strokes traced along the flow
   * refresh the skipping topography: this pass rides the undercoat */
  pc_box_blur(height, hbase, w, h, 2);
  if (form > 0.01f) {
    f32 dens = pc_clampf(form, 0.25f, 2.0f);
    f32 rad = pc_maxf(2.0f, (f32)size / 4.0f);
    f32 spacing = rad * 1.7f / dens;
    i32 gw = (i32)((f32)w / spacing) + 2;
    i32 gh = (i32)((f32)h / spacing) + 2;
    for (i32 gy = 0; gy < gh; gy++) {
      for (i32 gx = 0; gx < gw; gx++) {
        f32 j1 = pc_hash2(gx * 3 + 211, gy * 5 + 1);
        f32 j2 = pc_hash2(gx * 7 + 2, gy * 3 + 211);
        f32 cx = ((f32)gx + j1 - 0.5f) * spacing;
        f32 cy = ((f32)gy + j2 - 0.5f) * spacing;

        f32 wob = (pc_hash2(gx + 211, gy) - 0.5f) * 0.5f;
        f32 cw = pc_sinf(wob + PC_HALFPI), sw = pc_sinf(wob);
        f32 fbx = ldx * cw - ldy * sw;
        f32 fby = ldx * sw + ldy * cw;

        f32 thick = 0.45f + 0.15f * pc_hash2(gx * 13 + 211, gy * 17);
        paint_stroke(&s, cx, cy, rad, 10, alignment, thick, fbx, fby,
                     gx * 613 + gy * 769 + 211);
      }
    }
  }

  /* pass 3: detail - micro-strokes only where the image earns them
   * skipping topography now includes the form layer's ridges */
  pc_box_blur(height, hbase, w, h, 2);
  if (detail > 0.01f) {
    f32 dens = pc_clampf(detail, 0.25f, 2.0f);
    f32 rad = pc_maxf(1.0f, (f32)size / 10.0f);
    f32 spacing = pc_maxf(2.0f, rad * 2.2f) / dens;
    i32 gw = (i32)((f32)w / spacing) + 2;
    i32 gh = (i32)((f32)h / spacing) + 2;
    for (i32 gy = 0; gy < gh; gy++) {
      for (i32 gx = 0; gx < gw; gx++) {
        f32 j1 = pc_hash2(gx * 3 + 307, gy * 5 + 1);
        f32 j2 = pc_hash2(gx * 7 + 2, gy * 3 + 307);
        f32 cx = ((f32)gx + j1 - 0.5f) * spacing;
        f32 cy = ((f32)gy + j2 - 0.5f) * spacing;
        i32 icx = pc_clampi((i32)cx, 0, w - 1);
        i32 icy = pc_clampi((i32)cy, 0, h - 1);

        if (imp[(usize)icy * (usize)w + (usize)icx] < 0.30f)
          continue;

        f32 wob = (pc_hash2(gx + 307, gy) - 0.5f) * 0.6f;
        f32 cw = pc_sinf(wob + PC_HALFPI), sw = pc_sinf(wob);
        f32 fbx = ldx * cw - ldy * sw;
        f32 fby = ldx * sw + ldy * cw;

        f32 thick = 0.60f + 0.15f * pc_hash2(gx * 13 + 307, gy * 17);
        paint_stroke(&s, cx, cy, rad, 6, alignment, thick, fbx, fby,
                     gx * 419 + gy * 523 + 307);
      }
    }
  }

  /* soften the relief only (colors keep their painted edges):
   * two radius-1 passes round stroke ridges into slopes the light
   * can rake across instead of one-pixel cliffs */
  f32 *tmp = (f32 *)pc_alloc(n * 4);
  if (tmp) {
    pc_box_blur(height, tmp, w, h, 1);
    pc_box_blur(tmp, height, w, h, 1);
  }
}
