/*
 * Smoothed structure tensor and its flow field.
 *
 * Structure tensor at pixel is the outer product of the Sobel
 * gradient, smoothed over a neighborhood:
 *
 *   T = G_sigma * [ gx*gx  gx*gy ]
 *                 [ gx*gy  gy*gy ]
 *
 * Its minor eigenvector points *along* local image contours -
 * the direction painter's brush would travel.
 *
 * Eigenvalue spread gives anisotropy A = (l1 - l2) / (l1 + l2):
 * 0 in flat/isotropic regions, 1 on strong one-directional structure.
 *
 * Eigenvectors of a symmetric 2x2 are computed directly from
 * (b, lambda - a) / (lambda - c, b).
 */
#include "pc.h"

void pc_box3_blur(const f32 *src, f32 *dst, i32 w, i32 h) {
  for (i32 y = 0; y < h; y++) {
    for (i32 x = 0; x < w; x++) {
      f32 acc = 0.0f;
      for (i32 dy = -1; dy <= 1; dy++) {
        i32 yy = pc_clampi(y + dy, 0, h - 1);
        for (i32 dx = -1; dx <= 1; dx++) {
          i32 xx = pc_clampi(x + dx, 0, w - 1);
          acc += src[(usize)yy * (usize)w + (usize)xx];
        }
      }
      dst[(usize)y * (usize)w + (usize)x] = acc * (1.0f / 9.0f);
    }
  }
}

/*
 * Fills fx/fy with the unit flow direction (minor eigenvector)
 * and aniso with the anisotropy in [0, 1].
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
  f32 *tmp = (f32 *)pc_alloc(n * 4);
  if (!luma || !txx || !txy || !tyy || !tmp)
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

  /* Three box blurs approximate Gaussian (sigma ~ 1.7);
   * this is what makes neighboring flow vectors cohere into strokes */
  for (i32 p = 0; p < 3; p++) {
    pc_box3_blur(txx, tmp, w, h);
    memcpy(txx, tmp, n * 4);
    pc_box3_blur(txy, tmp, w, h);
    memcpy(txy, tmp, n * 4);
    pc_box3_blur(tyy, tmp, w, h);
    memcpy(tyy, tmp, n * 4);
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
