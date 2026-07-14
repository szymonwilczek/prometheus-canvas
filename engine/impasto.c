/*
 * Physical paint relief: heightmap synthesis + Blinn-Phong.
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
 * CRAQUELURE (pc_craquelure) - drying contraction of the paint film stores
 * tensile stress sigma ~ E * lap(height), weighted by film thickness.
 * Crack tips nucleate where stress peaks and propagate perpendicular to
 * the maximum tensile stress direction (the larger-magnitude eigenvector
 * of the local height Hessian), advancing while Griffith's energy release
 * rate G ~ sigma^2 * a exceeds the fracture energy.
 * Each step carves a narrow Gaussian V-groove out of the height field
 * and records its depth so the shader can trap light inside the fissure.
 *
 * Combined field is lit per pixel by Blinn-Phong with the light direction from
 * the elevation/azimuth in the pc_shade state.
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
                const pc_shade *sp) {
  pc_shade local = *sp;
  if (local.depth <= 0.0f && weave <= 0.0f)
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

  /* layer 3: canvas weave, then light the combined field */
  pc_add_weave(height, w, h, weave, weave_scale);
  if (local.depth <= 0.0f)
    local.depth = 20.0f; /* weave-only mode still needs normals
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
 * Blinn-Phong relief shading of an RGBA image against an arbitrary height
 * field, driven by the pc_shade state.
 * Diffuse is normalized against a flat surface and the specular term is offset
 * by the flat-surface glint, so zero-relief pixel keeps its exact original
 * color at any light angle.
 * Cavity darkening: pixel below its blurred neighborhood sits in a crevice
 * between strokes and receives ambient light last.
 *
 * ANISOTROPIC GLINT:
 * `tfx`/`tfy` (optional, may be NULL) is the local stroke tangent field.
 * Varnished brush grooves do not reflect like plastic:
 * light stretches into streak *across* the hairs.
 * Where the tangent is supplied, the isotropic Blinn-Phong lobe is blended
 * toward a Kajiya-Kay hair-strand term, sin(T,H)^n maximal when the half vector
 * is perpendicular to the groove direction - gated by the local slope so flat
 * varnish keeps no phantom sheen.
 * `aniso` sets the blend.
 */
void pc_shade_height(u8 *img, i32 w, i32 h, const f32 *height,
                     const pc_shade *sp) {
  f32 *blur = 0;
  if (sp->cavity > 0.0f) {
    blur = (f32 *)pc_alloc((usize)w * (usize)h * 4);
    if (blur)
      pc_box_blur(height, blur, w, h, 3);
  }

  f32 ce = pc_cosf(sp->elev);
  f32 lx = ce * pc_cosf(sp->azim);
  f32 ly = -ce * pc_sinf(sp->azim);
  f32 lz = pc_sinf(sp->elev);

  f32 hx = lx, hy = ly, hz = lz + 1.0f;
  f32 hlen = pc_sqrtf(hx * hx + hy * hy + hz * hz);
  hx /= hlen;
  hy /= hlen;
  hz /= hlen;

  const f32 AMBIENT = 0.30f, DIFFUSE = 0.70f;
  f32 flat = AMBIENT + DIFFUSE * pc_maxf(lz, 0.0f);
  f32 inv_flat = flat > 1e-6f ? 1.0f / flat : 1.0f;
  f32 flat_pow = pow_int(pc_maxf(hz, 0.0f), sp->shininess);
  f32 aniso = pc_clampf(sp->aniso, 0.0f, 1.0f);
  i32 kk_exp = pc_maxi(1, sp->shininess >> 1); /* sin^n via (sin^2)^(n/2) */

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

      f32 ndl = pc_maxf(nx * lx + ny * ly + nz * lz, 0.0f);
      f32 ndh = pc_maxf(nx * hx + ny * hy + nz * hz, 0.0f);

      f32 mult = (AMBIENT + DIFFUSE * ndl) * inv_flat;

      usize i = row + (usize)x;
      if (blur) {
        f32 cav = pc_maxf(0.0f, blur[i] - height[i]);
        mult *= 1.0f - pc_minf(0.6f, sp->cavity * cav * 8.0f);
      }

      /* specular relative to flat:
       * ridges glint, flats stay unchanged.
       * Slightly warm tint - linseed varnish is not neutral mirror */
      f32 s_spec = pow_int(ndh, sp->shininess) - flat_pow;
      if (sp->tfx && aniso > 0.0f) {
        /* Kajiya-Kay strand glint stretched across the groove:
         * T lies in the canvas plane along the stroke;
         * streak peaks where H is perpendicular to it.
         * slope-gated so only actual paint relief carries anisotropic sheen */
        f32 th = sp->tfx[i] * hx + sp->tfy[i] * hy;
        f32 sina2 = pc_maxf(0.0f, 1.0f - th * th);
        f32 kk = pow_int(sina2, kk_exp);
        f32 gate = pc_minf(1.0f, gm * 20.0f);
        s_spec += aniso * (gate * kk - s_spec);
      }

      f32 r = (f32)img[i * 4];
      f32 g = (f32)img[i * 4 + 1];
      f32 b = (f32)img[i * 4 + 2];

      /* light entrapment inside craquelure V-grooves:
       * multiple bounces off the fissure walls absorb nearly everything,
       * and centuries of dirt settle inside the grooves */
      if (sp->crack) {
        f32 c = pc_minf(1.0f, sp->crack[i] * 6.0f);
        mult *= 1.0f - 0.80f * c;
        s_spec *= 1.0f - c;
        f32 d = pc_clampf(sp->crack_dirt, 0.0f, 1.0f) * c;
        r += (46.0f - r) * d;
        g += (34.0f - g) * d;
        b += (22.0f - b) * d;
      }
      f32 spec = 255.0f * sp->specular * s_spec;

      img[i * 4] = pc_clamp255(r * mult + spec);
      img[i * 4 + 1] = pc_clamp255(g * mult + spec * 0.97f);
      img[i * 4 + 2] = pc_clamp255(b * mult + spec * 0.90f);
    }
  }
}
