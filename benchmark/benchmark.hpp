/*
 * Copyright (C) 2024 Intel Corporation
 *
 * Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 */

#include <benchmark/benchmark.h>
#include <random>
#include <umf/memory_pool.h>
#include <umf/memory_provider.h>

struct alloc_data {
    void *ptr;
    size_t size;
};

#define ALLOC_BENCHMARK_TEMPLATE_DEFINE(BaseClass, Method, ...)                \
    BENCHMARK_TEMPLATE_DEFINE_F(BaseClass, Method, __VA_ARGS__)                \
    (benchmark::State & state) {                                               \
        for (auto _ : state) {                                                 \
            bench(state);                                                      \
        }                                                                      \
    }

class alloc_size_interface {
  public:
    virtual void SetUp([[maybe_unused]] ::benchmark::State &state,
                       [[maybe_unused]] unsigned r) {}
    virtual void TearDown([[maybe_unused]] ::benchmark::State &state) {}
    virtual size_t nextSize() = 0;
};

class fix_alloc_size : public alloc_size_interface {
  public:
    virtual void SetUp(::benchmark::State &state, unsigned r) {
        size = state.range(r);
    }
    size_t nextSize() override { return size; };

  private:
    size_t size;
};

class uniform_alloc_size : public alloc_size_interface {
    using distribution = std::uniform_int_distribution<int>;

  public:
    virtual void SetUp(::benchmark::State &state, unsigned r) {
        dist.param(
            distribution::param_type(state.range(r), state.range(r + 1)));
        multiply = state.range(r + 2);
    }

    size_t nextSize() { return dist(generator) * multiply; }

  private:
    std::default_random_engine generator;
    distribution dist;
    size_t multiply;
};

struct provider_interface {
    umf_memory_provider_handle_t provider = NULL;
    virtual void SetUp(::benchmark::State &state) {
        if (state.thread_index() != 0) {
            return;
        }
        auto umf_result =
            umfMemoryProviderCreate(getOps(), getParams(), &provider);
        if (umf_result != UMF_RESULT_SUCCESS) {
            state.SkipWithError("umfMemoryProviderCreate() failed");
        }
    }

    virtual void TearDown([[maybe_unused]] ::benchmark::State &state) {
        if (state.thread_index() != 0) {
            return;
        }

        if (provider) {
            umfMemoryProviderDestroy(provider);
        }
    }

    virtual umf_memory_provider_ops_t *getOps() { return nullptr; }
    virtual void *getParams() { return nullptr; }
};

template <typename T,
          typename =
              std::enable_if_t<std::is_base_of<provider_interface, T>::value>>
struct pool_interface {
    virtual void SetUp(::benchmark::State &state) {
        provider.SetUp(state);
        if (state.thread_index() != 0) {
            return;
        }
        auto umf_result = umfPoolCreate(getOps(state), provider.provider,
                                        getParams(state), 0, &pool);
        if (umf_result != UMF_RESULT_SUCCESS) {
            state.SkipWithError("umfPoolCreate() failed");
        }
    }
    virtual void TearDown([[maybe_unused]] ::benchmark::State &state) {
        if (state.thread_index() != 0) {
            return;
        }
        if (pool) {
            umfPoolDestroy(pool);
        }
    };

    virtual umf_memory_pool_ops_t *
    getOps([[maybe_unused]] ::benchmark::State &state) {
        return nullptr;
    }
    virtual void *getParams([[maybe_unused]] ::benchmark::State &state) {
        return nullptr;
    }

    T provider;
    umf_memory_pool_handle_t pool;
};

template <typename S,
          typename =
              std::enable_if_t<std::is_base_of<alloc_size_interface, S>::value>>
class alloc_benchmark : public benchmark::Fixture {
  public:
    void SetUp(::benchmark::State &state) {
        if (state.thread_index() != 0) {
            return;
        }
        iterations = state.range(0);
        size.SetUp(state, 1);
        allocations.resize(state.threads());
        for (auto &i : allocations) {
            i.resize(iterations);
        }
    }

    void TearDown([[maybe_unused]] ::benchmark::State &state) {}

    void bench(benchmark::State &state) {
        auto tid = state.thread_index();
        for (size_t i = 0; i < iterations; i++) {
            auto s = size.nextSize();
            allocations[tid][i].ptr = benchAlloc(s);
            if (allocations[tid][i].ptr == NULL) {
                state.SkipWithError("allocation failed");
            }
            allocations[tid][i].size = s;
        }
        for (size_t i = 0; i < iterations; i++) {
            benchFree(allocations[tid][i].ptr, allocations[tid][i].size);
        }
    }

  protected:
    std::vector<std::vector<alloc_data>> allocations;
    size_t iterations;

  private:
    S size;
    virtual void *benchAlloc(size_t size) { return malloc(size); }
    virtual void benchFree(void *ptr, [[maybe_unused]] size_t size) {
        free(ptr);
    }
};

// TODO: implement jemalloc benchmark without umf
// #include <dlfcn.h>
// template <typename S,
//           typename =
//               std::enable_if_t<std::is_base_of<alloc_size_interface, S>::value>>
// class jemallocBenchmark : public allocBenchmark<S> {
//     void SetUp(::benchmark::State &state) {
//         allocBenchmark<S>::SetUp(state);
//         jemalloc_handle = dlopen("libjemalloc.so", RTLD_NOW);
//         malloc_func = (void *(*)(size_t))dlsym(jemalloc_handle, "malloc");
//         free_func = (void (*)(void *))dlsym(jemalloc_handle, "free");
//     }

//     void TearDown([[maybe_unused]] ::benchmark::State &state) {
//         dlclose(jemalloc_handle);
//     }

//   private:
//     void *(*malloc_func)(size_t);
//     void (*free_func)(void *);
//     void *jemalloc_handle;
//     virtual void *benchAlloc(size_t size) override { return malloc_func(size); }
//     virtual void benchFree(void *ptr, [[maybe_unused]] size_t size) override {
//         free_func(ptr);
//     }
// };

template <typename T, typename S,
          typename =
              std::enable_if_t<std::is_base_of<provider_interface, T>::value>,
          typename =
              std::enable_if_t<std::is_base_of<alloc_size_interface, S>::value>>
class provider_benchmark : public alloc_benchmark<S> {
  public:
    void SetUp(::benchmark::State &state) {
        alloc_benchmark<S>::SetUp(state);
        provider.SetUp(state);
    }

    void TearDown(::benchmark::State &state) {
        alloc_benchmark<S>::TearDown(state);
        provider.TearDown(state);
    }

  protected:
    T provider;

  private:
    virtual void *benchAlloc(size_t size) override {
        void *ptr;
        if (umfMemoryProviderAlloc(provider.provider, size, 0, &ptr) !=
            UMF_RESULT_SUCCESS) {
            return NULL;
        }
        return ptr;
    }
    virtual void benchFree(void *ptr, size_t size) override {
        umfMemoryProviderFree(provider.provider, ptr, size);
    }
};

// TODO: assert T to be a pool_interface<provider_interface>.
template <typename T, typename S,
          typename =
              std::enable_if_t<std::is_base_of<alloc_size_interface, S>::value>>
class pool_benchmark : public alloc_benchmark<S> {
  public:
    void SetUp(::benchmark::State &state) {
        alloc_benchmark<S>::SetUp(state);
        pool.SetUp(state);
    }

    void TearDown(::benchmark::State &state) {
        alloc_benchmark<S>::TearDown(state);
        pool.TearDown(state);
    }

  protected:
    T pool;

  private:
    virtual void *benchAlloc(size_t size) override {
        return umfPoolMalloc(pool.pool, size);
    }
    virtual void benchFree(void *ptr, [[maybe_unused]] size_t size) override {
        umfPoolFree(pool.pool, ptr);
    }
};
