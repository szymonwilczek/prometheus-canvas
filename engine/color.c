/*
 * Saturation and contrast, plain per-pixel linear algebra.
 *
 * Saturation scales chroma around the Rec.601 luma axis:
 *   out = luma + (in - luma) * s
 * Contrast is a linear ramp around mid-gray:
 *   out = (in - 128) * c + 128
 */
#include "pc.h"

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
