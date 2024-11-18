/*
 * Copyright (C) 2022-2024 Intel Corporation
 *
 * Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
*/

#include "pool_disjoint_internal.h"

static __TLS umf_result_t TLS_last_allocation_error;

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

slab_t *create_slab(bucket_t *bucket) {
    assert(bucket);

    slab_t *slab = umf_ba_global_alloc(sizeof(slab_t));
    if (slab == NULL) {
        LOG_ERR("allocation of new slab failed!");
        return NULL;
    }

    slab->num_allocated = 0;
    slab->first_free_chunk_idx = 0;
    slab->bucket = bucket;

    slab->iter =
        (slab_list_item_t *)umf_ba_global_alloc(sizeof(slab_list_item_t));
    if (slab->iter == NULL) {
        LOG_ERR("allocation of new slab iter failed!");
        umf_ba_global_free(slab);
        return NULL;
    }
    slab->iter->val = slab;
    slab->iter->prev = slab->iter->next = NULL;

    slab->num_chunks = bucket_slab_min_size(bucket) / bucket->size;
    slab->chunks = umf_ba_global_alloc(sizeof(bool) * slab->num_chunks);
    if (slab->chunks == NULL) {
        LOG_ERR("allocation of slab chunks failed!");
        umf_ba_global_free(slab->iter);
        umf_ba_global_free(slab);
        return NULL;
    }
    memset(slab->chunks, 0, sizeof(bool) * slab->num_chunks);

    // in case bucket size is not a multiple of slab_min_size, we would have
    // some padding at the end of the slab
    slab->slab_size = bucket_slab_alloc_size(bucket);

    // NOTE: originally slabs memory were allocated without alignment
    // with this registering a slab is simpler and doesn't require multimap
    umf_memory_provider_handle_t provider = bucket->pool->provider;
    umf_result_t res =
        umfMemoryProviderAlloc(provider, slab->slab_size,
                               bucket_slab_min_size(bucket), &slab->mem_ptr);
    if (res == UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY) {
        LOG_ERR("allocation of slab data failed!");
        destroy_slab(slab);
        return NULL;
    }

    // raw allocation is not available for user so mark it as inaccessible
    annotate_memory_inaccessible(slab->mem_ptr, slab->slab_size);

    LOG_DEBUG("bucket: %p, slab_size: %zu\n", (void *)bucket, slab->slab_size);
    return slab;
}

void destroy_slab(slab_t *slab) {
    LOG_DEBUG("bucket: %p, slab_size: %zu\n", (void *)slab->bucket,
              slab->slab_size);

    umf_memory_provider_handle_t provider = slab->bucket->pool->provider;
    umf_result_t res =
        umfMemoryProviderFree(provider, slab->mem_ptr, slab->slab_size);
    if (res != UMF_RESULT_SUCCESS) {
        LOG_ERR("deallocation of slab data failed!");
    }

    umf_ba_global_free(slab->chunks);
    umf_ba_global_free(slab->iter);
    umf_ba_global_free(slab);
}

// return the index of the first available chunk, SIZE_MAX otherwise
size_t slab_find_first_available_chunk_idx(const slab_t *slab) {
    // use the first free chunk index as a hint for the search
    bool *chunk = slab->chunks + sizeof(bool) * slab->first_free_chunk_idx;
    while (chunk != slab->chunks + sizeof(bool) * slab->num_chunks) {
        // false means not used
        if (*chunk == false) {
            size_t idx = (chunk - slab->chunks) / sizeof(bool);
            LOG_DEBUG("idx: %zu\n", idx);
            return idx;
        }
        chunk++;
    }

    LOG_DEBUG("idx: SIZE_MAX\n");
    return SIZE_MAX;
}

void *slab_get_chunk(slab_t *slab) {
    // assert(slab->num_allocated != slab->num_chunks);

    // free chunk must exist, otherwise we would have allocated another slab
    const size_t chunk_idx = slab_find_first_available_chunk_idx(slab);
    assert(chunk_idx != SIZE_MAX);

    void *free_chunk =
        (uint8_t *)slab->mem_ptr + chunk_idx * slab->bucket->size;

    // mark chunk as used
    slab->chunks[chunk_idx] = true;
    slab->num_allocated += 1;

    // use the found index as the next hint
    slab->first_free_chunk_idx = chunk_idx + 1;

    return free_chunk;
}

void *slab_get(const slab_t *slab) { return slab->mem_ptr; }
void *slab_get_end(const slab_t *slab) {
    return (uint8_t *)slab->mem_ptr + bucket_slab_min_size(slab->bucket);
}

void slab_free_chunk(slab_t *slab, void *ptr) {
    // This method should be called through bucket(since we might remove the
    // slab as a result), therefore all locks are done on that level.

    // Make sure that we're in the right slab
    assert(ptr >= slab_get(slab) && ptr < slab_get_end(slab));

    // Even if the pointer p was previously aligned, it's still inside the
    // corresponding chunk, so we get the correct index here.
    size_t chunk_idx =
        ((uint8_t *)ptr - (uint8_t *)slab->mem_ptr) / slab->bucket->size;

    // Make sure that the chunk was allocated
    assert(slab->chunks[chunk_idx] && "double free detected");
    slab->chunks[chunk_idx] = false;
    slab->num_allocated -= 1;

    if (chunk_idx < slab->first_free_chunk_idx) {
        slab->first_free_chunk_idx = chunk_idx;
    }

    LOG_DEBUG("chunk_idx: %zu, num_allocated: %zu, "
              "first_free_chunk_idx: %zu\n",
              chunk_idx, slab->num_allocated, slab->first_free_chunk_idx);
}

bool slab_has_avail(const slab_t *slab) {
    return slab->num_allocated != slab->num_chunks;
}

void slab_reg(slab_t *slab) {
    bucket_t *bucket = slab->bucket;

    // NOTE: changed vs original DisjointPool implementation - currently slab
    // is already aligned to bucket_slab_min_size. Additionally the end addr
    // points to the last byte of slab data
    void *start_addr = (void *)ALIGN_DOWN((size_t)slab_get(slab),
                                          bucket_slab_min_size(bucket));
    void *end_addr = (uint8_t *)(start_addr) + bucket_slab_min_size(bucket) - 1;

    LOG_DEBUG("slab: %p, start: %p, end %p\n", (void *)slab, start_addr,
              end_addr);

    slab_reg_by_addr(start_addr, slab);
    slab_reg_by_addr(end_addr, slab);
}

void slab_unreg(slab_t *slab) {
    bucket_t *bucket = slab->bucket;

    // NOTE: changed vs original DisjointPool implementation - currently slab
    // is already aligned to bucket_slab_min_size. Additionally the end addr
    // points to the last byte of slab data
    void *start_addr = (void *)ALIGN_DOWN((size_t)slab_get(slab),
                                          bucket_slab_min_size(bucket));
    void *end_addr = (uint8_t *)(start_addr) + bucket_slab_min_size(bucket) - 1;

    LOG_DEBUG("slab: %p, start: %p, end %p\n", (void *)slab, start_addr,
              end_addr);

    slab_unreg_by_addr(start_addr, slab);
    slab_unreg_by_addr(end_addr, slab);
}

bucket_t *create_bucket(size_t sz, disjoint_pool_t *pool,
                        umf_disjoint_pool_shared_limits_t *shared_limits) {

    bucket_t *bucket = (bucket_t *)umf_ba_global_alloc(sizeof(bucket_t));
    if (bucket == NULL) {
        LOG_ERR("allocation of new bucket failed!");
        return NULL;
    }

    bucket->size = sz;
    bucket->pool = pool;
    bucket->available_slabs = NULL;
    bucket->unavailable_slabs = NULL;
    bucket->chunked_slabs_in_pool = 0;
    bucket->alloc_pool_count = 0;
    bucket->free_count = 0;
    bucket->curr_slabs_in_use = 0;
    bucket->curr_slabs_in_pool = 0;
    bucket->max_slabs_in_pool = 0;
    bucket->alloc_count = 0;
    bucket->max_slabs_in_use = 0;
    bucket->shared_limits = shared_limits;

    utils_mutex_init(&bucket->bucket_lock);

    return bucket;
}

void destroy_bucket(bucket_t *bucket) {
    // use an extra tmp to store the next iterator before destroying the slab
    slab_list_item_t *it = NULL, *tmp = NULL;
    LL_FOREACH_SAFE(bucket->available_slabs, it, tmp) { destroy_slab(it->val); }
    LL_FOREACH_SAFE(bucket->unavailable_slabs, it, tmp) {
        destroy_slab(it->val);
    }

    utils_mutex_destroy_not_free(&bucket->bucket_lock);
    umf_ba_global_free(bucket);
}

void bucket_free_chunk(bucket_t *bucket, void *ptr, slab_t *slab,
                       bool *to_pool) {
    utils_mutex_lock(&bucket->bucket_lock);

    slab_free_chunk(slab, ptr);

    // in case if the slab was previously full and now has single available
    // chunk, it should be moved to the list of available slabs
    if (slab->num_allocated == (slab->num_chunks - 1)) {
        slab_list_item_t *slab_it = slab->iter;
        assert(slab_it->val != NULL);
        DL_DELETE(bucket->unavailable_slabs, slab_it);
        DL_PREPEND(bucket->available_slabs, slab_it);
    }

    // check if slab is empty, and pool it if we can
    *to_pool = true;
    if (slab->num_allocated == 0) {
        // The slab is now empty.
        // If pool has capacity then put the slab in the pool.
        // The to_pool parameter indicates whether the slab will be put in the
        // pool or freed.
        if (!bucket_can_pool(bucket, to_pool)) {
            // remove slab
            slab_list_item_t *slab_it = slab->iter;
            assert(slab_it->val != NULL);
            slab_unreg(slab_it->val);
            DL_DELETE(bucket->available_slabs, slab_it);
            destroy_slab(slab_it->val);
        }
    }

    utils_mutex_unlock(&bucket->bucket_lock);
}

void bucket_count_alloc(bucket_t *bucket, bool from_pool) {
    ++bucket->alloc_count;
    if (from_pool) {
        ++bucket->alloc_pool_count;
    }
}

void *bucket_get_chunk(bucket_t *bucket, bool *from_pool) {
    utils_mutex_lock(&bucket->bucket_lock);

    slab_list_item_t *slab_it = bucket_get_avail_slab(bucket, from_pool);
    if (slab_it == NULL) {
        utils_mutex_unlock(&bucket->bucket_lock);
        return NULL;
    }

    void *free_chunk = slab_get_chunk(slab_it->val);

    // if the slab is full, move it to unavailable slabs and update its iterator
    if (!(slab_has_avail(slab_it->val))) {
        DL_DELETE(bucket->available_slabs, slab_it);
        slab_it->prev = NULL;
        DL_PREPEND(bucket->unavailable_slabs, slab_it);
    }

    utils_mutex_unlock(&bucket->bucket_lock);
    return free_chunk;
}

size_t bucket_chunk_cut_off(bucket_t *bucket) {
    return bucket_slab_min_size(bucket) / 2;
}

size_t bucket_slab_alloc_size(bucket_t *bucket) {
    return utils_max(bucket->size, bucket_slab_min_size(bucket));
}

size_t bucket_slab_min_size(bucket_t *bucket) {
    return bucket->pool->params.SlabMinSize;
}

slab_list_item_t *bucket_get_avail_full_slab(bucket_t *bucket,
                                             bool *from_pool) {
    // return a slab that will be used for a single allocation
    if (bucket->available_slabs == NULL) {
        slab_t *slab = create_slab(bucket);
        if (slab == NULL) {
            LOG_ERR("create_slab failed!")
            return NULL;
        }

        slab_reg(slab);
        DL_PREPEND(bucket->available_slabs, slab->iter);
        *from_pool = false;
        bucket_update_stats(bucket, 1, 0);
    } else {
        bucket_decrement_pool(bucket, from_pool);
    }

    return bucket->available_slabs;
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

    DL_DELETE(bucket->available_slabs, slab_it);
    slab_it->prev = NULL;
    DL_PREPEND(bucket->unavailable_slabs, slab_it);

    utils_mutex_unlock(&bucket->bucket_lock);
    return ptr;
}

void bucket_free_slab(bucket_t *bucket, slab_t *slab, bool *to_pool) {
    utils_mutex_lock(&bucket->bucket_lock);

    slab_list_item_t *slab_it = slab->iter;
    assert(slab_it->val != NULL);
    if (bucket_can_pool(bucket, to_pool)) {
        DL_DELETE(bucket->unavailable_slabs, slab_it);
        slab_it->prev = NULL;
        DL_PREPEND(bucket->available_slabs, slab_it);
    } else {
        slab_unreg(slab_it->val);
        DL_DELETE(bucket->unavailable_slabs, slab_it);
        destroy_slab(slab_it->val);
    }
    utils_mutex_unlock(&bucket->bucket_lock);
}

slab_list_item_t *bucket_get_avail_slab(bucket_t *bucket, bool *from_pool) {
    if (bucket->available_slabs == NULL) {
        slab_t *slab = create_slab(bucket);
        if (slab == NULL) {
            LOG_ERR("create_slab failed!")
            return NULL;
        }

        slab_reg(slab);
        DL_PREPEND(bucket->available_slabs, slab->iter);
        bucket_update_stats(bucket, 1, 0);
        *from_pool = false;
    } else {
        slab_t *slab = bucket->available_slabs->val;
        if (slab->num_allocated == 0) {
            // If this was an empty slab, it was in the pool.
            // Now it is no longer in the pool, so update count.
            --bucket->chunked_slabs_in_pool;
            bucket_decrement_pool(bucket, from_pool);
        } else {
            // Allocation from existing slab is treated as from pool for statistics.
            *from_pool = true;
        }
    }

    return bucket->available_slabs;
}

size_t bucket_capacity(bucket_t *bucket) {
    // For buckets used in chunked mode, just one slab in pool is sufficient.
    // For larger buckets, the capacity could be more and is adjustable.
    if (bucket->size <= bucket_chunk_cut_off(bucket)) {
        return 1;
    } else {
        return bucket->pool->params.Capacity;
    }
}

void bucket_update_stats(bucket_t *bucket, int in_use, int in_pool) {
    if (bucket->pool->params.PoolTrace == 0) {
        return;
    }

    bucket->curr_slabs_in_use += in_use;
    bucket->max_slabs_in_use =
        utils_max(bucket->curr_slabs_in_use, bucket->max_slabs_in_use);

    bucket->curr_slabs_in_pool += in_pool;
    bucket->max_slabs_in_pool =
        utils_max(bucket->curr_slabs_in_pool, bucket->max_slabs_in_pool);

    // Increment or decrement current pool sizes based on whether
    // slab was added to or removed from pool.
    bucket->pool->params.CurPoolSize +=
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
    size_t new_free_slabs_in_bucket;

    // check if this bucket is used in chunked form or as full slabs
    bool chunked_bucket = bucket->size <= bucket_chunk_cut_off(bucket);
    if (chunked_bucket) {
        new_free_slabs_in_bucket = bucket->chunked_slabs_in_pool + 1;
    } else {
        // TODO optimize
        size_t avail_num = 0;
        slab_list_item_t *it = NULL;
        DL_FOREACH(bucket->available_slabs, it) { avail_num++; }
        new_free_slabs_in_bucket = avail_num + 1;
    }

    if (bucket_capacity(bucket) >= new_free_slabs_in_bucket) {
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
                if (chunked_bucket) {
                    ++bucket->chunked_slabs_in_pool;
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

utils_mutex_t *bucket_get_known_slabs_map_lock(bucket_t *bucket) {
    return &bucket->pool->known_slabs_map_lock;
}

void slab_reg_by_addr(void *addr, slab_t *slab) {
    bucket_t *bucket = slab->bucket;
    disjoint_pool_t *pool = bucket->pool;
    utils_mutex_t *lock = &pool->known_slabs_map_lock;
    critnib *slabs = pool->known_slabs;

    utils_mutex_lock(lock);

    // NODE: in the original Disjoint Pool implementation slabs was defined as
    // a multimap
    LOG_DEBUG("addr: %p, slab: %p\n", addr, (void *)slab);
    int ret = critnib_insert(slabs, (uintptr_t)addr, slab, 0);
    if (ret == ENOMEM) {
        LOG_ERR("register failed because of out of memory!");
    } else if (ret == EEXIST) {
        LOG_ERR("register failed because the address is already registered!");
    }

    utils_mutex_unlock(lock);
}

void slab_unreg_by_addr(void *addr, slab_t *slab) {
    bucket_t *bucket = slab->bucket;
    disjoint_pool_t *pool = bucket->pool;
    utils_mutex_t *lock = &pool->known_slabs_map_lock;
    critnib *slabs = pool->known_slabs;

    utils_mutex_lock(lock);

#ifndef NDEBUG
    // debug only
    // assume single-value per key
    slab_t *known_slab = (slab_t *)critnib_get(slabs, (uintptr_t)addr);
    assert(known_slab != NULL && "Slab is not found");
    assert(slab == known_slab);
    (void)known_slab;
#endif

    LOG_DEBUG("addr: %p, slab: %p\n", addr, (void *)slab);
    critnib_remove(slabs, (uintptr_t)addr);

    utils_mutex_unlock(lock);
}

//TODO add static
static size_t size_to_idx(disjoint_pool_t *pool, size_t size) {
    assert(size <= CutOff && "Unexpected size");
    assert(size > 0 && "Unexpected size");

    size_t min_bucket_size = (size_t)1 << pool->min_bucket_size_exp;
    if (size < min_bucket_size) {
        return 0;
    }

    // get the position of the leftmost set bit
    size_t position = getLeftmostSetBitPos(size);

    bool is_power_of_2 = 0 == (size & (size - 1));
    bool larger_than_halfway_between_powers_of_2 =
        !is_power_of_2 &&
        (bool)((size - 1) & ((uint64_t)(1) << (position - 1)));
    size_t index = (position - pool->min_bucket_size_exp) * 2 +
                   (int)(!is_power_of_2) +
                   (int)larger_than_halfway_between_powers_of_2;

    return index;
}

umf_disjoint_pool_shared_limits_t *
disjoint_pool_get_limits(disjoint_pool_t *pool) {
    if (pool->params.SharedLimits) {
        return pool->params.SharedLimits;
    } else {
        return pool->default_shared_limits;
    }
}

bucket_t *disjoint_pool_find_bucket(disjoint_pool_t *pool, size_t size) {
    size_t calculated_idx = size_to_idx(pool, size);

#ifndef NDEBUG
    // debug check
    bucket_t *bucket = pool->buckets[calculated_idx];
    assert(bucket->size >= size);
    (void)bucket;

    if (calculated_idx > 0) {
        bucket_t *bucket_prev = pool->buckets[calculated_idx - 1];
        assert(bucket_prev->size < size);
        (void)bucket_prev;
    }
#endif // NDEBUG

    return pool->buckets[calculated_idx];
}

// TODO
void disjoint_pool_print_stats(disjoint_pool_t *pool, bool *title_printed,
                               size_t *high_bucket_size,
                               size_t *high_peak_slabs_in_use,
                               const char *mt_name) {
    (void)title_printed; // TODO
    (void)mt_name;       // TODO

    *high_bucket_size = 0;
    *high_peak_slabs_in_use = 0;
    for (size_t i = 0; i < pool->buckets_num; i++) {
        // TODO
        //(*B).disjoint_pool_print_stats(title_printed, mt_name);
        bucket_t *bucket = pool->buckets[i];
        *high_peak_slabs_in_use =
            utils_max(bucket->max_slabs_in_use, *high_peak_slabs_in_use);
        if (bucket->alloc_count) {
            *high_bucket_size =
                utils_max(bucket_slab_alloc_size(bucket), *high_bucket_size);
        }
    }
}

// TODO remove
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

        TLS_last_allocation_error = ret;
        // throw MemoryProviderError{ret};
        return ret;
    }
    return UMF_RESULT_SUCCESS;
}

void *disjoint_pool_allocate(disjoint_pool_t *pool, size_t size,
                             bool *from_pool) {
    if (size == 0) {
        return NULL;
    }

    void *ptr = NULL;

    if (size > pool->params.MaxPoolableSize) {
        umf_result_t ret =
            umfMemoryProviderAlloc(pool->provider, size, 0, &ptr);
        if (ret != UMF_RESULT_SUCCESS) {
            TLS_last_allocation_error = ret;
            return NULL;
        }

        annotate_memory_undefined(ptr, size);
        return ptr;
    }

    *from_pool = false;
    bucket_t *bucket = disjoint_pool_find_bucket(pool, size);

    if (size > bucket_chunk_cut_off(bucket)) {
        ptr = bucket_get_slab(bucket, from_pool);
    } else {
        ptr = bucket_get_chunk(bucket, from_pool);
    }

    if (ptr == NULL) {
        // TODO get code from func
        TLS_last_allocation_error = UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    if (pool->params.PoolTrace > 1) {
        bucket_count_alloc(bucket, from_pool);
    }

    VALGRIND_DO_MEMPOOL_ALLOC(pool, ptr, size);
    annotate_memory_undefined(ptr, bucket->size);

    return ptr;
}

/*
// TODO?
std::ostream &operator<<(std::ostream &Os, slab_t &Slab) {
    Os << "Slab<" << slab_get(&Slab) << ", " << slab_get_end(&Slab) << ", "
       << slab->bucket->getSize() << ">";
    return Os;
}
*/

/*
// TODO move
void Bucket::printStats(bool &TitlePrinted, const std::string &Label) {
    if (alloc_count) {
        if (!TitlePrinted) {
            std::cout << Label << " memory statistics\n";
            std::cout << std::setw(14) << "Bucket Size" << std::setw(12)
                      << "Allocs" << std::setw(12) << "Frees" << std::setw(18)
                      << "Allocs from Pool" << std::setw(20)
                      << "Peak Slabs in Use" << std::setw(21)
                      << "Peak Slabs in Pool" << std::endl;
            TitlePrinted = true;
        }
        std::cout << std::setw(14) << getSize() << std::setw(12) << alloc_count
                  << std::setw(12) << free_count << std::setw(18)
                  << allocPoolCount << std::setw(20) << max_slabs_in_use
                  << std::setw(21) << max_slabs_in_pool << std::endl;
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

    VALGRIND_DO_CREATE_MEMPOOL(disjoint_pool, 0, 0);

    disjoint_pool->provider = provider;
    disjoint_pool->params = *dp_params;

    utils_mutex_init(&disjoint_pool->known_slabs_map_lock);
    disjoint_pool->known_slabs = critnib_new();

    // Generate buckets sized such as: 64, 96, 128, 192, ..., CutOff.
    // Powers of 2 and the value halfway between the powers of 2.
    size_t Size1 = disjoint_pool->params.MinBucketSize;

    // MinBucketSize cannot be larger than CutOff.
    Size1 = utils_min(Size1, CutOff);

    // Buckets sized smaller than the bucket default size- 8 aren't needed.
    Size1 = utils_max(Size1, UMF_DISJOINT_POOL_MIN_BUCKET_DEFAULT_SIZE);

    // Calculate the exponent for MinBucketSize used for finding buckets.
    disjoint_pool->min_bucket_size_exp = (size_t)log2Utils(Size1);
    disjoint_pool->default_shared_limits =
        umfDisjointPoolSharedLimitsCreate(SIZE_MAX);

    // count number of buckets, start from 1
    disjoint_pool->buckets_num = 1;
    size_t Size2 = Size1 + Size1 / 2;
    size_t ts2 = Size2, ts1 = Size1;
    for (; Size2 < CutOff; Size1 *= 2, Size2 *= 2) {
        disjoint_pool->buckets_num += 2;
    }
    disjoint_pool->buckets = (bucket_t **)umf_ba_global_alloc(
        sizeof(bucket_t *) * disjoint_pool->buckets_num);

    int i = 0;
    Size1 = ts1;
    Size2 = ts2;
    for (; Size2 < CutOff; Size1 *= 2, Size2 *= 2, i += 2) {
        disjoint_pool->buckets[i] = create_bucket(
            Size1, disjoint_pool, disjoint_pool_get_limits(disjoint_pool));
        disjoint_pool->buckets[i + 1] = create_bucket(
            Size2, disjoint_pool, disjoint_pool_get_limits(disjoint_pool));
    }
    disjoint_pool->buckets[i] = create_bucket(
        CutOff, disjoint_pool, disjoint_pool_get_limits(disjoint_pool));

    umf_result_t ret = umfMemoryProviderGetMinPageSize(
        provider, NULL, &disjoint_pool->provider_min_page_size);
    if (ret != UMF_RESULT_SUCCESS) {
        disjoint_pool->provider_min_page_size = 0;
    }

    *ppPool = (void *)disjoint_pool;

    return UMF_RESULT_SUCCESS;
}

void *disjoint_pool_malloc(void *pool, size_t size) {
    // For full-slab allocations indicates
    // whether slab is from Pool.

    disjoint_pool_t *hPool = (disjoint_pool_t *)pool;

    bool from_pool;
    void *ptr = disjoint_pool_allocate(hPool, size, &from_pool);

    if (hPool->params.PoolTrace > 2) {
        const char *MT = hPool->params.Name;
        (void)MT;
        //std::cout << "Allocated " << std::setw(8) << size << " " << MT
        //          << " bytes from " << (FromPool ? "Pool" : "Provider") << " ->"
        //          << ptr << std::endl;
    }

    return ptr;
}

void *disjoint_pool_calloc(void *pool, size_t num, size_t size) {
    (void)pool;
    (void)num;
    (void)size;

    // Not supported
    TLS_last_allocation_error = UMF_RESULT_ERROR_NOT_SUPPORTED;
    return NULL;
}

void *disjoint_pool_realloc(void *pool, void *ptr, size_t size) {
    (void)pool;
    (void)ptr;
    (void)size;

    // Not supported
    TLS_last_allocation_error = UMF_RESULT_ERROR_NOT_SUPPORTED;
    return NULL;
}

void *disjoint_pool_aligned_malloc(void *pool, size_t size, size_t alignment) {
    disjoint_pool_t *disjoint_pool = (disjoint_pool_t *)pool;

    bool from_pool;
    void *ptr = NULL;

    if (size == 0) {
        return NULL;
    }

    if (alignment <= 1) {
        return disjoint_pool_allocate(pool, size, &from_pool);
    }

    size_t aligned_size;
    if (alignment <= disjoint_pool->provider_min_page_size) {
        // This allocation will be served from a Bucket which size is multiple
        // of Alignment and Slab address is aligned to provider_min_page_size
        // so the address will be properly aligned.
        aligned_size = (size > 1) ? ALIGN_UP(size, alignment) : alignment;
    } else {
        // Slabs are only aligned to provider_min_page_size, we need to compensate
        // for that in case the allocation is within pooling limit.
        // TODO: consider creating properly-aligned Slabs on demand
        aligned_size = size + alignment - 1;
    }

    // Check if requested allocation size is within pooling limit.
    // If not, just request aligned pointer from the system.
    from_pool = false;
    if (aligned_size > disjoint_pool->params.MaxPoolableSize) {

        umf_result_t ret = umfMemoryProviderAlloc(disjoint_pool->provider, size,
                                                  alignment, &ptr);
        if (ret != UMF_RESULT_SUCCESS) {
            TLS_last_allocation_error = ret;
            return NULL;
        }

        assert(ptr);
        annotate_memory_undefined(ptr, size);
        return ptr;
    }

    bucket_t *bucket = disjoint_pool_find_bucket(pool, aligned_size);

    if (aligned_size > bucket_chunk_cut_off(bucket)) {
        ptr = bucket_get_slab(bucket, &from_pool);
    } else {
        ptr = bucket_get_chunk(bucket, &from_pool);
    }

    assert(ptr);
    if (disjoint_pool->params.PoolTrace > 1) {
        bucket_count_alloc(bucket, from_pool);
    }

    VALGRIND_DO_MEMPOOL_ALLOC(disjoint_pool, ALIGN_UP((size_t)ptr, alignment),
                              size);
    annotate_memory_undefined((void *)ALIGN_UP((size_t)ptr, alignment), size);
    return (void *)ALIGN_UP((size_t)ptr, alignment);

    if (disjoint_pool->params.PoolTrace > 2) {
        const char *MT = disjoint_pool->params.Name;
        (void)MT;
        //std::cout << "Allocated " << std::setw(8) << size << " " << MT
        //          << " bytes aligned at " << alignment << " from "
        //          << (FromPool ? "Pool" : "Provider") << " ->" << Ptr
        //          << std::endl;
    }

    return ptr;
}

size_t disjoint_pool_malloc_usable_size(void *pool, void *ptr) {
    (void)pool;
    (void)ptr;

    // Not supported
    return 0;
}

umf_result_t disjoint_pool_free(void *pool, void *ptr) {
    disjoint_pool_t *disjoint_pool = (disjoint_pool_t *)pool;
    if (ptr == NULL) {
        return UMF_RESULT_SUCCESS;
    }

    void *slab_ptr =
        (void *)ALIGN_DOWN((size_t)ptr, disjoint_pool->params.SlabMinSize);

    // Lock the map on read
    utils_mutex_lock(&disjoint_pool->known_slabs_map_lock);

    slab_t *slab =
        (slab_t *)critnib_get(disjoint_pool->known_slabs, (uintptr_t)slab_ptr);
    //auto Slabs = getKnownSlabs().equal_range(SlabPtr);
    if (slab == NULL) {
        utils_mutex_unlock(&disjoint_pool->known_slabs_map_lock);
        umf_result_t ret = memoryProviderFree(disjoint_pool->provider, ptr);
        return ret;
    }

    bool to_pool = false;
    // TODO - no multimap
    // for (auto It = Slabs.first; It != Slabs.second; ++It) {

    // The slab object won't be deleted until it's removed from the map which is
    // protected by the lock, so it's safe to access it here.
    if (ptr >= slab_get(slab) && ptr < slab_get_end(slab)) {
        // Unlock the map before freeing the chunk, it may be locked on write
        // there
        utils_mutex_unlock(&disjoint_pool->known_slabs_map_lock);
        bucket_t *bucket = slab->bucket;

        if (disjoint_pool->params.PoolTrace > 1) {
            bucket->free_count++;
        }

        VALGRIND_DO_MEMPOOL_FREE(pool, ptr);
        annotate_memory_inaccessible(ptr, bucket->size);
        if (bucket->size <= bucket_chunk_cut_off(bucket)) {
            bucket_free_chunk(bucket, ptr, slab, &to_pool);
        } else {
            bucket_free_slab(bucket, slab, &to_pool);
        }

        return UMF_RESULT_SUCCESS;
    }
    //} // for multimap

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

    // There is a rare case when we have a pointer from system allocation next
    // to some slab with an entry in the map. So we find a slab
    // but the range checks fail.
    assert(0);
    return UMF_RESULT_ERROR_MEMORY_PROVIDER_SPECIFIC;
}

umf_result_t disjoint_pool_get_last_allocation_error(void *pool) {
    (void)pool;

    return TLS_last_allocation_error;
}

// Define destructor for use with unique_ptr
void disjoint_pool_finalize(void *pool) {

    disjoint_pool_t *hPool = (disjoint_pool_t *)pool;

    for (size_t i = 0; i < hPool->buckets_num; i++) {
        destroy_bucket(hPool->buckets[i]);
    }

    VALGRIND_DO_DESTROY_MEMPOOL(hPool);

    umfDisjointPoolSharedLimitsDestroy(hPool->default_shared_limits);
    critnib_delete(hPool->known_slabs);

    utils_mutex_destroy_not_free(&hPool->known_slabs_map_lock);

    umf_ba_global_free(hPool);

    /*
    if (impl->getParams().PoolTrace > 1) {
        bool TitlePrinted = false;
        size_t HighBucketSize;
        size_t HighPeakSlabsInUse;
        auto name = impl->getParams().Name;
        //try { // cannot throw in destructor
        impl->disjoint_pool_print_stats(TitlePrinted, HighBucketSize, HighPeakSlabsInUse,
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
