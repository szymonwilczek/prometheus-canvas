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
 * Stages 1-5 at constant resolution:
 * Kuwahara -> pigment quantization -> saturation/contrast -> impasto.
 * Any stage collapses to no-op at its neutral parameter value
 * (radius 0, pigments 0, sat/contrast 1, depth 0).
 */
void pc_process(const u8 *src, i32 w, i32 h, u8 *dst, i32 kuwahara_radius,
                f32 edge_q, i32 pigments, f32 saturation, f32 contrast,
                f32 impasto_depth, f32 light_elev, f32 light_azim, f32 specular,
                i32 shininess) {
  pc_kuwahara(src, dst, w, h, kuwahara_radius, edge_q);
  pc_quantize(dst, w, h, pigments);
  pc_color_adjust(dst, w, h, saturation, contrast);
  pc_impasto(dst, w, h, impasto_depth, light_elev, light_azim, specular,
             shininess);
}

/*
 * Stages 6-7: Lanczos3 resample to (dw, dh), then unsharp mask.
 */
void pc_upscale(const u8 *src, i32 sw, i32 sh, u8 *dst, i32 dw, i32 dh,
                f32 unsharp_amount, f32 unsharp_sigma) {
  pc_lanczos3(src, sw, sh, dst, dw, dh);
  pc_unsharp(dst, dw, dh, unsharp_amount, unsharp_sigma);
}
