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

/* Nearest-neighbor sampling is enough here:
 * input is already pigment-quantized, and the Gaussian accumulation over
 * ~2*length taps averages away any half-pixel placement error. */
void pc_flow_strokes(u8 *img, i32 w, i32 h, i32 length, const f32 *fx,
                     const f32 *fy) {
  if (length < 1 || !fx || !fy)
    return;
  if (length > 32)
    length = 32;

  usize n = (usize)w * (usize)h;
  u8 *src = (u8 *)pc_alloc(n * 4);
  if (!src)
    return;

  memcpy(src, img, n * 4);

  /* Gaussian falloff depends only on the step index:
   * table it once instead of calling expf ~2*length times per pixel */
  f32 wtab[32];
  f32 sigma = (f32)length * 0.5f;
  f32 inv2s2 = 1.0f / (2.0f * sigma * sigma);
  for (i32 s = 1; s <= length; s++)
    wtab[s - 1] = pc_expf(-(f32)(s * s) * inv2s2);

  for (i32 y = 0; y < h; y++) {
    for (i32 x = 0; x < w; x++) {
      usize i = (usize)y * (usize)w + (usize)x;

      const u8 *c0 = src + i * 4;
      f32 accR = (f32)c0[0], accG = (f32)c0[1], accB = (f32)c0[2];
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

          i32 qx = (i32)(px + 0.5f);
          i32 qy = (i32)(py + 0.5f);
          usize q = (usize)qy * (usize)w + (usize)qx;
          f32 nx = fx[q], ny = fy[q];
          if (nx * dx + ny * dy < 0.0f) {
            nx = -nx;
            ny = -ny;
          }
          dx = nx;
          dy = ny;

          f32 wgt = wtab[s - 1];
          const u8 *c = src + q * 4;
          accR += wgt * (f32)c[0];
          accG += wgt * (f32)c[1];
          accB += wgt * (f32)c[2];
          wsum += wgt;
        }
      }

      f32 inv = 1.0f / wsum;
      img[i * 4] = pc_clamp255(accR * inv);
      img[i * 4 + 1] = pc_clamp255(accG * inv);
      img[i * 4 + 2] = pc_clamp255(accB * inv);
    }
  }
}
