/*
 * Polynomial approximations for the transcendentals the pipeline needs.
 * Accuracy targets are image processing, not IEEE:
 * every consumer quantizes to 8 bits at the end, so ~1e-5 relative
 * error is far below one LSB.
 */
#include "pc.h"

/*
 * sin(x), any x.
 * Range-reduce to [-pi, pi] around the nearest multiple of 2*pi,
 * fold into [-pi/2, pi/2] by symmetry, then a degree-9 odd Taylor polynomial.
 * Max abs error ~4e-6 on the folded interval.
 */
f32 pc_sinf(f32 x) {
  const f32 INV_2PI = 0.159154943091895f;

  f32 k = pc_floorf(x * INV_2PI + 0.5f);
  x -= k * PC_2PI;

  if (x > PC_HALFPI)
    x = PC_PI - x;
  if (x < -PC_HALFPI)
    x = -PC_PI - x;

  f32 x2 = x * x;
  return x * (1.0f + x2 * (-1.66666667e-1f +
                           x2 * (8.33333333e-3f + x2 * (-1.98412698e-4f +
                                                        x2 * 2.75573192e-6f))));
}

/*
 * exp(x) via 2^(x*log2 e):
 * split into integer exponent n (stuffed directly into the float's exponent
 * bits) and fractional part handled with a degree-5 Taylor polynomial of
 * e^(f*ln2), f in [0, 1).
 */
f32 pc_expf(f32 x) {
  if (x < -87.0f)
    return 0.0f;
  if (x > 88.0f)
    x = 88.0f;

  f32 y = x * 1.442695040889f; /* log2(e) */
  f32 n = pc_floorf(y);
  f32 t = (y - n) * 0.693147180560f; /* frac * ln2, in [0, ln2) */

  f32 p =
      1.0f +
      t * (1.0f + t * (0.5f + t * (0.166666667f +
                                   t * (4.16666667e-2f + t * 8.33333333e-3f))));

  union {
    u32 u;
    f32 f;
  } sc;
  sc.u = (u32)((i32)n + 127) << 23;
  return p * sc.f;
}

/*
 * ln(x), x > 0
 * Extract the binary exponent, map the mantissa m into [1, 2), then
 * ln(m) = 2*atanh((m-1)/(m+1)) as an odd series in
 * t = (m-1)/(m+1), |t| <= 1/3.
 * Max abs error ~1e-5.
 */
f32 pc_logf(f32 x) {
  union {
    f32 f;
    u32 u;
  } v = {x};

  i32 e = (i32)(v.u >> 23) - 127;
  v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
  f32 m = v.f;

  f32 t = (m - 1.0f) / (m + 1.0f);
  f32 t2 = t * t;
  f32 ln_m =
      2.0f * t * (1.0f + t2 * (0.333333333f + t2 * (0.2f + t2 * 0.142857143f)));

  return ln_m + (f32)e * 0.693147180560f;
}

f32 pc_powf(f32 base, f32 e) { return pc_expf(e * pc_logf(base)); }
