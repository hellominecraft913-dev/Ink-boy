#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_system.h"

size_t stub_internal_free = 1024U * 1024U;
size_t stub_internal_largest = 512U * 1024U;
int stub_internal_alloc_fails = 0;
long stub_live_allocs = 0;
long stub_internal_allocs = 0;
long stub_psram_allocs = 0;

void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities) {
  void *p;
  if ((capabilities & MALLOC_CAP_INTERNAL) && stub_internal_alloc_fails) return NULL;
  p = calloc(count, size);
  if (p) {
    ++stub_live_allocs;
    if (capabilities & MALLOC_CAP_INTERNAL) ++stub_internal_allocs;
    if (capabilities & MALLOC_CAP_SPIRAM) ++stub_psram_allocs;
  }
  return p;
}

void heap_caps_free(void *ptr) {
  if (ptr) --stub_live_allocs;
  free(ptr);
}

size_t heap_caps_get_free_size(unsigned capabilities) {
  return (capabilities & MALLOC_CAP_INTERNAL) ? stub_internal_free : 8U * 1024U * 1024U;
}

size_t heap_caps_get_largest_free_block(unsigned capabilities) {
  return (capabilities & MALLOC_CAP_INTERNAL) ? stub_internal_largest : 8U * 1024U * 1024U;
}

void esp_restart(void) { abort(); }
