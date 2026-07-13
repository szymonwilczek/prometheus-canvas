/*
 * O(1)-per-pixel box blur and the smoothed structure tensor
 *
 * Structure tensor at a pixel is the outer product of the Sobel gradient,
 * smoothed over a neighborhood:
 *
 *   T = G_sigma * [ gx*gx  gx*gy ]
 *                 [ gx*gy  gy*gy ]
 *
 * Its minor eigenvector points *along* local image contours -
 * the direction a painter's brush would travel.
 *
 * Eigenvalue spread gives anisotropy A = (l1 - l2) / (l1 + l2):
 * 0 in flat/isotropic regions, 1 on strong one-directional structure.
 *
 * Eigenvectors of a symmetric 2x2 are computed directly from
 * (b, lambda - a) / (lambda - c, b).
 */
#include "pc.h"

/*
 * Centered box blur of radius r, sliding-window in both axes:
 * O(1) amortized per pixel regardless of radius.
 * Borders divide by the true clipped sample count.
 * Vertical pass keeps running row-sum accumulator so memory
 * access stays row-major
 */
void pc_box_blur(const f32 *src, f32 *dst, i32 w, i32 h, i32 r) {
  f32 *tmp = (f32 *)pc_alloc((usize)w * (usize)h * 4);
  f32 *acc = (f32 *)pc_alloc((usize)w * 4);
  if (!tmp || !acc)
    return;

  /* horizontal src -> tmp */
  for (i32 y = 0; y < h; y++) {
    const f32 *row = src + (usize)y * (usize)w;
    f32 *out = tmp + (usize)y * (usize)w;
    f32 run = 0.0f;
    i32 hi = pc_mini(r, w - 1);
    for (i32 x = 0; x <= hi; x++)
      run += row[x];
    for (i32 x = 0; x < w; x++) {
      i32 lo = pc_maxi(x - r, 0);
      i32 up = pc_mini(x + r, w - 1);
      out[x] = run / (f32)(up - lo + 1);
      if (up + 1 < w)
        run += row[up + 1];
      if (x - r >= 0)
        run -= row[x - r];
    }
  }

  /* vertical tmp -> dst via running row sums */
  memset(acc, 0, (usize)w * 4);
  i32 vhi = pc_mini(r, h - 1);
  for (i32 y = 0; y <= vhi; y++) {
    const f32 *row = tmp + (usize)y * (usize)w;
    for (i32 x = 0; x < w; x++)
      acc[x] += row[x];
  }
  for (i32 y = 0; y < h; y++) {
    i32 lo = pc_maxi(y - r, 0);
    i32 up = pc_mini(y + r, h - 1);
    f32 inv = 1.0f / (f32)(up - lo + 1);
    f32 *out = dst + (usize)y * (usize)w;
    for (i32 x = 0; x < w; x++)
      out[x] = acc[x] * inv;
    if (up + 1 < h) {
      const f32 *add = tmp + (usize)(up + 1) * (usize)w;
      for (i32 x = 0; x < w; x++)
        acc[x] += add[x];
    }
    if (y - r >= 0) {
      const f32 *sub = tmp + (usize)(y - r) * (usize)w;
      for (i32 x = 0; x < w; x++)
        acc[x] -= sub[x];
    }
  }
}

/*
 * fills fx/fy with the unit flow direction (minor eigenvector) and aniso
 * with the anisotropy in [0, 1].
 * All three planes are w*h f32, allocated by the caller.
 * Scratch comes from the bump heap.
 */
void pc_structure_tensor(const u8 *img, i32 w, i32 h, f32 *fx, f32 *fy,
                         f32 *aniso) {
  usize n = (usize)w * (usize)h;

  f32 *luma = (f32 *)pc_alloc(n * 4);
  f32 *txx = (f32 *)pc_alloc(n * 4);
  f32 *txy = (f32 *)pc_alloc(n * 4);
  f32 *tyy = (f32 *)pc_alloc(n * 4);
  if (!luma || !txx || !txy || !tyy)
    return;

  for (usize i = 0; i < n; i++)
    luma[i] = pc_luma(img[i * 4], img[i * 4 + 1], img[i * 4 + 2]);

  for (i32 y = 0; y < h; y++) {
    i32 ym = pc_maxi(y - 1, 0), yp = pc_mini(y + 1, h - 1);
    for (i32 x = 0; x < w; x++) {
      i32 xm = pc_maxi(x - 1, 0), xp = pc_mini(x + 1, w - 1);
#define L(X, Y) luma[(usize)(Y) * (usize)w + (usize)(X)]
      f32 gx = (L(xp, ym) + 2.0f * L(xp, y) + L(xp, yp)) -
               (L(xm, ym) + 2.0f * L(xm, y) + L(xm, yp));
      f32 gy = (L(xm, yp) + 2.0f * L(x, yp) + L(xp, yp)) -
               (L(xm, ym) + 2.0f * L(x, ym) + L(xp, ym));
#undef L
      usize i = (usize)y * (usize)w + (usize)x;
      txx[i] = gx * gx;
      txy[i] = gx * gy;
      tyy[i] = gy * gy;
    }
  }

  /* Three radius-1 box passes ~ Gaussian sigma 1.7:
   * this is what makes neighboring flow vectors cohere into strokes.
   * Single wide flat box is NOT equivalent - its hard cutoff makes
   * the orientation field lock into blocky axis-aligned bundles */
  for (i32 p = 0; p < 3; p++) {
    pc_box_blur(txx, luma, w, h, 1);
    memcpy(txx, luma, n * 4);
    pc_box_blur(txy, luma, w, h, 1);
    memcpy(txy, luma, n * 4);
    pc_box_blur(tyy, luma, w, h, 1);
    memcpy(tyy, luma, n * 4);
  }

  for (usize i = 0; i < n; i++) {
    f32 a = txx[i], b = txy[i], c = tyy[i];
    f32 half_tr = 0.5f * (a + c);
    f32 half_df = 0.5f * (a - c);
    f32 root = pc_sqrtf(half_df * half_df + b * b);
    f32 l1 = half_tr + root, l2 = half_tr - root;

    aniso[i] = (l1 + l2) > 1e-6f ? (l1 - l2) / (l1 + l2) : 0.0f;

    /* Minor eigenvector:
     * try (b, l2 - a) and (l2 - c, b), keep the better-conditioned one */
    f32 vx1 = b, vy1 = l2 - a;
    f32 vx2 = l2 - c, vy2 = b;
    f32 n1 = vx1 * vx1 + vy1 * vy1;
    f32 n2 = vx2 * vx2 + vy2 * vy2;
    f32 vx, vy, nn;
    if (n1 >= n2) {
      vx = vx1;
      vy = vy1;
      nn = n1;
    } else {
      vx = vx2;
      vy = vy2;
      nn = n2;
    }

    if (nn < 1e-12f) {
      fx[i] = 1.0f;
      fy[i] = 0.0f;
      aniso[i] = 0.0f;
    } else {
      f32 inv = 1.0f / pc_sqrtf(nn);
      fx[i] = vx * inv;
      fy[i] = vy * inv;
    }
  }
}
