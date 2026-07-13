/*
 * k-means color quantization
 *
 * Simulates painter's limited physical palette:
 * all image colors are clustered into k pigments in RGB space and every
 * pixel snaps to its nearest pigment.
 *
 * Determinism over vibes:
 * centroids are seeded from luminance strata (equal-population luma
 * percentiles via a 256-bin histogram), not random picks, so the same
 * image + same k always yields the same palette.
 *
 * Cost control:
 * - Lloyd iterations run on at most 32768 stride-sampled pixels.
 * - full-image assignment goes through a 15-bit RGB555 lookup table
 *   (32768 entries), so mapping is O(N) table hits instead of O(N*k)
 *   distance evaluations.
 */
#include "pc.h"

#define PC_KMAX 64
#define PC_SAMPLES 32768
#define PC_LLOYD_IT 12

void pc_quantize(u8 *img, i32 w, i32 h, i32 k) {
  if (k < 2)
    return;
  if (k > PC_KMAX)
    k = PC_KMAX;

  usize n = (usize)w * (usize)h;

  usize stride = n / PC_SAMPLES;
  if (stride < 1)
    stride = 1;
  u8 *smp = (u8 *)pc_alloc(PC_SAMPLES * 3);
  if (!smp)
    return;
  i32 ns = 0;
  for (usize i = 0; i < n && ns < PC_SAMPLES; i += stride) {
    smp[ns * 3] = img[i * 4];
    smp[ns * 3 + 1] = img[i * 4 + 1];
    smp[ns * 3 + 2] = img[i * 4 + 2];
    ns++;
  }
  if (ns < k)
    return;

  /* luminance-stratified init */
  u32 hist[256];
  f32 binR[256], binG[256], binB[256];
  memset(hist, 0, sizeof hist);
  memset(binR, 0, sizeof binR);
  memset(binG, 0, sizeof binG);
  memset(binB, 0, sizeof binB);

  for (i32 i = 0; i < ns; i++) {
    u8 r = smp[i * 3], g = smp[i * 3 + 1], b = smp[i * 3 + 2];
    i32 l = pc_clampi((i32)pc_luma(r, g, b), 0, 255);
    hist[l]++;
    binR[l] += (f32)r;
    binG[l] += (f32)g;
    binB[l] += (f32)b;
  }

  f32 cr[PC_KMAX], cg[PC_KMAX], cb[PC_KMAX];
  {
    u32 total = (u32)ns, cum = 0;
    i32 bin = 0;
    for (i32 j = 0; j < k; j++) {
      u32 target = (u32)(((u64)total * (2u * (u32)j + 1u)) / (2u * (u32)k));
      while (bin < 255 && cum + hist[bin] <= target) {
        cum += hist[bin];
        bin++;
      }
      if (hist[bin] > 0) {
        cr[j] = binR[bin] / (f32)hist[bin];
        cg[j] = binG[bin] / (f32)hist[bin];
        cb[j] = binB[bin] / (f32)hist[bin];
      } else {
        cr[j] = cg[j] = cb[j] = (f32)bin;
      }
    }
  }

  /* Lloyd iterations on the sample set */
  f32 sumR[PC_KMAX], sumG[PC_KMAX], sumB[PC_KMAX];
  u32 cnt[PC_KMAX];

  for (i32 it = 0; it < PC_LLOYD_IT; it++) {
    for (i32 j = 0; j < k; j++) {
      sumR[j] = sumG[j] = sumB[j] = 0.0f;
      cnt[j] = 0;
    }
    for (i32 i = 0; i < ns; i++) {
      f32 r = (f32)smp[i * 3], g = (f32)smp[i * 3 + 1], b = (f32)smp[i * 3 + 2];
      f32 best = 1e30f;
      i32 bj = 0;
      for (i32 j = 0; j < k; j++) {
        f32 dr = r - cr[j], dg = g - cg[j], db = b - cb[j];
        f32 d = dr * dr + dg * dg + db * db;
        if (d < best) {
          best = d;
          bj = j;
        }
      }
      sumR[bj] += r;
      sumG[bj] += g;
      sumB[bj] += b;
      cnt[bj]++;
    }
    for (i32 j = 0; j < k; j++) {
      if (cnt[j] > 0) {
        cr[j] = sumR[j] / (f32)cnt[j];
        cg[j] = sumG[j] / (f32)cnt[j];
        cb[j] = sumB[j] / (f32)cnt[j];
      }
    }
  }

  /* RGB555 -> nearest centroid LUT, then O(N) remap */
  u8 *lut = (u8 *)pc_alloc(32768);
  if (!lut)
    return;
  for (i32 c = 0; c < 32768; c++) {
    f32 r = (f32)(((c >> 10) & 31) << 3) + 4.0f;
    f32 g = (f32)(((c >> 5) & 31) << 3) + 4.0f;
    f32 b = (f32)((c & 31) << 3) + 4.0f;
    f32 best = 1e30f;
    i32 bj = 0;
    for (i32 j = 0; j < k; j++) {
      f32 dr = r - cr[j], dg = g - cg[j], db = b - cb[j];
      f32 d = dr * dr + dg * dg + db * db;
      if (d < best) {
        best = d;
        bj = j;
      }
    }
    lut[c] = (u8)bj;
  }

  u8 palR[PC_KMAX], palG[PC_KMAX], palB[PC_KMAX];
  for (i32 j = 0; j < k; j++) {
    palR[j] = pc_clamp255(cr[j]);
    palG[j] = pc_clamp255(cg[j]);
    palB[j] = pc_clamp255(cb[j]);
  }

  for (usize i = 0; i < n; i++) {
    u32 c = ((u32)(img[i * 4] >> 3) << 10) | ((u32)(img[i * 4 + 1] >> 3) << 5) |
            (u32)(img[i * 4 + 2] >> 3);
    u8 j = lut[c];
    img[i * 4] = palR[j];
    img[i * 4 + 1] = palG[j];
    img[i * 4 + 2] = palB[j];
  }
}
