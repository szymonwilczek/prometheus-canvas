/*
 * Orchestration entry points called from JavaScript
 *
 * Host protocol per job:
 *   pc_reset();
 *   src = pc_alloc(w*h*4);          // host writes RGBA here
 *   dst = pc_alloc(outW*outH*4);    // host reads RGBA from here
 *   pc_process(...) / pc_upscale(...);
 *
 * Stages allocate scratch from the same bump heap.
 * Nothing survives the next pc_reset()
 */
#include "pc.h"

/*
 * Stroke tangents for the anisotropic varnish glint:
 * structure tensor of the *painted* canvas runs along the streaks and grooves
 * the strokes actually left.
 * Returns tfx (and tfy via out param), or 0 when anisotropy is off
 * or the heap is exhausted.
 */
static const f32 *paint_tangents(const u8 *img, i32 w, i32 h, f32 anisotropy,
                                 f32 **out_ty) {
  *out_ty = 0;
  if (anisotropy <= 0.0f)
    return 0;
  usize n = (usize)w * (usize)h;
  f32 *tx = (f32 *)pc_alloc(n * 4);
  f32 *ty = (f32 *)pc_alloc(n * 4);
  f32 *ta = (f32 *)pc_alloc(n * 4);
  if (!tx || !ty || !ta)
    return 0;
  pc_structure_tensor(img, w, h, tx, ty, ta);
  *out_ty = ty;
  return tx;
}

/* drying fractures carved into a stroke renderer's height field */
static void apply_craquelure(f32 *height, i32 w, i32 h, f32 tension, f32 depth,
                             pc_shade *sp) {
  if (tension <= 0.0f)
    return;
  f32 *crack = (f32 *)pc_alloc((usize)w * (usize)h * 4);
  if (!crack)
    return;
  pc_craquelure(height, crack, w, h, tension, depth);
  sp->crack = crack;
}

/*
 * GEOMETRIC CANVAS STRETCH & CORNER WARP.
 *
 * Canvas stretched over wooden frame is pinned at discrete tacks along the
 * bars; between tacks the fabric sags inward, and at the corners the
 * folded-over cloth wrinkles under the diagonal pull.
 * Displacement field u over the boundary band is the solution of the 2D
 * Navier-Cauchy elastostatic equation
 *
 *   (1 - nu) lap(u) + (1 + nu) grad(div u) = 0
 *
 * with Dirichlet conditions on the frame edge (tack scallops + corner ripples),
 * relaxed by Gauss-Seidel on coarse node grid and applied to the image as
 * inverse bilinear warp.
 *
 * The effect is confined to the outer ~4% of the picture, exactly where real
 * stretcher deforms the cloth.
 * Extreme border additionally receives rounded shading roll-off -
 * canvas wrapping over the side of the bar tilts its normal away from the light
 * - and radial fold shading in the corners.
 */
#define PC_WG 64 /* warp grid cells per side (PC_WG+1 nodes) */

void pc_canvas_warp(u8 *img, i32 w, i32 h, f32 tension, f32 poisson,
                    f32 wrinkle_freq) {
  if (tension <= 0.0f || w < 32 || h < 32)
    return;

  const i32 gn = PC_WG + 1;
  f32 *ux = (f32 *)pc_alloc((usize)gn * (usize)gn * 4);
  f32 *uy = (f32 *)pc_alloc((usize)gn * (usize)gn * 4);
  u8 *src = (u8 *)pc_alloc((usize)w * (usize)h * 4);
  if (!ux || !uy || !src)
    return;
  memset(ux, 0, (usize)gn * (usize)gn * 4);
  memset(uy, 0, (usize)gn * (usize)gn * 4);
  memcpy(src, img, (usize)w * (usize)h * 4);

  f32 nu = pc_clampf(poisson, 0.0f, 0.49f);
  f32 wf = pc_clampf(wrinkle_freq, 1.0f, 16.0f);
  f32 mind = (f32)pc_mini(w, h);
  f32 band = 0.04f * mind;          /* elastically active border, px */
  f32 amp = tension * 0.55f * band; /* max inward sag between tacks */
  const i32 TACKS = 7;

  /* Dirichlet boundary:
   * pinned at the tacks (scallop zero), sagging inward mid-span,
   * tangential ripples where the corner fold puckers the cloth */
  for (i32 k = 0; k < gn; k++) {
    f32 s = (f32)k / (f32)PC_WG;
    f32 scallop = 0.5f * (1.0f - pc_cosf(PC_2PI * (f32)TACKS * s));
    f32 sag = amp * scallop;
    f32 sc = pc_minf(s, 1.0f - s); /* distance to nearest corner */
    f32 fold = 0.0f;
    if (sc < 0.12f)
      fold = amp * 0.8f * (1.0f - sc / 0.12f) * pc_sinf(sc * wf * 8.0f);

    ux[(usize)k * (usize)gn + 0] = sag; /* left edge pulls right */
    uy[(usize)k * (usize)gn + 0] = fold;
    ux[(usize)k * (usize)gn + (usize)PC_WG] = -sag; /* right edge */
    uy[(usize)k * (usize)gn + (usize)PC_WG] = fold;
    uy[(usize)k] = sag; /* top edge pulls down */
    ux[(usize)k] = fold;
    uy[(usize)PC_WG * (usize)gn + (usize)k] = -sag; /* bottom edge */
    ux[(usize)PC_WG * (usize)gn + (usize)k] = fold;
  }

  /* Gauss-Seidel relaxation of the Navier-Cauchy operator.
   * x-equation:  (1-nu)(lap ux) + (1+nu)(d2x ux + dxdy uy) = 0
   * y-equation:  (1-nu)(lap uy) + (1+nu)(d2y uy + dxdy ux) = 0 */
  f32 denom = 4.0f * (1.0f - nu) + 2.0f * (1.0f + nu);
  f32 inv_denom = 1.0f / denom;
  for (i32 it = 0; it < 160; it++) {
    for (i32 gy = 1; gy < PC_WG; gy++) {
      for (i32 gx = 1; gx < PC_WG; gx++) {
        usize c = (usize)gy * (usize)gn + (usize)gx;
        usize e = c + 1, ww = c - 1;
        usize no = c - (usize)gn, so = c + (usize)gn;
        f32 cross_y =
            0.25f * (uy[no + 1] - uy[no - 1] - uy[so + 1] + uy[so - 1]);
        f32 cross_x =
            0.25f * (ux[no + 1] - ux[no - 1] - ux[so + 1] + ux[so - 1]);
        ux[c] = ((1.0f - nu) * (ux[e] + ux[ww] + ux[no] + ux[so]) +
                 (1.0f + nu) * (ux[e] + ux[ww] + cross_y)) *
                inv_denom;
        uy[c] = ((1.0f - nu) * (uy[e] + uy[ww] + uy[no] + uy[so]) +
                 (1.0f + nu) * (uy[no] + uy[so] + cross_x)) *
                inv_denom;
      }
    }
  }

  /* inverse warp + wrap-around shading */
  f32 roll = pc_maxf(2.0f, 0.012f * mind); /* canvas rolling over the bar */
  f32 corner = 0.10f * mind;               /* corner fold shading radius */
  f32 gsx = (f32)PC_WG / (f32)(w - 1);
  f32 gsy = (f32)PC_WG / (f32)(h - 1);
  for (i32 y = 0; y < h; y++) {
    for (i32 x = 0; x < w; x++) {
      /* elastic displacement, bilinear over the node grid */
      f32 gx = (f32)x * gsx, gy = (f32)y * gsy;
      i32 g0x = pc_mini((i32)gx, PC_WG - 1);
      i32 g0y = pc_mini((i32)gy, PC_WG - 1);
      f32 fx = gx - (f32)g0x, fy = gy - (f32)g0y;
      usize c = (usize)g0y * (usize)gn + (usize)g0x;
      f32 dx =
          (ux[c] * (1.0f - fx) + ux[c + 1] * fx) * (1.0f - fy) +
          (ux[c + (usize)gn] * (1.0f - fx) + ux[c + (usize)gn + 1] * fx) * fy;
      f32 dy =
          (uy[c] * (1.0f - fx) + uy[c + 1] * fx) * (1.0f - fy) +
          (uy[c + (usize)gn] * (1.0f - fx) + uy[c + (usize)gn + 1] * fx) * fy;

      /* deformation is physical only over the stretcher band:
       * fade the relaxed solution to zero past ~2.5 bands so
       * the picture center stays pinned */
      f32 du = (f32)pc_mini(pc_mini(x, w - 1 - x), pc_mini(y, h - 1 - y));
      f32 m = 1.0f - pc_clampf((du - band) / (1.5f * band), 0.0f, 1.0f);
      m = m * m * (3.0f - 2.0f * m);
      dx *= m;
      dy *= m;

      /* inverse bilinear sample, clamped at the border */
      f32 sxf = pc_clampf((f32)x - dx, 0.0f, (f32)(w - 1));
      f32 syf = pc_clampf((f32)y - dy, 0.0f, (f32)(h - 1));
      i32 sx0 = (i32)sxf, sy0 = (i32)syf;
      i32 sx1 = pc_mini(sx0 + 1, w - 1), sy1 = pc_mini(sy0 + 1, h - 1);
      f32 tx = sxf - (f32)sx0, ty = syf - (f32)sy0;
      usize p00 = ((usize)sy0 * (usize)w + (usize)sx0) * 4;
      usize p10 = ((usize)sy0 * (usize)w + (usize)sx1) * 4;
      usize p01 = ((usize)sy1 * (usize)w + (usize)sx0) * 4;
      usize p11 = ((usize)sy1 * (usize)w + (usize)sx1) * 4;

      /* 3D roll-off:
       * cloth wraps around the bar side, its normal tilting away
       * from the viewer/light */
      f32 shade = 1.0f;
      if (du < roll) {
        f32 t = 1.0f - du / roll;
        shade = 0.32f + 0.68f * pc_cosf(t * t * 1.15f);
      }
      /* radial fold shading where the corner cloth puckers */
      f32 eu = (f32)pc_mini(x, w - 1 - x);
      f32 ev = (f32)pc_mini(y, h - 1 - y);
      if (eu < corner && ev < corner) {
        f32 decay = (1.0f - eu / corner) * (1.0f - ev / corner);
        f32 phase = (eu - ev) * wf * 6.0f / corner;
        shade *= 1.0f + 0.22f * tension * decay * pc_sinf(phase);
      }

      for (i32 ch = 0; ch < 3; ch++) {
        f32 v = ((f32)src[p00 + (usize)ch] * (1.0f - tx) +
                 (f32)src[p10 + (usize)ch] * tx) *
                    (1.0f - ty) +
                ((f32)src[p01 + (usize)ch] * (1.0f - tx) +
                 (f32)src[p11 + (usize)ch] * tx) *
                    ty;
        img[((usize)y * (usize)w + (usize)x) * 4 + (usize)ch] =
            pc_clamp255(v * shade);
      }
    }
  }
}

/*
 * Stages 1-5 at constant resolution:
 * Kuwahara -> flow strokes -> pigment quantization -> saturation/
 * contrast -> impasto (ridges + bristles + canvas weave + craquelure,
 * layered varnish/SSS shading) -> elastic canvas warp.
 * Any stage collapses to a no-op at its neutral parameter value
 * (radius 0, stroke length 0, pigments 0, sat/contrast 1, depth 0,
 * scatter 0, varnish 0, tension 0).
 */
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
                f32 wrinkle_freq) {
  pc_kuwahara(src, dst, w, h, kuwahara_radius, edge_q);
  /* Quantize before any stroke pass:
   * all renderers then work with limited physical palette */
  pc_quantize(dst, w, h, pigments);

  pc_shade sp = {
      .depth = impasto_depth,
      .elev = light_elev,
      .azim = light_azim,
      .specular = specular,
      .shininess = shininess,
      .cavity = cavity,
      .tfx = 0,
      .tfy = 0,
      .aniso = anisotropy,
      .sss_scatter = sss_scatter,
      .sss_absorb = sss_absorb,
      .varnish = varnish,
      .varnish_ior = varnish_ior,
      .gloss_dep = gloss_dep,
      .crack = 0,
      .crack_dirt = crack_dirt,
  };

  if (render_mode == 2) {
    /* Multi-scale SBR:
     * image is repainted bottom-to-top from traced vector strokes
     * (undercoat slabs -> flow-following form strokes -> importance-gated
     * micro-detail), each stroke writing parabolic thickness profile into
     * persistent height field; the per-pixel texture stages do not apply */
    usize n = (usize)w * (usize)h;
    f32 *height = (f32 *)pc_alloc(n * 4);
    if (!height)
      return;
    pc_sbr(dst, height, w, h, knife_size, sbr_undercoat, sbr_form, sbr_detail,
           sbr_alignment, bristle, light_azim, knife_dry, knife_drag, vibrancy,
           fringe);
    pc_color_adjust(dst, w, h, saturation, contrast);
    pc_pigment_noise(dst, w, h, pigment_noise, noise_scale);
    pc_add_weave(height, w, h, weave, weave_scale);
    apply_craquelure(height, w, h, crack_tension, crack_depth, &sp);
    f32 *tfy;
    sp.tfx = paint_tangents(dst, w, h, anisotropy, &tfy);
    sp.tfy = tfy;
    pc_shade_height(dst, w, h, height, &sp);
  } else if (render_mode == 1) {
    /* Palette knife:
     * the image is rebuilt from discrete smears that carry their own relief;
     * the per-pixel texture stages (flow LIC, Sobel ridges, bristles) do not
     * apply */
    usize n = (usize)w * (usize)h;
    f32 *height = (f32 *)pc_alloc(n * 4);
    if (!height)
      return;
    pc_knife(dst, height, w, h, knife_size, 3, knife_detail, light_azim, 0.10f,
             knife_ridge, knife_dry, knife_drag, vibrancy, fringe);
    pc_color_adjust(dst, w, h, saturation, contrast);
    pc_pigment_noise(dst, w, h, pigment_noise, noise_scale);
    pc_add_weave(height, w, h, weave, weave_scale);
    apply_craquelure(height, w, h, crack_tension, crack_depth, &sp);
    f32 *tfy;
    sp.tfx = paint_tangents(dst, w, h, anisotropy, &tfy);
    sp.tfy = tfy;
    pc_shade_height(dst, w, h, height, &sp);
  } else {
    /* render_mode 0 - LEGACY per-pixel LIC brush.
     * Kept for compatibility;
     * the physics-based SBR renderer (mode 2) is the default brush.
     * This path knows nothing of the paint physics
     * (skipping, dragging, Kubelka-Munk, fringing, anisotropic glint) */
    if (stroke_length > 0) {
      usize n = (usize)w * (usize)h;
      f32 *fx = (f32 *)pc_alloc(n * 4);
      f32 *fy = (f32 *)pc_alloc(n * 4);
      f32 *aniso = (f32 *)pc_alloc(n * 4);
      if (fx && fy && aniso) {
        pc_structure_tensor(dst, w, h, fx, fy, aniso);
        pc_flow_strokes(dst, w, h, stroke_length, fx, fy);
      }
    }

    pc_color_adjust(dst, w, h, saturation, contrast);
    pc_pigment_noise(dst, w, h, pigment_noise, noise_scale);
    sp.aniso = 0.0f; /* legacy path has no stroke tangents */
    pc_impasto(dst, w, h, bristle, weave, weave_scale, crack_tension,
               crack_depth, &sp);
  }

  /* final spatial stage: stretched canvas itself deforms */
  pc_canvas_warp(dst, w, h, warp_tension, warp_poisson, wrinkle_freq);
}

/*
 * Stages 6-7: Lanczos3 resample to (dw, dh), then unsharp mask.
 */
void pc_upscale(const u8 *src, i32 sw, i32 sh, u8 *dst, i32 dw, i32 dh,
                f32 unsharp_amount, f32 unsharp_sigma) {
  pc_lanczos3(src, sw, sh, dst, dw, dh);
  pc_unsharp(dst, dw, dh, unsharp_amount, unsharp_sigma);
}
