/*
 * Copyright (C) 2022-2024 Intel Corporation
 *
 * Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
*/

#ifndef UMF_POOL_DISJOINT_INTERNAL_H
#define UMF_POOL_DISJOINT_INTERNAL_H 1

#include <assert.h>
#include <errno.h>
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

typedef struct bucket_t bucket_t;
typedef struct slab_t slab_t;
typedef struct slab_list_item_t slab_list_item_t;
typedef struct disjoint_pool_t disjoint_pool_t;

typedef struct bucket_t {
    size_t size;

    // Linked list of slabs which have at least 1 available chunk.
    slab_list_item_t *available_slabs;

    // Linked list of slabs with 0 available chunk.
    slab_list_item_t *unavailable_slabs;

    // Protects the bucket and all the corresponding slabs
    utils_mutex_t bucket_lock;

    // Reference to the allocator context, used access memory allocation
    // routines, slab map and etc.
    disjoint_pool_t *pool;

    umf_disjoint_pool_shared_limits_t *shared_limits;

    // For buckets used in chunked mode, a counter of slabs in the pool.
    // For allocations that use an entire slab each, the entries in the Available
    // list are entries in the pool.Each slab is available for a new
    // allocation.The size of the Available list is the size of the pool.
    // For allocations that use slabs in chunked mode, slabs will be in the
    // Available list if any one or more of their chunks is free.The entire slab
    // is not necessarily free, just some chunks in the slab are free. To
    // implement pooling we will allow one slab in the Available list to be
    // entirely empty. Normally such a slab would have been freed. But
    // now we don't, and treat this slab as "in the pool".
    // When a slab becomes entirely free we have to decide whether to return it
    // to the provider or keep it allocated. A simple check for size of the
    // Available list is not sufficient to check whether any slab has been
    // pooled yet.We would have to traverse the entire Available listand check
    // if any of them is entirely free. Instead we keep a counter of entirely
    // empty slabs within the Available list to speed up the process of checking
    // if a slab in this bucket is already pooled.
    size_t chunked_slabs_in_pool;

    // Statistics
    size_t alloc_pool_count;
    size_t free_count;
    size_t curr_slabs_in_use;
    size_t curr_slabs_in_pool;
    size_t max_slabs_in_pool;
    size_t alloc_count;
    size_t max_slabs_in_use;
} bucket_t;

// Represents the allocated memory block of size 'slab_min_size'
// Internally, it splits the memory block into chunks. The number of
// chunks depends of the size of a Bucket which created the Slab.
// Note: Bucket's methods are responsible for thread safety of Slab access,
// so no locking happens here.
typedef struct slab_t {
    // Pointer to the allocated memory of slab_min_size bytes
    void *mem_ptr;
    size_t slab_size;

    // Represents the current state of each chunk: if the bit is set then the
    // chunk is allocated, and if the chunk is free for allocation otherwise
    bool *chunks;
    size_t num_chunks;

    // Total number of allocated chunks at the moment.
    size_t num_allocated;

    // The bucket which the slab belongs to
    bucket_t *bucket;

    // Hints where to start search for free chunk in a slab
    size_t first_free_chunk_idx;

    // Store iterator to the corresponding node in avail/unavail list
    // to achieve O(1) removal
    slab_list_item_t *iter;
} slab_t;

typedef struct slab_list_item_t {
    slab_t *val;
    struct slab_list_item_t *prev, *next;
} slab_list_item_t;

typedef struct umf_disjoint_pool_shared_limits_t {
    size_t max_size;
    size_t total_size; // requires atomic access
} umf_disjoint_pool_shared_limits_t;

typedef struct disjoint_pool_t {
    // It's important for the map to be destroyed last after buckets and their
    // slabs This is because slab's destructor removes the object from the map.
    critnib *known_slabs; // (void *, slab_t *)

    // TODO: prev std::shared_timed_mutex - ok?
    utils_mutex_t known_slabs_map_lock;

    // Handle to the memory provider
    umf_memory_provider_handle_t provider;

    // Array of bucket_t*
    bucket_t **buckets;
    size_t buckets_num;

    // Configuration for this instance
    umf_disjoint_pool_params_t params;

    umf_disjoint_pool_shared_limits_t *default_shared_limits;

    // Used in algorithm for finding buckets
    size_t min_bucket_size_exp;

    // Coarse-grain allocation min alignment
    size_t provider_min_page_size;
} disjoint_pool_t;

slab_t *create_slab(bucket_t *bucket);
void destroy_slab(slab_t *slab);

void *slab_get(const slab_t *slab);
void *slab_get_end(const slab_t *slab);
void *slab_get_chunk(slab_t *slab);

bool slab_has_avail(const slab_t *slab);
void slab_free_chunk(slab_t *slab, void *ptr);

void slab_reg(slab_t *slab);
void slab_reg_by_addr(void *addr, slab_t *slab);
void slab_unreg(slab_t *slab);
void slab_unreg_by_addr(void *addr, slab_t *slab);

bucket_t *create_bucket(size_t sz, disjoint_pool_t *pool,
                        umf_disjoint_pool_shared_limits_t *shared_limits);
void destroy_bucket(bucket_t *bucket);

void bucket_update_stats(bucket_t *bucket, int in_use, int in_pool);
bool bucket_can_pool(bucket_t *bucket, bool *to_pool);
void bucket_decrement_pool(bucket_t *bucket, bool *from_pool);
void *bucket_get_chunk(bucket_t *bucket, bool *from_pool);
size_t bucket_chunk_cut_off(bucket_t *bucket);
size_t bucket_capacity(bucket_t *bucket);
void bucket_free_chunk(bucket_t *bucket, void *ptr, slab_t *slab,
                       bool *to_pool);
void bucket_count_alloc(bucket_t *bucket, bool from_pool);

void *bucket_get_slab(bucket_t *bucket, bool *from_pool);
size_t bucket_slab_alloc_size(bucket_t *bucket);
size_t bucket_slab_min_size(bucket_t *bucket);
slab_list_item_t *bucket_get_avail_slab(bucket_t *bucket, bool *from_pool);
slab_list_item_t *bucket_get_avail_full_slab(bucket_t *bucket, bool *from_pool);
void bucket_free_slab(bucket_t *bucket, slab_t *slab, bool *to_pool);

bucket_t *disjoint_pool_find_bucket(disjoint_pool_t *pool, size_t size);

#endif // UMF_POOL_DISJOINT_INTERNAL_H
