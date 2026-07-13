/*
 * Stroke-based palette-knife renderer.
 *
 * Everything else in the engine filters pixels; this stage *paints*.
 * It is classic stroke-based rendering (Haeberli 1990, Hertzmann
 * 1998): the image is rebuilt from discrete, overlapping smears of
 * flat paint, exactly the way a palette knife lays down one load of
 * pigment per motion.
 *
 * Per smear:
 * - PLACEMENT: jittered grid, coarse-to-fine layers.
 *   Layer 0 covers the whole canvas with broad smears;
 *   finer layers only touch places where the canvas still differs from the
 *   target image (mean luma error over the smear footprint), so faces regain
 *   detail while skies stay as wide slabs - the decision painter makes with
 *   their eyes, expressed as an error integral.
 * - ORIENTATION: along the structure-tensor flow where the image has direction;
 *   in flat regions the knife follows the light azimuth with hash jitter, which
 *   is what makes skies read as deliberate directional work instead of noise.
 * - COLOR: one flat mix sampled from the target under the smear center, blended
 *   toward a second sample at the far end (a knife drags the color it picks
 *   up), plus a per-smear tint jitter - no two loads of paint are mixed
 *   identically.
 * - RELIEF: each smear writes its own thickness profile into the height field,
 *   *overwriting* what was below (wet paint covers wet paint):
 *   body that thickens toward the lift-off end, a slightly hollowed center
 *   and twin raised beads along the lateral margins where the flat blade
 *   squeezes displaced paint outward (per-smear tilt favors one bead).
 *   Blinn-Phong pass then lights real layered strokes instead of a texture.
 * - SKIPPING: a dry blade rides the peaks of the existing relief and the
 *   canvas tooth, leaving organic voids in the valleys (threshold `dry`).
 * - SMUDGING: wet-on-wet - each pixel folds in pigment sampled upstream
 *   along the drag, so the blade visibly shears the layer below (`drag`).
 *
 * Deterministic: all randomness is the integer hash of grid indices.
 */
#include "pc.h"

static f32 smoothstep(f32 e0, f32 e1, f32 x) {
  f32 t = pc_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/*
 * Pigment fringe:
 * bristles and the blade edge push the densest pigment particles to the stroke
 * margin.
 * Just inside the AA rim the paint runs darker and slightly *more* saturated -
 * luminance drops while chroma gains - which is what draws the organic dark
 * halo around every overlapping stroke.
 * `vn` is the lateral coordinate (0 spine, 1 rim).
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

/* mean target color over sparse 3x3 sample cross of radius r */
static void sample_color(const u8 *src, i32 w, i32 h, i32 cx, i32 cy, i32 r,
                         f32 *out) {
  f32 sr = 0, sg = 0, sb = 0;
  i32 cnt = 0;
  for (i32 dy = -1; dy <= 1; dy++) {
    for (i32 dx = -1; dx <= 1; dx++) {
      i32 x = pc_clampi(cx + dx * r, 0, w - 1);
      i32 y = pc_clampi(cy + dy * r, 0, h - 1);
      const u8 *p = src + ((usize)y * (usize)w + (usize)x) * 4;
      sr += (f32)p[0];
      sg += (f32)p[1];
      sb += (f32)p[2];
      cnt++;
    }
  }
  out[0] = sr / (f32)cnt;
  out[1] = sg / (f32)cnt;
  out[2] = sb / (f32)cnt;
}

/* mean |luma error| between canvas and target over the smear footprint */
static f32 region_error(const u8 *canvas, const u8 *target, i32 w, i32 h,
                        i32 cx, i32 cy, i32 r) {
  f32 err = 0;
  i32 cnt = 0;
  i32 step = pc_maxi(1, r / 2);
  for (i32 dy = -r; dy <= r; dy += step) {
    for (i32 dx = -r; dx <= r; dx += step) {
      i32 x = pc_clampi(cx + dx, 0, w - 1);
      i32 y = pc_clampi(cy + dy, 0, h - 1);
      usize o = ((usize)y * (usize)w + (usize)x) * 4;
      f32 lc = pc_luma(canvas[o], canvas[o + 1], canvas[o + 2]);
      f32 lt = pc_luma(target[o], target[o + 1], target[o + 2]);
      err += pc_fabsf(lc - lt);
      cnt++;
    }
  }
  return err / (f32)cnt;
}

/*
 * One knife smear.
 * `hbase` is the relief as it stood at the start of the current layer
 * (blurred), the reference topography the blade skips across.
 * `ridge` scales the squeezed twin edge beads, `tilt` (signed, per smear)
 * favors one bead over the other as the blade rides at an angle,
 * `dry` is the skipping threshold and `drag` the wet-on-wet pigment pickup.
 */
static void draw_smear(u8 *img, f32 *height, const f32 *hbase, i32 w, i32 h,
                       f32 cx, f32 cy, f32 dx, f32 dy, f32 len, f32 wid,
                       const f32 *c0, const f32 *c1, f32 thick, f32 ridge,
                       f32 tilt, f32 dry, f32 drag, f32 vib, f32 fringe,
                       i32 sd1, i32 sd2) {
  f32 hl = len * 0.5f, hw = wid * 0.5f;
  f32 reach = hl + hw;
  i32 x0 = pc_maxi(0, (i32)(cx - reach));
  i32 x1 = pc_mini(w - 1, (i32)(cx + reach) + 1);
  i32 y0 = pc_maxi(0, (i32)(cy - reach));
  i32 y1 = pc_mini(h - 1, (i32)(cy + reach) + 1);

  for (i32 py = y0; py <= y1; py++) {
    for (i32 px = x0; px <= x1; px++) {
      f32 rx = (f32)px - cx, ry = (f32)py - cy;
      f32 a = rx * dx + ry * dy;  /* along the smear  */
      f32 b = -rx * dy + ry * dx; /* across the smear */

      /* signed distance to the rectangle, half-pixel AA */
      f32 cov = pc_minf(hl - pc_fabsf(a), hw - pc_fabsf(b));
      if (cov <= -0.5f)
        continue;
      f32 alpha = pc_clampf(cov + 0.5f, 0.0f, 1.0f);

      f32 u = a / len + 0.5f;    /* 0 at start, 1 at lift-off */
      f32 vn = pc_fabsf(b) / hw; /* 0 center, 1 long edge     */

      /* Thickness - the squeezed profile of a flat blade:
       * metal presses the body of the smear slightly hollow and pushes
       * the displaced paint OUTWARD into two raised beads along the lateral
       * margins (peaking near vn ~ 0.85).
       * `tilt` leans the blade so one bead squeezes out heavier than the other,
       * and the whole slab still thickens toward lift-off */
      f32 bead =
          smoothstep(0.50f, 0.85f, vn) * (1.0f - smoothstep(0.88f, 1.0f, vn));
      f32 hollow = 1.0f - 0.22f * ridge * (1.0f - smoothstep(0.35f, 0.75f, vn));
      f32 side = 1.0f + tilt * (b > 0.0f ? 1.0f : -1.0f);
      f32 hp =
          thick * (0.50f + 0.50f * u) * (hollow + 0.55f * ridge * bead * side);

      /* the smear, not the tile: blade never covers evenly.
       * Coverage runs in hash-jittered lanes parallel to the drag
       * (the paint below shows through as streaks) and thins toward lift-off,
       * where the knife runs out of paint and drags the layer underneath
       * instead */
      i32 lane = (i32)((b / wid + 0.5f) * 7.0f + 16.0f);
      f32 lane_op = 0.62f + 0.38f * pc_hash2(lane * 31 + sd1, sd2);
      /* exponential paint depletion:
       * load thins fast once the blade has spent its first push,
       * then coasts on residue */
      f32 opacity = alpha * lane_op * (0.55f + 0.45f * pc_expf(-1.8f * u * u));

      usize i = (usize)py * (usize)w + (usize)px;

      /* Dry-brush skipping:
       * starved blade rides the micro-topography, depositing only on the peaks
       * of what is already on the canvas.
       * `peak` scores this pixel against the layer-start relief (+ canvas tooth
       * noise);
       * `dry` slides the deposit threshold - 0 wet knife lays paint everywhere,
       * 1 only the highest points catch pigment and the valleys stay as organic
       * voids */
      if (dry > 0.0f) {
        f32 tooth = pc_hash2(px * 3 + 11, py * 3 + 29);
        f32 peak = pc_clampf(0.5f + (height[i] - hbase[i]) * 8.0f, 0.0f, 1.0f);
        peak = pc_clampf(peak + (tooth - 0.5f) * 0.35f, 0.0f, 1.0f);
        f32 dep = smoothstep(dry - 0.30f, dry + 0.15f, peak);
        opacity *= 1.0f - dry * (1.0f - dep);
      }

      /* flat knife mix dragged toward the far-end sample */
      f32 c[3] = {c0[0] + (c1[0] - c0[0]) * u, c0[1] + (c1[1] - c0[1]) * u,
                  c0[2] + (c1[2] - c0[2]) * u};

      /* Wet-on-wet smudging + contamination:
       * blade shears the wet layer below and carries some of it forward -
       * re-sample the canvas a few pixels upstream (against the drag direction)
       * and fold that pigment into the load, mixing subtractively.
       * Contamination rises exponentially along the stroke:
       * at the tail the original load is depleted and the blade is mostly
       * pushing what it picked up */
      if (drag > 0.0f) {
        i32 ux = pc_clampi((i32)((f32)px - dx * 3.0f), 0, w - 1);
        i32 uy = pc_clampi((i32)((f32)py - dy * 3.0f), 0, h - 1);
        const u8 *up = img + ((usize)uy * (usize)w + (usize)ux) * 4;
        f32 upf[3] = {(f32)up[0], (f32)up[1], (f32)up[2]};
        f32 m = drag * 0.55f * (1.0f - pc_expf(-2.5f * u));
        pc_mix_paint(c, upf, m, vib, c);
      }

      /* dark pigment halo at the lateral margin, then mix as paint */
      apply_fringe(c, vn, fringe);

      usize o = i * 4;
      f32 cur[3] = {(f32)img[o], (f32)img[o + 1], (f32)img[o + 2]};
      pc_mix_paint(cur, c, opacity, vib, cur);
      img[o] = pc_clamp255(cur[0]);
      img[o + 1] = pc_clamp255(cur[1]);
      img[o + 2] = pc_clamp255(cur[2]);
      img[o + 3] = 255;
      /* relief follows coverage:
       * where the streak is thin the old layer's height survives -
       * layered paint, visibly */
      height[i] += (hp - height[i]) * opacity;
    }
  }
}

void pc_knife(u8 *img, f32 *height, i32 w, i32 h, i32 size, i32 layers,
              f32 detail, f32 azim, f32 tint, f32 ridge, f32 dry, f32 drag,
              f32 vib, f32 fringe) {
  if (size < 4)
    size = 4;
  if (layers < 1)
    layers = 1;
  if (layers > 4)
    layers = 4;
  ridge = pc_clampf(ridge, 0.0f, 1.0f);
  dry = pc_clampf(dry, 0.0f, 1.0f);
  drag = pc_clampf(drag, 0.0f, 1.0f);
  vib = pc_clampf(vib, 0.0f, 1.0f);
  fringe = pc_clampf(fringe, 0.0f, 1.0f);

  usize n = (usize)w * (usize)h;
  u8 *target = (u8 *)pc_alloc(n * 4);
  f32 *fx = (f32 *)pc_alloc(n * 4);
  f32 *fy = (f32 *)pc_alloc(n * 4);
  f32 *aniso = (f32 *)pc_alloc(n * 4);
  f32 *hbase = (f32 *)pc_alloc(n * 4);
  if (!target || !fx || !fy || !aniso || !hbase)
    return;

  memcpy(target, img, n * 4);
  pc_structure_tensor(target, w, h, fx, fy, aniso);
  memset(height, 0, n * 4);

  /* flat-region knife direction follows the light */
  f32 ldx = pc_sinf(azim + PC_HALFPI); /* cos(azim) */
  f32 ldy = -pc_sinf(azim);

  /* finer layers touch the canvas only where it still disagrees with the
   * target:
   * detail 1 -> threshold 6 luma levels (dense detail), detail 0 -> 50 (broad
   * slabs only).
   * The floor sits above the per-smear tint jitter so jitter alone can never
   * summon the next layer */
  f32 err_thresh = 6.0f + (1.0f - pc_clampf(detail, 0.0f, 1.0f)) * 44.0f;

  for (i32 L = 0; L < layers; L++) {
    i32 sz = size >> L;
    if (sz < 3)
      break;
    /* topography the blade skips across is the relief as it stood when this
     * layer began, softened so single lane streak does not read as a mountain
     */
    pc_box_blur(height, hbase, w, h, 2);
    f32 spacing = (f32)sz * 0.62f;
    i32 gw = (i32)((f32)w / spacing) + 2;
    i32 gh = (i32)((f32)h / spacing) + 2;

    for (i32 gy = 0; gy < gh; gy++) {
      for (i32 gx = 0; gx < gw; gx++) {
        i32 seed = (L + 1) * 7919;
        f32 j1 = pc_hash2(gx * 3 + seed, gy * 5 + 1);
        f32 j2 = pc_hash2(gx * 7 + 2, gy * 3 + seed);
        f32 cx = ((f32)gx + j1 - 0.5f) * spacing;
        f32 cy = ((f32)gy + j2 - 0.5f) * spacing;
        i32 icx = pc_clampi((i32)cx, 0, w - 1);
        i32 icy = pc_clampi((i32)cy, 0, h - 1);

        if (L > 0 && region_error(img, target, w, h, icx, icy, sz) < err_thresh)
          continue;

        /* sign-aligned average of the flow over the smear footprint:
         * one coherent direction per smear instead of whatever single pixel
         * sat under its center */
        f32 afx = 0, afy = 0, aan = 0;
        {
          f32 rfx = fx[(usize)icy * (usize)w + (usize)icx];
          f32 rfy = fy[(usize)icy * (usize)w + (usize)icx];
          for (i32 sy = -1; sy <= 1; sy++) {
            for (i32 sx = -1; sx <= 1; sx++) {
              i32 qx = pc_clampi(icx + sx * (sz / 2), 0, w - 1);
              i32 qy = pc_clampi(icy + sy * (sz / 2), 0, h - 1);
              usize q = (usize)qy * (usize)w + (usize)qx;
              f32 vx = fx[q], vy = fy[q];
              if (vx * rfx + vy * rfy < 0.0f) {
                vx = -vx;
                vy = -vy;
              }
              afx += vx;
              afy += vy;
              aan += aniso[q];
            }
          }
          aan *= 1.0f / 9.0f;
        }

        f32 dx, dy;
        f32 flen = pc_sqrtf(afx * afx + afy * afy);
        if (aan < 0.45f || flen < 1e-3f) {
          /* flat paint follows the light, hand-wobbled */
          f32 wob = (pc_hash2(gx + seed, gy) - 0.5f) * 0.3f;
          f32 cw = pc_sinf(wob + PC_HALFPI), sw = pc_sinf(wob);
          dx = ldx * cw - ldy * sw;
          dy = ldx * sw + ldy * cw;
        } else {
          dx = afx / flen;
          dy = afy / flen;
        }

        f32 len = (f32)sz * (1.6f + 0.7f * pc_hash2(gx + 11, gy + seed));
        f32 wid = (f32)sz * (0.70f + 0.30f * pc_hash2(gx + 23, gy + 7));

        f32 c0[3], c1[3];
        sample_color(target, w, h, icx, icy, pc_maxi(1, sz / 3), c0);
        sample_color(target, w, h,
                     pc_clampi((i32)(cx + dx * len * 0.5f), 0, w - 1),
                     pc_clampi((i32)(cy + dy * len * 0.5f), 0, h - 1),
                     pc_maxi(1, sz / 3), c1);

        /* no two loads of paint are mixed identically */
        f32 tj = 1.0f + tint * (pc_hash2(gx + seed * 2, gy + 13) - 0.5f) * 2.0f;
        for (i32 k = 0; k < 3; k++) {
          c0[k] = pc_clampf(c0[k] * tj, 0.0f, 255.0f);
          c1[k] = pc_clampf(c1[k] * tj, 0.0f, 255.0f);
        }

        f32 thick = 0.35f + 0.30f * pc_hash2(gx * 13 + seed, gy * 17);

        /* per-smear blade angle: one edge bead squeezes out heavier */
        f32 tilt =
            ridge * (pc_hash2(gx * 29 + seed, gy * 31 + 5) - 0.5f) * 0.8f;

        draw_smear(img, height, hbase, w, h, cx, cy, dx, dy, len, wid, c0, c1,
                   thick, ridge, tilt, dry, drag, vib, fringe, gx * 131 + seed,
                   gy * 197 + 3);
      }
    }
  }

  /* soften the relief only (colors keep their hard knife edges):
   * two radius-1 passes round the step between overlapping smears
   * into slope the light can actually rake across */
  f32 *tmp = (f32 *)pc_alloc(n * 4);
  if (tmp) {
    pc_box_blur(height, tmp, w, h, 1);
    pc_box_blur(tmp, height, w, h, 1);
  }
}
