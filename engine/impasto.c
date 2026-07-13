/*
 * 3D paint relief: Sobel heightmap + Blinn-Phong shading.
 *
 * Oil paint is physically three-dimensional: it piles up along stroke
 * boundaries and raking light catches the ridges.
 *
 * Reconstruction:
 * 1. Heightmap = Sobel gradient magnitude of the painted image's luma.
 *    After the Kuwahara stage the image is piecewise-flat, so gradient
 *    magnitude is concentrated exactly at stroke boundaries - where
 *    real painter's brush would leave ridges.
 * 2. Two 3x3 box blurs give the ridges soft flanks
 * (approximating a small Gaussian) so light has slopes to play on.
 * 3. Surface normal from central differences of the heightmap, scaled
 *    by the depth slider: N = normalize(-dh/dx * d, -dh/dy * d, 1)
 * 4. Blinn-Phong per pixel with light direction from elevation/azimuth:
 *    I = ambient + diffuse * max(N.L, 0), + specular term max(N.H, 0)^shininess
 *    (H = half-vector between L and the viewer).
 *    Diffuse is normalized against flat surface so untextured areas keep their
 *    original brightness at any light angle.
 */
#include "pc.h"

static void box3_blur(const f32 *src, f32 *dst, i32 w, i32 h) {
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

static f32 pow_int(f32 x, i32 e) {
  f32 r = 1.0f;
  while (e > 0) {
    if (e & 1)
      r *= x;
    x *= x;
    e >>= 1;
  }
  return r;
}

void pc_impasto(u8 *img, i32 w, i32 h, f32 depth, f32 elev, f32 azim,
                f32 specular, i32 shininess) {
  if (depth <= 0.0f)
    return;

  usize n = (usize)w * (usize)h;
  f32 *luma = (f32 *)pc_alloc(n * 4);
  f32 *height = (f32 *)pc_alloc(n * 4);
  f32 *tmp = (f32 *)pc_alloc(n * 4);
  if (!luma || !height || !tmp)
    return;

  for (usize i = 0; i < n; i++)
    luma[i] = pc_luma(img[i * 4], img[i * 4 + 1], img[i * 4 + 2]);

  /* Sobel gradient magnitude, normalized to [0, 1]
   * Max |Gx| is 4 * 255 = 1020, so the magnitude is at most 1020 * sqrt(2) */
  const f32 NORM = 1.0f / 1442.5f;
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
      height[(usize)y * (usize)w + (usize)x] =
          pc_sqrtf(gx * gx + gy * gy) * NORM;
    }
  }

  /* soft ridge flanks: two box blurs ~ Gaussian */
  box3_blur(height, tmp, w, h);
  box3_blur(tmp, height, w, h);

  /* light direction from elevation/azimuth
   * (screen y points down),
   * viewer along +z,
   * H = normalize(L + V) */
  f32 ce = pc_sinf(elev + PC_HALFPI);      /* cos(elev) */
  f32 lx = ce * pc_sinf(azim + PC_HALFPI); /* cos(azim) * cos(elev) */
  f32 ly = -ce * pc_sinf(azim);
  f32 lz = pc_sinf(elev);

  f32 hx = lx, hy = ly, hz = lz + 1.0f;
  f32 hlen = pc_sqrtf(hx * hx + hy * hy + hz * hz);
  hx /= hlen;
  hy /= hlen;
  hz /= hlen;

  const f32 AMBIENT = 0.30f, DIFFUSE = 0.70f;
  f32 flat = AMBIENT + DIFFUSE * pc_maxf(lz, 0.0f);
  f32 inv_flat = flat > 1e-6f ? 1.0f / flat : 1.0f;
  f32 flat_spec = specular * pow_int(pc_maxf(hz, 0.0f), shininess);

  for (i32 y = 0; y < h; y++) {
    i32 ym = pc_maxi(y - 1, 0), yp = pc_mini(y + 1, h - 1);
    for (i32 x = 0; x < w; x++) {
      i32 xm = pc_maxi(x - 1, 0), xp = pc_mini(x + 1, w - 1);
      usize row = (usize)y * (usize)w;

      f32 dhx = (height[row + (usize)xp] - height[row + (usize)xm]) * 0.5f;
      f32 dhy = (height[(usize)yp * (usize)w + (usize)x] -
                 height[(usize)ym * (usize)w + (usize)x]) *
                0.5f;

      f32 nx = -dhx * depth, ny = -dhy * depth, nz = 1.0f;
      f32 inv_len = 1.0f / pc_sqrtf(nx * nx + ny * ny + 1.0f);
      nx *= inv_len;
      ny *= inv_len;
      nz *= inv_len;

      f32 ndl = pc_maxf(nx * lx + ny * ly + nz * lz, 0.0f);
      f32 ndh = pc_maxf(nx * hx + ny * hy + nz * hz, 0.0f);

      /* normalized so a flat pixel (N = +z) multiplies by 1 */
      f32 mult = (AMBIENT + DIFFUSE * ndl) * inv_flat;
      /* specular relative to flat: ridges glint, flats unchanged */
      f32 spec = 255.0f * (specular * pow_int(ndh, shininess) - flat_spec);

      usize o = (row + (usize)x) * 4;
      img[o] = pc_clamp255((f32)img[o] * mult + spec);
      img[o + 1] = pc_clamp255((f32)img[o + 1] * mult + spec);
      img[o + 2] = pc_clamp255((f32)img[o + 2] * mult + spec);
    }
  }
}
