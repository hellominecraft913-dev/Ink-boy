#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities);
void heap_caps_free(void *ptr);
