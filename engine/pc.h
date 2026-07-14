/*
 * Prometheus engine core.
 *
 * Freestanding C99 targeting wasm32.
 * No libc, no emscripten runtime.
 * Math comes from hand-rolled polynomial approximations +
 * native WASM float instructions exposed as clang builtins.
 */
#ifndef PC_H
#define PC_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed int i32;
typedef float f32;
typedef unsigned long usize; /* 32-bit on wasm32 */

#define PC_PI 3.14159265358979f
#define PC_HALFPI 1.57079632679490f
#define PC_2PI 6.28318530717959f

/* bump allocator over the WASM linear heap */
void *pc_alloc(usize n);
void pc_reset(void);

/* compiler is free to emit calls to these even in freestanding mode */
void *memset(void *dst, int c, usize n);
void *memcpy(void *dst, const void *src, usize n);

/* polynomial approximations */
f32 pc_sinf(f32 x);
f32 pc_expf(f32 x);
f32 pc_logf(f32 x);
f32 pc_powf(f32 base, f32 e); /* base > 0 */

static inline f32 pc_cosf(f32 x) { return pc_sinf(x + PC_HALFPI); }

/* WASM instructions */
static inline f32 pc_sqrtf(f32 x) { return __builtin_sqrtf(x); }
static inline f32 pc_fabsf(f32 x) { return __builtin_fabsf(x); }
static inline f32 pc_floorf(f32 x) { return __builtin_floorf(x); }

static inline i32 pc_mini(i32 a, i32 b) { return a < b ? a : b; }
static inline i32 pc_maxi(i32 a, i32 b) { return a > b ? a : b; }
static inline i32 pc_clampi(i32 x, i32 lo, i32 hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}
static inline f32 pc_minf(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 pc_maxf(f32 a, f32 b) { return a > b ? a : b; }
static inline f32 pc_clampf(f32 x, f32 lo, f32 hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

/* round-to-nearest with saturation into byte */
static inline u8 pc_clamp255(f32 x) {
  i32 i = (i32)(x + 0.5f);
  return (u8)(i < 0 ? 0 : (i > 255 ? 255 : i));
}

/* Rec. 601 luma from 8-bit RGB.
 * Result in [0, 255] */
static inline f32 pc_luma(u8 r, u8 g, u8 b) {
  return 0.299f * (f32)r + 0.587f * (f32)g + 0.114f * (f32)b;
}

/* deterministic 2D hash noise in [0, 1) */
static inline f32 pc_hash2(i32 x, i32 y) {
  u32 v = (u32)x * 374761393u + (u32)y * 668265263u;
  v = (v ^ (v >> 13)) * 1274126177u;
  v ^= v >> 16;
  return (f32)(v & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

/* tensor.c */
void pc_box_blur(const f32 *src, f32 *dst, i32 w, i32 h, i32 r);
void pc_structure_tensor(const u8 *img, i32 w, i32 h, f32 *fx, f32 *fy,
                         f32 *aniso);

/* pipeline stages */
void pc_kuwahara(const u8 *src, u8 *dst, i32 w, i32 h, i32 radius, f32 q);
void pc_flow_strokes(u8 *img, i32 w, i32 h, i32 length, const f32 *fx,
                     const f32 *fy);
void pc_quantize(u8 *img, i32 w, i32 h, i32 k);
void pc_color_adjust(u8 *img, i32 w, i32 h, f32 saturation, f32 contrast);
void pc_mix_paint(const f32 *a, const f32 *b, f32 t, f32 vibrancy, f32 *out);
void pc_pigment_noise(u8 *img, i32 w, i32 h, f32 amount, f32 scale);

/*
 * 8-band spectral engine (color.c).
 *
 * Reflectance spectra are discretized into PC_NB bands spanning
 * 380-730 nm (band width 43.75 nm).
 *
 * RGB -> SPD uses Smits' basis-spectra upsampling;
 * SPD -> RGB integrates the CIE 1931 2-degree observer
 * (Wyman-Sloan-Shirley analytic fits) under physical illuminant SPD,
 * anchored so that the D65 round trip is exact on the sRGB primaries.
 */
#define PC_NB 8

/* illuminant ids for pc_spectral_setup / pc_shade.illuminant */
#define PC_ILL_D65 0    /* CIE D65 daylight, 6504 K            */
#define PC_ILL_A 1      /* CIE A incandescent, 2856 K Planck   */
#define PC_ILL_F11 2    /* CIE F11 tri-band fluorescent        */
#define PC_ILL_CANDLE 3 /* candle flame, ~1900 K Planck        */

/* Select the gallery illuminant and the metameric-shift strength.
 * strength 0 collapses every spectral path to the sRGB bypass. */
void pc_spectral_setup(i32 illuminant, f32 strength);

/* linear rgb in [0,1] -> 8-band reflectance (Smits spectral upsampling) */
void pc_rgb_to_spd(f32 r, f32 g, f32 b, f32 *spd);
/* 8-band SPD -> linear rgb under the D65 reference observer */
void pc_spd_to_rgb_ref(const f32 *spd, f32 *rgb);
/* 8-band SPD -> linear rgb under the selected gallery illuminant */
void pc_spd_to_display(const f32 *spd, f32 *rgb);
/* linear rgb tint of the selected illuminant (flat white reflector) */
void pc_display_white(f32 *rgb);

/* sRGB transfer function, c in [0,255] */
f32 pc_srgbf_to_linear(f32 c);
u8 pc_linear_to_srgb(f32 v);

/* multi-pass glazing:
 * filter an opaque base reflectance spectrum through stack of `layers` diluted
 * velatura films of the same local pigment
 * (finite-thickness Kubelka-Munk films + Saunderson boundary correction) */
void pc_glaze_apply(f32 *spd, f32 thickness, i32 layers, f32 dilution,
                    f32 scatter, f32 ior);

/* linseed-oil yellowing:
 * extra absorption in the 400-460 nm bands, double-pass through the aged
 * binder film */
void pc_age_yellow(f32 *spd, f32 amount);

/* metal-soap efflorescence:
 * crystalline micro-deposits grown in the relief valleys, written into
 * the height field; effl receives the crystal map */
void pc_efflorescence(f32 *height, const f32 *crack, f32 *effl, i32 w, i32 h,
                      f32 density, f32 scale);
/*
 * Full physical shading state for pc_shade_height.
 * Optional pointers may be 0;
 * each feature collapses to a no-op at its neutral value
 * (aniso 0, sss_scatter 0, varnish 0, crack 0).
 */
typedef struct {
  f32 depth;      /* heightmap gradient multiplier for the normals */
  f32 elev;       /* light elevation, radians */
  f32 azim;       /* light azimuth, radians */
  f32 specular;   /* paint-layer specular strength */
  i32 shininess;  /* Blinn-Phong exponent of the paint layer */
  f32 cavity;     /* ambient occlusion in paint valleys */
  const f32 *tfx; /* stroke tangent field for Kajiya-Kay glint, or 0 */
  const f32 *tfy;
  f32 aniso; /* isotropic -> strand glint blend */

  /* BSSRDF dipole subsurface scattering of the oil binder */
  f32 sss_scatter; /* reduced scattering coefficient sigma_s', px^-1 */
  f32 sss_absorb;  /* absorption coefficient sigma_a, px^-1 */

  /* two-layer refractive varnish */
  f32 varnish;     /* varnish layer thickness (0 disables the layer) */
  f32 varnish_ior; /* refractive index of the varnish, ~1.5 */
  f32 gloss_dep;   /* gloss-map dependency on pigment density/thickness */

  /* stress-fracture craquelure */
  const f32 *crack; /* V-groove depth per pixel from pc_craquelure, or 0 */
  f32 crack_dirt;   /* age: dirt accumulated inside the grooves */

  /* spectral rendering: gallery illuminant & metameric shift */
  i32 illuminant; /* PC_ILL_* id of the light source */
  f32 spectral;   /* metameric shift strength; 0 = colorimetric sRGB bypass */

  /* multi-pass glazing: velatura layer stack over the opaque base */
  i32 glaze_layers;   /* number of glaze films, 0 removes the stack */
  f32 glaze_dilution; /* binder:pigment ratio of each glaze pass (0-1) */
  f32 glaze_scatter;  /* residual pigment scattering inside the glaze (0-1) */
  f32 glaze_ior;      /* refractive index of the glaze medium, ~1.48 */

  /* binder degradation: linseed yellowing + metal-soap efflorescence */
  f32 age;          /* artwork age (0 = fresh, 1 = centuries) */
  f32 yellowing;    /* linseed yellowing strength at full age */
  f32 effl_density; /* metal-soap crystal coverage, 0 = none */
  f32 effl_scale;   /* crystal colony size, px */
  f32 effl_rough;   /* how matte the crystalline deposits shade (0-1) */
  const f32 *effl;  /* crystal map written by pc_efflorescence, or 0 */
} pc_shade;

void pc_impasto(u8 *img, i32 w, i32 h, f32 bristle, f32 weave, f32 weave_scale,
                f32 crack_tension, f32 crack_depth, const pc_shade *sp);

/* shared relief helpers (impasto.c) */
void pc_add_weave(f32 *height, i32 w, i32 h, f32 weave, f32 scale);
void pc_shade_height(u8 *img, i32 w, i32 h, const f32 *height,
                     const pc_shade *sp);
void pc_craquelure(f32 *height, f32 *crack, i32 w, i32 h, f32 tension,
                   f32 vdepth);

/* pipeline.c: elastic canvas-stretch warp of the final image */
void pc_canvas_warp(u8 *img, i32 w, i32 h, f32 tension, f32 poisson,
                    f32 wrinkle_freq);

/* pipeline.c entry point (see pipeline.c for the stage order) */
void pc_process(const u8 *src, i32 w, i32 h, u8 *dst, i32 kuwahara_radius,
                f32 edge_q, i32 stroke_length, i32 pigments, f32 saturation,
                f32 contrast, f32 impasto_depth, f32 light_elev, f32 light_azim,
                f32 specular, i32 shininess, f32 bristle, f32 weave,
                f32 weave_scale, f32 cavity, f32 pigment_noise, f32 noise_scale,
                i32 render_mode, i32 knife_size, f32 knife_detail,
                f32 sbr_undercoat, f32 sbr_form, f32 sbr_detail,
                f32 sbr_alignment, f32 knife_ridge, f32 knife_dry,
                f32 knife_drag, f32 vibrancy, f32 anisotropy, f32 fringe,
                f32 sss_scatter, f32 sss_absorb, f32 varnish, f32 varnish_ior,
                f32 gloss_dep, f32 crack_tension, f32 crack_depth,
                f32 crack_dirt, f32 warp_tension, f32 warp_poisson,
                f32 wrinkle_freq, i32 illuminant, f32 spectral,
                i32 glaze_layers, f32 glaze_dilution, f32 glaze_scatter,
                f32 glaze_ior, f32 age, f32 yellowing, f32 effl_density,
                f32 effl_scale, f32 effl_rough);

/* field.c */
void pc_importance(const u8 *img, i32 w, i32 h, f32 *imp);
void pc_sbr_field(const u8 *img, i32 w, i32 h, f32 *fx, f32 *fy, f32 *aniso,
                  f32 *imp);

/* sbr.c */
void pc_sbr(u8 *img, f32 *height, i32 w, i32 h, i32 size, f32 undercoat,
            f32 form, f32 detail, f32 alignment, f32 bristle, f32 azim, f32 dry,
            f32 drag, f32 vib, f32 fringe);

/* knife.c */
void pc_knife(u8 *img, f32 *height, i32 w, i32 h, i32 size, i32 layers,
              f32 detail, f32 azim, f32 tint, f32 ridge, f32 dry, f32 drag,
              f32 vib, f32 fringe);
void pc_lanczos3(const u8 *src, i32 sw, i32 sh, u8 *dst, i32 dw, i32 dh);
void pc_unsharp(u8 *img, i32 w, i32 h, f32 amount, f32 sigma);

#endif /* PC_H */
