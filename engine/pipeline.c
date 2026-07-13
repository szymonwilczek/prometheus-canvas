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
 * Kuwahara -> flow strokes -> pigment quantization -> saturation/
 * contrast -> impasto (ridges + bristles + canvas weave, Blinn-Phong).
 * Any stage collapses to a no-op at its neutral parameter value
 * (radius 0, stroke length 0, pigments 0, sat/contrast 1, depth 0).
 */
void pc_process(const u8 *src, i32 w, i32 h, u8 *dst, i32 kuwahara_radius,
                f32 edge_q, i32 stroke_length, i32 pigments, f32 saturation,
                f32 contrast, f32 impasto_depth, f32 light_elev, f32 light_azim,
                f32 specular, i32 shininess, f32 bristle, f32 weave,
                f32 weave_scale) {
  pc_kuwahara(src, dst, w, h, kuwahara_radius, edge_q);
  /* quantize before the stroke pass:
   * flow advection then smears the hard pigment boundaries directionally,
   * turning posterization bands into brushy directional transitions */
  pc_quantize(dst, w, h, pigments);
  pc_flow_strokes(dst, w, h, stroke_length);
  pc_color_adjust(dst, w, h, saturation, contrast);
  pc_impasto(dst, w, h, impasto_depth, light_elev, light_azim, specular,
             shininess, bristle, weave, weave_scale);
}

/*
 * Stages 6-7: Lanczos3 resample to (dw, dh), then unsharp mask.
 */
void pc_upscale(const u8 *src, i32 sw, i32 sh, u8 *dst, i32 dw, i32 dh,
                f32 unsharp_amount, f32 unsharp_sigma) {
  pc_lanczos3(src, sw, sh, dst, dw, dh);
  pc_unsharp(dst, dw, dh, unsharp_amount, unsharp_sigma);
}
