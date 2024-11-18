# Copyright (C) 2024 Intel Corporation
# Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.TXT
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import random
from utils.utils import git_clone
from .base import Benchmark, Suite
from .result import Result
from utils.utils import run, create_build_path
from .options import options
import os
import csv
import io

def isUMFAvailable():
    return options.umf is not None

class UMFSuite(Suite):    
    def __init__(self, directory):
        self.directory = directory
        if not isUMFAvailable():
            print("UMF install prefix path not provided SUITE")
    
    def setup(self):
        if not isUMFAvailable():
            return
        self.built = True

    def benchmarks(self) -> list[Benchmark]:
        if not isUMFAvailable():
            return
        
        benches = [
            GBench(self),
        ]

        return benches

class ComputeUMFBenchmark(Benchmark):
    def __init__(self, bench, name):
        self.bench = bench
        self.bench_name = name
        super().__init__(bench.directory)

    def bin_args(self) -> list[str]:
        return []

    def extra_env_vars(self) -> dict:
        return {}

    def unit(self):
        return "μs"

    def setup(self):
        if not isUMFAvailable():
            print("UMF prefix path not provided BENCHMARK", options.umf)
            return

        self.benchmark_bin = os.path.join(options.umf, 'benchmark', self.bench_name)

    def run(self, env_vars) -> list[Result]:
        command = [
            f"{self.benchmark_bin}",
        #     f"--test={self.test}",
        #     "--csv",
        #     "--noHeaders"
        ]

        print("benchmark path", self.benchmark_bin)
        command += self.bin_args()
        env_vars.update(self.extra_env_vars())

        result = self.run_bench(command, env_vars)
        print("IN RUN --- RESULT\n", result)
        (label, mean) = self.parse_output(result)
        return [ Result(label=self.name(), value=mean, command=command, env=env_vars, stdout=result) ]

    def parse_output(self, output):
        csv_file = io.StringIO(output)
        # print("RESULT\n", csv_file.read())
        reader = csv.reader(csv_file)
        next(reader, None)
        data_row = next(reader, None)
        if data_row is None:
            raise ValueError("Benchmark output does not contain data.")
        try:
            label = data_row[0]
            mean = float(data_row[1])
            return (label, mean)
        except (ValueError, IndexError) as e:
            raise ValueError(f"Error parsing output: {e}")

    def teardown(self):
        return

# --benchmark_out_format=json --benchmark_out=./x    
class GBench(ComputeUMFBenchmark):
    def __init__(self, bench):
        super().__init__(bench, "umf-benchmark")

    def name(self):
        return self.bench_name

    def bin_args(self):
        return ["--benchmark_format=csv"]
    # --benchmark_format describes stdout output
    # --benchmark_out=<file> and --benchmark_out_format=<format>
    # describe output to a file 

    def unit(self):
        return "ns"
    # default unit
    # might be changed globally with --benchmark_time_unit={ns|us|ms|s}
    # the change affects only benchmark where time unit has not been set
    # explicitly
    

    