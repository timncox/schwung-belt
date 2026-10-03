/*
 * patch_alloc — a bump allocator so the vendored engine needs no edits.
 *
 * belt_create() does five calloc()s and belt_destroy() frees them. Daisy has
 * no meaningful heap, and rewriting belt_create() would fork the engine.
 *
 * Instead the ARM build compiles belt_core.c with
 *     -Dcalloc=patch_calloc -Dfree=patch_free
 * pointing those calls at a static pool. Allocation happens once at boot and
 * is never returned, so free() is a no-op -- correct here, because the module
 * creates exactly one engine and keeps it until power-off.
 *
 * Same pattern as smack-versio's versio_alloc, with one difference worth
 * knowing: Belt's whole working set is small. Measured from belt_core.c:
 *
 *     in_ring   16384 * sizeof(float)      =  64 KB
 *     dry_ring   4096 * 2 * sizeof(int16)  =  16 KB
 *     acc        4096 * 2 * sizeof(float)  =  32 KB
 *     yin_ring   4096 * sizeof(float)      =  16 KB
 *     belt_t     (7 voices + params)        ~  a few KB
 *                                            ---------
 *                                            ~128 KB
 *
 * That fits internal SRAM, so unlike smack (a 16 MB ring) this port does NOT
 * need SDRAM. The pool is declared in belt_patch.cpp; see the note there about
 * why it lives in SRAM rather than DSY_SDRAM_BSS.
 *
 * This file stays plain C so it can be unit-tested natively.
 */
#ifndef PATCH_ALLOC_H
#define PATCH_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void   patch_alloc_init(void *pool, size_t bytes);
void  *patch_calloc(size_t nmemb, size_t size);
void   patch_free(void *ptr);

/* For the boot-time log: how much of the pool the engine actually took. */
size_t patch_alloc_used(void);
size_t patch_alloc_capacity(void);

/* Set when an allocation did not fit. If this is true after belt_create(),
 * the module is broken and must say so rather than run half-initialised. */
int    patch_alloc_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* PATCH_ALLOC_H */
