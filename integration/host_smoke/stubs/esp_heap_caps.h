#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_INTERNAL 4U
void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(unsigned capabilities);
size_t heap_caps_get_largest_free_block(unsigned capabilities);

/* Host-test controls and counters (defined in esp_stubs.c). */
extern size_t stub_internal_free;        /* what heap_caps_get_free_size(INTERNAL) reports */
extern size_t stub_internal_largest;     /* ... and the largest free block */
extern int stub_internal_alloc_fails;    /* make INTERNAL allocations return NULL */
extern long stub_live_allocs;            /* allocations not yet freed (leak check) */
extern long stub_internal_allocs;        /* INTERNAL allocations made */
extern long stub_psram_allocs;           /* SPIRAM allocations made */
