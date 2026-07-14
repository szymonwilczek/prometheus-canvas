/*
 * Physical paint relief:
 * Heightmap synthesis, stress-fracture craquelure, and layered optical shading
 * model.
 *
 * Oil paint is three-dimensional.
 * Height field is assembled from three mathematically distinct layers:
 *
 * 1. RIDGES - Sobel gradient magnitude of the painted image.
 *    After Kuwahara + flow strokes the image is piecewise-flat, so gradient
 *    energy sits exactly on stroke boundaries, where loaded brush deposits
 *    its thickest bead of paint. Two box blurs give the ridges soft flanks.
 * 2. BRISTLES - grooves scratched by individual brush hairs.
 *    Phase runs across the local stroke direction (perpendicular coordinate
 *    against the structure-tensor flow field), jittered by hash noise
 *    so hairs are irregular, amplitude gated by anisotropy so grooves
 *    exist only where there is actual directional structure.
 * 3. CANVAS WEAVE - procedural plain-weave fabric:
 *    warp/weft threads alternate over/under on checkerboard, each thread's
 *    crown is |sin| shaped, thread thickness jittered per-cell.
 *    Weave is attenuated where the paint ridge is thick - heavy impasto hides
 *    the canvas, thin washes reveal it, exactly like a real painting.
 *
 * On top of the relief three physical aging/optics stages operate:
 *
 * CRAQUELURE (pc_craquelure) - drying contraction of the paint film stores
 * tensile stress sigma ~ E * lap(height), weighted by film thickness.
 * Crack tips nucleate where stress peaks and propagate perpendicular to
 * the maximum tensile stress direction (the larger-magnitude eigenvector
 * of the local height Hessian), advancing while Griffith's energy release
 * rate G ~ sigma^2 * a exceeds the fracture energy.
 * Each step carves a narrow Gaussian V-groove out of the height field
 * and records its depth so the shader can trap light inside the fissure.
 *
 * SUBSURFACE SCATTERING - linseed binder is translucent;
 * diffuse irradiance is redistributed with Jensen's BSSRDF dipole approximation
 * evaluated as 5-tap kernel, and thin high-gradient stroke edges gain warm
 * transmission glow (light bleeding through the paint rim).
 *
 * TWO-LAYER VARNISH - smooth refractive film (its own brush-stroke
 * micro-relief) sits above the paint: sharp Fresnel-weighted first-surface
 * glint reflects off the varnish, while the light that enters is bent by
 * Snell's law before it shades the paint grooves underneath.
 * Gloss map derived from pigment density and film thickness modulates the
 * paint-layer lobe (thick/dark passages dry matte, oil-rich glazes dry glossy).
 */
#include "pc.h"

/*
 * Linseed binder optical dispersion across the 8 spectral bands:
 * blue light is absorbed and scattered more strongly than red inside
 * the oil film, so the BSSRDF dipole constants (alpha', sigma_tr)
 * become per-wavelength-band quantities when spectral shading is active.
 */
static const f32 BAND_SA[PC_NB] = {1.55f, 1.32f, 1.12f, 0.98f,
                                   0.88f, 0.80f, 0.74f, 0.70f};
static const f32 BAND_SS[PC_NB] = {1.22f, 1.13f, 1.06f, 1.00f,
                                   0.95f, 0.91f, 0.88f, 0.85f};

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

/* bilinear sample of an f32 plane, coordinates clamped to the border */
static f32 sample_plane(const f32 *p, i32 w, i32 h, f32 x, f32 y) {
  x = pc_clampf(x, 0.0f, (f32)(w - 1));
  y = pc_clampf(y, 0.0f, (f32)(h - 1));
  i32 x0 = (i32)x, y0 = (i32)y;
  i32 x1 = pc_mini(x0 + 1, w - 1), y1 = pc_mini(y0 + 1, h - 1);
  f32 fx = x - (f32)x0, fy = y - (f32)y0;
  f32 a = p[(usize)y0 * (usize)w + (usize)x0];
  f32 b = p[(usize)y0 * (usize)w + (usize)x1];
  f32 c = p[(usize)y1 * (usize)w + (usize)x0];
  f32 d = p[(usize)y1 * (usize)w + (usize)x1];
  return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

/*
 * Unit eigenvector of the larger-|lambda| eigenvalue of the symmetric
 * tensor [a b; b c] - the direction of maximum tensile stress
 */
static void principal_dir(f32 a, f32 b, f32 c, f32 *ex, f32 *ey) {
  f32 tr = 0.5f * (a + c);
  f32 dif = 0.5f * (a - c);
  f32 rad = pc_sqrtf(dif * dif + b * b);
  f32 l1 = tr + rad, l2 = tr - rad;
  f32 l = pc_fabsf(l1) >= pc_fabsf(l2) ? l1 : l2;
  f32 vx = b, vy = l - a;
  if (vx * vx + vy * vy < 1e-12f) {
    vx = l - c;
    vy = b;
  }
  if (vx * vx + vy * vy < 1e-12f) {
    vx = 1.0f;
    vy = 0.0f;
  }
  f32 inv = 1.0f / pc_sqrtf(vx * vx + vy * vy);
  *ex = vx * inv;
  *ey = vy * inv;
}

typedef struct {
  f32 x, y;   /* tip position */
  f32 dx, dy; /* unit propagation direction */
} pc_tip;

#define PC_MAX_TIPS 4096

/*
 * Stress-induced fracture network.
 *
 * tension  - drying tension: nucleation density and how far cracks run.
 * vdepth   - V-groove depth carved out of the height field.
 * crack    - out: per-pixel groove depth (light-trap map for the shader).
 *
 * Walk is fully deterministic: every random draw is pc_hash2 of quantized
 * position and step counters.
 */
void pc_craquelure(f32 *height, f32 *crack, i32 w, i32 h, f32 tension,
                   f32 vdepth) {
  memset(crack, 0, (usize)w * (usize)h * 4);
  if (tension <= 0.0f || vdepth <= 0.0f || w < 8 || h < 8)
    return;

  usize n = (usize)w * (usize)h;
  f32 *smooth = (f32 *)pc_alloc(n * 4);
  f32 *stress = (f32 *)pc_alloc(n * 4);
  f32 *hxx = (f32 *)pc_alloc(n * 4);
  f32 *hxy = (f32 *)pc_alloc(n * 4);
  f32 *hyy = (f32 *)pc_alloc(n * 4);
  pc_tip *tips = (pc_tip *)pc_alloc(sizeof(pc_tip) * PC_MAX_TIPS);
  if (!smooth || !stress || !hxx || !hxy || !hyy || !tips)
    return;

  pc_box_blur(height, smooth, w, h, 3);

  /* sigma = E * lap(height):
   * drying contraction of the film pulling against the rigid ground;
   * thicker layers store more strain energy */
  f32 smax = 1e-6f;
  for (i32 y = 0; y < h; y++) {
    i32 ym = pc_maxi(y - 1, 0), yp = pc_mini(y + 1, h - 1);
    for (i32 x = 0; x < w; x++) {
      i32 xm = pc_maxi(x - 1, 0), xp = pc_mini(x + 1, w - 1);
#define S(X, Y) smooth[(usize)(Y) * (usize)w + (usize)(X)]
      f32 cc = S(x, y);
      f32 sxx = S(xp, y) - 2.0f * cc + S(xm, y);
      f32 syy = S(x, yp) - 2.0f * cc + S(x, ym);
      f32 sxy = 0.25f * (S(xp, yp) - S(xm, yp) - S(xp, ym) + S(xm, ym));
      usize i = (usize)y * (usize)w + (usize)x;
      hxx[i] = sxx;
      hxy[i] = sxy;
      hyy[i] = syy;
      f32 s = pc_fabsf(sxx + syy) * (0.35f + 2.5f * cc);
      stress[i] = s;
      if (s > smax)
        smax = s;
#undef S
    }
  }
  /* total stress = uniform biaxial drying contraction of the whole film
   * (every point of the layer shrinks against the rigid ground) + normalized
   * local concentration where the film curvature peaks */
  f32 inv_smax = 1.0f / smax;
  f32 base = 0.45f * tension;
  for (usize i = 0; i < n; i++)
    stress[i] = pc_minf(1.0f, base + stress[i] * inv_smax);

  /* nucleation:
   * jittered cell grid, seeds where normalized stress clears
   * tension-controlled threshold;
   * each seed spawns two opposed tips */
  i32 ntips = 0;
  const i32 CS = 16;
  f32 thresh = 0.55f - 0.45f * tension;
  for (i32 cy = 0; cy * CS < h; cy++) {
    for (i32 cx = 0; cx * CS < w; cx++) {
      f32 px = (f32)(cx * CS) + pc_hash2(cx * 3 + 1, cy * 5 + 2) * (f32)CS;
      f32 py = (f32)(cy * CS) + pc_hash2(cx * 7 + 5, cy * 11 + 3) * (f32)CS;
      if (px >= (f32)(w - 2) || py >= (f32)(h - 2))
        continue;
      f32 s0 = sample_plane(stress, w, h, px, py);
      if (s0 < thresh)
        continue;
      if (pc_hash2(cx * 13 + 7, cy * 17 + 9) > 0.20f + 0.70f * tension)
        continue;
      f32 tx, ty;
      principal_dir(sample_plane(hxx, w, h, px, py),
                    sample_plane(hxy, w, h, px, py),
                    sample_plane(hyy, w, h, px, py), &tx, &ty);
      /* crack runs perpendicular to the maximum tensile stress vector */
      f32 dx = -ty, dy = tx;
      if (ntips + 2 > PC_MAX_TIPS)
        break;
      tips[ntips++] = (pc_tip){px, py, dx, dy};
      tips[ntips++] = (pc_tip){px, py, -dx, -dy};
    }
  }

  /* Griffith:
   * energy release rate G ~ pi * sigma^2 * a / E' grows with crack length a,
   * fracture energy Gc shrinks as drying tension rises */
  f32 gc = 0.030f + 0.11f * (1.0f - tension);
  const f32 INV2S2 = 1.0f / (2.0f * 0.7f * 0.7f); /* groove sigma 0.7 px */

  for (i32 t = 0; t < ntips; t++) {
    f32 px = tips[t].x, py = tips[t].y;
    f32 dx = tips[t].dx, dy = tips[t].dy;
    for (i32 step = 0; step < 512; step++) {
      f32 s = sample_plane(stress, w, h, px, py);
      f32 g_rate = s * s * (0.6f + 0.05f * (f32)step);
      if (g_rate < gc)
        break;

      /* steer perpendicular to the local maximum tensile stress;
       * where the stress tensor is nearly isotropic
       * (flat film, pure biaxial drying contraction)
       * there is no preferred axis, so the steering weight vanishes
       * and the tip keeps its jittered inertial path */
      f32 ha = sample_plane(hxx, w, h, px, py);
      f32 hb = sample_plane(hxy, w, h, px, py);
      f32 hc = sample_plane(hyy, w, h, px, py);
      f32 ex, ey;
      principal_dir(ha, hb, hc, &ex, &ey);
      f32 steer =
          0.22f *
          pc_minf(1.0f, (pc_fabsf(ha) + 2.0f * pc_fabsf(hb) + pc_fabsf(hc)) *
                            inv_smax * 3.0f);
      f32 cx = -ey, cy2 = ex;
      if (cx * dx + cy2 * dy < 0.0f) {
        cx = -cx;
        cy2 = -cy2;
      }
      dx = (1.0f - steer) * dx + steer * cx;
      dy = (1.0f - steer) * dy + steer * cy2;
      /* deterministic kink jitter: real fracture surfaces are never smooth */
      f32 ang =
          (pc_hash2((i32)(px * 4.0f) + step, (i32)(py * 4.0f) + t) - 0.5f) *
          0.30f;
      f32 ca = pc_cosf(ang), sa = pc_sinf(ang);
      f32 rx = dx * ca - dy * sa, ry = dx * sa + dy * ca;
      f32 inv = 1.0f / pc_sqrtf(rx * rx + ry * ry + 1e-12f);
      dx = rx * inv;
      dy = ry * inv;

      px += dx;
      py += dy;
      if (px < 1.5f || py < 1.5f || px > (f32)w - 2.5f || py > (f32)h - 2.5f)
        break;

      f32 d0 = vdepth * (0.045f + 0.10f * s);

      /* arrest at an existing crack:
       * probe ahead of the tip (beyond this tip's own freshly carved stamps) -
       * junctions knit the network */
      if (step > 5 && sample_plane(crack, w, h, px + dx * 2.5f,
                                   py + dy * 2.5f) > d0 * 0.35f) {
        step = 512; /* one final stamp below, then stop */
      }

      /* carve the V-groove: narrow Gaussian depth profile across the tip */
      i32 ix = (i32)px, iy = (i32)py;
      for (i32 oy = -2; oy <= 2; oy++) {
        i32 yy = pc_clampi(iy + oy, 0, h - 1);
        for (i32 ox = -2; ox <= 2; ox++) {
          i32 xx = pc_clampi(ix + ox, 0, w - 1);
          f32 ddx = (f32)xx - px, ddy = (f32)yy - py;
          f32 g = d0 * pc_expf(-(ddx * ddx + ddy * ddy) * INV2S2);
          usize i = (usize)yy * (usize)w + (usize)xx;
          if (g > crack[i]) {
            height[i] -= g - crack[i];
            crack[i] = g;
          }
        }
      }
      if (step >= 512)
        break;

      /* bifurcation: high local stress splits the tip into a T-junction */
      if (ntips < PC_MAX_TIPS &&
          pc_hash2((i32)(px * 7.0f) + step * 3, (i32)(py * 13.0f) + t) <
              0.010f + 0.030f * s * tension) {
        tips[ntips++] = (pc_tip){px, py, -dy, dx};
      }
    }
  }
}

void pc_impasto(u8 *img, i32 w, i32 h, f32 bristle, f32 weave, f32 weave_scale,
                f32 crack_tension, f32 crack_depth, const pc_shade *sp) {
  pc_shade local = *sp;
  if (local.depth <= 0.0f && weave <= 0.0f && crack_tension <= 0.0f &&
      local.varnish <= 0.0f)
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
  pc_box_blur(height, tmp, w, h, 2);
  memcpy(height, tmp, n * 4);

  /* layer 2: bristle grooves along the flow field
   * Tensor is taken from the *current* (post-stroke) image,
   * not the shared pre-stroke field:
   * quantization edges before the flow pass carry near-1 anisotropy everywhere,
   * which floods the gate and turns grooves into full-field bars */
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
          /* fine paint granularity */
          f32 grain = pc_hash2(x, y) - 0.5f;
          /* Two gates:
           * aniso^2 keeps grooves aligned with genuine directional structure,
           * and the ridge height keeps them where paint is actually thick.
           * Without the second gate the nearest-sampled stroke pass leaves
           * enough micro-edges to flood the anisotropy gate across flat
           * regions */
          f32 gate = aniso[i] * aniso[i] * pc_minf(1.0f, height[i] * 6.0f);
          height[i] += bristle * gate * (groove * 0.035f + grain * 0.02f);
        }
      }
    }
  }

  /* layer 3: canvas weave,
   * then the drying fractures cut through the whole stack
   * (real craquelure reaches down to the ground layer) */
  pc_add_weave(height, w, h, weave, weave_scale);
  if (crack_tension > 0.0f) {
    f32 *crack = (f32 *)pc_alloc(n * 4);
    if (crack) {
      pc_craquelure(height, crack, w, h, crack_tension, crack_depth);
      local.crack = crack;
    }
  }
  if (local.depth <= 0.0f)
    local.depth = 20.0f; /* weave/crack-only mode still needs normals
                          * to have something to bite on */
  pc_shade_height(img, w, h, height, &local);
}

/*
 * Plain-weave canvas added to an existing height field:
 * warp/weft threads alternate over/under on checkerboard, each thread crowned
 * by |sin|, thickness jittered per cell - attenuated where the paint is already
 * thick, exactly like real primed canvas
 */
void pc_add_weave(f32 *height, i32 w, i32 h, f32 weave, f32 weave_scale) {
  if (weave <= 0.0f || weave_scale < 1.0f)
    return;

  f32 inv_s = 1.0f / weave_scale;
  for (i32 y = 0; y < h; y++) {
    for (i32 x = 0; x < w; x++) {
      usize i = (usize)y * (usize)w + (usize)x;
      f32 u = (f32)x * inv_s, v = (f32)y * inv_s;
      i32 cu = (i32)pc_floorf(u), cv = (i32)pc_floorf(v);
      f32 warp = pc_fabsf(pc_sinf(PC_PI * u));
      f32 weft = pc_fabsf(pc_sinf(PC_PI * v));
      f32 thread = ((cu + cv) & 1) ? warp : weft;
      f32 jitter = 0.85f + 0.3f * pc_hash2(cu, cv);
      f32 cover = 1.0f - pc_minf(1.0f, height[i] * 3.0f);
      height[i] += weave * 0.02f * thread * jitter * cover;
    }
  }
}

/*
 * Snell refraction of incident unit vector (pointing toward the surface) about
 * unit normal N, eta = n_outside / n_inside.
 * Entering the denser medium k stays positive;
 * the clamp only guards float noise.
 */
static void refract_dir(f32 ix, f32 iy, f32 iz, f32 nx, f32 ny, f32 nz, f32 eta,
                        f32 *tx, f32 *ty, f32 *tz) {
  f32 ci = -(ix * nx + iy * ny + iz * nz);
  f32 k = 1.0f - eta * eta * (1.0f - ci * ci);
  if (k < 0.0f)
    k = 0.0f;
  f32 ct = pc_sqrtf(k);
  *tx = eta * ix + (eta * ci - ct) * nx;
  *ty = eta * iy + (eta * ci - ct) * ny;
  *tz = eta * iz + (eta * ci - ct) * nz;
}

/*
 * Jensen's dipole diffuse reflectance profile:
 *   Rd(r) = alpha'/(4 pi) * [ zr (1 + str*dr) e^(-str*dr) / dr^3
 *                           + zv (1 + str*dv) e^(-str*dv) / dv^3 ]
 * with the real source at depth zr below the surface and the mirrored
 * virtual source at zv above it.
 */
static f32 dipole_rd(f32 r, f32 alpha, f32 str, f32 zr, f32 zv) {
  f32 dr = pc_sqrtf(r * r + zr * zr);
  f32 dv = pc_sqrtf(r * r + zv * zv);
  f32 tr = str * dr, tv = str * dv;
  f32 er = (1.0f + tr) * pc_expf(-tr) / (dr * dr * dr);
  f32 ev = (1.0f + tv) * pc_expf(-tv) / (dv * dv * dv);
  return alpha * (1.0f / (4.0f * PC_PI)) * (zr * er + zv * ev);
}

/*
 * Layered relief shading of an RGBA image against an arbitrary height field.
 *
 * Pass A rasterizes per-pixel irradiance and specular energy:
 *  - paint normals from crisp-remapped central differences;
 *  - if a varnish layer is present, its own brush-stroke micro-relief yields
 *    a per-pixel varnish normal: the Fresnel share F of the light glints off
 *    that smooth outer surface (sharp isotropic lobe), the (1-F) share is
 *    bent by Snell's law and shades the paint grooves underneath with the
 *    refracted light/view vectors, attenuated once more on exit;
 *  - the paint lobe is modulated by a gloss map: matte where pigment is
 *    dense/thick, glossy where the oil-rich film is thin and bright, and
 *    blended toward a Kajiya-Kay strand glint across bristle grooves;
 *  - craquelure grooves trap light: diffuse and specular are heavily
 *    attenuated by the local crack depth (cavity of the V-groove).
 *
 * Pass B redistributes the diffuse irradiance with the BSSRDF dipole kernel
 * (subsurface scattering in the binder), adds the warm transmission glow at
 * thin stroke edges, settles dirt into old cracks, and composes the result.
 * Diffuse is normalized against a flat surface and every specular term is
 * offset by its flat-surface value, so a zero-relief pixel keeps its exact
 * original color at any light angle.
 */
void pc_shade_height(u8 *img, i32 w, i32 h, const f32 *height,
                     const pc_shade *sp) {
  usize n = (usize)w * (usize)h;
  f32 *irr = (f32 *)pc_alloc(n * 4);
  f32 *spc = (f32 *)pc_alloc(n * 4);
  if (!irr || !spc)
    return;

  f32 *blur = 0;
  if (sp->cavity > 0.0f) {
    blur = (f32 *)pc_alloc(n * 4);
    if (blur)
      pc_box_blur(height, blur, w, h, 3);
  }

  /* subsurface dipole constants (sigma_s' = 0 disables the gather) */
  i32 sss_on = sp->sss_scatter > 0.0f;
  f32 alpha = 0.0f, str = 0.0f, zr = 0.0f, zv = 0.0f;
  f32 w_c = 1.0f, w_t = 0.0f;
  f32 A_dip = 0.0f;
  i32 tap = 1;
  f32 *edge = 0;
  if (sss_on) {
    f32 ss = sp->sss_scatter;
    f32 sa = pc_maxf(sp->sss_absorb, 1e-3f);
    f32 st = ss + sa;               /* sigma_t' */
    alpha = ss / st;                /* alpha'   */
    str = pc_sqrtf(3.0f * sa * st); /* sigma_tr */
    const f32 NB = 1.48f;           /* linseed binder IOR */
    f32 fdr = -1.440f / (NB * NB) + 0.710f / NB + 0.668f + 0.0636f * NB;
    A_dip = (1.0f + fdr) / (1.0f - fdr);
    zr = 1.0f / st;
    zv = zr * (1.0f + 4.0f * A_dip / 3.0f);
    tap = pc_clampi((i32)(1.0f / str + 0.5f), 1, 4);
    f32 rd0 = dipole_rd(0.0f, alpha, str, zr, zv);
    f32 rd1 = dipole_rd((f32)tap, alpha, str, zr, zv);
    f32 sum = rd0 + 4.0f * rd1;
    w_c = rd0 / sum;
    w_t = rd1 / sum;
    edge = (f32 *)pc_alloc(n * 4);
    if (!edge)
      sss_on = 0;
  }

  /*
   * Spectral shading:
   * when any spectral feature is active, pass B runs on 8 wavelength bands
   * instead of 3 display channels - pigment reflectance is upsampled per pixel,
   * the glaze stack / chemical aging filter each band, the dipole gather uses
   * per-band binder coefficients, and the reflected spectrum is integrated
   * under the selected gallery illuminant (the metameric shift).
   */
  i32 spec_shade = sp->spectral > 0.0f || sp->glaze_layers > 0 ||
                   (sp->age > 0.0f && sp->yellowing > 0.0f);
  f32 ill_rgb[3] = {1.0f, 1.0f, 1.0f};
  f32 wc_b[PC_NB], wt_b[PC_NB], alpha_b[PC_NB], str_b[PC_NB];
  for (i32 k = 0; k < PC_NB; k++) {
    wc_b[k] = 1.0f;
    wt_b[k] = 0.0f;
    alpha_b[k] = 0.0f;
    str_b[k] = 0.0f;
  }
  if (spec_shade) {
    pc_display_white(ill_rgb);
    if (sss_on) {
      for (i32 k = 0; k < PC_NB; k++) {
        f32 sa_k = pc_maxf(sp->sss_absorb, 1e-3f) * BAND_SA[k];
        f32 ss_k = sp->sss_scatter * BAND_SS[k];
        f32 st_k = ss_k + sa_k;
        alpha_b[k] = ss_k / st_k;
        str_b[k] = pc_sqrtf(3.0f * sa_k * st_k);
        f32 zr_k = 1.0f / st_k;
        f32 zv_k = zr_k * (1.0f + 4.0f * A_dip / 3.0f);
        f32 rd0 = dipole_rd(0.0f, alpha_b[k], str_b[k], zr_k, zv_k);
        f32 rd1 = dipole_rd((f32)tap, alpha_b[k], str_b[k], zr_k, zv_k);
        f32 sum = rd0 + 4.0f * rd1;
        wc_b[k] = rd0 / sum;
        wt_b[k] = rd1 / sum;
      }
    }
  }

  /* light and air-side half vector */
  f32 ce = pc_cosf(sp->elev);
  f32 lx = ce * pc_cosf(sp->azim);
  f32 ly = -ce * pc_sinf(sp->azim);
  f32 lz = pc_sinf(sp->elev);

  f32 hx = lx, hy = ly, hz = lz + 1.0f;
  f32 hlen = pc_sqrtf(hx * hx + hy * hy + hz * hz);
  hx /= hlen;
  hy /= hlen;
  hz /= hlen;

  /* varnish layer constants */
  i32 varnish_on = sp->varnish > 0.0f;
  f32 nv_ior = pc_clampf(sp->varnish_ior, 1.01f, 2.5f);
  f32 eta = 1.0f / nv_ior;
  f32 f0 = (nv_ior - 1.0f) / (nv_ior + 1.0f);
  f0 *= f0;
  f32 v_amp = sp->varnish * 0.5f;
  const f32 VFREQ = 0.13f; /* varnish brush-stroke pitch ~48 px */
  const f32 VFREQ2 = 0.031f;
  i32 sv_exp = pc_mini(sp->shininess * 4, 400);

  /* flat-surface reference (refracted when varnish is on)
   * so zero relief stays color-neutral */
  f32 flx = lx, fly = ly, flz = lz;
  if (varnish_on) {
    f32 tx, ty, tz;
    refract_dir(-lx, -ly, -lz, 0.0f, 0.0f, 1.0f, eta, &tx, &ty, &tz);
    flx = -tx;
    fly = -ty;
    flz = -tz;
  }
  f32 fhx = flx, fhy = fly, fhz = flz + 1.0f;
  f32 fhlen = pc_sqrtf(fhx * fhx + fhy * fhy + fhz * fhz);
  fhz /= fhlen;

  const f32 AMBIENT = 0.30f, DIFFUSE = 0.70f;
  f32 flat = AMBIENT + DIFFUSE * pc_maxf(flz, 0.0f);
  f32 inv_flat = flat > 1e-6f ? 1.0f / flat : 1.0f;
  i32 shin_lo = pc_maxi(2, sp->shininess >> 2); /* matte broadened lobe */
  f32 flat_hi = pow_int(pc_maxf(fhz, 0.0f), sp->shininess);
  f32 flat_lo = pow_int(pc_maxf(fhz, 0.0f), shin_lo);
  f32 flat_v = varnish_on ? pow_int(pc_maxf(hz, 0.0f), sv_exp) : 0.0f;
  f32 aniso = pc_clampf(sp->aniso, 0.0f, 1.0f);
  i32 kk_exp = pc_maxi(1, sp->shininess >> 1); /* sin^n via (sin^2)^(n/2) */
  f32 gloss_dep = pc_clampf(sp->gloss_dep, 0.0f, 1.0f);

  /* pass A: irradiance + specular fields */
  for (i32 y = 0; y < h; y++) {
    i32 ym = pc_maxi(y - 1, 0), yp = pc_mini(y + 1, h - 1);
    for (i32 x = 0; x < w; x++) {
      i32 xm = pc_maxi(x - 1, 0), xp = pc_mini(x + 1, w - 1);
      usize row = (usize)y * (usize)w;

      f32 dhx = (height[row + (usize)xp] - height[row + (usize)xm]) * 0.5f;
      f32 dhy = (height[(usize)yp * (usize)w + (usize)x] -
                 height[(usize)ym * (usize)w + (usize)x]) *
                0.5f;

      /* Normal sharpening:
       * central differences over the blurred height field make every transition
       * soft mound.
       * Remap the slope magnitude through a smoothstep sigmoid - gentle slopes
       * relax, steep stroke fractures keep (and slightly gain) their gradient -
       * so paint edges shade as crisp breaks instead of soap.
       * 0.35 floor keeps faint weave and grain texture alive */
      f32 gm = pc_sqrtf(dhx * dhx + dhy * dhy);
      if (gm > 1e-6f) {
        f32 gn = pc_minf(gm * 16.0f, 1.0f);
        f32 shaped = gn * gn * (3.0f - 2.0f * gn);
        f32 crisp = 0.35f + 0.65f * (shaped / gn);
        dhx *= crisp;
        dhy *= crisp;
      }

      f32 nx = -dhx * sp->depth, ny = -dhy * sp->depth, nz = 1.0f;
      f32 inv_len = 1.0f / pc_sqrtf(nx * nx + ny * ny + 1.0f);
      nx *= inv_len;
      ny *= inv_len;
      nz *= inv_len;

      usize i = row + (usize)x;

      /* effective light/half vectors for the paint layer
       * + the varnish first-surface glint */
      f32 plx = lx, ply = ly, plz = lz;
      f32 phx = hx, phy = hy, phz = hz;
      f32 fres = 0.0f, spec_v = 0.0f;
      if (varnish_on) {
        /* varnish brush-stroke micro-relief, analytic derivatives */
        f32 wob = 1.9f * pc_sinf((f32)x * 0.017f);
        f32 ph = (f32)y * VFREQ + wob;
        f32 cph = pc_cosf(ph);
        f32 ph2 = ((f32)x + (f32)y) * VFREQ2 + 0.7f;
        f32 cph2 = pc_cosf(ph2);
        f32 dvdx = v_amp * (cph * 1.9f * 0.017f * pc_cosf((f32)x * 0.017f) +
                            0.35f * VFREQ2 * cph2);
        f32 dvdy = v_amp * (VFREQ * cph + 0.35f * VFREQ2 * cph2);
        f32 vnx = -dvdx, vny = -dvdy, vnz = 1.0f;
        f32 vinv = 1.0f / pc_sqrtf(vnx * vnx + vny * vny + 1.0f);
        vnx *= vinv;
        vny *= vinv;
        vnz *= vinv;

        f32 ci = pc_maxf(vnx * lx + vny * ly + vnz * lz, 0.0f);
        f32 om = 1.0f - ci;
        f32 om2 = om * om;
        fres = f0 + (1.0f - f0) * om2 * om2 * om; /* Schlick */

        /* first surface reflection:
         * sharp isotropic glint off the smooth outer varnish,
         * relative to its flat value */
        f32 ndhv = pc_maxf(vnx * hx + vny * hy + vnz * hz, 0.0f);
        spec_v = fres * (pow_int(ndhv, sv_exp) - flat_v);

        /* transmitted share is bent by Snell's law before it reaches the paint:
         * refracted light, refracted view, new half vector */
        f32 tx, ty, tz;
        refract_dir(-lx, -ly, -lz, vnx, vny, vnz, eta, &tx, &ty, &tz);
        plx = -tx;
        ply = -ty;
        plz = -tz;
        refract_dir(0.0f, 0.0f, -1.0f, vnx, vny, vnz, eta, &tx, &ty, &tz);
        f32 pvx = -tx, pvy = -ty, pvz = -tz;
        phx = plx + pvx;
        phy = ply + pvy;
        phz = plz + pvz;
        f32 philen = pc_sqrtf(phx * phx + phy * phy + phz * phz);
        if (philen > 1e-6f) {
          phx /= philen;
          phy /= philen;
          phz /= philen;
        }
      }

      f32 ndl = pc_maxf(nx * plx + ny * ply + nz * plz, 0.0f);
      f32 ndh = pc_maxf(nx * phx + ny * phy + nz * phz, 0.0f);

      f32 E = AMBIENT + DIFFUSE * ndl;

      if (blur) {
        f32 cav = pc_maxf(0.0f, blur[i] - height[i]);
        E *= 1.0f - pc_minf(0.6f, sp->cavity * cav * 8.0f);
      }

      /* gloss map:
       * pigment-dense / thick passages dry matte,
       * thin oil-rich films dry glossy */
      f32 gloss = 1.0f;
      if (gloss_dep > 0.0f) {
        f32 dark = 1.0f - pc_luma(img[i * 4], img[i * 4 + 1], img[i * 4 + 2]) *
                              (1.0f / 255.0f);
        gloss = pc_clampf(
            1.0f - gloss_dep *
                       (0.6f * dark + 0.4f * pc_minf(1.0f, height[i] * 2.5f)),
            0.05f, 1.0f);
      }

      /* paint-layer lobe:
       * matte pixels get the broadened low exponent,
       * glossy pixels the full one, both relative to flat */
      f32 p_hi = pow_int(ndh, sp->shininess) - flat_hi;
      f32 p_lo = pow_int(ndh, shin_lo) - flat_lo;
      f32 s_spec = p_lo + (p_hi - p_lo) * gloss;
      if (sp->tfx && aniso > 0.0f) {
        /* Kajiya-Kay strand glint stretched across the groove:
         * T lies in the canvas plane along the stroke;
         * streak peaks where H is perpendicular to it.
         * slope-gated so only actual paint relief carries anisotropic sheen */
        f32 th = sp->tfx[i] * phx + sp->tfy[i] * phy;
        f32 sina2 = pc_maxf(0.0f, 1.0f - th * th);
        f32 kk = pow_int(sina2, kk_exp);
        f32 gate = pc_minf(1.0f, gm * 20.0f);
        s_spec += aniso * (gate * kk - s_spec);
      }
      s_spec *= gloss;
      if (varnish_on) {
        f32 tr = 1.0f - fres; /* enter and exit through the boundary */
        s_spec *= tr * tr;
      }

      /* light entrapment inside craquelure V-grooves:
       * multiple bounces off the fissure walls absorb nearly everything */
      if (sp->crack) {
        f32 c = pc_minf(1.0f, sp->crack[i] * 6.0f);
        E *= 1.0f - 0.80f * c;
        s_spec *= 1.0f - c;
        spec_v *= 1.0f - c;
      }

      irr[i] = E;
      spc[i] = sp->specular * (s_spec + spec_v);
      if (edge)
        edge[i] = pc_minf(1.0f, gm * 10.0f);
    }
  }

  /* pass B: dipole gather + composition */
  f32 dirt = pc_clampf(sp->crack_dirt, 0.0f, 1.0f);
  for (i32 y = 0; y < h; y++) {
    i32 ym = pc_maxi(y - tap, 0), yp = pc_mini(y + tap, h - 1);
    for (i32 x = 0; x < w; x++) {
      usize i = (usize)y * (usize)w + (usize)x;
      f32 D = irr[i];
      f32 warm = 0.0f;
      f32 sumn = 0.0f;
      if (sss_on) {
        i32 xm = pc_maxi(x - tap, 0), xp = pc_mini(x + tap, w - 1);
        sumn = irr[(usize)y * (usize)w + (usize)xm] +
               irr[(usize)y * (usize)w + (usize)xp] +
               irr[(usize)ym * (usize)w + (usize)x] +
               irr[(usize)yp * (usize)w + (usize)x];
        D = w_c * irr[i] + w_t * sumn;
        warm = 0.55f * alpha * edge[i] * pc_expf(-str * 6.0f * height[i]);
      }

      f32 r = (f32)img[i * 4];
      f32 g = (f32)img[i * 4 + 1];
      f32 b = (f32)img[i * 4 + 2];

      /* centuries of dirt settle inside the grooves */
      if (sp->crack && dirt > 0.0f) {
        f32 c = dirt * pc_minf(1.0f, sp->crack[i] * 6.0f);
        r += (46.0f - r) * c;
        g += (34.0f - g) * c;
        b += (22.0f - b) * c;
      }

      if (!spec_shade) {
        f32 mult = D * inv_flat;
        f32 spec = 255.0f * spc[i];
        img[i * 4] = pc_clamp255(r * (mult + warm * 1.10f) + spec);
        img[i * 4 + 1] = pc_clamp255(g * (mult + warm * 0.55f) + spec * 0.97f);
        img[i * 4 + 2] = pc_clamp255(b * (mult + warm * 0.22f) + spec * 0.90f);
        continue;
      }

      /*
       * Spectral composition:
       * Pigment reflectance is upsampled to 8 bands, filtered by the physical
       * film stack, shaded by the per-band dipole irradiance, and the emerging
       * spectral radiance is integrated under the gallery illuminant.
       * Scalar diffuse multiplier of the sRGB path acts in gamma space, so it
       * is raised to 2.2 here to keep the perceived shading contrast identical
       * between both paths.
       */
      f32 spd[PC_NB];
      pc_rgb_to_spd(pc_srgbf_to_linear(r), pc_srgbf_to_linear(g),
                    pc_srgbf_to_linear(b), spd);

      f32 rad[PC_NB];
      for (i32 k = 0; k < PC_NB; k++) {
        f32 Db = sss_on ? wc_b[k] * irr[i] + wt_b[k] * sumn : D;
        f32 wb = sss_on ? 0.55f * alpha_b[k] * edge[i] *
                              pc_expf(-str_b[k] * 6.0f * height[i])
                        : 0.0f;
        f32 m = pc_maxf(Db * inv_flat + wb, 0.0f);
        rad[k] = spd[k] * pc_powf(m + 1e-5f, 2.2f);
      }
      f32 outl[3];
      pc_spd_to_display(rad, outl);

      /* dielectric first-surface lobe reflects the gallery light directly -
       * under candle light even the glints turn amber */
      f32 sadd = spc[i];
      img[i * 4] = pc_linear_to_srgb(outl[0] + sadd * ill_rgb[0]);
      img[i * 4 + 1] = pc_linear_to_srgb(outl[1] + sadd * ill_rgb[1] * 0.97f);
      img[i * 4 + 2] = pc_linear_to_srgb(outl[2] + sadd * ill_rgb[2] * 0.90f);
    }
  }
}
