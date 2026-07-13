/*
 * Bump allocator over the WASM linear heap.
 *
 * Pipeline is a single-shot batch job:
 * JS host calls pc_reset() once per frame, allocates input/output buffers,
 * runs the pipeline (which allocates its own scratch planes), reads the result,
 * and the next frame resets everything again.
 *
 * No free list is needed; peak memory is bounded by one frame's working set.
 */
#include "pc.h"

extern unsigned char __heap_base;

static usize heap_off = 0;

void pc_reset(void) { heap_off = 0; }

void *pc_alloc(usize n) {
  usize base = (usize)&__heap_base;
  usize p = (base + heap_off + 15u) & ~(usize)15u;
  usize end = p + n;
  usize have = (usize)__builtin_wasm_memory_size(0) << 16;

  if (end > have) {
    usize need = ((end - have) + 65535u) >> 16;
    if (__builtin_wasm_memory_grow(0, need) == (usize)-1)
      return 0;
  }
  heap_off = end - base;
  return (void *)p;
}

void *memset(void *dst, int c, usize n) {
  u8 *d = (u8 *)dst;
  for (usize i = 0; i < n; i++)
    d[i] = (u8)c;
  return dst;
}

void *memcpy(void *dst, const void *src, usize n) {
  u8 *d = (u8 *)dst;
  const u8 *s = (const u8 *)src;
  for (usize i = 0; i < n; i++)
    d[i] = s[i];
  return dst;
}
