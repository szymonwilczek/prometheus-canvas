/*
 * Lanczos3 resampling and luma-based unsharp mask.
 *
 * Lanczos kernel, a = 3:
 *   L(x) = sinc(x) * sinc(x/3)  for |x| < 3, else 0
 *   sinc(x) = sin(pi x) / (pi x)
 *
 * Implemented as two separable passes
 * (horizontal to a float plane, then vertical)
 * with kernel weights precomputed once per destination coordinate.
 * Weights are renormalized to sum to 1 so brightness is preserved
 * even where the 6-tap window is clipped at the border.
 */
#include "pc.h"

#define TAPS 6

static f32 lanczos3(f32 x) {
  x = pc_fabsf(x);
  if (x < 1e-6f)
    return 1.0f;
  if (x >= 3.0f)
    return 0.0f;
  f32 px = PC_PI * x;
  return 3.0f * pc_sinf(px) * pc_sinf(px / 3.0f) / (px * px);
}

/* per-destination-coordinate kernel:
 * first source tap index (clamped later) and 6 normalized weights */
static void build_kernel(i32 sn, i32 dn, i32 *base, f32 *wts) {
  f32 scale = (f32)sn / (f32)dn;
  for (i32 d = 0; d < dn; d++) {
    f32 center = ((f32)d + 0.5f) * scale - 0.5f;
    i32 i0 = (i32)pc_floorf(center) - 2;
    f32 sum = 0.0f;
    for (i32 t = 0; t < TAPS; t++) {
      f32 wgt = lanczos3(center - (f32)(i0 + t));
      wts[d * TAPS + t] = wgt;
      sum += wgt;
    }
    f32 inv = 1.0f / sum;
    for (i32 t = 0; t < TAPS; t++)
      wts[d * TAPS + t] *= inv;
    base[d] = i0;
  }
}

void pc_lanczos3(const u8 *src, i32 sw, i32 sh, u8 *dst, i32 dw, i32 dh) {
  if (sw == dw && sh == dh) {
    memcpy(dst, src, (usize)sw * (usize)sh * 4u);
    return;
  }

  i32 *bx = (i32 *)pc_alloc((usize)dw * 4);
  f32 *wx = (f32 *)pc_alloc((usize)dw * TAPS * 4);
  i32 *by = (i32 *)pc_alloc((usize)dh * 4);
  f32 *wy = (f32 *)pc_alloc((usize)dh * TAPS * 4);
  /* horizontal pass output: dw x sh, 3 channels, f32 */
  f32 *mid = (f32 *)pc_alloc((usize)dw * (usize)sh * 3 * 4);
  if (!bx || !wx || !by || !wy || !mid)
    return;

  build_kernel(sw, dw, bx, wx);
  build_kernel(sh, dh, by, wy);

  for (i32 y = 0; y < sh; y++) {
    const u8 *row = src + (usize)y * (usize)sw * 4;
    f32 *out = mid + (usize)y * (usize)dw * 3;
    for (i32 x = 0; x < dw; x++) {
      f32 r = 0, g = 0, b = 0;
      i32 i0 = bx[x];
      const f32 *wt = wx + x * TAPS;
      for (i32 t = 0; t < TAPS; t++) {
        i32 sx = pc_clampi(i0 + t, 0, sw - 1);
        f32 wv = wt[t];
        r += wv * (f32)row[sx * 4];
        g += wv * (f32)row[sx * 4 + 1];
        b += wv * (f32)row[sx * 4 + 2];
      }
      out[x * 3] = r;
      out[x * 3 + 1] = g;
      out[x * 3 + 2] = b;
    }
  }

  for (i32 y = 0; y < dh; y++) {
    i32 i0 = by[y];
    const f32 *wt = wy + y * TAPS;
    u8 *out = dst + (usize)y * (usize)dw * 4;
    for (i32 x = 0; x < dw; x++) {
      f32 r = 0, g = 0, b = 0;
      for (i32 t = 0; t < TAPS; t++) {
        i32 sy = pc_clampi(i0 + t, 0, sh - 1);
        const f32 *px = mid + ((usize)sy * (usize)dw + (usize)x) * 3;
        f32 wv = wt[t];
        r += wv * px[0];
        g += wv * px[1];
        b += wv * px[2];
      }
      out[x * 4] = pc_clamp255(r);
      out[x * 4 + 1] = pc_clamp255(g);
      out[x * 4 + 2] = pc_clamp255(b);
      out[x * 4 + 3] = 255;
    }
  }
}

/*
 * unsharp mask on the luma channel only (avoids chroma fringing):
 *   out_rgb = in_rgb + amount * (luma - gaussian(luma))
 * Gaussian is separable with kernel radius ceil(2.5 * sigma)
 */
void pc_unsharp(u8 *img, i32 w, i32 h, f32 amount, f32 sigma) {
  if (amount <= 0.0f || sigma <= 0.0f)
    return;

  i32 kr = (i32)(2.5f * sigma + 0.999f);
  if (kr < 1)
    kr = 1;
  if (kr > 16)
    kr = 16;

  f32 kern[33];
  f32 ksum = 0.0f;
  for (i32 i = -kr; i <= kr; i++) {
    f32 v = pc_expf(-(f32)(i * i) / (2.0f * sigma * sigma));
    kern[i + kr] = v;
    ksum += v;
  }
  for (i32 i = 0; i <= 2 * kr; i++)
    kern[i] /= ksum;

  usize n = (usize)w * (usize)h;
  f32 *luma = (f32 *)pc_alloc(n * 4);
  f32 *blur = (f32 *)pc_alloc(n * 4);
  if (!luma || !blur)
    return;

  for (usize i = 0; i < n; i++)
    luma[i] = pc_luma(img[i * 4], img[i * 4 + 1], img[i * 4 + 2]);

  /* horizontal luma -> blur, vertical blur -> blur (via luma reuse) */
  for (i32 y = 0; y < h; y++) {
    const f32 *row = luma + (usize)y * (usize)w;
    f32 *out = blur + (usize)y * (usize)w;
    for (i32 x = 0; x < w; x++) {
      f32 acc = 0.0f;
      for (i32 i = -kr; i <= kr; i++)
        acc += kern[i + kr] * row[pc_clampi(x + i, 0, w - 1)];
      out[x] = acc;
    }
  }
  for (i32 x = 0; x < w; x++) {
    for (i32 y = 0; y < h; y++) {
      f32 acc = 0.0f;
      for (i32 i = -kr; i <= kr; i++)
        acc += kern[i + kr] *
               blur[(usize)pc_clampi(y + i, 0, h - 1) * (usize)w + (usize)x];
      /* store the vertical result back into luma:
       * it is the final blurred plane;
       * original luma at (x,y) is no longer needed once this column
       * position is written
       * -- but the kernel reads *blur*, not luma, so no aliasing occurs */
      luma[(usize)y * (usize)w + (usize)x] =
          pc_luma(img[((usize)y * (usize)w + (usize)x) * 4],
                  img[((usize)y * (usize)w + (usize)x) * 4 + 1],
                  img[((usize)y * (usize)w + (usize)x) * 4 + 2]) -
          acc; /* luma now holds (luma - gaussian) */
    }
  }

  for (usize i = 0; i < n; i++) {
    f32 d = amount * luma[i];
    img[i * 4] = pc_clamp255((f32)img[i * 4] + d);
    img[i * 4 + 1] = pc_clamp255((f32)img[i * 4 + 1] + d);
    img[i * 4 + 2] = pc_clamp255((f32)img[i * 4 + 2] + d);
  }
}
