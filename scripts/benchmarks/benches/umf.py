# Copyright (C) 2024-2025 Intel Corporation
# Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.TXT
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import random
from utils.utils import git_clone
from .base import Benchmark, Suite
from utils.result import Result
from utils.utils import run, create_build_path
from options import options
from utils.oneapi import get_oneapi
import os
import csv
import io


def isUMFAvailable():
    return options.umf is not None


class UMFSuite(Suite):
    def __init__(self, directory):
        self.directory = directory

    def name(self) -> str:
        return "UMF"

    def setup(self):
        if not isUMFAvailable():
            return []
        self.built = True

    def benchmarks(self) -> list[Benchmark]:
        if not isUMFAvailable():
            return []

        benches = [
            GBench(self),
            GBenchUmfProxy(self),
            GBenchJemalloc(self),
            GBenchTbbProxy(self),
            GBenchMemoryOverhead(self),
        ]

        return benches


class ComputeUMFBenchmark(Benchmark):
    def __init__(self, bench, name):
        super().__init__(bench.directory, bench)

        self.bench = bench
        self.bench_name = name
        self.oneapi = get_oneapi()

        self.col_name = None
        self.col_iterations = None
        self.col_real_time = None
        self.col_cpu_time = None
        self.col_time_unit = None

        self.col_statistics_time = None

    def bin_args(self) -> list[str]:
        return []

    def extra_env_vars(self) -> dict:
        return {}

    def setup(self):
        if not isUMFAvailable():
            print("UMF prefix path not provided")
            return

        self.benchmark_bin = os.path.join(options.umf, "benchmark", self.bench_name)

    def run(self, env_vars) -> list[Result]:
        command = [
            f"{self.benchmark_bin}"
        ]

        list_all_command = command + ["--benchmark_list_tests"]
        umf_lib = options.umf + "lib"
        # print("DIRECOTRY", umf_lib)
        all_names = self.run_bench(
            list_all_command, env_vars, add_sycl=False, ld_library=[umf_lib] #self.oneapi.tbb_lib()]
        )

        # print("all names:", all_names)

        command += self.bin_args()
        env_vars.update(self.extra_env_vars())

        results = []

        # print(all_names.splitlines())

        for name in all_names.splitlines():
            specific_benchmark = command + ["--benchmark_filter=" + name]
            print(specific_benchmark)

            result = self.run_bench(
                specific_benchmark, env_vars, add_sycl=False, ld_library=[umf_lib] #self.oneapi.tbb_lib()]
            )

            parsed = self.parse_output(result)
            for r in parsed:
                (config, pool, mean) = r
                label = f"{config} {pool}"
                results.append(
                    Result(
                        label=label,
                        value=mean,
                        command=command,
                        env=env_vars,
                        stdout=result,
                        unit=self.unit(), #"ns",
                        explicit_group=config,
                    )
                )
        return results

    # Implementation with self.col_* indices could lead to the division by None
    def get_mean(self, datarow):
        raise NotImplementedError()

    def teardown(self):
        return


class GBench(ComputeUMFBenchmark):
    def __init__(self, bench):
        super().__init__(bench, "umf-benchmark")

        self.is_preloaded = False
        self.is_memory_overhead_checked = False

        self.num_cols_with_memory = 13

        self.col_name = 0
        self.col_iterations = 1
        self.col_real_time = 2
        self.col_cpu_time = 3
        self.col_time_unit = 4
        self.col_memory_overhead = 11

        self.idx_pool = 0
        self.idx_config = 1
        self.name_separator = "/"

        self.col_statistics_time = self.col_real_time

    def is_memory_statistics_included(self, data_row):
        return len(data_row) == self.num_cols_with_memory

    def name(self):
        return self.bench_name

    # --benchmark_format describes stdout output
    # --benchmark_out=<file> and --benchmark_out_format=<format>
    # describe output to a file
    def bin_args(self):
        return ["--benchmark_format=csv"]

    # the default unit
    # might be changed globally with --benchmark_time_unit={ns|us|ms|s}
    # the change affects only benchmark where time unit has not been set
    # explicitly
    def unit(self):
        return "ns"

    # these benchmarks are not stable, so set this at a large value
    def stddev_threshold(self) -> float:
        return 0.2  # 20%

    def get_pool_and_config(self, full_name):
        list_split = full_name.split(self.name_separator, 1)
        if len(list_split) != 2:
            raise ValueError("Incorrect benchmark name format: ", full_name)

        return list_split[self.idx_pool], list_split[self.idx_config]

    def get_mean(self, datarow):
        return float(datarow[self.col_statistics_time])

    def get_memory_overhead(self, datarow):
        return float(datarow[self.col_memory_overhead])


    # def parse_output(self, output):
    #     csv_file = io.StringIO(output)
    #     reader = csv.reader(csv_file)

    #     data_row = next(reader, None)
    #     if data_row is None:
    #         raise ValueError("Benchmark output does not contain data.")

    #     results = []
    #     for row in reader:
    #         try:
    #             full_name = row[self.col_name]
    #             pool, config = self.get_pool_and_config(full_name)
    #             mean = self.get_mean(row)
    #             results.append((config, pool, mean))
    #         except KeyError as e:
    #             raise ValueError(f"Error parsing output: {e}")

    #     return results
    
    def parse_output(self, output):
        csv_file = io.StringIO(output)
        reader = csv.reader(csv_file)

        data_row = next(reader, None)
        if data_row is None:
            raise ValueError("Benchmark output does not contain data.")

        results = []
        print("len data row", len(data_row), "\ndata row:\n", data_row)
        for row in reader:
            try:
                full_name = row[self.col_name]
                pool, config = self.get_pool_and_config(full_name)
                statistics = None
                is_row_matched_to_statistics_type = False

                if not self.is_memory_overhead_checked \
                and not self.is_memory_statistics_included(row):
                    statistics = self.get_mean(row)
                    is_row_matched_to_statistics_type = True

                    # At this moment, preloaded benchmarks 
                    # do not support memory statitics
                    if self.is_preloaded:
                        pool = self.get_preloaded_name(pool)
                        config = self.get_preloaded_name(config)

                if self.is_memory_overhead_checked \
                and self.is_memory_statistics_included(row):
                    statistics = self.get_memory_overhead(row)
                    is_row_matched_to_statistics_type = True

                if is_row_matched_to_statistics_type:
                    results.append((config, pool, statistics))

            except KeyError as e:
                raise ValueError(f"Error parsing output: {e}")

        return results


class GBenchPreloaded(GBench):
    def __init__(self, bench, lib_to_be_replaced, replacing_lib):
        super().__init__(bench)

        self.is_preloaded = True
        
        self.lib_to_be_replaced = lib_to_be_replaced
        self.replacing_lib = replacing_lib

    def bin_args(self):
        full_args = super().bin_args()
        full_args.append(f"--benchmark_filter={self.lib_to_be_replaced}")

        return full_args

    def get_preloaded_name(self, pool_name) -> str:
        new_pool_name = pool_name.replace(self.lib_to_be_replaced, self.replacing_lib)

        return new_pool_name

    # def parse_output(self, output):
    #     csv_file = io.StringIO(output)
    #     reader = csv.reader(csv_file)

    #     data_row = next(reader, None)
    #     if data_row is None:
    #         raise ValueError("Benchmark output does not contain data.")

    #     results = []
    #     for row in reader:
    #         try:
    #             full_name = row[self.col_name]
    #             pool, config = self.get_pool_and_config(full_name)
    #             mean = self.get_mean(row)
    #             updated_pool = self.get_preloaded_name(pool)
    #             updated_config = self.get_preloaded_name(config)

    #             results.append((updated_config, updated_pool, mean))
    #         except KeyError as e:
    #             raise ValueError(f"Error parsing output: {e}")

    #     return results


class GBenchGlibc(GBenchPreloaded):
    def __init__(self, bench, replacing_lib):
        super().__init__(bench, lib_to_be_replaced="glibc", replacing_lib=replacing_lib)


class GBenchUmfProxy(GBenchGlibc):
    def __init__(self, bench):
        super().__init__(bench, replacing_lib="umfProxy")

    def extra_env_vars(self) -> dict:
        umf_proxy_path = os.path.join(options.umf, "lib", "libumf_proxy.so")
        return {"LD_PRELOAD": umf_proxy_path}


class GBenchJemalloc(GBenchGlibc):
    def __init__(self, bench):
        super().__init__(bench, replacing_lib="jemalloc")

    def extra_env_vars(self) -> dict:
        return {"LD_PRELOAD": "libjemalloc.so"}


class GBenchTbbProxy(GBenchGlibc):
    def __init__(self, bench):
        super().__init__(bench, replacing_lib="tbbProxy")

    def extra_env_vars(self) -> dict:
        return {"LD_PRELOAD": "libtbbmalloc_proxy.so"}

class GBenchMemoryOverhead(GBench):
    def __init__(self, bench):
        super().__init__(bench)

        self.is_memory_overhead_checked = True

    def unit(self):
        return "%"
