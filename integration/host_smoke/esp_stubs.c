#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_system.h"

void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities) {
  (void)capabilities;
  return calloc(count, size);
}

void heap_caps_free(void *ptr) { free(ptr); }

void esp_restart(void) { abort(); }
