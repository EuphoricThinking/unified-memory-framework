/*
 * Copyright (C) 2022-2024 Intel Corporation
 *
 * Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
*/

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <umf/memory_pool.h>
#include <umf/memory_pool_ops.h>
#include <umf/memory_provider.h>
#include <umf/pools/pool_disjoint.h>

#include "base_alloc_global.h"
#include "utils_common.h"
#include "utils_concurrency.h"
#include "utils_log.h"
#include "utils_sanitizers.h"

#include "pool_disjoint_temp.h"

#ifdef __cplusplus
extern "C" {
#endif

// Temporary solution for disabling memory poisoning. This is needed because
// AddressSanitizer does not support memory poisoning for GPU allocations.
// More info: https://github.com/oneapi-src/unified-memory-framework/issues/634
#ifndef POISON_MEMORY
#define POISON_MEMORY 0
#endif

/*static */ void annotate_memory_inaccessible(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
#if (POISON_MEMORY != 0)
    utils_annotate_memory_inaccessible(ptr, size);
#endif
}

/*static*/ void annotate_memory_undefined(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
#if (POISON_MEMORY != 0)
    utils_annotate_memory_undefined(ptr, size);
#endif
}

size_t bucket_get_slab_min_size(const bucket_t bucket);
size_t bucket_get_slab_alloc_size(const bucket_t bucket);
size_t bucket_get_size(const bucket_t bucket);
umf_memory_provider_handle_t bucket_get_provider(const bucket_t bucket);

void slab_reg(slab_t *slab);
void slab_unreg(slab_t *slab);

slab_t *create_slab(bucket_t bucket) {
    // In case bucket size is not a multiple of SlabMinSize, we would have
    // some padding at the end of the slab.
    slab_t *slab = umf_ba_global_alloc(sizeof(slab_t));
    // TODO check res and errors here and everywhere
    // TODO use logger
    slab->num_allocated = 0;
    slab->first_free_chunk_idx = 0;
    slab->bucket = bucket;

    slab->iter =
        (slab_list_item_t *)umf_ba_global_alloc(sizeof(slab_list_item_t));
    slab->iter->val = slab;
    slab->iter->prev = slab->iter->next = NULL;

    slab->num_chunks =
        bucket_get_slab_min_size(bucket) / bucket_get_size(bucket);
    slab->chunks = umf_ba_global_alloc(sizeof(bool) * slab->num_chunks);
    memset(slab->chunks, 0, sizeof(bool) * slab->num_chunks);

    slab->slab_size = bucket_get_slab_alloc_size(bucket);
    umf_result_t res = umfMemoryProviderAlloc(
        bucket_get_provider(bucket), slab->slab_size, 0, &slab->mem_ptr);

    if (res == UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY) {
        destroy_slab(slab);
        return NULL;
    }

    annotate_memory_inaccessible(slab->mem_ptr, slab->slab_size);
    fprintf(stderr, "[DP create_slab] bucket: %p, slab_size: %zu\n", bucket,
            slab->slab_size);

    return slab;
}

void destroy_slab(slab_t *slab) {
    fprintf(stderr, "[DP destroy_slab] bucket: %p, slab_size: %zu\n",
            slab->bucket, slab->slab_size);

    umf_result_t res = umfMemoryProviderFree(bucket_get_provider(slab->bucket),
                                             slab->mem_ptr, slab->slab_size);
    assert(res == UMF_RESULT_SUCCESS);
    umf_ba_global_free(slab->chunks);
    umf_ba_global_free(slab->iter);
    umf_ba_global_free(slab);
}

size_t slab_get_num_allocated(const slab_t *slab) {
    return slab->num_allocated;
}

size_t slab_get_num_chunks(const slab_t *slab) { return slab->num_chunks; }

// Return the index of the first available chunk, SIZE_MAX otherwise
size_t slab_find_first_available_chunk_idx(const slab_t *slab) {
    // Use the first free chunk index as a hint for the search.
    bool *chunk = slab->chunks + sizeof(bool) * slab->first_free_chunk_idx;
    while (chunk != slab->chunks + sizeof(bool) * slab->num_chunks) {
        // false means not used
        if (*chunk == false) {
            size_t idx = (chunk - slab->chunks) / sizeof(bool);
            fprintf(stderr,
                    "[DP slab_find_first_available_chunk_idx] idx: %zu\n", idx);
            return idx;
        }
        chunk++;
    }

    fprintf(stderr, "[DP slab_find_first_available_chunk_idx] idx: SIZE_MAX\n");
    return SIZE_MAX;
}

void *slab_get_chunk(slab_t *slab) {
    // assert(slab->num_allocated != slab->num_chunks);

    const size_t chunk_idx = slab_find_first_available_chunk_idx(slab);
    // Free chunk must exist, otherwise we would have allocated another slab
    assert(chunk_idx != SIZE_MAX);

    void *free_chunk =
        (uint8_t *)slab->mem_ptr + chunk_idx * slab_get_chunk_size(slab);
    // mark as used
    slab->chunks[chunk_idx] = true;
    slab->num_allocated += 1;

    // Use the found index as the next hint
    slab->first_free_chunk_idx = chunk_idx;

    fprintf(stderr, "[DP slab_get_chunk] num_allocated: %zu\n",
            slab->num_allocated);

    return free_chunk;
}

void *slab_get_end(const slab_t *slab) {
    return (uint8_t *)slab->mem_ptr + bucket_get_slab_min_size(slab->bucket);
}

// TODO remove? why need getter/setter?
void *slab_get(const slab_t *slab) { return slab->mem_ptr; }
bucket_t slab_get_bucket(const slab_t *slab) { return slab->bucket; }
size_t slab_get_chunk_size(const slab_t *slab) {
    return bucket_get_size(slab->bucket);
}

void slab_free_chunk(slab_t *slab, void *ptr) {
    // This method should be called through bucket(since we might remove the
    // slab as a result), therefore all locks are done on that level.

    // Make sure that we're in the right slab
    assert(ptr >= slab_get(slab) && ptr < slab_get_end(slab));

    // Even if the pointer p was previously aligned, it's still inside the
    // corresponding chunk, so we get the correct index here.
    size_t chunk_idx =
        ((uint8_t *)ptr - (uint8_t *)slab->mem_ptr) / slab_get_chunk_size(slab);

    // Make sure that the chunk was allocated
    assert(slab->chunks[chunk_idx] && "double free detected");
    slab->chunks[chunk_idx] = false;
    slab->num_allocated -= 1;

    if (chunk_idx < slab->first_free_chunk_idx) {
        slab->first_free_chunk_idx = chunk_idx;
    }

    fprintf(stderr,
            "[DP slab_free_chunk] chunk_idx: %zu, num_allocated: %zu, "
            "first_free_chunk_idx: %zu\n",
            chunk_idx, slab->num_allocated, slab->first_free_chunk_idx);
}

bool slab_has_avail(const slab_t *slab) {
    return slab->num_allocated != slab->num_chunks;
}

#ifdef __cplusplus
}
#endif
