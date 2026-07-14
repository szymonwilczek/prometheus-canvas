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
 *
 * SPECTRAL RENDERING (metamerism):
 * Three display channels cannot express how pigment behaves under change of
 * light source:
 * two paints that match under daylight drift apart under incandescent light
 * because their underlying reflectance spectra differ (metamerism).
 * Engine therefore carries a continuous spectral power distribution discretized
 * into PC_NB = 8 bands over 380-730 nm:
 *
 *   upsampling    sRGB -> reflectance via Smits' seven basis spectra
 *                 (white/cyan/magenta/yellow/red/green/blue),
 *                 resampled from his 10-bin tabulation onto our band centers;
 *   illuminants   CIE D65 (tabulated), CIE A and candle light (Planck's law
 *                 at 2856 K / 1900 K, evaluated analytically per band),
 *                 and CIE F11 (band-averaged tri-band fluorescent).
 *                 Each SPD is normalized to unit luminous power
 *                 (sum ybar*E = 1);
 *   downsampling  reflected spectral energy is multiplied by the illuminant
 *                 SPD and integrated against the CIE 1931 2-degree color
 *                 matching functions (Wyman-Sloan-Shirley piecewise-Gaussian
 *                 analytic fits), then XYZ -> linear sRGB.
 *
 * 3x3 anchoring matrix - the inverse of the D65 round trip of the sRGB
 * primaries - is applied after downsampling, so under D65 the spectral pipeline
 * reproduces the input colorimetry exactly on the primaries and to within float
 * noise elsewhere. Under any other illuminant the residual is precisely the
 * physical metameric shift, unmasked by chromatic adaptation
 * (painting in candle-lit room really does look that warm).
 */
#include "pc.h"

/* Kubelka-Munk helpers, defined with the mixing code below */
static inline f32 km_ks(f32 r);
static inline f32 km_r(f32 ks);

/* band centers, 380-730 nm in 8 equal 43.75 nm bands */
static const f32 BAND_L[PC_NB] = {401.875f, 445.625f, 489.375f, 533.125f,
                                  576.875f, 620.625f, 664.375f, 708.125f};

/*
 * Smits (1999) "An RGB to Spectrum Conversion for Reflectances":
 * 10 bins over 380-720 nm (bin centers 397 + 34k nm)
 */
static const f32 SM_WHITE[10] = {1.0000f, 1.0000f, 0.9999f, 0.9993f, 0.9992f,
                                 0.9998f, 1.0000f, 1.0000f, 1.0000f, 1.0000f};
static const f32 SM_CYAN[10] = {0.9710f, 0.9426f, 1.0007f, 1.0007f, 1.0007f,
                                1.0007f, 0.1564f, 0.0000f, 0.0000f, 0.0000f};
static const f32 SM_MAGENTA[10] = {1.0000f, 1.0000f, 0.9685f, 0.2229f, 0.0000f,
                                   0.0458f, 0.8369f, 1.0000f, 1.0000f, 0.9959f};
static const f32 SM_YELLOW[10] = {0.0001f, 0.0000f, 0.1088f, 0.6651f, 1.0000f,
                                  1.0000f, 0.9996f, 0.9586f, 0.9685f, 0.9840f};
static const f32 SM_RED[10] = {0.1012f, 0.0515f, 0.0000f, 0.0000f, 0.0000f,
                               0.0000f, 0.8325f, 1.0149f, 1.0149f, 1.0149f};
static const f32 SM_GREEN[10] = {0.0000f, 0.0000f, 0.0273f, 0.7937f, 1.0000f,
                                 0.9418f, 0.1719f, 0.0000f, 0.0000f, 0.0025f};
static const f32 SM_BLUE[10] = {1.0000f, 1.0000f, 0.8916f, 0.3323f, 0.0000f,
                                0.0000f, 0.0003f, 0.0369f, 0.0483f, 0.0496f};

/*
 * CIE D65 relative SPD sampled at our band centers
 * (linear interpolation of the CIE 10 nm tabulation).
 */
static const f32 D65_SPD[PC_NB] = {84.39f, 111.69f, 109.25f, 106.67f,
                                   95.96f, 87.42f,  81.12f,  73.83f};

/*
 * CIE F11 tri-band fluorescent, band-averaged:
 * mercury line at 435.8 nm, terbium band around 545 nm, europium band around
 * 611 nm; nearly no power in the deep blue and far red.
 * Band weights are constrained so the 8-band integration reproduces the exact
 * CIE F11 white point
 * (x = 0.3805, y = 0.3769 -> unadapted linear sRGB 1.41, 0.93, 0.53).
 */
static const f32 F11_SPD[PC_NB] = {2.55f, 9.62f,  2.97f, 15.59f,
                                   9.32f, 17.90f, 2.74f, 0.98f};

static i32 g_spec_init = 0;
static f32 g_basis[7][PC_NB]; /* white cyan magenta yellow red green blue */
static f32 g_cmf[3][PC_NB];   /* xbar ybar zbar at band centers */
static f32 g_ill[4][PC_NB];   /* D65, A, F11, candle - luminance-normalized */
static f32 g_ill_mix[PC_NB];  /* active display illuminant (D65 <-> selected) */
static f32 g_corr[9];         /* D65 round-trip anchoring matrix, row-major */
static f32 g_srgb_lut[256];   /* sRGB byte -> linear */
static f32 g_spectral_mix = 0.0f; /* gates spectral Kubelka-Munk mixing */

/* piecewise Gaussian used by the Wyman-Sloan-Shirley CMF fits */
static f32 cmf_g(f32 x, f32 mu, f32 s1, f32 s2) {
  f32 s = x < mu ? s1 : s2;
  f32 t = (x - mu) / s;
  return pc_expf(-0.5f * t * t);
}

/* relative Planck radiator SPD at wavelength lam (nm), temperature T (K) */
static f32 planck(f32 lam, f32 T) {
  f32 u = lam * 0.002f; /* lam/500: keeps lam^-5 in comfortable float range */
  f32 x = 1.4388e7f / (lam * T); /* c2 / (lam T), c2 in nm*K */
  return 1.0f / (u * u * u * u * u * (pc_expf(x) - 1.0f));
}

/* linear interpolation of a Smits 10-bin table at wavelength lam */
static f32 smits_at(const f32 *bins, f32 lam) {
  f32 t = (lam - 397.0f) * (1.0f / 34.0f);
  if (t <= 0.0f)
    return bins[0];
  if (t >= 9.0f)
    return bins[9];
  i32 i = (i32)t;
  f32 f = t - (f32)i;
  return bins[i] + (bins[i + 1] - bins[i]) * f;
}

/* Smits basis combination; inputs clamped to [0, inf) by the callers */
static void rgb_to_spd_raw(f32 r, f32 g, f32 b, f32 *spd) {
  const f32 *W = g_basis[0], *C = g_basis[1], *M = g_basis[2];
  const f32 *Y = g_basis[3], *R = g_basis[4], *G = g_basis[5];
  const f32 *B = g_basis[6];
  for (i32 k = 0; k < PC_NB; k++) {
    f32 v;
    if (r <= g && r <= b) {
      v = r * W[k];
      if (g <= b)
        v += (g - r) * C[k] + (b - g) * B[k];
      else
        v += (b - r) * C[k] + (g - b) * G[k];
    } else if (g <= r && g <= b) {
      v = g * W[k];
      if (r <= b)
        v += (r - g) * M[k] + (b - r) * B[k];
      else
        v += (b - g) * M[k] + (r - b) * R[k];
    } else {
      v = b * W[k];
      if (r <= g)
        v += (r - b) * Y[k] + (g - r) * G[k];
      else
        v += (g - b) * Y[k] + (r - g) * R[k];
    }
    spd[k] = pc_clampf(v, 0.0f, 1.05f);
  }
}

/* SPD under illuminant -> CIE 1931 XYZ -> linear sRGB (no anchoring) */
static void spd_to_lin_raw(const f32 *spd, const f32 *ill, f32 *rgb) {
  f32 X = 0.0f, Y = 0.0f, Z = 0.0f;
  for (i32 k = 0; k < PC_NB; k++) {
    f32 e = ill[k] * spd[k];
    X += g_cmf[0][k] * e;
    Y += g_cmf[1][k] * e;
    Z += g_cmf[2][k] * e;
  }
  rgb[0] = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
  rgb[1] = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
  rgb[2] = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
}

/* 3x3 inverse by adjugate; falls back to identity on a singular input */
static void inv3(const f32 *m, f32 *o) {
  f32 c00 = m[4] * m[8] - m[5] * m[7];
  f32 c01 = m[5] * m[6] - m[3] * m[8];
  f32 c02 = m[3] * m[7] - m[4] * m[6];
  f32 det = m[0] * c00 + m[1] * c01 + m[2] * c02;
  if (pc_fabsf(det) < 1e-9f) {
    for (i32 i = 0; i < 9; i++)
      o[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    return;
  }
  f32 id = 1.0f / det;
  o[0] = c00 * id;
  o[1] = (m[2] * m[7] - m[1] * m[8]) * id;
  o[2] = (m[1] * m[5] - m[2] * m[4]) * id;
  o[3] = c01 * id;
  o[4] = (m[0] * m[8] - m[2] * m[6]) * id;
  o[5] = (m[2] * m[3] - m[0] * m[5]) * id;
  o[6] = c02 * id;
  o[7] = (m[1] * m[6] - m[0] * m[7]) * id;
  o[8] = (m[0] * m[4] - m[1] * m[3]) * id;
}

static void spectral_init(void) {
  if (g_spec_init)
    return;
  g_spec_init = 1;

  /* Smits basis spectra resampled onto the 8 band centers */
  const f32 *sm[7] = {SM_WHITE, SM_CYAN,  SM_MAGENTA, SM_YELLOW,
                      SM_RED,   SM_GREEN, SM_BLUE};
  for (i32 j = 0; j < 7; j++)
    for (i32 k = 0; k < PC_NB; k++)
      g_basis[j][k] = smits_at(sm[j], BAND_L[k]);

  /* CIE 1931 2-degree observer, Wyman-Sloan-Shirley analytic fits */
  for (i32 k = 0; k < PC_NB; k++) {
    f32 l = BAND_L[k];
    g_cmf[0][k] = 1.056f * cmf_g(l, 599.8f, 37.9f, 31.0f) +
                  0.362f * cmf_g(l, 442.0f, 16.0f, 26.7f) -
                  0.065f * cmf_g(l, 501.1f, 20.4f, 26.2f);
    g_cmf[1][k] = 0.821f * cmf_g(l, 568.8f, 46.9f, 40.5f) +
                  0.286f * cmf_g(l, 530.9f, 16.3f, 31.1f);
    g_cmf[2][k] = 1.217f * cmf_g(l, 437.0f, 11.8f, 36.0f) +
                  0.681f * cmf_g(l, 459.0f, 26.0f, 13.8f);
  }

  /* illuminant SPDs, each normalized to unit luminous power */
  for (i32 k = 0; k < PC_NB; k++) {
    g_ill[PC_ILL_D65][k] = D65_SPD[k];
    g_ill[PC_ILL_A][k] = planck(BAND_L[k], 2856.0f);
    g_ill[PC_ILL_F11][k] = F11_SPD[k];
    g_ill[PC_ILL_CANDLE][k] = planck(BAND_L[k], 1900.0f);
  }
  for (i32 j = 0; j < 4; j++) {
    f32 lum = 0.0f;
    for (i32 k = 0; k < PC_NB; k++)
      lum += g_cmf[1][k] * g_ill[j][k];
    f32 inv = 1.0f / lum;
    for (i32 k = 0; k < PC_NB; k++)
      g_ill[j][k] *= inv;
  }
  memcpy(g_ill_mix, g_ill[PC_ILL_D65], sizeof(g_ill_mix));

  /* sRGB decode LUT */
  for (i32 i = 0; i < 256; i++) {
    f32 c = (f32)i * (1.0f / 255.0f);
    g_srgb_lut[i] = c <= 0.04045f
                        ? c * (1.0f / 12.92f)
                        : pc_powf((c + 0.055f) * (1.0f / 1.055f), 2.4f);
  }

  /* anchoring matrix: inverse of the D65 round trip of the sRGB primaries */
  f32 P[9];
  for (i32 p = 0; p < 3; p++) {
    f32 spd[PC_NB], col[3];
    rgb_to_spd_raw(p == 0 ? 1.0f : 0.0f, p == 1 ? 1.0f : 0.0f,
                   p == 2 ? 1.0f : 0.0f, spd);
    spd_to_lin_raw(spd, g_ill[PC_ILL_D65], col);
    P[p] = col[0];
    P[3 + p] = col[1];
    P[6 + p] = col[2];
  }
  inv3(P, g_corr);
}

void pc_spectral_setup(i32 illuminant, f32 strength) {
  spectral_init();
  g_spectral_mix = pc_clampf(strength, 0.0f, 1.0f);
  i32 il = pc_clampi(illuminant, 0, 3);
  for (i32 k = 0; k < PC_NB; k++)
    g_ill_mix[k] = g_ill[PC_ILL_D65][k] +
                   (g_ill[il][k] - g_ill[PC_ILL_D65][k]) * g_spectral_mix;
}

void pc_rgb_to_spd(f32 r, f32 g, f32 b, f32 *spd) {
  spectral_init();
  rgb_to_spd_raw(pc_maxf(r, 0.0f), pc_maxf(g, 0.0f), pc_maxf(b, 0.0f), spd);
}

static void spd_to_rgb_anchored(const f32 *spd, const f32 *ill, f32 *rgb) {
  f32 raw[3];
  spd_to_lin_raw(spd, ill, raw);
  for (i32 k = 0; k < 3; k++)
    rgb[k] = pc_maxf(g_corr[k * 3] * raw[0] + g_corr[k * 3 + 1] * raw[1] +
                         g_corr[k * 3 + 2] * raw[2],
                     0.0f);
}

void pc_spd_to_rgb_ref(const f32 *spd, f32 *rgb) {
  spectral_init();
  spd_to_rgb_anchored(spd, g_ill[PC_ILL_D65], rgb);
}

void pc_spd_to_display(const f32 *spd, f32 *rgb) {
  spectral_init();
  spd_to_rgb_anchored(spd, g_ill_mix, rgb);
}

void pc_display_white(f32 *rgb) {
  spectral_init();
  f32 flat[PC_NB];
  for (i32 k = 0; k < PC_NB; k++)
    flat[k] = 1.0f;
  spd_to_rgb_anchored(flat, g_ill_mix, rgb);
}

f32 pc_srgbf_to_linear(f32 c) {
  spectral_init();
  f32 cn = pc_clampf(c, 0.0f, 255.0f) * (1.0f / 255.0f);
  return cn <= 0.04045f ? cn * (1.0f / 12.92f)
                        : pc_powf((cn + 0.055f) * (1.0f / 1.055f), 2.4f);
}

u8 pc_linear_to_srgb(f32 v) {
  v = pc_clampf(v, 0.0f, 1.0f);
  f32 s =
      v <= 0.0031308f ? v * 12.92f : 1.055f * pc_powf(v, 1.0f / 2.4f) - 0.055f;
  return pc_clamp255(s * 255.0f);
}

/*
 * Real glazed passage is a stack of films:
 * opaque underpainting and one or more thin, heavily diluted layers of pigment
 * suspended in transparent binder.
 * Light refracts into the stack, is partially absorbed and scattered inside
 * every film, reflects between the internal boundaries, reaches the opaque
 * base, and emerges back through the whole stack toward the eye.
 *
 * Each film is solved analytically per wavelength band with the
 * finite-thickness Kubelka-Munk two-flux solution:
 *
 *   a = (S + K) / S          b = sqrt(a^2 - 1)
 *   R = sinh(bSd) / (a sinh(bSd) + b cosh(bSd))
 *   T = b        / (a sinh(bSd) + b cosh(bSd))
 *
 * which in the dilute, scatter-free limit (S -> 0) degenerates exactly to
 * the Beer-Lambert law T = exp(-K d), R = 0 - a pure velatura filter.
 * Films are then composed downward with Kubelka's layering relation
 *
 *   R_stack = R_1 + T_1^2 R_below / (1 - R_1 R_below)
 *
 * (the geometric series of all internal bounce orders), and the air/varnish
 * boundary of the top film adds the Saunderson correction with k1 the normal
 * Fresnel reflectance of the glaze medium and k2 its internal diffuse
 * reflectance (Egan-Hilgeman fit, the same expression the dipole uses).
 */

/* finite-thickness Kubelka-Munk film: reflectance + transmittance */
static void km_film(f32 K, f32 S, f32 d, f32 *R, f32 *T) {
  if (S < 1e-4f) {
    /* Beer-Lambert limit of the two-flux solution */
    *R = 0.0f;
    *T = pc_expf(-pc_minf(K * d, 60.0f));
    return;
  }
  f32 a = (S + K) / S;
  f32 b = pc_sqrtf(pc_maxf(a * a - 1.0f, 1e-8f));
  f32 x = pc_minf(b * S * d, 40.0f);
  f32 e = pc_expf(x), ei = 1.0f / e;
  f32 sh = 0.5f * (e - ei), ch = 0.5f * (e + ei);
  f32 den = a * sh + b * ch;
  *R = sh / den;
  *T = b / den;
}

void pc_glaze_apply(f32 *spd, f32 thickness, i32 layers, f32 dilution,
                    f32 scatter, f32 ior) {
  if (layers <= 0)
    return;
  spectral_init();

  layers = pc_mini(layers, 3);
  f32 dil = pc_clampf(dilution, 0.0f, 1.0f);
  f32 conc = 1.0f - 0.88f * dil; /* pigment concentration of each pass */
  f32 n = pc_clampf(ior, 1.01f, 2.0f);

  /* Saunderson boundary constants of the glaze medium */
  f32 k1 = (n - 1.0f) / (n + 1.0f);
  k1 *= k1;
  f32 k2 = -1.440f / (n * n) + 0.710f / n + 0.668f + 0.0636f * n;

  /* optical depth of one glaze pass scales with the local relief
   * (paint pools thick in the stroke valleys, thin over the ridges' flanks);
   * every additional pass adds its own film, deepening the tone the way
   * painter builds saturation by repeated velatura */
  f32 dl = 0.18f + 1.1f * pc_minf(pc_maxf(thickness, 0.0f), 1.5f);

  for (i32 k = 0; k < PC_NB; k++) {
    f32 Rbase = pc_clampf(spd[k], 0.0f, 0.999f);

    /* glaze carries the same local pigment, diluted:
     * its K/S ratio is that of the base paint,
     * its absolute K and S scale with concentration */
    f32 ks = km_ks(Rbase);
    f32 Sl = conc * scatter * 0.9f;
    f32 Kl = conc * ks;

    f32 Rl, Tl;
    km_film(Kl, Sl, dl, &Rl, &Tl);

    /* compose the stack from the base upward */
    f32 Rst = Rbase;
    for (i32 j = 0; j < layers; j++)
      Rst = Rl + Tl * Tl * Rst / (1.0f - pc_minf(Rl * Rst, 0.98f));

    /* air boundary of the top film */
    Rst = k1 +
          (1.0f - k1) * (1.0f - k2) * Rst / (1.0f - k2 * pc_minf(Rst, 0.999f));
    spd[k] = pc_clampf(Rst, 0.0f, 1.05f);
  }
}

/* Oxidative degradation products of linseed oil
 * (conjugated polyene chromophores) absorb in the blue-violet:
 * aged binder acts as long-pass filter whose absorption coefficient sigma_a
 * rises steeply below ~470 nm.
 * Per-band relative absorption profile below peaks in the 400-460 nm bands
 * and vanishes above ~560 nm, and the light crosses the film twice
 * (in and out), hence the factor 2 in the Beer-Lambert exponent.
 * `amount` carries the non-linear age response and the local thickness/pigment
 * weighting computed by the shader.
 */
static const f32 YELLOW_ABS[PC_NB] = {1.00f, 0.85f, 0.28f, 0.07f,
                                      0.02f, 0.0f,  0.0f,  0.0f};

void pc_age_yellow(f32 *spd, f32 amount) {
  if (amount <= 0.0f)
    return;
  for (i32 k = 0; k < PC_NB; k++)
    if (YELLOW_ABS[k] > 0.0f)
      spd[k] *= pc_expf(-2.0f * amount * YELLOW_ABS[k]);
}

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
 *
 * When the spectral engine is active (pc_spectral_setup with strength > 0)
 * the Kubelka-Munk mix runs independently in all PC_NB wavelength bands:
 * both paints are upsampled to reflectance spectra, K/S mixes linearly by
 * concentration per band, and the mixed spectrum is brought back through the
 * D65 reference observer. The spectral result is applied as a correction on top
 * of the exact linear mix against the round trip of that same linear mix,
 * so the upsample/downsample bias cancels and repeated wet-on-wet mixing cannot
 * drift - what remains is precisely the spectral-vs-3-channel difference in
 * gamut sharpening (real yellow+blue makes green, not gray).
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
  if (g_spectral_mix <= 0.0f || vibrancy <= 0.0f)
    return;

  const f32 I255 = 1.0f / 255.0f;
  f32 sa[PC_NB], sb[PC_NB], sm[PC_NB], sl[PC_NB];
  rgb_to_spd_raw(pc_clampf(a[0] * I255, 0.0f, 1.0f),
                 pc_clampf(a[1] * I255, 0.0f, 1.0f),
                 pc_clampf(a[2] * I255, 0.0f, 1.0f), sa);
  rgb_to_spd_raw(pc_clampf(b[0] * I255, 0.0f, 1.0f),
                 pc_clampf(b[1] * I255, 0.0f, 1.0f),
                 pc_clampf(b[2] * I255, 0.0f, 1.0f), sb);
  f32 lin3[3];
  for (i32 k = 0; k < 3; k++)
    lin3[k] = a[k] + (b[k] - a[k]) * t;
  rgb_to_spd_raw(pc_clampf(lin3[0] * I255, 0.0f, 1.0f),
                 pc_clampf(lin3[1] * I255, 0.0f, 1.0f),
                 pc_clampf(lin3[2] * I255, 0.0f, 1.0f), sl);
  for (i32 k = 0; k < PC_NB; k++) {
    f32 ks = km_ks(sa[k]) * (1.0f - t) + km_ks(sb[k]) * t;
    sm[k] = km_r(ks);
  }
  f32 rs[3], rl[3];
  spd_to_rgb_anchored(sm, g_ill[PC_ILL_D65], rs);
  spd_to_rgb_anchored(sl, g_ill[PC_ILL_D65], rl);
  for (i32 k = 0; k < 3; k++) {
    f32 sub_s = lin3[k] + (rs[k] - rl[k]) * 255.0f;
    f32 target = lin3[k] + (sub_s - lin3[k]) * vibrancy;
    out[k] += (target - out[k]) * g_spectral_mix;
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
