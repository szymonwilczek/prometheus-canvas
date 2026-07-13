/*
 * Generalized Kuwahara filter, the painterly foundation.
 *
 * Classic form (Kuwahara 1976):
 * For each pixel examine four overlapping square sectors (NW/NE/SW/SE)
 * of side (radius+1) and output the mean color of the sector with the
 * lowest luminance variance.
 * Homogeneous regions collapse into flat "paint splotches" while
 * structural edges survive, because a sector that straddles an edge has
 * high variance and loses.
 *
 * Generalization:
 * Instead of a hard argmin, every sector contributes with weight
 * (1 + sigma^2)^(-q).
 * q is the edge-preservation exponent:
 * q -> 0 blends all sectors (soft strokes), large q approaches the
 * classic minimum-variance pick (hard stroke boundaries).
 * Blend is computed in log-space so large q cannot underflow all four weights.
 *
 * Optimization:
 * Sector statistics come from an *anchored* sliding-window box sum.
 * B(x,y) = sum over the window [x-r..x, y-r..y] is computed separably in O(1)
 * amortized per pixel, so the whole filter is O(W*H) regardless of brush
 * radius.
 *
 * Four sectors of pixel p are then just B sampled at four offsets:
 *   NW = B(x,     y)        NE = B(x+r,   y)
 *   SW = B(x,     y+r)      SE = B(x+r,   y+r)
 *
 * Near borders the window is clipped and divided by the true sample count
 * (countX * countY factorizes per axis).
 */
#include "pc.h"

/* Anchored sliding-window box sum of one f32 plane, window [i-r..i]
 * Horizontal pass src->tmp, vertical pass tmp->dst
 * (in place is fine because the vertical pass runs column-independent
 * on its own copy) */
static void box_sum_anchored(const f32 *src, f32 *tmp, f32 *dst, i32 w, i32 h,
                             i32 r) {
  for (i32 y = 0; y < h; y++) {
    const f32 *row = src + (usize)y * (usize)w;
    f32 *out = tmp + (usize)y * (usize)w;
    f32 run = 0.0f;
    for (i32 x = 0; x < w; x++) {
      run += row[x];
      if (x - r - 1 >= 0)
        run -= row[x - r - 1];
      out[x] = run;
    }
  }
  for (i32 x = 0; x < w; x++) {
    f32 run = 0.0f;
    for (i32 y = 0; y < h; y++) {
      run += tmp[(usize)y * (usize)w + (usize)x];
      if (y - r - 1 >= 0)
        run -= tmp[(usize)(y - r - 1) * (usize)w + (usize)x];
      dst[(usize)y * (usize)w + (usize)x] = run;
    }
  }
}

void pc_kuwahara(const u8 *src, u8 *dst, i32 w, i32 h, i32 radius, f32 q) {
  if (radius < 1) {
    memcpy(dst, src, (usize)w * (usize)h * 4u);
    return;
  }

  usize n = (usize)w * (usize)h;
  i32 r = radius;

  /* five statistic planes:
   * mean R/G/B,
   * mean luma,
   * mean luma^2 */
  f32 *pr = (f32 *)pc_alloc(n * 4);
  f32 *pg = (f32 *)pc_alloc(n * 4);
  f32 *pb = (f32 *)pc_alloc(n * 4);
  f32 *pl = (f32 *)pc_alloc(n * 4);
  f32 *pl2 = (f32 *)pc_alloc(n * 4);
  f32 *tmp = (f32 *)pc_alloc(n * 4);
  if (!pr || !pg || !pb || !pl || !pl2 || !tmp)
    return;

  for (usize i = 0; i < n; i++) {
    u8 R = src[i * 4], G = src[i * 4 + 1], B = src[i * 4 + 2];
    f32 l = pc_luma(R, G, B);
    pr[i] = (f32)R;
    pg[i] = (f32)G;
    pb[i] = (f32)B;
    pl[i] = l;
    pl2[i] = l * l;
  }

  /* in-place anchored box sums
   * (src and dst may alias per plane because box_sum_anchored reads src
   * only in the horizontal pass) */
  box_sum_anchored(pr, tmp, pr, w, h, r);
  box_sum_anchored(pg, tmp, pg, w, h, r);
  box_sum_anchored(pb, tmp, pb, w, h, r);
  box_sum_anchored(pl, tmp, pl, w, h, r);
  box_sum_anchored(pl2, tmp, pl2, w, h, r);

  for (i32 y = 0; y < h; y++) {
    for (i32 x = 0; x < w; x++) {
      /* anchor points of the four sectors, clipped to the image
       * window at anchor (ax, ay) covers [ax-r..ax, ay-r..ay] */
      i32 ax0 = x, ax1 = pc_mini(x + r, w - 1);
      i32 ay0 = y, ay1 = pc_mini(y + r, h - 1);

      i32 axs[4] = {ax0, ax1, ax0, ax1};
      i32 ays[4] = {ay0, ay0, ay1, ay1};

      f32 mr[4], mg[4], mb[4], lw[4];
      f32 lmax = -1e30f;

      for (i32 s = 0; s < 4; s++) {
        i32 ax = axs[s], ay = ays[s];
        usize idx = (usize)ay * (usize)w + (usize)ax;

        f32 cnt = (f32)((pc_mini(ax, r) + 1) * (pc_mini(ay, r) + 1));
        f32 inv = 1.0f / cnt;

        f32 ml = pl[idx] * inv;
        f32 var = pc_maxf(0.0f, pl2[idx] * inv - ml * ml);

        mr[s] = pr[idx] * inv;
        mg[s] = pg[idx] * inv;
        mb[s] = pb[idx] * inv;

        /* log of (1 + var)^(-q), kept in log-space for range */
        lw[s] = -q * pc_logf(1.0f + var);
        if (lw[s] > lmax)
          lmax = lw[s];
      }

      f32 wr = 0, wg = 0, wb = 0, wsum = 0;
      for (i32 s = 0; s < 4; s++) {
        f32 wgt = pc_expf(lw[s] - lmax);
        wr += wgt * mr[s];
        wg += wgt * mg[s];
        wb += wgt * mb[s];
        wsum += wgt;
      }
      f32 inv = 1.0f / wsum;

      usize o = ((usize)y * (usize)w + (usize)x) * 4;
      dst[o] = pc_clamp255(wr * inv);
      dst[o + 1] = pc_clamp255(wg * inv);
      dst[o + 2] = pc_clamp255(wb * inv);
      dst[o + 3] = src[o + 3];
    }
  }
}
