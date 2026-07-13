/*
 * Saturation/contrast, pigment mixing noise, and subtractive paint mixing.
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
 *
 * PAINT MIXING is not light mixing. Averaging RGB is what screens do;
 * pigments *absorb* - two wet paints meeting on canvas mix subtractively,
 * and linear interpolation drags every transition through digital gray.
 * pc_mix_paint interpolates per channel in Kubelka-Munk absorption/scattering
 * space instead:
 *
 *   K/S = (1 - R)^2 / (2R)      reflectance -> absorption ratio
 *   K/S mixes linearly by pigment concentration
 *   R   = 1 + K/S - sqrt((K/S)^2 + 2 K/S)   back to reflectance
 *
 * K/S curve is hyperbolic in darkness, so dark pigment dominates the mix the
 * way real paint does, mid-mixes hold their chroma, and stroke overlaps read
 * as wet paint instead of alpha compositing. `vibrancy` fades the model in over
 * plain linear RGB.
 */
#include "pc.h"

/* reflectance in (0,1) -> Kubelka-Munk absorption/scattering ratio */
static inline f32 km_ks(f32 r) {
  f32 rr = pc_clampf(r, 0.004f, 0.996f);
  return (1.0f - rr) * (1.0f - rr) / (2.0f * rr);
}

/* absorption/scattering ratio -> reflectance */
static inline f32 km_r(f32 ks) {
  return 1.0f + ks - pc_sqrtf(ks * ks + 2.0f * ks);
}

/*
 * Mix paint `b` into paint `a` at concentration t
 * (both 0-255 RGB, result in out, out may alias a)
 * vibrancy 0 = plain linear RGB, 1 = full subtractive Kubelka-Munk
 */
void pc_mix_paint(const f32 *a, const f32 *b, f32 t, f32 vibrancy, f32 *out) {
  for (i32 k = 0; k < 3; k++) {
    f32 lin = a[k] + (b[k] - a[k]) * t;
    if (vibrancy > 0.0f) {
      f32 ks = km_ks(a[k] * (1.0f / 255.0f)) * (1.0f - t) +
               km_ks(b[k] * (1.0f / 255.0f)) * t;
      f32 sub = km_r(ks) * 255.0f;
      out[k] = lin + (sub - lin) * vibrancy;
    } else {
      out[k] = lin;
    }
  }
}

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
