

#ifndef TEMP_H
#define TEMP_H 1

void annotate_memory_inaccessible(void *ptr, size_t size);
void annotate_memory_undefined(void *ptr, size_t size);

umf_disjoint_pool_shared_limits_t *shared_limits_create(size_t max_size);
void shared_limits_destroy(umf_disjoint_pool_shared_limits_t *shared_limits);

typedef struct slab_list_item_t slab_list_item_t;

typedef struct bucket_t {
    size_t Size;

    // List of slabs which have at least 1 available chunk.
    slab_list_item_t *AvailableSlabs;

    // List of slabs with 0 available chunk.
    slab_list_item_t *UnavailableSlabs;

    // Protects the bucket and all the corresponding slabs
    utils_mutex_t bucket_lock;

    // Reference to the allocator context, used access memory allocation
    // routines, slab map and etc.
    void *OwnAllocCtx;

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
    size_t chunkedSlabsInPool;

    // Statistics
    size_t allocPoolCount;
    size_t freeCount;
    size_t currSlabsInUse;
    size_t currSlabsInPool;
    size_t maxSlabsInPool;

    // Statistics
    size_t allocCount;
    size_t maxSlabsInUse;

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

slab_t *create_slab(bucket_t *bucket);
void destroy_slab(slab_t *slab);

void *slab_get(const slab_t *slab);
void *slab_get_end(const slab_t *slab);
bucket_t *slab_get_bucket(slab_t *slab);
void *slab_get_chunk(slab_t *slab);
size_t slab_get_num_chunks(const slab_t *slab);
size_t slab_get_chunk_size(const slab_t *slab);
size_t slab_get_num_allocated(const slab_t *slab);

bool slab_has_avail(const slab_t *slab);
void slab_free_chunk(slab_t *slab, void *ptr);

void slab_reg(slab_t *slab);
void slab_reg_by_addr(void *addr, slab_t *slab);
void slab_unreg(slab_t *slab);
void slab_unreg_by_addr(void *addr, slab_t *slab);

bucket_t *create_bucket(size_t sz, void *alloc_ctx,
                        umf_disjoint_pool_shared_limits_t *shared_limits);
void destroy_bucket(bucket_t *bucket);

void bucket_update_stats(bucket_t *bucket, int in_use, int in_pool);
bool bucket_can_pool(bucket_t *bucket, bool *to_pool);
void bucket_on_free_chunk(bucket_t *bucket, slab_t *slab, bool *to_pool);
void bucket_decrement_pool(bucket_t *bucket, bool *from_pool);
void *bucket_get_chunk(bucket_t *bucket, bool *from_pool);
size_t bucket_get_size(bucket_t *bucket);
size_t bucket_chunk_cut_off(bucket_t *bucket);
size_t bucket_capacity(bucket_t *bucket);
void bucket_free_chunk(bucket_t *bucket, void *ptr, slab_t *slab,
                       bool *to_pool);
void bucket_count_alloc(bucket_t *bucket, bool from_pool);
void bucket_count_free(bucket_t *bucket);

void *bucket_get_slab(bucket_t *bucket, bool *from_pool);
size_t bucket_slab_alloc_size(bucket_t *bucket);
size_t bucket_slab_min_size(bucket_t *bucket);
slab_list_item_t *bucket_get_avail_slab(bucket_t *bucket, bool *from_pool);
slab_list_item_t *bucket_get_avail_full_slab(bucket_t *bucket, bool *from_pool);
void bucket_free_slab(bucket_t *bucket, slab_t *slab, bool *to_pool);

umf_disjoint_pool_shared_limits_t *bucket_get_limits(bucket_t *bucket);
umf_disjoint_pool_params_t *bucket_get_params(bucket_t *bucket);
umf_memory_provider_handle_t bucket_get_mem_handle(bucket_t *bucket);
utils_mutex_t *bucket_get_known_slabs_map_lock(bucket_t *bucket);
critnib *bucket_get_known_slabs(bucket_t *bucket);

typedef struct AllocImpl {
    // It's important for the map to be destroyed last after buckets and their
    // slabs This is because slab's destructor removes the object from the map.
    critnib *known_slabs; // (void *, slab_t *)

    // prev std::shared_timed_mutex - ok?
    utils_mutex_t known_slabs_map_lock;

    // Handle to the memory provider
    umf_memory_provider_handle_t MemHandle;

    // Store as unique_ptrs since Bucket is not Movable(because of std::mutex)
    bucket_t **buckets;
    size_t buckets_num;

    // Configuration for this instance
    umf_disjoint_pool_params_t params;

    umf_disjoint_pool_shared_limits_t *DefaultSharedLimits;

    // Used in algorithm for finding buckets
    size_t MinBucketSizeExp;

    // Coarse-grain allocation min alignment
    size_t ProviderMinPageSize;

} AllocImpl;

AllocImpl *create_AllocImpl(umf_memory_provider_handle_t hProvider,
                            umf_disjoint_pool_params_t *params);
void destroy_AllocImpl(AllocImpl *ai);

bucket_t *AllocImpl_findBucket(AllocImpl *ai, size_t Size);
umf_result_t AllocImpl_deallocate(AllocImpl *ai, void *Ptr, bool *ToPool);
umf_disjoint_pool_shared_limits_t *AllocImpl_getLimits(AllocImpl *ai);
void *AllocImpl_allocate(AllocImpl *ai, size_t Size, bool *FromPool);
void *AllocImpl_allocate_align(AllocImpl *ai, size_t Size, size_t Alignment,
                               bool *FromPool);

umf_memory_provider_handle_t AllocImpl_getMemHandle(AllocImpl *ai);
utils_mutex_t *AllocImpl_getKnownSlabsMapLock(AllocImpl *ai);
critnib *AllocImpl_getKnownSlabs(AllocImpl *ai);
size_t AllocImpl_SlabMinSize(AllocImpl *ai);
umf_disjoint_pool_params_t *AllocImpl_getParams(AllocImpl *ai);

#endif // TEMP_H
