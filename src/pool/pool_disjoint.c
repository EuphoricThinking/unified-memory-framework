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
#include "uthash/utlist.h"
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

typedef struct umf_disjoint_pool_shared_limits_t {
    size_t max_size;
    size_t total_size; // requires atomic access
} umf_disjoint_pool_shared_limits_t;

umf_disjoint_pool_shared_limits_t *shared_limits_create(size_t max_size) {
    umf_disjoint_pool_shared_limits_t *ptr =
        umf_ba_global_alloc(sizeof(umf_disjoint_pool_shared_limits_t));
    ptr->max_size = max_size;
    ptr->total_size = 0;
    return ptr;
}

void shared_limits_destroy(umf_disjoint_pool_shared_limits_t *shared_limits) {
    umf_ba_global_free(shared_limits);
}

size_t bucket_get_size(bucket_t *bucket);

void slab_reg(slab_t *slab);
void slab_unreg(slab_t *slab);

slab_t *create_slab(bucket_t *bucket) {
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

    slab->num_chunks = bucket_slab_min_size(bucket) / bucket_get_size(bucket);
    slab->chunks = umf_ba_global_alloc(sizeof(bool) * slab->num_chunks);
    memset(slab->chunks, 0, sizeof(bool) * slab->num_chunks);

    slab->slab_size = bucket_slab_alloc_size(bucket);
    umf_result_t res = umfMemoryProviderAlloc(
        bucket_get_mem_handle(bucket), slab->slab_size, 0, &slab->mem_ptr);

    if (res == UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY) {
        destroy_slab(slab);
        return NULL;
    }

    annotate_memory_inaccessible(slab->mem_ptr, slab->slab_size);
    fprintf(stderr, "[DP create_slab] bucket: %p, slab_size: %zu\n",
            (void *)bucket, slab->slab_size);

    return slab;
}

void destroy_slab(slab_t *slab) {
    fprintf(stderr, "[DP destroy_slab] bucket: %p, slab_size: %zu\n",
            (void *)slab->bucket, slab->slab_size);

    umf_result_t res = umfMemoryProviderFree(
        bucket_get_mem_handle(slab->bucket), slab->mem_ptr, slab->slab_size);
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
    return (uint8_t *)slab->mem_ptr + bucket_slab_min_size(slab->bucket);
}

// TODO remove? why need getter/setter?
void *slab_get(const slab_t *slab) { return slab->mem_ptr; }
bucket_t *slab_get_bucket(slab_t *slab) { return slab->bucket; }
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

void slab_reg(slab_t *slab) {
    bucket_t *bucket = slab_get_bucket(slab);
    void *start_addr = (void *)ALIGN_DOWN((size_t)slab_get(slab),
                                          bucket_slab_min_size(bucket));
    void *end_addr = (uint8_t *)(start_addr) + bucket_slab_min_size(bucket);

    slab_reg_by_addr(start_addr, slab);
    slab_reg_by_addr(end_addr, slab);
}

void slab_unreg(slab_t *slab) {
    bucket_t *bucket = slab_get_bucket(slab);
    void *start_addr = (void *)ALIGN_DOWN((size_t)slab_get(slab),
                                          bucket_slab_min_size(bucket));
    void *end_addr = (uint8_t *)(start_addr) + bucket_slab_min_size(bucket);

    slab_unreg_by_addr(start_addr, slab);
    slab_unreg_by_addr(end_addr, slab);
}

bucket_t *create_bucket(size_t Sz, void *AllocCtx,
                        umf_disjoint_pool_shared_limits_t *shared_limits) {
    bucket_t *bucket = (bucket_t *)umf_ba_global_alloc(sizeof(bucket_t));

    bucket->Size = Sz;
    bucket->OwnAllocCtx = AllocCtx;
    bucket->AvailableSlabs = NULL;
    bucket->UnavailableSlabs = NULL;
    bucket->chunkedSlabsInPool = 0;
    bucket->allocPoolCount = 0;
    bucket->freeCount = 0;
    bucket->currSlabsInUse = 0;
    bucket->currSlabsInPool = 0;
    bucket->maxSlabsInPool = 0;
    bucket->allocCount = 0;
    bucket->maxSlabsInUse = 0;

    bucket->shared_limits = shared_limits;
    assert(shared_limits);

    utils_mutex_init(&bucket->bucket_lock);

    return bucket;
}

void destroy_bucket(bucket_t *bucket) {
    slab_list_item_t *it = NULL, *tmp = NULL;
    // TODO check eng
    // use extra tmp to store next iterator before the slab is destroyed
    LL_FOREACH_SAFE(bucket->AvailableSlabs, it, tmp) { destroy_slab(it->val); }
    LL_FOREACH_SAFE(bucket->UnavailableSlabs, it, tmp) {
        destroy_slab(it->val);
    }

    utils_mutex_destroy_not_free(&bucket->bucket_lock);

    umf_ba_global_free(bucket);
}

// The lock must be acquired before calling this method
void bucket_on_free_chunk(bucket_t *bucket, slab_t *slab, bool *ToPool) {
    *ToPool = true;

    // In case if the slab was previously full and now has 1 available
    // chunk, it should be moved to the list of available slabs
    if (slab_get_num_allocated(slab) == (slab_get_num_chunks(slab) - 1)) {
        slab_list_item_t *slab_it = slab->iter;
        assert(slab_it->val != NULL);
        DL_DELETE(bucket->UnavailableSlabs, slab_it);
        DL_PREPEND(bucket->AvailableSlabs, slab_it);
    }

    // Check if slab is empty, and pool it if we can.
    if (slab_get_num_allocated(slab) == 0) {
        // The slab is now empty.
        // If pool has capacity then put the slab in the pool.
        // The ToPool parameter indicates whether the Slab will be put in the
        // pool or freed.
        if (!bucket_can_pool(bucket, ToPool)) {
            // Note: since the slab is stored as unique_ptr, just remove it from
            // the list to destroy the object.
            slab_list_item_t *slab_it = slab->iter;
            assert(slab_it->val != NULL);
            slab_unreg(slab_it->val);
            DL_DELETE(bucket->AvailableSlabs, slab_it);
            destroy_slab(slab_it->val);
        }
    }
}

// Return the allocation size of this bucket.
size_t bucket_get_size(bucket_t *bucket) { return bucket->Size; }

void *bucket_get_alloc_ctx(bucket_t *bucket) { return bucket->OwnAllocCtx; }

void bucket_count_free(bucket_t *bucket) { ++bucket->freeCount; }

void bucket_free_chunk(bucket_t *bucket, void *ptr, slab_t *Slab,
                       bool *ToPool) {
    utils_mutex_lock(&bucket->bucket_lock);

    slab_free_chunk(Slab, ptr);
    bucket_on_free_chunk(bucket, Slab, ToPool);

    utils_mutex_unlock(&bucket->bucket_lock);
}

void bucket_count_alloc(bucket_t *bucket, bool FromPool) {
    ++bucket->allocCount;
    if (FromPool) {
        ++bucket->allocPoolCount;
    }
}

void *bucket_get_chunk(bucket_t *bucket, bool *FromPool) {
    utils_mutex_lock(&bucket->bucket_lock);

    slab_list_item_t *slab_it = bucket_get_avail_slab(bucket, FromPool);
    if (slab_it == NULL) {
        utils_mutex_unlock(&bucket->bucket_lock);
        return NULL;
    }

    void *free_chunk = slab_get_chunk(slab_it->val);

    // If the slab is full, move it to unavailable slabs and update its iterator
    if (!(slab_has_avail(slab_it->val))) {
        DL_DELETE(bucket->AvailableSlabs, slab_it);
        DL_PREPEND(bucket->UnavailableSlabs, slab_it);
    }

    utils_mutex_unlock(&bucket->bucket_lock);
    return free_chunk;
}

size_t bucket_chunk_cut_off(bucket_t *bucket) {
    return bucket_slab_min_size(bucket) / 2;
}

size_t bucket_slab_alloc_size(bucket_t *bucket) {
    // return max
    return (bucket_get_size(bucket) > bucket_slab_min_size(bucket))
               ? bucket_get_size(bucket)
               : bucket_slab_min_size(bucket);
}

size_t bucket_slab_min_size(bucket_t *bucket) {
    return bucket_get_params(bucket)->SlabMinSize;
}

slab_list_item_t *bucket_get_avail_full_slab(bucket_t *bucket,
                                             bool *from_pool) {
    // Return a slab that will be used for a single allocation.
    if (bucket->AvailableSlabs == NULL) {
        slab_t *slab = create_slab(bucket);
        if (slab == NULL) {
            //assert(0);
            return NULL;
        }

        slab_reg(slab);
        DL_PREPEND(bucket->AvailableSlabs, slab->iter);
        *from_pool = false;
        bucket_update_stats(bucket, 1, 0);
    } else {
        bucket_decrement_pool(bucket, from_pool);
    }

    return bucket->AvailableSlabs;
}

void *bucket_get_slab(bucket_t *bucket, bool *from_pool) {
    utils_mutex_lock(&bucket->bucket_lock);

    slab_list_item_t *slab_it = bucket_get_avail_full_slab(bucket, from_pool);
    if (slab_it == NULL) {
        utils_mutex_unlock(&bucket->bucket_lock);
        return NULL;
    }
    slab_t *slab = slab_it->val;
    void *ptr = slab_get(slab);

    DL_DELETE(bucket->AvailableSlabs, slab_it);
    DL_PREPEND(bucket->UnavailableSlabs, slab_it);

    utils_mutex_unlock(&bucket->bucket_lock);
    return ptr;
}

void bucket_free_slab(bucket_t *bucket, slab_t *slab, bool *to_pool) {
    utils_mutex_lock(&bucket->bucket_lock);

    slab_list_item_t *slab_it = slab->iter;
    assert(slab_it->val != NULL);
    if (bucket_can_pool(bucket, to_pool)) {
        DL_DELETE(bucket->UnavailableSlabs, slab_it);
        DL_PREPEND(bucket->AvailableSlabs, slab_it);
    } else {
        slab_unreg(slab_it->val);
        DL_DELETE(bucket->UnavailableSlabs, slab_it);
        destroy_slab(slab_it->val);
    }
    utils_mutex_unlock(&bucket->bucket_lock);
}

slab_list_item_t *bucket_get_avail_slab(bucket_t *bucket, bool *from_pool) {
    if (bucket->AvailableSlabs == NULL) {
        slab_t *slab = create_slab(bucket);
        if (slab == NULL) {
            // TODO log
            // TODO replace asserts
            return NULL;
        }

        slab_reg(slab);
        DL_PREPEND(bucket->AvailableSlabs, slab->iter);
        bucket_update_stats(bucket, 1, 0);
        *from_pool = false;
    } else {
        if (slab_get_num_allocated(bucket->AvailableSlabs->val) == 0) {
            // If this was an empty slab, it was in the pool.
            // Now it is no longer in the pool, so update count.
            --bucket->chunkedSlabsInPool;
            bucket_decrement_pool(bucket, from_pool);
        } else {
            // Allocation from existing slab is treated as from pool for statistics.
            *from_pool = true;
        }
    }

    return bucket->AvailableSlabs;
}

size_t bucket_capacity(bucket_t *bucket) {
    // For buckets used in chunked mode, just one slab in pool is sufficient.
    // For larger buckets, the capacity could be more and is adjustable.
    if (bucket_get_size(bucket) <= bucket_chunk_cut_off(bucket)) {
        return 1;
    } else {
        return bucket_get_params(bucket)->Capacity;
    }
}

size_t bucket_max_poolable_size(bucket_t *bucket) {
    return bucket_get_params(bucket)->MaxPoolableSize;
}

void bucket_update_stats(bucket_t *bucket, int in_use, int in_pool) {
    if (bucket_get_params(bucket)->PoolTrace == 0) {
        return;
    }

    bucket->currSlabsInUse += in_use;
    bucket->maxSlabsInUse =
        utils_max(bucket->currSlabsInUse, bucket->maxSlabsInUse);
    bucket->currSlabsInPool += in_pool;
    bucket->maxSlabsInPool =
        utils_max(bucket->currSlabsInPool, bucket->maxSlabsInPool);

    // Increment or decrement current pool sizes based on whether
    // slab was added to or removed from pool.
    bucket_get_params(bucket)->CurPoolSize +=
        in_pool * bucket_slab_alloc_size(bucket);
}

// If a slab was available in the pool then note that the current pooled
// size has reduced by the size of a slab in this bucket.
void bucket_decrement_pool(bucket_t *bucket, bool *from_pool) {
    *from_pool = true;
    bucket_update_stats(bucket, 1, -1);
    utils_fetch_and_add64(&bucket->shared_limits->total_size,
                          -(long long)bucket_slab_alloc_size(bucket));
}

bool bucket_can_pool(bucket_t *bucket, bool *to_pool) {
    size_t NewFreeSlabsInBucket;
    // Check if this bucket is used in chunked form or as full slabs.
    bool chunkedBucket =
        bucket_get_size(bucket) <= bucket_chunk_cut_off(bucket);
    if (chunkedBucket) {
        NewFreeSlabsInBucket = bucket->chunkedSlabsInPool + 1;
    } else {
        // TODO optimize
        size_t avail_num = 0;
        slab_list_item_t *it = NULL;
        DL_FOREACH(bucket->AvailableSlabs, it) { avail_num++; }
        NewFreeSlabsInBucket = avail_num + 1;
    }
    if (bucket_capacity(bucket) >= NewFreeSlabsInBucket) {
        size_t pool_size = bucket->shared_limits->total_size;
        while (true) {
            size_t new_pool_size = pool_size + bucket_slab_alloc_size(bucket);

            if (bucket->shared_limits->max_size < new_pool_size) {
                break;
            }

            if (utils_compare_exchange(&bucket->shared_limits->total_size,
                                       &pool_size, &new_pool_size)) {
                if (chunkedBucket) {
                    ++bucket->chunkedSlabsInPool;
                }

                bucket_update_stats(bucket, -1, 1);
                *to_pool = true;
                return true;
            }
        }
    }

    bucket_update_stats(bucket, -1, 0);
    *to_pool = false;
    return false;
}

#ifdef __cplusplus
}
#endif
