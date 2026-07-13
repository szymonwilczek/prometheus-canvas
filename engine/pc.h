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
void pc_impasto(u8 *img, i32 w, i32 h, f32 depth, f32 elev, f32 azim,
                f32 specular, i32 shininess, f32 bristle, f32 weave,
                f32 weave_scale);
void pc_lanczos3(const u8 *src, i32 sw, i32 sh, u8 *dst, i32 dw, i32 dh);
void pc_unsharp(u8 *img, i32 w, i32 h, f32 amount, f32 sigma);

#endif /* PC_H */
