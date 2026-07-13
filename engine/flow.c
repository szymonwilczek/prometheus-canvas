/*
 * Flow-guided stroke elongation (line integral convolution).
 *
 * Kuwahara stage produces isotropic paint splotches.
 * Real painter drags the brush *along* contours.
 *
 * This pass advects each pixel's color along the structure-tensor flow field:
 * starting at the pixel it marches `length` unit steps in both directions,
 * re-reading the local flow at every step (so strokes bend around curves),
 * and accumulates bilinear color samples under Gaussian falloff.
 *
 * Splotches stretch into brush strokes that follow the geometry of the image
 * - the visual signature of painted canvas.
 */
#include "pc.h"

static void bilinear(const u8 *img, i32 w, i32 h, f32 x, f32 y, f32 *out) {
  x = pc_clampf(x, 0.0f, (f32)(w - 1));
  y = pc_clampf(y, 0.0f, (f32)(h - 1));
  i32 x0 = (i32)x, y0 = (i32)y;
  i32 x1 = pc_mini(x0 + 1, w - 1), y1 = pc_mini(y0 + 1, h - 1);
  f32 tx = x - (f32)x0, ty = y - (f32)y0;

  const u8 *p00 = img + ((usize)y0 * (usize)w + (usize)x0) * 4;
  const u8 *p10 = img + ((usize)y0 * (usize)w + (usize)x1) * 4;
  const u8 *p01 = img + ((usize)y1 * (usize)w + (usize)x0) * 4;
  const u8 *p11 = img + ((usize)y1 * (usize)w + (usize)x1) * 4;

  for (i32 c = 0; c < 3; c++) {
    f32 top = (f32)p00[c] + ((f32)p10[c] - (f32)p00[c]) * tx;
    f32 bot = (f32)p01[c] + ((f32)p11[c] - (f32)p01[c]) * tx;
    out[c] = top + (bot - top) * ty;
  }
}

void pc_flow_strokes(u8 *img, i32 w, i32 h, i32 length) {
  if (length < 1)
    return;

  usize n = (usize)w * (usize)h;
  f32 *fx = (f32 *)pc_alloc(n * 4);
  f32 *fy = (f32 *)pc_alloc(n * 4);
  f32 *aniso = (f32 *)pc_alloc(n * 4);
  u8 *src = (u8 *)pc_alloc(n * 4);
  if (!fx || !fy || !aniso || !src)
    return;

  memcpy(src, img, n * 4);
  pc_structure_tensor(src, w, h, fx, fy, aniso);

  f32 sigma = (f32)length * 0.5f;
  f32 inv2s2 = 1.0f / (2.0f * sigma * sigma);

  for (i32 y = 0; y < h; y++) {
    for (i32 x = 0; x < w; x++) {
      usize i = (usize)y * (usize)w + (usize)x;

      f32 acc[3];
      f32 c0[3];
      bilinear(src, w, h, (f32)x, (f32)y, c0);
      acc[0] = c0[0];
      acc[1] = c0[1];
      acc[2] = c0[2];
      f32 wsum = 1.0f;

      /* march both directions along the flow, re-reading the
       * field each step and keeping direction continuity */
      for (i32 dir = -1; dir <= 1; dir += 2) {
        f32 px = (f32)x, py = (f32)y;
        f32 dx = fx[i] * (f32)dir, dy = fy[i] * (f32)dir;

        for (i32 s = 1; s <= length; s++) {
          px += dx;
          py += dy;
          if (px < 0.0f || py < 0.0f || px > (f32)(w - 1) || py > (f32)(h - 1))
            break;

          i32 qx = pc_clampi((i32)(px + 0.5f), 0, w - 1);
          i32 qy = pc_clampi((i32)(py + 0.5f), 0, h - 1);
          usize q = (usize)qy * (usize)w + (usize)qx;
          f32 nx = fx[q], ny = fy[q];
          if (nx * dx + ny * dy < 0.0f) {
            nx = -nx;
            ny = -ny;
          }
          dx = nx;
          dy = ny;

          f32 wgt = pc_expf(-(f32)(s * s) * inv2s2);
          f32 c[3];
          bilinear(src, w, h, px, py, c);
          acc[0] += wgt * c[0];
          acc[1] += wgt * c[1];
          acc[2] += wgt * c[2];
          wsum += wgt;
        }
      }

      f32 inv = 1.0f / wsum;
      img[i * 4] = pc_clamp255(acc[0] * inv);
      img[i * 4 + 1] = pc_clamp255(acc[1] * inv);
      img[i * 4 + 2] = pc_clamp255(acc[2] * inv);
    }
  }
}
