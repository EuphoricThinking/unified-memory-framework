/*
 * Copyright (C) 2024 Intel Corporation
 *
 * Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 */

#include <benchmark/benchmark.h>
#include <umf/pools/pool_proxy.h>
#include <umf/pools/pool_scalable.h>
#include <umf/providers/provider_level_zero.h>
#include <umf/providers/provider_os_memory.h>

#ifdef UMF_BUILD_LIBUMF_POOL_DISJOINT
#include <umf/pools/pool_disjoint.h>
#endif

#ifdef UMF_BUILD_LIBUMF_POOL_JEMALLOC
#include <umf/pools/pool_jemalloc.h>
#endif

#include "benchmark.hpp"

struct os_provider : public provider_interface {
    umf_os_memory_provider_params_t params = umfOsMemoryProviderParamsDefault();
    virtual void *getParams() override { return &params; }
    virtual umf_memory_provider_ops_t *getOps() override {
        return umfOsMemoryProviderOps();
    }
};

template <typename T> class proxy_pool : public pool_interface<T> {
  private:
    virtual umf_memory_pool_ops_t *
    getOps([[maybe_unused]] ::benchmark::State &state) override {
        return umfProxyPoolOps();
    }
    virtual void *
    getParams([[maybe_unused]] ::benchmark::State &state) override {
        return nullptr;
    }
};

#ifdef UMF_BUILD_LIBUMF_POOL_DISJOINT
template <typename T> class disjoint_pool : public pool_interface<T> {
    umf_disjoint_pool_params_t disjoint_memory_pool_params;
    virtual umf_memory_pool_ops_t *
    getOps([[maybe_unused]] ::benchmark::State &state) override {
        return umfDisjointPoolOps();
    }
    virtual void *
    getParams([[maybe_unused]] ::benchmark::State &state) override {
        size_t page_size;
        if (umfMemoryProviderGetMinPageSize(
                pool_interface<T>::provider.provider, NULL, &page_size) !=
            UMF_RESULT_SUCCESS) {
            return NULL;
        }
        disjoint_memory_pool_params.SlabMinSize = page_size;
        disjoint_memory_pool_params.MaxPoolableSize = page_size * 2;
        disjoint_memory_pool_params.Capacity = state.range(0);

        disjoint_memory_pool_params.MinBucketSize = page_size;
        return &disjoint_memory_pool_params;
    }
};
#endif

#ifdef UMF_BUILD_LIBUMF_POOL_JEMALLOC
template <typename T> class jemalloc_pool : public pool_interface<T> {
  private:
    virtual umf_memory_pool_ops_t *
    getOps([[maybe_unused]] ::benchmark::State &state) {
        return umfJemallocPoolOps();
    }
    virtual void *getParams([[maybe_unused]] ::benchmark::State &state) {
        return NULL;
    }
};
#endif

template <typename T> class scalable_pool : public pool_interface<T> {
  private:
    virtual umf_memory_pool_ops_t *
    getOps([[maybe_unused]] ::benchmark::State &state) {
        return umfScalablePoolOps();
    }
    virtual void *getParams([[maybe_unused]] ::benchmark::State &state) {
        return NULL;
    }
};

// Benchmarks scenarios:

ALLOC_BENCHMARK_TEMPLATE_DEFINE(alloc_benchmark, stdmalloc, fix_alloc_size);
BENCHMARK_REGISTER_F(alloc_benchmark, stdmalloc)->Args({1000, 4096});
BENCHMARK_REGISTER_F(alloc_benchmark, stdmalloc)->Args({1000, 10 * 4096});

ALLOC_BENCHMARK_TEMPLATE_DEFINE(provider_benchmark, bench, os_provider,
                                fix_alloc_size);
BENCHMARK_REGISTER_F(provider_benchmark, bench)
    ->Args({1000, 4096})
    ->Args({1000, 100 * 4096});

ALLOC_BENCHMARK_TEMPLATE_DEFINE(pool_benchmark, bench, proxy_pool<os_provider>,
                                fix_alloc_size);

BENCHMARK_REGISTER_F(pool_benchmark, bench)->Args({1000, 4096});

#ifdef UMF_BUILD_LIBUMF_POOL_DISJOINT
ALLOC_BENCHMARK_TEMPLATE_DEFINE(pool_benchmark, disjoint_pool,
                                disjoint_pool<os_provider>, fix_alloc_size);
BENCHMARK_REGISTER_F(pool_benchmark, disjoint_pool)->Args({1000, 4096});
#endif

#ifdef UMF_BUILD_LIBUMF_POOL_JEMALLOC
ALLOC_BENCHMARK_TEMPLATE_DEFINE(pool_benchmark, jemalloc_pool,
                                jemalloc_pool<os_provider>, fix_alloc_size);
BENCHMARK_REGISTER_F(pool_benchmark, jemalloc_pool)->Args({1000, 4096});

ALLOC_BENCHMARK_TEMPLATE_DEFINE(pool_benchmark, jemalloc_pool_size,
                                jemalloc_pool<os_provider>, uniform_alloc_size);

BENCHMARK_REGISTER_F(pool_benchmark, jemalloc_pool_size)
    ->Args({1000, 1, 5, 4096});
#endif

ALLOC_BENCHMARK_TEMPLATE_DEFINE(pool_benchmark, scalable_pool_size,
                                scalable_pool<os_provider>, uniform_alloc_size);

BENCHMARK_REGISTER_F(pool_benchmark, scalable_pool_size)
    ->Args({1000, 1, 5, 4096});

BENCHMARK_MAIN();
