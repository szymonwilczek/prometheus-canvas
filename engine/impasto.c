/*
 * Physical paint relief: heightmap synthesis + Blinn-Phong.
 *
 * Oil paint is three-dimensional.
 * Height field is assembled from three mathematically distinct layers:
 *
 * 1. RIDGES - Sobel gradient magnitude of the painted image.
 *    After Kuwahara + flow strokes the image is piecewise-flat, so
 *    gradient energy sits exactly on stroke boundaries, where a loaded
 *    brush deposits its thickest bead of paint.
 *    Two box blurs give the ridges soft flanks.
 * 2. BRISTLES - grooves scratched by individual brush hairs.
 *    Phase runs across the local stroke direction (perpendicular
 *    coordinate against the structure-tensor flow field), jittered by
 *    hash noise so hairs are irregular, amplitude gated by anisotropy
 *    so grooves exist only where there is actual directional structure.
 * 3. CANVAS WEAVE - procedural plain-weave fabric:
 *    warp/weft threads alternate over/under on a checkerboard, each
 *    thread's crown is |sin| shaped, thread thickness jittered per-cell.
 *    Weave is attenuated where the paint ridge is thick - heavy impasto
 *    hides the canvas, thin washes reveal it, exactly like real painting.
 *
 * Combined field is lit per pixel by Blinn-Phong with the light direction from
 * the elevation/azimuth sliders.
 * Diffuse is normalized against a flat surface, and the specular term is offset
 * by the flat-surface glint, so zero-relief pixel keeps its exact original
 * color at any light angle.
 */
#include "pc.h"

static f32 pow_int(f32 x, i32 e) {
  f32 r = 1.0f;
  while (e > 0) {
    if (e & 1)
      r *= x;
    x *= x;
    e >>= 1;
  }
  return r;
}

void pc_impasto(u8 *img, i32 w, i32 h, f32 depth, f32 elev, f32 azim,
                f32 specular, i32 shininess, f32 bristle, f32 weave,
                f32 weave_scale) {
  if (depth <= 0.0f && weave <= 0.0f)
    return;

  usize n = (usize)w * (usize)h;
  f32 *luma = (f32 *)pc_alloc(n * 4);
  f32 *height = (f32 *)pc_alloc(n * 4);
  f32 *tmp = (f32 *)pc_alloc(n * 4);
  if (!luma || !height || !tmp)
    return;

  for (usize i = 0; i < n; i++)
    luma[i] = pc_luma(img[i * 4], img[i * 4 + 1], img[i * 4 + 2]);

  /* layer 1: stroke-boundary ridges */
  const f32 NORM = 1.0f / 1442.5f; /* max Sobel magnitude 1020*sqrt(2) */
  for (i32 y = 0; y < h; y++) {
    i32 ym = pc_maxi(y - 1, 0), yp = pc_mini(y + 1, h - 1);
    for (i32 x = 0; x < w; x++) {
      i32 xm = pc_maxi(x - 1, 0), xp = pc_mini(x + 1, w - 1);
#define L(X, Y) luma[(usize)(Y) * (usize)w + (usize)(X)]
      f32 gx = (L(xp, ym) + 2.0f * L(xp, y) + L(xp, yp)) -
               (L(xm, ym) + 2.0f * L(xm, y) + L(xm, yp));
      f32 gy = (L(xm, yp) + 2.0f * L(x, yp) + L(xp, yp)) -
               (L(xm, ym) + 2.0f * L(x, ym) + L(xp, ym));
#undef L
      f32 mag = pc_sqrtf(gx * gx + gy * gy) * NORM;
      /* mag^1.4: keeps bold stroke ridges, fades the faint
       * outlines around low-contrast pigment bands */
      height[(usize)y * (usize)w + (usize)x] =
          mag > 1e-6f ? pc_powf(mag, 1.4f) : 0.0f;
    }
  }
  pc_box3_blur(height, tmp, w, h);
  pc_box3_blur(tmp, height, w, h);

  /* layer 2: bristle grooves along the flow field */
  if (bristle > 0.0f) {
    f32 *fx = (f32 *)pc_alloc(n * 4);
    f32 *fy = (f32 *)pc_alloc(n * 4);
    f32 *aniso = (f32 *)pc_alloc(n * 4);
    if (fx && fy && aniso) {
      pc_structure_tensor(img, w, h, fx, fy, aniso);
      const f32 GROOVE_FREQ = PC_2PI / 3.0f; /* ~3 px per hair */
      for (i32 y = 0; y < h; y++) {
        for (i32 x = 0; x < w; x++) {
          usize i = (usize)y * (usize)w + (usize)x;
          /* coordinate across the stroke; grooves run along it */
          f32 t = -(f32)x * fy[i] + (f32)y * fx[i];
          /* per-region phase jitter so hairs stay irregular */
          f32 jit = pc_hash2(x >> 3, y >> 3) * PC_2PI;
          f32 groove = pc_sinf(t * GROOVE_FREQ + jit);
          /* Fine paint granularity. */
          f32 grain = pc_hash2(x, y) - 0.5f;
          /* aniso^2 gates out noise-driven false structure
           * so grooves appear only along genuine strokes */
          f32 gate = aniso[i] * aniso[i];
          height[i] += bristle * gate * (groove * 0.035f + grain * 0.02f);
        }
      }
    }
  }

  /* layer 3: plain-weave canvas, revealed where paint is thin */
  if (weave > 0.0f && weave_scale >= 1.0f) {
    f32 inv_s = 1.0f / weave_scale;
    for (i32 y = 0; y < h; y++) {
      for (i32 x = 0; x < w; x++) {
        usize i = (usize)y * (usize)w + (usize)x;
        f32 u = (f32)x * inv_s, v = (f32)y * inv_s;
        i32 cu = (i32)pc_floorf(u), cv = (i32)pc_floorf(v);
        f32 warp = pc_fabsf(pc_sinf(PC_PI * u));
        f32 weft = pc_fabsf(pc_sinf(PC_PI * v));
        /* over/under alternation of threads on checkerboard,
         * thread crown thickness jittered per cell */
        f32 thread = ((cu + cv) & 1) ? warp : weft;
        f32 jitter = 0.85f + 0.3f * pc_hash2(cu, cv);
        f32 cover = 1.0f - pc_minf(1.0f, height[i] * 3.0f);
        height[i] += weave * 0.02f * thread * jitter * cover;
      }
    }
  }

  /* Blinn-Phong over the combined field */
  f32 ce = pc_sinf(elev + PC_HALFPI);      /* cos(elev) */
  f32 lx = ce * pc_sinf(azim + PC_HALFPI); /* cos(azim) * cos(elev) */
  f32 ly = -ce * pc_sinf(azim);
  f32 lz = pc_sinf(elev);

  f32 hx = lx, hy = ly, hz = lz + 1.0f;
  f32 hlen = pc_sqrtf(hx * hx + hy * hy + hz * hz);
  hx /= hlen;
  hy /= hlen;
  hz /= hlen;

  const f32 AMBIENT = 0.30f, DIFFUSE = 0.70f;
  f32 flat = AMBIENT + DIFFUSE * pc_maxf(lz, 0.0f);
  f32 inv_flat = flat > 1e-6f ? 1.0f / flat : 1.0f;
  f32 flat_spec = specular * pow_int(pc_maxf(hz, 0.0f), shininess);
  /* weave-only mode still needs normals to have something to bite on */
  f32 slope = depth > 0.0f ? depth : 20.0f;

  for (i32 y = 0; y < h; y++) {
    i32 ym = pc_maxi(y - 1, 0), yp = pc_mini(y + 1, h - 1);
    for (i32 x = 0; x < w; x++) {
      i32 xm = pc_maxi(x - 1, 0), xp = pc_mini(x + 1, w - 1);
      usize row = (usize)y * (usize)w;

      f32 dhx = (height[row + (usize)xp] - height[row + (usize)xm]) * 0.5f;
      f32 dhy = (height[(usize)yp * (usize)w + (usize)x] -
                 height[(usize)ym * (usize)w + (usize)x]) *
                0.5f;

      f32 nx = -dhx * slope, ny = -dhy * slope, nz = 1.0f;
      f32 inv_len = 1.0f / pc_sqrtf(nx * nx + ny * ny + 1.0f);
      nx *= inv_len;
      ny *= inv_len;
      nz *= inv_len;

      f32 ndl = pc_maxf(nx * lx + ny * ly + nz * lz, 0.0f);
      f32 ndh = pc_maxf(nx * hx + ny * hy + nz * hz, 0.0f);

      f32 mult = (AMBIENT + DIFFUSE * ndl) * inv_flat;
      f32 spec = 255.0f * (specular * pow_int(ndh, shininess) - flat_spec);

      usize o = (row + (usize)x) * 4;
      img[o] = pc_clamp255((f32)img[o] * mult + spec);
      img[o + 1] = pc_clamp255((f32)img[o + 1] * mult + spec);
      img[o + 2] = pc_clamp255((f32)img[o + 2] * mult + spec);
    }
  }
}
