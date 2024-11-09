// Copyright (C) 2023 Intel Corporation
// Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cctype>
#include <iomanip>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// TODO: replace with logger?
#include <iostream>

#include "provider/provider_tracking.h"

#include "uthash/utlist.h"

#include "../cpp_helpers.hpp"
#include "base_alloc_global.h"
#include "pool_disjoint.h"
#include "umf.h"
#include "utils_common.h"
#include "utils_log.h"
#include "utils_math.h"
#include "utils_sanitizers.h"

// TODO remove
#ifdef __cplusplus
extern "C" {
#endif

#include "pool_disjoint_temp.h"

struct slab_t;

#ifdef __cplusplus
}
#endif
// end TODO remove

typedef struct umf_disjoint_pool_shared_limits_t {
    size_t MaxSize;
    std::atomic<size_t> TotalSize;
} umf_disjoint_pool_shared_limits_t;

class DisjointPool {
  public:
    class AllocImpl;
    using Config = umf_disjoint_pool_params_t;

    umf_result_t initialize(umf_memory_provider_handle_t provider,
                            umf_disjoint_pool_params_t *parameters);
    void *malloc(size_t size);
    void *calloc(size_t, size_t);
    void *realloc(void *, size_t);
    void *aligned_malloc(size_t size, size_t alignment);
    size_t malloc_usable_size(void *);
    umf_result_t free(void *ptr);
    umf_result_t get_last_allocation_error();

    DisjointPool();
    ~DisjointPool();

  private:
    std::unique_ptr<AllocImpl> impl;
};

umf_disjoint_pool_shared_limits_t *
umfDisjointPoolSharedLimitsCreate(size_t MaxSize) {
    return new umf_disjoint_pool_shared_limits_t{MaxSize, 0};
}

void umfDisjointPoolSharedLimitsDestroy(
    umf_disjoint_pool_shared_limits_t *limits) {
    delete limits;
}

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
static constexpr size_t CutOff = (size_t)1 << 31; // 2GB

typedef struct MemoryProviderError {
    umf_result_t code;
} MemoryProviderError_t;

bucket_t *create_bucket(size_t Sz, DisjointPool::AllocImpl *AllocCtx) {
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

class DisjointPool::AllocImpl {
    // It's important for the map to be destroyed last after buckets and their
    // slabs This is because slab's destructor removes the object from the map.
    std::unordered_multimap<void *, slab_t *> KnownSlabs;
    std::shared_timed_mutex KnownSlabsMapLock;

    // Handle to the memory provider
    umf_memory_provider_handle_t MemHandle;

    // Store as unique_ptrs since Bucket is not Movable(because of std::mutex)
    std::vector<bucket_t *> Buckets;

    // Configuration for this instance
    umf_disjoint_pool_params_t params;

    umf_disjoint_pool_shared_limits_t DefaultSharedLimits = {
        (std::numeric_limits<size_t>::max)(), 0};

    // Used in algorithm for finding buckets
    std::size_t MinBucketSizeExp;

    // Coarse-grain allocation min alignment
    size_t ProviderMinPageSize;

  public:
    AllocImpl(umf_memory_provider_handle_t hProvider,
              umf_disjoint_pool_params_t *params)
        : MemHandle{hProvider}, params(*params) {

        VALGRIND_DO_CREATE_MEMPOOL(this, 0, 0);

        // Generate buckets sized such as: 64, 96, 128, 192, ..., CutOff.
        // Powers of 2 and the value halfway between the powers of 2.
        auto Size1 = this->params.MinBucketSize;
        // MinBucketSize cannot be larger than CutOff.
        Size1 = std::min(Size1, CutOff);
        // Buckets sized smaller than the bucket default size- 8 aren't needed.
        Size1 = std::max(Size1, UMF_DISJOINT_POOL_MIN_BUCKET_DEFAULT_SIZE);
        // Calculate the exponent for MinBucketSize used for finding buckets.
        MinBucketSizeExp = (size_t)log2Utils(Size1);
        auto Size2 = Size1 + Size1 / 2;
        for (; Size2 < CutOff; Size1 *= 2, Size2 *= 2) {
            // TODO copy allocimpl
            Buckets.push_back(create_bucket(Size1, this));
            Buckets.push_back(create_bucket(Size2, this));
        }
        Buckets.push_back(create_bucket(CutOff, this));

        auto ret = umfMemoryProviderGetMinPageSize(hProvider, nullptr,
                                                   &ProviderMinPageSize);
        if (ret != UMF_RESULT_SUCCESS) {
            ProviderMinPageSize = 0;
        }
    }

    ~AllocImpl() {

        for (auto it = Buckets.begin(); it != Buckets.end(); it++) {
            destroy_bucket(*it);
        }

        VALGRIND_DO_DESTROY_MEMPOOL(this);
    }

    void *allocate(size_t Size, size_t Alignment, bool &FromPool);
    void *allocate(size_t Size, bool &FromPool);
    void deallocate(void *Ptr, bool &ToPool);

    umf_memory_provider_handle_t getMemHandle() { return MemHandle; }

    std::shared_timed_mutex &getKnownSlabsMapLock() {
        return KnownSlabsMapLock;
    }

    std::unordered_multimap<void *, slab_t *> &getKnownSlabs() {
        return KnownSlabs;
    }

    size_t SlabMinSize() { return params.SlabMinSize; };

    umf_disjoint_pool_params_t &getParams() { return params; }

    umf_disjoint_pool_shared_limits_t *getLimits() {
        if (params.SharedLimits) {
            return params.SharedLimits;
        } else {
            return &DefaultSharedLimits;
        }
    };

    void printStats(bool &TitlePrinted, size_t &HighBucketSize,
                    size_t &HighPeakSlabsInUse, const std::string &Label);

  private:
    bucket_t *findBucket(size_t Size);
    std::size_t sizeToIdx(size_t Size);
};

static void *memoryProviderAlloc(umf_memory_provider_handle_t hProvider,
                                 size_t size, size_t alignment = 0) {
    void *ptr;
    auto ret = umfMemoryProviderAlloc(hProvider, size, alignment, &ptr);
    if (ret != UMF_RESULT_SUCCESS) {
        throw MemoryProviderError{ret};
    }
    annotate_memory_inaccessible(ptr, size);
    return ptr;
}

static void memoryProviderFree(umf_memory_provider_handle_t hProvider,
                               void *ptr) {
    size_t size = 0;

    if (ptr) {
        umf_alloc_info_t allocInfo = {NULL, 0, NULL};
        umf_result_t umf_result = umfMemoryTrackerGetAllocInfo(ptr, &allocInfo);
        if (umf_result == UMF_RESULT_SUCCESS) {
            size = allocInfo.baseSize;
        }
    }

    auto ret = umfMemoryProviderFree(hProvider, ptr, size);
    if (ret != UMF_RESULT_SUCCESS) {
        throw MemoryProviderError{ret};
    }
}

bool operator==(const slab_t &Lhs, const slab_t &Rhs) {
    return slab_get(&Lhs) == slab_get(&Rhs);
}

/*
std::ostream &operator<<(std::ostream &Os, slab_t &Slab) {
    Os << "Slab<" << slab_get(&Slab) << ", " << slab_get_end(&Slab) << ", "
       << slab_get_bucket(&Slab)->getSize() << ">";
    return Os;
}
*/

// If a slab was available in the pool then note that the current pooled
// size has reduced by the size of a slab in this bucket.
void bucket_decrement_pool(bucket_t *bucket, bool *FromPool) {
    *FromPool = true;
    bucket_update_stats(bucket, 1, -1);
    bucket_get_limits(bucket)->TotalSize -= bucket_slab_alloc_size(bucket);
}

slab_list_item_t *bucket_get_avail_full_slab(bucket_t *bucket, bool *FromPool) {
    // Return a slab that will be used for a single allocation.
    if (bucket->AvailableSlabs == NULL) {
        slab_t *slab = create_slab(bucket);
        if (slab == NULL) {
            utils_mutex_unlock(&bucket->bucket_lock);
            throw MemoryProviderError{UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY};
        }

        slab_reg(slab);
        DL_PREPEND(bucket->AvailableSlabs, slab->iter);
        *FromPool = false;
        bucket_update_stats(bucket, 1, 0);
    } else {
        bucket_decrement_pool(bucket, FromPool);
    }

    return bucket->AvailableSlabs;
}

void *bucket_get_slab(bucket_t *bucket, bool *FromPool) {
    utils_mutex_lock(&bucket->bucket_lock);

    slab_list_item_t *slab_it = bucket_get_avail_full_slab(bucket, FromPool);
    slab_t *slab = slab_it->val;
    void *ptr = slab_get(slab);

    DL_DELETE(bucket->AvailableSlabs, slab_it);
    DL_PREPEND(bucket->UnavailableSlabs, slab_it);

    utils_mutex_unlock(&bucket->bucket_lock);
    return ptr;
}

void bucket_free_slab(bucket_t *bucket, slab_t *slab, bool *ToPool) {
    utils_mutex_lock(&bucket->bucket_lock);

    slab_list_item_t *slab_it = slab->iter;
    assert(slab_it->val != NULL);
    if (bucket_can_pool(bucket, ToPool)) {
        DL_DELETE(bucket->UnavailableSlabs, slab_it);
        DL_PREPEND(bucket->AvailableSlabs, slab_it);
    } else {
        slab_unreg(slab_it->val);
        DL_DELETE(bucket->UnavailableSlabs, slab_it);
        destroy_slab(slab_it->val);
    }
    utils_mutex_unlock(&bucket->bucket_lock);
}

slab_list_item_t *bucket_get_avail_slab(bucket_t *bucket, bool *FromPool) {
    if (bucket->AvailableSlabs == NULL) {
        slab_t *slab = create_slab(bucket);
        if (slab == NULL) {
            throw MemoryProviderError{UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY};
        }

        slab_reg(slab);
        DL_PREPEND(bucket->AvailableSlabs, slab->iter);
        bucket_update_stats(bucket, 1, 0);
        *FromPool = false;
    } else {
        if (slab_get_num_allocated(bucket->AvailableSlabs->val) == 0) {
            // If this was an empty slab, it was in the pool.
            // Now it is no longer in the pool, so update count.
            --bucket->chunkedSlabsInPool;
            bucket_decrement_pool(bucket, FromPool);
        } else {
            // Allocation from existing slab is treated as from pool for statistics.
            *FromPool = true;
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

void bucket_update_stats(bucket_t *bucket, int InUse, int InPool) {
    if (bucket_get_params(bucket)->PoolTrace == 0) {
        return;
    }
    bucket->currSlabsInUse += InUse;
    bucket->maxSlabsInUse =
        std::max(bucket->currSlabsInUse, bucket->maxSlabsInUse);
    bucket->currSlabsInPool += InPool;
    bucket->maxSlabsInPool =
        std::max(bucket->currSlabsInPool, bucket->maxSlabsInPool);
    // Increment or decrement current pool sizes based on whether
    // slab was added to or removed from pool.
    bucket_get_params(bucket)->CurPoolSize +=
        InPool * bucket_slab_alloc_size(bucket);
}

/*
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

void *DisjointPool::AllocImpl::allocate(size_t Size, bool &FromPool) try {
    void *Ptr;

    if (Size == 0) {
        return nullptr;
    }

    FromPool = false;
    if (Size > getParams().MaxPoolableSize) {
        Ptr = memoryProviderAlloc(getMemHandle(), Size);
        annotate_memory_undefined(Ptr, Size);
        return Ptr;
    }

    bucket_t *bucket = findBucket(Size);

    if (Size > bucket_chunk_cut_off(bucket)) {
        Ptr = bucket_get_slab(bucket, &FromPool);
    } else {
        Ptr = bucket_get_chunk(bucket, &FromPool);
    }

    if (getParams().PoolTrace > 1) {
        bucket_count_alloc(bucket, FromPool);
    }

    VALGRIND_DO_MEMPOOL_ALLOC(this, Ptr, Size);
    annotate_memory_undefined(Ptr, bucket_get_size(bucket));

    return Ptr;
} catch (MemoryProviderError &e) {
    umf::getPoolLastStatusRef<DisjointPool>() = e.code;
    return nullptr;
}

void *DisjointPool::AllocImpl::allocate(size_t Size, size_t Alignment,
                                        bool &FromPool) try {
    void *Ptr;

    if (Size == 0) {
        return nullptr;
    }

    if (Alignment <= 1) {
        return allocate(Size, FromPool);
    }

    size_t AlignedSize;
    if (Alignment <= ProviderMinPageSize) {
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
    FromPool = false;
    if (AlignedSize > getParams().MaxPoolableSize) {
        Ptr = memoryProviderAlloc(getMemHandle(), Size, Alignment);
        annotate_memory_undefined(Ptr, Size);
        return Ptr;
    }

    bucket_t *bucket = findBucket(AlignedSize);

    if (AlignedSize > bucket_chunk_cut_off(bucket)) {
        Ptr = bucket_get_slab(bucket, &FromPool);
    } else {
        Ptr = bucket_get_chunk(bucket, &FromPool);
    }

    if (getParams().PoolTrace > 1) {
        bucket_count_alloc(bucket, FromPool);
    }

    VALGRIND_DO_MEMPOOL_ALLOC(this, ALIGN_UP((size_t)Ptr, Alignment), Size);
    annotate_memory_undefined((void *)ALIGN_UP((size_t)Ptr, Alignment), Size);
    return (void *)ALIGN_UP((size_t)Ptr, Alignment);
} catch (MemoryProviderError &e) {
    umf::getPoolLastStatusRef<DisjointPool>() = e.code;
    return nullptr;
}

std::size_t DisjointPool::AllocImpl::sizeToIdx(size_t Size) {
    assert(Size <= CutOff && "Unexpected size");
    assert(Size > 0 && "Unexpected size");

    size_t MinBucketSize = (size_t)1 << MinBucketSizeExp;
    if (Size < MinBucketSize) {
        return 0;
    }

    // Get the position of the leftmost set bit.
    size_t position = getLeftmostSetBitPos(Size);

    auto isPowerOf2 = 0 == (Size & (Size - 1));
    auto largerThanHalfwayBetweenPowersOf2 =
        !isPowerOf2 && bool((Size - 1) & (uint64_t(1) << (position - 1)));
    auto index = (position - MinBucketSizeExp) * 2 + (int)(!isPowerOf2) +
                 (int)largerThanHalfwayBetweenPowersOf2;

    return index;
}

bucket_t *DisjointPool::AllocImpl::findBucket(size_t Size) {
    auto calculatedIdx = sizeToIdx(Size);
    bucket_t *bucket = Buckets[calculatedIdx];
    assert(bucket_get_size(bucket) >= Size);
    if (calculatedIdx > 0) {
        bucket_t *bucket_prev = Buckets[calculatedIdx - 1];
        assert(bucket_get_size(bucket_prev) < Size);
    }

    return Buckets[calculatedIdx];
}

void DisjointPool::AllocImpl::deallocate(void *Ptr, bool &ToPool) {
    if (Ptr == nullptr) {
        return;
    }

    auto *SlabPtr = (void *)ALIGN_DOWN((size_t)Ptr, SlabMinSize());

    // Lock the map on read
    std::shared_lock<std::shared_timed_mutex> Lk(getKnownSlabsMapLock());

    ToPool = false;
    auto Slabs = getKnownSlabs().equal_range(SlabPtr);
    if (Slabs.first == Slabs.second) {
        Lk.unlock();
        memoryProviderFree(getMemHandle(), Ptr);
        return;
    }

    for (auto It = Slabs.first; It != Slabs.second; ++It) {
        // The slab object won't be deleted until it's removed from the map which is
        // protected by the lock, so it's safe to access it here.
        auto &Slab = It->second;
        if (Ptr >= slab_get(Slab) && Ptr < slab_get_end(Slab)) {
            // Unlock the map before freeing the chunk, it may be locked on write
            // there
            Lk.unlock();
            bucket_t *bucket = slab_get_bucket(Slab);

            if (getParams().PoolTrace > 1) {
                bucket_count_free(bucket);
            }

            VALGRIND_DO_MEMPOOL_FREE(this, Ptr);
            annotate_memory_inaccessible(Ptr, bucket_get_size(bucket));
            if (bucket_get_size(bucket) <= bucket_chunk_cut_off(bucket)) {
                bucket_free_chunk(bucket, Ptr, Slab, &ToPool);
            } else {
                bucket_free_slab(bucket, Slab, &ToPool);
            }

            return;
        }
    }

    Lk.unlock();
    // There is a rare case when we have a pointer from system allocation next
    // to some slab with an entry in the map. So we find a slab
    // but the range checks fail.
    memoryProviderFree(getMemHandle(), Ptr);
}

void DisjointPool::AllocImpl::printStats(bool &TitlePrinted,
                                         size_t &HighBucketSize,
                                         size_t &HighPeakSlabsInUse,
                                         const std::string &MTName) {
    (void)TitlePrinted; // TODO
    (void)MTName;       // TODO

    HighBucketSize = 0;
    HighPeakSlabsInUse = 0;
    for (auto &B : Buckets) {
        // TODO
        //(*B).printStats(TitlePrinted, MTName);
        bucket_t *bucket = B;
        HighPeakSlabsInUse =
            std::max(bucket->maxSlabsInUse, HighPeakSlabsInUse);
        if ((*B).allocCount) {
            HighBucketSize =
                std::max(bucket_slab_alloc_size(bucket), HighBucketSize);
        }
    }
}

umf_result_t DisjointPool::initialize(umf_memory_provider_handle_t provider,
                                      umf_disjoint_pool_params_t *parameters) {
    if (!provider) {
        return UMF_RESULT_ERROR_INVALID_ARGUMENT;
    }
    // MinBucketSize parameter must be a power of 2 for bucket sizes
    // to generate correctly.
    if (!parameters->MinBucketSize ||
        !((parameters->MinBucketSize & (parameters->MinBucketSize - 1)) == 0)) {
        return UMF_RESULT_ERROR_INVALID_ARGUMENT;
    }

    impl = std::make_unique<AllocImpl>(provider, parameters);
    return UMF_RESULT_SUCCESS;
}

void *DisjointPool::malloc(size_t size) { // For full-slab allocations indicates
                                          // whether slab is from Pool.
    bool FromPool;
    auto Ptr = impl->allocate(size, FromPool);

    if (impl->getParams().PoolTrace > 2) {
        auto MT = impl->getParams().Name;
        std::cout << "Allocated " << std::setw(8) << size << " " << MT
                  << " bytes from " << (FromPool ? "Pool" : "Provider") << " ->"
                  << Ptr << std::endl;
    }
    return Ptr;
}

void *DisjointPool::calloc(size_t, size_t) {
    // Not supported
    umf::getPoolLastStatusRef<DisjointPool>() = UMF_RESULT_ERROR_NOT_SUPPORTED;
    return NULL;
}

void *DisjointPool::realloc(void *, size_t) {
    // Not supported
    umf::getPoolLastStatusRef<DisjointPool>() = UMF_RESULT_ERROR_NOT_SUPPORTED;
    return NULL;
}

void *DisjointPool::aligned_malloc(size_t size, size_t alignment) {
    bool FromPool;
    auto Ptr = impl->allocate(size, alignment, FromPool);

    if (impl->getParams().PoolTrace > 2) {
        auto MT = impl->getParams().Name;
        std::cout << "Allocated " << std::setw(8) << size << " " << MT
                  << " bytes aligned at " << alignment << " from "
                  << (FromPool ? "Pool" : "Provider") << " ->" << Ptr
                  << std::endl;
    }
    return Ptr;
}

size_t DisjointPool::malloc_usable_size(void *) {
    // Not supported
    return 0;
}

umf_result_t DisjointPool::free(void *ptr) try {
    bool ToPool;
    impl->deallocate(ptr, ToPool);

    if (impl->getParams().PoolTrace > 2) {
        auto MT = impl->getParams().Name;
        std::cout << "Freed " << MT << " " << ptr << " to "
                  << (ToPool ? "Pool" : "Provider")
                  << ", Current total pool size "
                  << impl->getLimits()->TotalSize.load()
                  << ", Current pool size for " << MT << " "
                  << impl->getParams().CurPoolSize << "\n";
    }
    return UMF_RESULT_SUCCESS;
} catch (MemoryProviderError &e) {
    return e.code;
}

umf_result_t DisjointPool::get_last_allocation_error() {
    return umf::getPoolLastStatusRef<DisjointPool>();
}

DisjointPool::DisjointPool() {}

// Define destructor for use with unique_ptr
DisjointPool::~DisjointPool() {
    bool TitlePrinted = false;
    size_t HighBucketSize;
    size_t HighPeakSlabsInUse;
    if (impl->getParams().PoolTrace > 1) {
        auto name = impl->getParams().Name;
        try { // cannot throw in destructor
            impl->printStats(TitlePrinted, HighBucketSize, HighPeakSlabsInUse,
                             name);
            if (TitlePrinted) {
                std::cout << "Current Pool Size "
                          << impl->getLimits()->TotalSize.load() << std::endl;
                std::cout << "Suggested Setting=;"
                          << std::string(1, (char)tolower(name[0]))
                          << std::string(name + 1) << ":" << HighBucketSize
                          << "," << HighPeakSlabsInUse << ",64K" << std::endl;
            }
        } catch (...) { // ignore exceptions
        }
    }
}

static umf_memory_pool_ops_t UMF_DISJOINT_POOL_OPS =
    umf::poolMakeCOps<DisjointPool, umf_disjoint_pool_params_t>();

umf_memory_pool_ops_t *umfDisjointPoolOps(void) {
    return &UMF_DISJOINT_POOL_OPS;
}

// TODO remove
#ifdef __cplusplus
extern "C" {
#endif

umf_disjoint_pool_params_t *bucket_get_params(bucket_t *bucket) {
    auto t = (DisjointPool::AllocImpl *)bucket->OwnAllocCtx;
    return &t->getParams();
}

umf_disjoint_pool_shared_limits_t *bucket_get_limits(bucket_t *bucket) {
    auto t = (DisjointPool::AllocImpl *)bucket->OwnAllocCtx;
    return t->getLimits();
}

umf_memory_provider_handle_t bucket_get_mem_handle(bucket_t *bucket) {
    auto t = (DisjointPool::AllocImpl *)bucket->OwnAllocCtx;
    return t->getMemHandle();
}

std::unordered_multimap<void *, slab_t *> *
bucket_get_known_slabs(bucket_t *bucket) {
    auto t = (DisjointPool::AllocImpl *)bucket->OwnAllocCtx;
    return &t->getKnownSlabs();
}

std::shared_timed_mutex *bucket_get_known_slabs_map_lock(bucket_t *bucket) {
    auto t = (DisjointPool::AllocImpl *)bucket->OwnAllocCtx;
    return &t->getKnownSlabsMapLock();
}

void slab_reg_by_addr(void *addr, slab_t *slab) {
    bucket_t *bucket = slab_get_bucket(slab);
    auto Lock = bucket_get_known_slabs_map_lock(bucket);
    auto Map = bucket_get_known_slabs(bucket);

    std::lock_guard<std::shared_timed_mutex> Lg(*Lock);
    Map->insert({addr, slab});
}

void slab_unreg_by_addr(void *addr, slab_t *slab) {
    bucket_t *bucket = slab_get_bucket(slab);
    auto Lock = bucket_get_known_slabs_map_lock(bucket);
    auto Map = bucket_get_known_slabs(bucket);

    std::lock_guard<std::shared_timed_mutex> Lg(*Lock);

    auto Slabs = Map->equal_range(addr);
    // At least the must get the current slab from the map.
    assert(Slabs.first != Slabs.second && "Slab is not found");

    for (auto It = Slabs.first; It != Slabs.second; ++It) {
        if (It->second == slab) {
            Map->erase(It);
            return;
        }
    }

    assert(false && "Slab is not found");
}

bool bucket_can_pool(bucket_t *bucket, bool *ToPool) {
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
        size_t PoolSize = bucket_get_limits(bucket)->TotalSize;
        while (true) {
            size_t NewPoolSize = PoolSize + bucket_slab_alloc_size(bucket);

            if (bucket_get_limits(bucket)->MaxSize < NewPoolSize) {
                break;
            }

            if (bucket_get_limits(bucket)->TotalSize.compare_exchange_strong(
                    PoolSize, NewPoolSize)) {
                if (chunkedBucket) {
                    ++bucket->chunkedSlabsInPool;
                }

                bucket_update_stats(bucket, -1, 1);
                *ToPool = true;
                return true;
            }
        }
    }

    bucket_update_stats(bucket, -1, 0);
    *ToPool = false;
    return false;
}

#ifdef __cplusplus
}
#endif
// end TODO remove
