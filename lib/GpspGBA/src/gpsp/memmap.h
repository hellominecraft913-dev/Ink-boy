#ifndef _MEMMAP_H
#define _MEMMAP_H

/* Maps the JIT translation cache. The block is placed near the text
 * segment via iterative address hints (never MAP_FIXED) so emitted
 * calls and jumps stay within branch range. Returns NULL when no
 * suitable block could be mapped; callers must fall back to the
 * interpreter in that case. */
void *map_jit_block(unsigned size);
void unmap_jit_block(void *bufptr, unsigned size);

/* Apple silicon W^X: MAP_JIT memory is executable by default and only
 * writable per-thread between jit_write_enable() and jit_exec_enable().
 * Every path that emits or patches code in the translation cache must
 * run inside such a window and leave the thread in the executable
 * state before translated code runs again. No-ops everywhere else. */
#if defined(MMAP_JIT_CACHE) && defined(__APPLE__) && defined(__aarch64__)
  #include <pthread.h>
  static inline void jit_write_enable(void)
  {
    pthread_jit_write_protect_np(0);
  }
  static inline void jit_exec_enable(void)
  {
    pthread_jit_write_protect_np(1);
  }
#else
  static inline void jit_write_enable(void) {}
  static inline void jit_exec_enable(void) {}
#endif

#endif
