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

#include "utils_concurrency.h"

// TODO remove
#ifdef __cplusplus
extern "C" {
#endif

#include "pool_disjoint_temp.h"

#ifdef __cplusplus
}
#endif
// end TODO remove

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
    return shared_limits_create(MaxSize);
}

void umfDisjointPoolSharedLimitsDestroy(
    umf_disjoint_pool_shared_limits_t *limits) {
    shared_limits_destroy(limits);
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

class DisjointPool::AllocImpl {
    // It's important for the map to be destroyed last after buckets and their
    // slabs This is because slab's destructor removes the object from the map.
    std::unordered_multimap<void *, slab_t *> KnownSlabs;

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

  public:
    AllocImpl(umf_memory_provider_handle_t hProvider,
              umf_disjoint_pool_params_t *params)
        : MemHandle{hProvider}, params(*params) {

        VALGRIND_DO_CREATE_MEMPOOL(this, 0, 0);

        utils_mutex_init(&known_slabs_map_lock);

        // Generate buckets sized such as: 64, 96, 128, 192, ..., CutOff.
        // Powers of 2 and the value halfway between the powers of 2.
        size_t Size1 = this->params.MinBucketSize;

        // MinBucketSize cannot be larger than CutOff.
        Size1 = std::min(Size1, CutOff);

        // Buckets sized smaller than the bucket default size- 8 aren't needed.
        Size1 = std::max(Size1, UMF_DISJOINT_POOL_MIN_BUCKET_DEFAULT_SIZE);

        // Calculate the exponent for MinBucketSize used for finding buckets.
        MinBucketSizeExp = (size_t)log2Utils(Size1);
        DefaultSharedLimits = shared_limits_create(SIZE_MAX);

        // count number of buckets, start from 1
        buckets_num = 1;
        auto Size2 = Size1 + Size1 / 2;
        size_t ts2 = Size2, ts1 = Size1;
        for (; Size2 < CutOff; Size1 *= 2, Size2 *= 2) {
            buckets_num += 2;
        }
        buckets =
            (bucket_t **)umf_ba_global_alloc(sizeof(bucket_t *) * buckets_num);

        int i = 0;
        Size1 = ts1;
        Size2 = ts2;
        for (; Size2 < CutOff; Size1 *= 2, Size2 *= 2, i += 2) {
            buckets[i] = create_bucket(Size1, this, this->getLimits());
            buckets[i + 1] = create_bucket(Size2, this, this->getLimits());
        }
        buckets[i] = create_bucket(CutOff, this, this->getLimits());

        auto ret = umfMemoryProviderGetMinPageSize(hProvider, nullptr,
                                                   &ProviderMinPageSize);
        if (ret != UMF_RESULT_SUCCESS) {
            ProviderMinPageSize = 0;
        }
    }

    ~AllocImpl() {
        // TODO
        // destroy DefaultSharedLimits

        for (size_t i = 0; i < buckets_num; i++) {
            destroy_bucket(buckets[i]);
        }

        VALGRIND_DO_DESTROY_MEMPOOL(this);

        utils_mutex_destroy_not_free(&known_slabs_map_lock);
    }

    void *allocate(size_t Size, size_t Alignment, bool &FromPool);
    void *allocate(size_t Size, bool &FromPool);
    umf_result_t deallocate(void *Ptr, bool &ToPool);

    umf_memory_provider_handle_t getMemHandle() { return MemHandle; }

    utils_mutex_t *getKnownSlabsMapLock() { return &known_slabs_map_lock; }

    std::unordered_multimap<void *, slab_t *> &getKnownSlabs() {
        return KnownSlabs;
    }

    size_t SlabMinSize() { return params.SlabMinSize; };

    umf_disjoint_pool_params_t &getParams() { return params; }

    umf_disjoint_pool_shared_limits_t *getLimits() {
        if (params.SharedLimits) {
            return params.SharedLimits;
        } else {
            return DefaultSharedLimits;
        }
    };

    void printStats(bool &TitlePrinted, size_t &HighBucketSize,
                    size_t &HighPeakSlabsInUse, const std::string &Label);

  private:
    bucket_t *findBucket(size_t Size);
    size_t sizeToIdx(size_t Size);
};

static void *memoryProviderAlloc(umf_memory_provider_handle_t hProvider,
                                 size_t size, size_t alignment = 0) {
    void *ptr;
    auto ret = umfMemoryProviderAlloc(hProvider, size, alignment, &ptr);
    if (ret != UMF_RESULT_SUCCESS) {
        umf::getPoolLastStatusRef<DisjointPool>() = ret;
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

    auto ret = umfMemoryProviderFree(hProvider, ptr, size);
    if (ret != UMF_RESULT_SUCCESS) {

        umf::getPoolLastStatusRef<DisjointPool>() = ret;
        // throw MemoryProviderError{ret};
        return ret;
    }
    return UMF_RESULT_SUCCESS;
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

void *DisjointPool::AllocImpl::allocate(size_t Size, bool &FromPool) {
    void *Ptr;

    if (Size == 0) {
        return nullptr;
    }

    FromPool = false;
    if (Size > getParams().MaxPoolableSize) {
        Ptr = memoryProviderAlloc(getMemHandle(), Size);

        if (Ptr == NULL) {
            // TODO get code from func
            umf::getPoolLastStatusRef<DisjointPool>() =
                UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
            return nullptr;
        }

        annotate_memory_undefined(Ptr, Size);
        return Ptr;
    }

    bucket_t *bucket = findBucket(Size);

    if (Size > bucket_chunk_cut_off(bucket)) {
        Ptr = bucket_get_slab(bucket, &FromPool);
    } else {
        Ptr = bucket_get_chunk(bucket, &FromPool);
    }

    if (Ptr == NULL) {
        // TODO get code from func
        umf::getPoolLastStatusRef<DisjointPool>() =
            UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
        return nullptr;
    }

    if (getParams().PoolTrace > 1) {
        bucket_count_alloc(bucket, FromPool);
    }

    VALGRIND_DO_MEMPOOL_ALLOC(this, Ptr, Size);
    annotate_memory_undefined(Ptr, bucket_get_size(bucket));

    return Ptr;
}

void *DisjointPool::AllocImpl::allocate(size_t Size, size_t Alignment,
                                        bool &FromPool) {
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
        assert(Ptr);
        annotate_memory_undefined(Ptr, Size);
        return Ptr;
    }

    bucket_t *bucket = findBucket(AlignedSize);

    if (AlignedSize > bucket_chunk_cut_off(bucket)) {
        Ptr = bucket_get_slab(bucket, &FromPool);
    } else {
        Ptr = bucket_get_chunk(bucket, &FromPool);
    }

    assert(Ptr);
    if (getParams().PoolTrace > 1) {
        bucket_count_alloc(bucket, FromPool);
    }

    VALGRIND_DO_MEMPOOL_ALLOC(this, ALIGN_UP((size_t)Ptr, Alignment), Size);
    annotate_memory_undefined((void *)ALIGN_UP((size_t)Ptr, Alignment), Size);
    return (void *)ALIGN_UP((size_t)Ptr, Alignment);
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
    bucket_t *bucket = buckets[calculatedIdx];
    assert(bucket_get_size(bucket) >= Size);
    (void)bucket;

    if (calculatedIdx > 0) {
        bucket_t *bucket_prev = buckets[calculatedIdx - 1];
        assert(bucket_get_size(bucket_prev) < Size);
        (void)bucket_prev;
    }

    return buckets[calculatedIdx];
}

umf_result_t DisjointPool::AllocImpl::deallocate(void *Ptr, bool &ToPool) {
    if (Ptr == nullptr) {
        return UMF_RESULT_SUCCESS;
    }

    auto *SlabPtr = (void *)ALIGN_DOWN((size_t)Ptr, SlabMinSize());

    // Lock the map on read
    utils_mutex_lock(getKnownSlabsMapLock());

    ToPool = false;
    auto Slabs = getKnownSlabs().equal_range(SlabPtr);
    if (Slabs.first == Slabs.second) {
        utils_mutex_unlock(getKnownSlabsMapLock());
        umf_result_t ret = memoryProviderFree(getMemHandle(), Ptr);
        return ret;
    }

    for (auto It = Slabs.first; It != Slabs.second; ++It) {
        // The slab object won't be deleted until it's removed from the map which is
        // protected by the lock, so it's safe to access it here.
        auto &Slab = It->second;
        if (Ptr >= slab_get(Slab) && Ptr < slab_get_end(Slab)) {
            // Unlock the map before freeing the chunk, it may be locked on write
            // there
            utils_mutex_unlock(getKnownSlabsMapLock());
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

            return UMF_RESULT_SUCCESS;
        }
    }

    utils_mutex_unlock(getKnownSlabsMapLock());
    // There is a rare case when we have a pointer from system allocation next
    // to some slab with an entry in the map. So we find a slab
    // but the range checks fail.
    memoryProviderFree(getMemHandle(), Ptr);
    return UMF_RESULT_SUCCESS;
}

void DisjointPool::AllocImpl::printStats(bool &TitlePrinted,
                                         size_t &HighBucketSize,
                                         size_t &HighPeakSlabsInUse,
                                         const std::string &MTName) {
    (void)TitlePrinted; // TODO
    (void)MTName;       // TODO

    HighBucketSize = 0;
    HighPeakSlabsInUse = 0;
    for (size_t i = 0; i < buckets_num; i++) {
        // TODO
        //(*B).printStats(TitlePrinted, MTName);
        bucket_t *bucket = buckets[i];
        HighPeakSlabsInUse =
            utils_max(bucket->maxSlabsInUse, HighPeakSlabsInUse);
        if (bucket->allocCount) {
            HighBucketSize =
                utils_max(bucket_slab_alloc_size(bucket), HighBucketSize);
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

umf_result_t DisjointPool::free(void *ptr) {
    bool ToPool;
    umf_result_t ret = impl->deallocate(ptr, ToPool);
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

umf_result_t DisjointPool::get_last_allocation_error() {
    return umf::getPoolLastStatusRef<DisjointPool>();
}

DisjointPool::DisjointPool() {}

// Define destructor for use with unique_ptr
DisjointPool::~DisjointPool() {
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

umf_memory_provider_handle_t bucket_get_mem_handle(bucket_t *bucket) {
    auto t = (DisjointPool::AllocImpl *)bucket->OwnAllocCtx;
    return t->getMemHandle();
}

std::unordered_multimap<void *, slab_t *> *
bucket_get_known_slabs(bucket_t *bucket) {
    auto t = (DisjointPool::AllocImpl *)bucket->OwnAllocCtx;
    return &t->getKnownSlabs();
}

utils_mutex_t *bucket_get_known_slabs_map_lock(bucket_t *bucket) {
    auto t = (DisjointPool::AllocImpl *)bucket->OwnAllocCtx;
    return t->getKnownSlabsMapLock();
}

void slab_reg_by_addr(void *addr, slab_t *slab) {
    bucket_t *bucket = slab_get_bucket(slab);
    auto Lock = bucket_get_known_slabs_map_lock(bucket);
    auto Map = bucket_get_known_slabs(bucket);

    utils_mutex_lock(Lock);
    Map->insert({addr, slab});
    utils_mutex_unlock(Lock);
}

void slab_unreg_by_addr(void *addr, slab_t *slab) {
    bucket_t *bucket = slab_get_bucket(slab);
    auto Lock = bucket_get_known_slabs_map_lock(bucket);
    auto Map = bucket_get_known_slabs(bucket);

    utils_mutex_lock(Lock);

    auto Slabs = Map->equal_range(addr);
    // At least the must get the current slab from the map.
    assert(Slabs.first != Slabs.second && "Slab is not found");

    for (auto It = Slabs.first; It != Slabs.second; ++It) {
        if (It->second == slab) {
            Map->erase(It);
            utils_mutex_unlock(Lock);
            return;
        }
    }

    assert(false && "Slab is not found");
    utils_mutex_unlock(Lock);
}

#ifdef __cplusplus
}
#endif
// end TODO remove
