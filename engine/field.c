/*
 * Stroke-guidance fields for the SBR painter:
 * coherent orientation flow + visual-importance map.
 *
 * Raw structure tensor (tensor.c) already yields per-pixel contour
 * direction and anisotropy, but painter needs two more things:
 *
 * - COHERENCE: neighboring stroke seeds must agree on direction, or
 *   the canvas dissolves into cross-hatched noise.
 *   Tensor smoothing works on the *tensor*, which is orientation-agnostic;
 *   the eigenvector it yields can still flip sign or jitter between adjacent
 *   pixels.
 *   Two extra sign-aligned 3x3 vector smoothing passes settle the field into
 *   the long, laminar flow that traced strokes can actually follow.
 *
 * - IMPORTANCE: where the small detail strokes belong.
 *   High-frequency contrast (eyes, hair, fabric edges) earns dense
 *   micro-strokes; flat sky earns none.
 *   Importance is the blurred Sobel contrast of luma, normalized so
 *   the strongest feature in the image sits at 1.
 */
#include "pc.h"

/*
 * One sign-aligned 3x3 smoothing pass over a unit vector field.
 * Each neighbor is flipped to agree with the center vector before averaging,
 * so antiparallel contour directions reinforce instead of cancelling.
 * Output is re-normalized; degenerate sums keep the original vector.
 */
static void vector_smooth(f32 *fx, f32 *fy, i32 w, i32 h) {
  usize n = (usize)w * (usize)h;
  f32 *sx = (f32 *)pc_alloc(n * 4);
  f32 *sy = (f32 *)pc_alloc(n * 4);
  if (!sx || !sy)
    return;

  for (i32 y = 0; y < h; y++) {
    i32 ym = pc_maxi(y - 1, 0), yp = pc_mini(y + 1, h - 1);
    for (i32 x = 0; x < w; x++) {
      i32 xm = pc_maxi(x - 1, 0), xp = pc_mini(x + 1, w - 1);
      usize i = (usize)y * (usize)w + (usize)x;
      f32 rx = fx[i], ry = fy[i];
      f32 ax = 0.0f, ay = 0.0f;

      for (i32 qy = ym; qy <= yp; qy++) {
        for (i32 qx = xm; qx <= xp; qx++) {
          usize q = (usize)qy * (usize)w + (usize)qx;
          f32 vx = fx[q], vy = fy[q];
          if (vx * rx + vy * ry < 0.0f) {
            vx = -vx;
            vy = -vy;
          }
          ax += vx;
          ay += vy;
        }
      }

      f32 len2 = ax * ax + ay * ay;
      if (len2 < 1e-12f) {
        sx[i] = rx;
        sy[i] = ry;
      } else {
        f32 inv = 1.0f / pc_sqrtf(len2);
        sx[i] = ax * inv;
        sy[i] = ay * inv;
      }
    }
  }

  memcpy(fx, sx, n * 4);
  memcpy(fy, sy, n * 4);
}

/*
 * Visual-importance map in [0, 1]: blurred Sobel contrast of luma, normalized
 * against the strongest feature in the image.
 * Detail strokes are gated on this plane.
 */
void pc_importance(const u8 *img, i32 w, i32 h, f32 *imp) {
  usize n = (usize)w * (usize)h;
  f32 *luma = (f32 *)pc_alloc(n * 4);
  f32 *mag = (f32 *)pc_alloc(n * 4);
  if (!luma || !mag) {
    memset(imp, 0, n * 4);
    return;
  }

  for (usize i = 0; i < n; i++)
    luma[i] = pc_luma(img[i * 4], img[i * 4 + 1], img[i * 4 + 2]);

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
      mag[(usize)y * (usize)w + (usize)x] = pc_sqrtf(gx * gx + gy * gy) * NORM;
    }
  }

  /* blur pools edge energy into regions - feature is important,
   * not just its one-pixel outline */
  pc_box_blur(mag, imp, w, h, 3);

  f32 mx = 0.0f;
  for (usize i = 0; i < n; i++)
    mx = pc_maxf(mx, imp[i]);
  if (mx > 1e-6f) {
    f32 inv = 1.0f / mx;
    for (usize i = 0; i < n; i++)
      imp[i] *= inv;
  }
}

/*
 * Fills fx/fy with the coherent unit stroke direction, aniso with anisotropy
 * in [0, 1], and imp with visual importance in [0, 1].
 * All four planes are w*h f32, allocated by the caller.
 */
void pc_sbr_field(const u8 *img, i32 w, i32 h, f32 *fx, f32 *fy, f32 *aniso,
                  f32 *imp) {
  pc_structure_tensor(img, w, h, fx, fy, aniso);
  vector_smooth(fx, fy, w, h);
  vector_smooth(fx, fy, w, h);
  pc_importance(img, w, h, imp);
}
