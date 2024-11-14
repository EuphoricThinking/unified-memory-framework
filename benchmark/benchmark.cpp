#include <iostream>

#include <benchmark/benchmark.h>
#include <umf/ipc.h>
#include <umf/memory_pool.h>
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

struct alloc_data {
    void *ptr;
    size_t size;
};

#define ALLOC_BECHMARK_DEFINE(A, B)                                            \
    BENCHMARK_DEFINE_F(A, B)                                                   \
    (benchmark::State & state) {                                               \
        for (auto _ : state) {                                                 \
            bench(state);                                                      \
        }                                                                      \
    }
#define ALLOC_BECHMARK_TEMPLATE_DEFINE(A, B, C)                                \
    BENCHMARK_TEMPLATE_DEFINE_F(A, B, C)                                       \
    (benchmark::State & state) {                                               \
        for (auto _ : state) {                                                 \
            bench(state);                                                      \
        }                                                                      \
    }

class allocBenchmark : public benchmark::Fixture {
  public:
    void SetUp(::benchmark::State &state) {
        if (state.thread_index() != 0) {
            return;
        }
        iterations = state.range(0);
        allocations.resize(state.threads());
        for (auto &i : allocations) {
            i.resize(iterations);
        }
    }

    void TearDown([[maybe_unused]] ::benchmark::State &state) {}

    void bench([[maybe_unused]] benchmark::State &state) {
        auto tid = state.thread_index();
        for (size_t i = 0; i < iterations; i++) {
            allocations[tid][i].ptr = benchAlloc(4096);
            if (allocations[tid][i].ptr == NULL) {
                state.SkipWithError("allocation failed");
            }
            allocations[tid][i].size = 4096;
        }
        for (size_t i = 0; i < iterations; i++) {
            benchFree(allocations[tid][i].ptr, allocations[tid][i].size);
        }
    }

  protected:
    std::vector<std::vector<alloc_data>> allocations;
    size_t iterations;

  private:
    virtual void *benchAlloc(size_t size) { return malloc(size); }
    virtual void benchFree(void *ptr, [[maybe_unused]] size_t size) {
        free(ptr);
    }
};

ALLOC_BECHMARK_DEFINE(allocBenchmark, stdmalloc);

BENCHMARK_REGISTER_F(allocBenchmark, stdmalloc)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000);

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

struct os_provider : public provider_interface {
    umf_os_memory_provider_params_t params = umfOsMemoryProviderParamsDefault();
    virtual void *getParams() override { return &params; }
    virtual umf_memory_provider_ops_t *getOps() override {
        return umfOsMemoryProviderOps();
    }
};

template <typename T,
          typename =
              std::enable_if_t<std::is_base_of<provider_interface, T>::value>>
class providerBenchmark : public allocBenchmark {
  public:
    void SetUp(::benchmark::State &state) {
        allocBenchmark::SetUp(state);
        provider.SetUp(state);
    }

    void TearDown([[maybe_unused]] ::benchmark::State &state) {
        allocBenchmark::TearDown(state);
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
ALLOC_BECHMARK_TEMPLATE_DEFINE(providerBenchmark, bench, os_provider);

BENCHMARK_REGISTER_F(providerBenchmark, bench)->Arg(100)->Arg(1000)->Arg(10000);

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

template <typename T> class poolBenchmark : public allocBenchmark {
  public:
    void SetUp(::benchmark::State &state) {
        allocBenchmark::SetUp(state);
        pool.SetUp(state);
    }

    void TearDown([[maybe_unused]] ::benchmark::State &state) {
        allocBenchmark::TearDown(state);
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

ALLOC_BECHMARK_TEMPLATE_DEFINE(poolBenchmark, bench, proxy_pool<os_provider>);
BENCHMARK_REGISTER_F(poolBenchmark, bench)->Arg(100)->Arg(1000)->Arg(10000);

#if (defined UMF_BUILD_LIBUMF_POOL_DISJOINT)

template <typename T> class disjoint_pool : public pool_interface<T> {
    umf_disjoint_pool_params_t disjoint_memory_pool_params;
    virtual umf_memory_pool_ops_t *
    getOps([[maybe_unused]] ::benchmark::State &state) override {
        return umfDisjointPoolOps();
    }
    virtual void *
    getParams([[maybe_unused]] ::benchmark::State &state) override {
        size_t page_size = 4096;
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

ALLOC_BECHMARK_TEMPLATE_DEFINE(poolBenchmark, disjoint_pool,
                               disjoint_pool<os_provider>);

BENCHMARK_REGISTER_F(poolBenchmark, disjoint_pool)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000);
#endif

#if (defined UMF_BUILD_LIBUMF_POOL_JEMALLOC)
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

ALLOC_BECHMARK_TEMPLATE_DEFINE(poolBenchmark, jemalloc_pool,
                               jemalloc_pool<os_provider>);

BENCHMARK_REGISTER_F(poolBenchmark, jemalloc_pool)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    //    ->ThreadPerCpu()
    ->ComputeStatistics("maxx", [](const std::vector<double> &v) -> double {
        return *(std::max_element(std::begin(v), std::end(v)));
    });

void BM_spin_empty(benchmark::State &state) {
    for (auto _ : state) {
        for (int x = 0; x < state.range(0); ++x) {
            benchmark::DoNotOptimize(x);
        }
    }
}

BENCHMARK(BM_spin_empty)
    ->ComputeStatistics("maxx",
                        [](const std::vector<double> &v) -> double {
                            return *(
                                std::max_element(std::begin(v), std::end(v)));
                        })
    ->Arg(512);
BENCHMARK_MAIN();
