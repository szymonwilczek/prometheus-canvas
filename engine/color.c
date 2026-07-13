/*
 * Saturation/contrast and pigment mixing noise.
 *
 * Saturation scales chroma around the Rec.601 luma axis:
 *   out = luma + (in - luma) * s
 * Contrast is a linear ramp around mid-gray:
 *   out = (in - 128) * c + 128
 *
 * Pigment noise models the fact that a painter never mixes a batch of
 * paint perfectly:
 * two-octave value noise modulates luminance few percent, at the spatial scale
 * of a brush load of paint.
 * It is what keeps large single-pigment areas from reading as flat plastic.
 */
#include "pc.h"

/* bilinear value noise from the integer hash, smoothstep-interpolated */
static f32 vnoise(f32 x, f32 y) {
  f32 xf = pc_floorf(x), yf = pc_floorf(y);
  i32 xi = (i32)xf, yi = (i32)yf;
  f32 tx = x - xf, ty = y - yf;
  tx = tx * tx * (3.0f - 2.0f * tx);
  ty = ty * ty * (3.0f - 2.0f * ty);

  f32 a = pc_hash2(xi, yi), b = pc_hash2(xi + 1, yi);
  f32 c = pc_hash2(xi, yi + 1), d = pc_hash2(xi + 1, yi + 1);
  f32 top = a + (b - a) * tx;
  f32 bot = c + (d - c) * tx;
  return top + (bot - top) * ty;
}

void pc_pigment_noise(u8 *img, i32 w, i32 h, f32 amount, f32 scale) {
  if (amount <= 0.0f || scale < 1.0f)
    return;

  f32 inv1 = 1.0f / scale;
  f32 inv2 = 1.0f / (scale * 0.37f);

  for (i32 y = 0; y < h; y++) {
    for (i32 x = 0; x < w; x++) {
      f32 n = 0.65f * vnoise((f32)x * inv1, (f32)y * inv1) +
              0.35f * vnoise((f32)x * inv2 + 37.2f, (f32)y * inv2 + 11.7f);
      f32 m = 1.0f + amount * 0.30f * (n - 0.5f) * 2.0f;

      usize o = ((usize)y * (usize)w + (usize)x) * 4;
      img[o] = pc_clamp255((f32)img[o] * m);
      img[o + 1] = pc_clamp255((f32)img[o + 1] * m);
      img[o + 2] = pc_clamp255((f32)img[o + 2] * m);
    }
  }
}

void pc_color_adjust(u8 *img, i32 w, i32 h, f32 saturation, f32 contrast) {
  if (saturation == 1.0f && contrast == 1.0f)
    return;

  usize n = (usize)w * (usize)h;
  for (usize i = 0; i < n; i++) {
    f32 r = (f32)img[i * 4], g = (f32)img[i * 4 + 1], b = (f32)img[i * 4 + 2];
    f32 l = 0.299f * r + 0.587f * g + 0.114f * b;

    r = l + (r - l) * saturation;
    g = l + (g - l) * saturation;
    b = l + (b - l) * saturation;

    r = (r - 128.0f) * contrast + 128.0f;
    g = (g - 128.0f) * contrast + 128.0f;
    b = (b - 128.0f) * contrast + 128.0f;

    img[i * 4] = pc_clamp255(r);
    img[i * 4 + 1] = pc_clamp255(g);
    img[i * 4 + 2] = pc_clamp255(b);
  }
}
