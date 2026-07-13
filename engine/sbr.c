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
 * Deterministic: all randomness is the integer hash of grid indices.
 */
#include "pc.h"

typedef struct {
  u8 *img;          /* canvas being painted            */
  const u8 *target; /* what the painting should become */
  const f32 *fx, *fy, *aniso, *imp;
  i32 w, h;
} Sbr;

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
 * One capsule segment of traced stroke:
 * every pixel within `rad` of the segment [a, b] takes the stroke color.
 * Half-pixel AA on the rim;
 * per-pixel hash grain keeps the coverage from reading as vector fill.
 * `fade` models the brush running out of paint toward the stroke tail.
 */
static void stamp_segment(Sbr *s, f32 ax, f32 ay, f32 bx, f32 by, f32 rad,
                          const f32 *col, f32 fade, i32 seed) {
  i32 x0 = pc_maxi(0, (i32)(pc_minf(ax, bx) - rad) - 1);
  i32 x1 = pc_mini(s->w - 1, (i32)(pc_maxf(ax, bx) + rad) + 1);
  i32 y0 = pc_maxi(0, (i32)(pc_minf(ay, by) - rad) - 1);
  i32 y1 = pc_mini(s->h - 1, (i32)(pc_maxf(ay, by) + rad) + 1);

  f32 dx = bx - ax, dy = by - ay;
  f32 len2 = dx * dx + dy * dy;
  f32 inv_len2 = len2 > 1e-6f ? 1.0f / len2 : 0.0f;

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

      /* bristled coverage grain: paint never lays down evenly */
      f32 grain = 0.82f + 0.18f * pc_hash2(px + seed, py - seed);
      f32 op = alpha * grain;

      usize o = ((usize)py * (usize)s->w + (usize)px) * 4;
      u8 *im = s->img;
      im[o] = pc_clamp255((f32)im[o] + (col[0] - (f32)im[o]) * op);
      im[o + 1] = pc_clamp255((f32)im[o + 1] + (col[1] - (f32)im[o + 1]) * op);
      im[o + 2] = pc_clamp255((f32)im[o + 2] + (col[2] - (f32)im[o + 2]) * op);
      im[o + 3] = 255;
    }
  }
}

/*
 * Undercoat slab:
 * one flat oriented rectangle of paint, streaked along the drag direction
 * in hash-jittered lanes like loaded knife pass.
 */
static void stamp_slab(Sbr *s, f32 cx, f32 cy, f32 dx, f32 dy, f32 len, f32 wid,
                       const f32 *col, i32 sd) {
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
      f32 lane_op = 0.85f + 0.15f * pc_hash2(lane * 31 + sd, sd);
      f32 op = alpha * lane_op;

      usize o = ((usize)py * (usize)s->w + (usize)px) * 4;
      u8 *im = s->img;
      im[o] = pc_clamp255((f32)im[o] + (col[0] - (f32)im[o]) * op);
      im[o + 1] = pc_clamp255((f32)im[o + 1] + (col[1] - (f32)im[o + 1]) * op);
      im[o + 2] = pc_clamp255((f32)im[o + 2] + (col[2] - (f32)im[o + 2]) * op);
      im[o + 3] = 255;
    }
  }
}

/*
 * Trace one stroke from an anchor and stamp it segment by segment.
 *
 * Path re-reads the flow field every `rad` pixels (with sign continuity, so
 * the stroke never doubles back on itself) and blends the new field direction
 * against straight continuation by `alignment`.
 * Termination: length budget, canvas edge, or the target color under the brush
 * drifting too far from the load of paint the stroke carries.
 */
static void paint_stroke(Sbr *s, f32 x0, f32 y0, f32 rad, i32 max_pts,
                         f32 alignment, f32 fbx, f32 fby, i32 seed) {
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

    f32 fade = 1.0f - 0.35f * (f32)pts / (f32)max_pts;
    stamp_segment(s, px, py, nx, ny, rad, col, fade, seed + pts * 97);
    px = nx;
    py = ny;

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
        dx = bx / bl;
        dy = by / bl;
      }
    }
  }

  /* boundary right at the anchor: still leave one dab of paint */
  if (pts == 1)
    stamp_segment(s, x0, y0, x0, y0, rad, col, 1.0f, seed);
}

/*
 * Paint the canvas in three passes, bottom to top.
 * `undercoat`, `form` and `detail` are per-pass stroke density multipliers;
 * 0 skips the pass entirely.
 * `size` is the undercoat slab size in px;
 * the finer passes derive their brush radii from it.
 */
void pc_sbr(u8 *img, i32 w, i32 h, i32 size, f32 undercoat, f32 form,
            f32 detail, f32 alignment, f32 azim) {
  if (size < 8)
    size = 8;
  alignment = pc_clampf(alignment, 0.0f, 1.0f);

  usize n = (usize)w * (usize)h;
  u8 *target = (u8 *)pc_alloc(n * 4);
  f32 *fx = (f32 *)pc_alloc(n * 4);
  f32 *fy = (f32 *)pc_alloc(n * 4);
  f32 *aniso = (f32 *)pc_alloc(n * 4);
  f32 *imp = (f32 *)pc_alloc(n * 4);
  if (!target || !fx || !fy || !aniso || !imp)
    return;

  memcpy(target, img, n * 4);
  pc_sbr_field(target, w, h, fx, fy, aniso, imp);

  Sbr s = {img, target, fx, fy, aniso, imp, w, h};

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
        stamp_slab(&s, cx, cy, dx, dy, len, wid, col, gx * 131 + gy * 197);
      }
    }
  }

  /* pass 2: form - medium strokes traced along the flow */
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

        paint_stroke(&s, cx, cy, rad, 10, alignment, fbx, fby,
                     gx * 613 + gy * 769 + 211);
      }
    }
  }

  /* pass 3: detail - micro-strokes only where the image earns them */
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

        paint_stroke(&s, cx, cy, rad, 6, alignment, fbx, fby,
                     gx * 419 + gy * 523 + 307);
      }
    }
  }
}
