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

#include "critnib/critnib.h"
#include "uthash/utlist.h"

#include "base_alloc_global.h"
#include "provider_tracking.h"
#include "utils_common.h"
#include "utils_concurrency.h"
#include "utils_log.h"
#include "utils_math.h"
#include "utils_sanitizers.h"

#include "pool_disjoint_temp.h"

// TODO remove
#ifdef __cplusplus
extern "C" {
#endif

//static <- make static rename to TLS_last_allocation_error
__TLS umf_result_t TLS_last_allocation_error_dp;

// Allocations are a minimum of 4KB/64KB/2MB even when a smaller size is
// requested. The implementation distinguishes between allocations of size
// ChunkCutOff = (minimum-alloc-size / 2) and those that are larger.
// Allocation requests smaller than ChunkCutoff use chunks taken from a single
// coarse-grain allocation. Thus, for example, for a 64KB minimum allocation
// size, and 8-byte allocations, only 1 in ~8000 requests results in a new
// coarse-grain allocation. Freeing results only in a chunk of a larger
// allocation to be marked as available and no real return to the system. An
// allocation is returned to the system only when all chunks in the larger
// allocation are freed by the program. Allocations larger than ChunkCutOff use
// a separate coarse-grain allocation for each request. These are subject to
// "pooling". That is, when such an allocation is freed by the program it is
// retained in a pool. The pool is available for future allocations, which means
// there are fewer actual coarse-grain allocations/deallocations.

// The largest size which is allocated via the allocator.
// Allocations with size > CutOff bypass the pool and
// go directly to the provider.
static size_t CutOff = (size_t)1 << 31; // 2GB

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

umf_disjoint_pool_shared_limits_t *
umfDisjointPoolSharedLimitsCreate(size_t max_size) {
    umf_disjoint_pool_shared_limits_t *ptr =
        umf_ba_global_alloc(sizeof(umf_disjoint_pool_shared_limits_t));
    ptr->max_size = max_size;
    ptr->total_size = 0;
    return ptr;
}

void umfDisjointPoolSharedLimitsDestroy(
    umf_disjoint_pool_shared_limits_t *limits) {
    umf_ba_global_free(limits);
}

typedef struct disjoint_pool_t {
    AllocImpl *impl;
} disjoint_pool_t;

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

    // NOTE: originally slabs memory were allocated without alignment
    // with this registering a slab is simpler and doesn't require multimap
    umf_result_t res =
        umfMemoryProviderAlloc(bucket_get_mem_handle(bucket), slab->slab_size,
                               bucket_slab_min_size(bucket), &slab->mem_ptr);

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
    (void)res;

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
    // NOTE: changed vs original - slab is already aligned to bucket_slab_min_size
    // I also decr end_addr by 1
    void *start_addr = (void *)ALIGN_DOWN((size_t)slab_get(slab),
                                          bucket_slab_min_size(bucket));
    void *end_addr = (uint8_t *)(start_addr) + bucket_slab_min_size(bucket) - 1;

    fprintf(stderr, "[DP slab_reg] slab: %p, start: %p, end %p\n", (void *)slab,
            start_addr, end_addr);

    slab_reg_by_addr(start_addr, slab);
    slab_reg_by_addr(end_addr, slab);
}

void slab_unreg(slab_t *slab) {
    bucket_t *bucket = slab_get_bucket(slab);
    // NOTE: changed vs original - slab is already aligned to bucket_slab_min_size
    // I also decr end_addr by 1
    void *start_addr = (void *)ALIGN_DOWN((size_t)slab_get(slab),
                                          bucket_slab_min_size(bucket));
    void *end_addr = (uint8_t *)(start_addr) + bucket_slab_min_size(bucket) - 1;

    fprintf(stderr, "[DP slab_unreg] slab: %p, start: %p, end %p\n",
            (void *)slab, start_addr, end_addr);

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
        size_t pool_size = 0;
        utils_atomic_load_acquire(&bucket->shared_limits->total_size,
                                  &pool_size);
        while (true) {
            size_t new_pool_size = pool_size + bucket_slab_alloc_size(bucket);

            if (bucket->shared_limits->max_size < new_pool_size) {
                break;
            }

// TODO!!!
#ifdef _WIN32
            if (bucket->shared_limits->total_size != new_pool_size) {
                bucket->shared_limits->total_size = new_pool_size;
#else
            if (utils_compare_exchange(&bucket->shared_limits->total_size,
                                       &pool_size, &new_pool_size)) {
#endif
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

umf_disjoint_pool_params_t *bucket_get_params(bucket_t *bucket) {
    AllocImpl *t = (AllocImpl *)bucket->OwnAllocCtx;
    return AllocImpl_getParams(t);
}

umf_memory_provider_handle_t bucket_get_mem_handle(bucket_t *bucket) {
    AllocImpl *t = (AllocImpl *)bucket->OwnAllocCtx;
    return AllocImpl_getMemHandle(t);
}

critnib *bucket_get_known_slabs(bucket_t *bucket) {
    AllocImpl *t = (AllocImpl *)bucket->OwnAllocCtx;
    return AllocImpl_getKnownSlabs(t);
}

utils_mutex_t *bucket_get_known_slabs_map_lock(bucket_t *bucket) {
    AllocImpl *t = (AllocImpl *)bucket->OwnAllocCtx;
    return AllocImpl_getKnownSlabsMapLock(t);
}

void slab_reg_by_addr(void *addr, slab_t *slab) {
    bucket_t *bucket = slab_get_bucket(slab);
    utils_mutex_t *lock = bucket_get_known_slabs_map_lock(bucket);
    critnib *slabs = bucket_get_known_slabs(bucket);

    utils_mutex_lock(lock);

    // TODO multimap
    slab_t *t = (slab_t *)critnib_get(slabs, (uintptr_t)addr);
    assert(t == NULL);
    (void)t;

    fprintf(stderr, "[DP slab_reg_by_addr] addr: %p, slab: %p\n", addr,
            (void *)slab);
    critnib_insert(slabs, (uintptr_t)addr, slab, 0);

    // debug
    slab_t *s = (slab_t *)critnib_get(slabs, (uintptr_t)addr);
    assert(s != NULL);
    (void)s;

    utils_mutex_unlock(lock);
}

void slab_unreg_by_addr(void *addr, slab_t *slab) {
    bucket_t *bucket = slab_get_bucket(slab);
    utils_mutex_t *lock = bucket_get_known_slabs_map_lock(bucket);
    critnib *slabs = bucket_get_known_slabs(bucket);

    utils_mutex_lock(lock);

    // debug only
    // assume single-value per key
    slab_t *known_slab = (slab_t *)critnib_get(slabs, (uintptr_t)addr);
    assert(known_slab != NULL && "Slab is not found");
    assert(slab == known_slab);
    (void)known_slab;

    fprintf(stderr, "[DP slab_unreg_by_addr] addr: %p, slab: %p\n", addr,
            (void *)slab);
    critnib_remove(slabs, (uintptr_t)addr);

    utils_mutex_unlock(lock);
}

AllocImpl *create_AllocImpl(umf_memory_provider_handle_t hProvider,
                            umf_disjoint_pool_params_t *params) {

    AllocImpl *ai = (AllocImpl *)umf_ba_global_alloc(sizeof(AllocImpl));

    VALGRIND_DO_CREATE_MEMPOOL(ai, 0, 0);
    ai->MemHandle = hProvider;
    ai->params = *params;

    utils_mutex_init(&ai->known_slabs_map_lock);
    ai->known_slabs = critnib_new();

    // Generate buckets sized such as: 64, 96, 128, 192, ..., CutOff.
    // Powers of 2 and the value halfway between the powers of 2.
    size_t Size1 = ai->params.MinBucketSize;

    // MinBucketSize cannot be larger than CutOff.
    Size1 = utils_min(Size1, CutOff);

    // Buckets sized smaller than the bucket default size- 8 aren't needed.
    Size1 = utils_max(Size1, UMF_DISJOINT_POOL_MIN_BUCKET_DEFAULT_SIZE);

    // Calculate the exponent for MinBucketSize used for finding buckets.
    ai->MinBucketSizeExp = (size_t)log2Utils(Size1);
    ai->DefaultSharedLimits = umfDisjointPoolSharedLimitsCreate(SIZE_MAX);

    // count number of buckets, start from 1
    ai->buckets_num = 1;
    size_t Size2 = Size1 + Size1 / 2;
    size_t ts2 = Size2, ts1 = Size1;
    for (; Size2 < CutOff; Size1 *= 2, Size2 *= 2) {
        ai->buckets_num += 2;
    }
    ai->buckets =
        (bucket_t **)umf_ba_global_alloc(sizeof(bucket_t *) * ai->buckets_num);

    int i = 0;
    Size1 = ts1;
    Size2 = ts2;
    for (; Size2 < CutOff; Size1 *= 2, Size2 *= 2, i += 2) {
        ai->buckets[i] = create_bucket(Size1, ai, AllocImpl_getLimits(ai));
        ai->buckets[i + 1] = create_bucket(Size2, ai, AllocImpl_getLimits(ai));
    }
    ai->buckets[i] = create_bucket(CutOff, ai, AllocImpl_getLimits(ai));

    umf_result_t ret = umfMemoryProviderGetMinPageSize(
        hProvider, NULL, &ai->ProviderMinPageSize);
    if (ret != UMF_RESULT_SUCCESS) {
        ai->ProviderMinPageSize = 0;
    }

    return ai;
}

void destroy_AllocImpl(AllocImpl *ai) {
    // TODO
    // destroy DefaultSharedLimits

    for (size_t i = 0; i < ai->buckets_num; i++) {
        destroy_bucket(ai->buckets[i]);
    }

    VALGRIND_DO_DESTROY_MEMPOOL(ai);

    umfDisjointPoolSharedLimitsDestroy(ai->DefaultSharedLimits);
    critnib_delete(ai->known_slabs);

    utils_mutex_destroy_not_free(&ai->known_slabs_map_lock);

    umf_ba_global_free(ai);
}

umf_memory_provider_handle_t AllocImpl_getMemHandle(AllocImpl *ai) {
    return ai->MemHandle;
}

utils_mutex_t *AllocImpl_getKnownSlabsMapLock(AllocImpl *ai) {
    return &ai->known_slabs_map_lock;
}

critnib *AllocImpl_getKnownSlabs(AllocImpl *ai) { return ai->known_slabs; }

size_t AllocImpl_SlabMinSize(AllocImpl *ai) { return ai->params.SlabMinSize; }

umf_disjoint_pool_params_t *AllocImpl_getParams(AllocImpl *ai) {
    return &ai->params;
}

size_t AllocImpl_sizeToIdx(AllocImpl *ai, size_t size) {
    assert(size <= CutOff && "Unexpected size");
    assert(size > 0 && "Unexpected size");

    size_t MinBucketSize = (size_t)1 << ai->MinBucketSizeExp;
    if (size < MinBucketSize) {
        return 0;
    }

    // Get the position of the leftmost set bit.
    size_t position = getLeftmostSetBitPos(size);

    bool isPowerOf2 = 0 == (size & (size - 1));
    bool largerThanHalfwayBetweenPowersOf2 =
        !isPowerOf2 && (bool)((size - 1) & ((uint64_t)(1) << (position - 1)));
    size_t index = (position - ai->MinBucketSizeExp) * 2 + (int)(!isPowerOf2) +
                   (int)largerThanHalfwayBetweenPowersOf2;

    return index;
}

umf_disjoint_pool_shared_limits_t *AllocImpl_getLimits(AllocImpl *ai) {
    if (ai->params.SharedLimits) {
        return ai->params.SharedLimits;
    } else {
        return ai->DefaultSharedLimits;
    }
}

bucket_t *AllocImpl_findBucket(AllocImpl *ai, size_t Size) {
    size_t calculatedIdx = AllocImpl_sizeToIdx(ai, Size);
    bucket_t *bucket = ai->buckets[calculatedIdx];
    assert(bucket_get_size(bucket) >= Size);
    (void)bucket;

    if (calculatedIdx > 0) {
        bucket_t *bucket_prev = ai->buckets[calculatedIdx - 1];
        assert(bucket_get_size(bucket_prev) < Size);
        (void)bucket_prev;
    }

    return ai->buckets[calculatedIdx];
}

void AllocImpl_printStats(AllocImpl *ai, bool *TitlePrinted,
                          size_t *HighBucketSize, size_t *HighPeakSlabsInUse,
                          const char *MTName) {
    (void)TitlePrinted; // TODO
    (void)MTName;       // TODO

    *HighBucketSize = 0;
    *HighPeakSlabsInUse = 0;
    for (size_t i = 0; i < ai->buckets_num; i++) {
        // TODO
        //(*B).printStats(TitlePrinted, MTName);
        bucket_t *bucket = ai->buckets[i];
        *HighPeakSlabsInUse =
            utils_max(bucket->maxSlabsInUse, *HighPeakSlabsInUse);
        if (bucket->allocCount) {
            *HighBucketSize =
                utils_max(bucket_slab_alloc_size(bucket), *HighBucketSize);
        }
    }
}

static void *memoryProviderAlloc(umf_memory_provider_handle_t hProvider,
                                 size_t size, size_t alignment) {
    void *ptr;
    umf_result_t ret = umfMemoryProviderAlloc(hProvider, size, alignment, &ptr);
    if (ret != UMF_RESULT_SUCCESS) {
        TLS_last_allocation_error_dp = ret;
        return NULL;
    }
    annotate_memory_inaccessible(ptr, size);
    return ptr;
}

static umf_result_t memoryProviderFree(umf_memory_provider_handle_t hProvider,
                                       void *ptr) {
    size_t size = 0;

    if (ptr) {
        umf_alloc_info_t allocInfo = {NULL, 0, NULL};
        umf_result_t umf_result = umfMemoryTrackerGetAllocInfo(ptr, &allocInfo);
        if (umf_result == UMF_RESULT_SUCCESS) {
            size = allocInfo.baseSize;
        }
    }

    umf_result_t ret = umfMemoryProviderFree(hProvider, ptr, size);
    if (ret != UMF_RESULT_SUCCESS) {

        TLS_last_allocation_error_dp = ret;
        // throw MemoryProviderError{ret};
        return ret;
    }
    return UMF_RESULT_SUCCESS;
}

void *AllocImpl_allocate(AllocImpl *ai, size_t Size, bool *FromPool) {
    void *Ptr;

    if (Size == 0) {
        return NULL;
    }

    *FromPool = false;
    if (Size > AllocImpl_getParams(ai)->MaxPoolableSize) {
        Ptr = memoryProviderAlloc(AllocImpl_getMemHandle(ai), Size, 0);

        if (Ptr == NULL) {
            // TODO get code from func
            TLS_last_allocation_error_dp = UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
            return NULL;
        }

        annotate_memory_undefined(Ptr, Size);
        return Ptr;
    }

    bucket_t *bucket = AllocImpl_findBucket(ai, Size);

    if (Size > bucket_chunk_cut_off(bucket)) {
        Ptr = bucket_get_slab(bucket, FromPool);
    } else {
        Ptr = bucket_get_chunk(bucket, FromPool);
    }

    if (Ptr == NULL) {
        // TODO get code from func
        TLS_last_allocation_error_dp = UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    if (AllocImpl_getParams(ai)->PoolTrace > 1) {
        bucket_count_alloc(bucket, FromPool);
    }

    VALGRIND_DO_MEMPOOL_ALLOC(ai, Ptr, Size);
    annotate_memory_undefined(Ptr, bucket_get_size(bucket));

    return Ptr;
}

void *AllocImpl_allocate_align(AllocImpl *ai, size_t Size, size_t Alignment,
                               bool *FromPool) {
    void *Ptr;

    if (Size == 0) {
        return NULL;
    }

    if (Alignment <= 1) {
        return AllocImpl_allocate(ai, Size, FromPool);
    }

    size_t AlignedSize;
    if (Alignment <= ai->ProviderMinPageSize) {
        // This allocation will be served from a Bucket which size is multiple
        // of Alignment and Slab address is aligned to ProviderMinPageSize
        // so the address will be properly aligned.
        AlignedSize = (Size > 1) ? ALIGN_UP(Size, Alignment) : Alignment;
    } else {
        // Slabs are only aligned to ProviderMinPageSize, we need to compensate
        // for that in case the allocation is within pooling limit.
        // TODO: consider creating properly-aligned Slabs on demand
        AlignedSize = Size + Alignment - 1;
    }

    // Check if requested allocation size is within pooling limit.
    // If not, just request aligned pointer from the system.
    *FromPool = false;
    if (AlignedSize > AllocImpl_getParams(ai)->MaxPoolableSize) {
        Ptr = memoryProviderAlloc(AllocImpl_getMemHandle(ai), Size, Alignment);
        assert(Ptr);
        annotate_memory_undefined(Ptr, Size);
        return Ptr;
    }

    bucket_t *bucket = AllocImpl_findBucket(ai, AlignedSize);

    if (AlignedSize > bucket_chunk_cut_off(bucket)) {
        Ptr = bucket_get_slab(bucket, FromPool);
    } else {
        Ptr = bucket_get_chunk(bucket, FromPool);
    }

    assert(Ptr);
    if (AllocImpl_getParams(ai)->PoolTrace > 1) {
        bucket_count_alloc(bucket, FromPool);
    }

    VALGRIND_DO_MEMPOOL_ALLOC(ai, ALIGN_UP((size_t)Ptr, Alignment), Size);
    annotate_memory_undefined((void *)ALIGN_UP((size_t)Ptr, Alignment), Size);
    return (void *)ALIGN_UP((size_t)Ptr, Alignment);
}

umf_result_t AllocImpl_deallocate(AllocImpl *ai, void *Ptr, bool *ToPool) {
    if (Ptr == NULL) {
        return UMF_RESULT_SUCCESS;
    }

    void *SlabPtr = (void *)ALIGN_DOWN((size_t)Ptr, AllocImpl_SlabMinSize(ai));

    // Lock the map on read
    utils_mutex_lock(AllocImpl_getKnownSlabsMapLock(ai));

    *ToPool = false;

    slab_t *slab = (slab_t *)critnib_get(ai->known_slabs, (uintptr_t)SlabPtr);
    //auto Slabs = getKnownSlabs().equal_range(SlabPtr);
    if (slab == NULL) {
        utils_mutex_unlock(AllocImpl_getKnownSlabsMapLock(ai));
        umf_result_t ret = memoryProviderFree(AllocImpl_getMemHandle(ai), Ptr);
        return ret;
    }

    // TODO - no multimap
    // for (auto It = Slabs.first; It != Slabs.second; ++It) {

    // The slab object won't be deleted until it's removed from the map which is
    // protected by the lock, so it's safe to access it here.
    if (Ptr >= slab_get(slab) && Ptr < slab_get_end(slab)) {
        // Unlock the map before freeing the chunk, it may be locked on write
        // there
        utils_mutex_unlock(AllocImpl_getKnownSlabsMapLock(ai));
        bucket_t *bucket = slab_get_bucket(slab);

        if (AllocImpl_getParams(ai)->PoolTrace > 1) {
            bucket_count_free(bucket);
        }

        VALGRIND_DO_MEMPOOL_FREE(ai, Ptr);
        annotate_memory_inaccessible(Ptr, bucket_get_size(bucket));
        if (bucket_get_size(bucket) <= bucket_chunk_cut_off(bucket)) {
            bucket_free_chunk(bucket, Ptr, slab, ToPool);
        } else {
            bucket_free_slab(bucket, slab, ToPool);
        }

        return UMF_RESULT_SUCCESS;
    }
    //} // for multimap

    utils_mutex_unlock(AllocImpl_getKnownSlabsMapLock(ai));
    // There is a rare case when we have a pointer from system allocation next
    // to some slab with an entry in the map. So we find a slab
    // but the range checks fail.
    memoryProviderFree(AllocImpl_getMemHandle(ai), Ptr);
    return UMF_RESULT_SUCCESS;
}

/*
// TODO?
std::ostream &operator<<(std::ostream &Os, slab_t &Slab) {
    Os << "Slab<" << slab_get(&Slab) << ", " << slab_get_end(&Slab) << ", "
       << slab_get_bucket(&Slab)->getSize() << ">";
    return Os;
}
*/

/*
// TODO move
void Bucket::printStats(bool &TitlePrinted, const std::string &Label) {
    if (allocCount) {
        if (!TitlePrinted) {
            std::cout << Label << " memory statistics\n";
            std::cout << std::setw(14) << "Bucket Size" << std::setw(12)
                      << "Allocs" << std::setw(12) << "Frees" << std::setw(18)
                      << "Allocs from Pool" << std::setw(20)
                      << "Peak Slabs in Use" << std::setw(21)
                      << "Peak Slabs in Pool" << std::endl;
            TitlePrinted = true;
        }
        std::cout << std::setw(14) << getSize() << std::setw(12) << allocCount
                  << std::setw(12) << freeCount << std::setw(18)
                  << allocPoolCount << std::setw(20) << maxSlabsInUse
                  << std::setw(21) << maxSlabsInPool << std::endl;
    }
}
*/

umf_result_t disjoint_pool_initialize(umf_memory_provider_handle_t provider,
                                      void *params, void **ppPool) {
    if (!provider) {
        return UMF_RESULT_ERROR_INVALID_ARGUMENT;
    }

    disjoint_pool_t *disjoint_pool =
        (disjoint_pool_t *)umf_ba_global_alloc(sizeof(struct disjoint_pool_t));
    if (!disjoint_pool) {
        return UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
    }

    umf_disjoint_pool_params_t *dp_params =
        (umf_disjoint_pool_params_t *)params;

    // MinBucketSize parameter must be a power of 2 for bucket sizes
    // to generate correctly.
    if (!dp_params->MinBucketSize ||
        !((dp_params->MinBucketSize & (dp_params->MinBucketSize - 1)) == 0)) {
        return UMF_RESULT_ERROR_INVALID_ARGUMENT;
    }

    disjoint_pool->impl = create_AllocImpl(provider, dp_params);
    *ppPool = (void *)disjoint_pool;

    return UMF_RESULT_SUCCESS;
}

void *disjoint_pool_malloc(void *pool, size_t size) {
    // For full-slab allocations indicates
    // whether slab is from Pool.

    disjoint_pool_t *hPool = (disjoint_pool_t *)pool;

    bool FromPool;
    void *Ptr = AllocImpl_allocate(hPool->impl, size, &FromPool);

    if (AllocImpl_getParams(hPool->impl)->PoolTrace > 2) {
        const char *MT = AllocImpl_getParams(hPool->impl)->Name;
        (void)MT;
        //std::cout << "Allocated " << std::setw(8) << size << " " << MT
        //          << " bytes from " << (FromPool ? "Pool" : "Provider") << " ->"
        //          << Ptr << std::endl;
    }
    return Ptr;
}

void *disjoint_pool_calloc(void *pool, size_t num, size_t size) {
    (void)pool;
    (void)num;
    (void)size;

    // Not supported
    TLS_last_allocation_error_dp = UMF_RESULT_ERROR_NOT_SUPPORTED;
    return NULL;
}

void *disjoint_pool_realloc(void *pool, void *ptr, size_t size) {
    (void)pool;
    (void)ptr;
    (void)size;

    // Not supported
    TLS_last_allocation_error_dp = UMF_RESULT_ERROR_NOT_SUPPORTED;
    return NULL;
}

void *disjoint_pool_aligned_malloc(void *pool, size_t size, size_t alignment) {
    disjoint_pool_t *hPool = (disjoint_pool_t *)pool;

    bool FromPool;
    void *Ptr =
        AllocImpl_allocate_align(hPool->impl, size, alignment, &FromPool);

    if (AllocImpl_getParams(hPool->impl)->PoolTrace > 2) {
        const char *MT = AllocImpl_getParams(hPool->impl)->Name;
        (void)MT;
        //std::cout << "Allocated " << std::setw(8) << size << " " << MT
        //          << " bytes aligned at " << alignment << " from "
        //          << (FromPool ? "Pool" : "Provider") << " ->" << Ptr
        //          << std::endl;
    }

    return Ptr;
}

size_t disjoint_pool_malloc_usable_size(void *pool, void *ptr) {
    (void)pool;
    (void)ptr;

    // Not supported
    return 0;
}

umf_result_t disjoint_pool_free(void *pool, void *ptr) {
    disjoint_pool_t *hPool = (disjoint_pool_t *)pool;

    bool ToPool;
    umf_result_t ret = AllocImpl_deallocate(hPool->impl, ptr, &ToPool);
    /*
    if (ret == UMF_RESULT_SUCCESS) {

        if (impl->getParams().PoolTrace > 2) {
            auto MT = impl->getParams().Name;
            std::cout << "Freed " << MT << " " << ptr << " to "
                      << (ToPool ? "Pool" : "Provider")
                      << ", Current total pool size "
                      << impl->getLimits()->TotalSize.load()
                      << ", Current pool size for " << MT << " "
                      << impl->getParams().CurPoolSize << "\n";
        }
    }*/
    return ret;
}

umf_result_t disjoint_pool_get_last_allocation_error(void *pool) {
    (void)pool;

    return TLS_last_allocation_error_dp;
}

// Define destructor for use with unique_ptr
void disjoint_pool_finalize(void *pool) {

    disjoint_pool_t *hPool = (disjoint_pool_t *)pool;
    destroy_AllocImpl(hPool->impl);

    /*
    if (impl->getParams().PoolTrace > 1) {
        bool TitlePrinted = false;
        size_t HighBucketSize;
        size_t HighPeakSlabsInUse;
        auto name = impl->getParams().Name;
        //try { // cannot throw in destructor
        impl->printStats(TitlePrinted, HighBucketSize, HighPeakSlabsInUse,
                         name);
        if (TitlePrinted) {
            std::cout << "Current Pool Size "
                      << impl->getLimits()->TotalSize.load() << std::endl;
            std::cout << "Suggested Setting=;"
                      << std::string(1, (char)tolower(name[0]))
                      << std::string(name + 1) << ":" << HighBucketSize << ","
                      << HighPeakSlabsInUse << ",64K" << std::endl;
        }
        //} catch (...) { // ignore exceptions
        // }
    }
    */
}

static umf_memory_pool_ops_t UMF_DISJOINT_POOL_OPS = {
    .version = UMF_VERSION_CURRENT,
    .initialize = disjoint_pool_initialize,
    .finalize = disjoint_pool_finalize,
    .malloc = disjoint_pool_malloc,
    .calloc = disjoint_pool_calloc,
    .realloc = disjoint_pool_realloc,
    .aligned_malloc = disjoint_pool_aligned_malloc,
    .malloc_usable_size = disjoint_pool_malloc_usable_size,
    .free = disjoint_pool_free,
    .get_last_allocation_error = disjoint_pool_get_last_allocation_error,
};

umf_memory_pool_ops_t *umfDisjointPoolOps(void) {
    return &UMF_DISJOINT_POOL_OPS;
}

// TODO remove
#ifdef __cplusplus
}
#endif
