#!/usr/bin/env python3

# Copyright (C) 2024 Intel Corporation
# Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.TXT
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from benches.compute import *
from benches.velocity import VelocityBench
from benches.syclbench import *
from benches.llamacpp import *
from benches.test import TestSuite
from benches.options import Compare, options
from output_markdown import generate_markdown
from output_html import generate_html
from history import BenchmarkHistory
from benches.umf import *
from utils.utils import prepare_workdir;

import argparse
import re
import statistics

# Update this if you are changing the layout of the results files
INTERNAL_WORKDIR_VERSION = '2.0'

def run_iterations(benchmark: Benchmark, env_vars, iters: int, results: dict[str, list[Result]]):
    for iter in range(iters):
        print(f"running {benchmark.name()}, iteration {iter}... ", end='', flush=True)
        bench_results = benchmark.run(env_vars)
        if bench_results is None:
            print(f"did not finish (OK for sycl-bench).")
            break

        for bench_result in bench_results:
            # TODO: report failures in markdown/html ?
            if not bench_result.passed:
                print(f"complete ({bench_result.label}: verification FAILED)")
                continue

            print(f"complete ({bench_result.label}: {bench_result.value:.3f} {bench_result.unit}).")

            bench_result.name = bench_result.label
            bench_result.lower_is_better = benchmark.lower_is_better()

            if bench_result.label not in results:
                results[bench_result.label] = []

            results[bench_result.label].append(bench_result)

# https://www.statology.org/modified-z-score/
def modified_z_score(values: list[float]) -> list[float]:
    median = statistics.median(values)
    mad = statistics.median([abs(v - median) for v in values])
    if mad == 0:
        return [0] * len(values)
    return [(0.6745 * (v - median)) / mad for v in values]

def remove_outliers(results: dict[str, list[Result]], threshold: float = 3.5) -> dict[str, list[Result]]:
    new_results = {}
    for key, rlist in results.items():
        # don't eliminate outliers on first pass
        if len(rlist) <= options.iterations:
            new_results[key] = rlist
            continue

        values = [r.value for r in rlist]
        z_scores = modified_z_score(values)
        filtered_rlist = [r for r, z in zip(rlist, z_scores) if abs(z) <= threshold]

        if not filtered_rlist:
            new_results[key] = rlist
        else:
            new_results[key] = filtered_rlist

    return new_results

def process_results(results: dict[str, list[Result]]) -> tuple[bool, list[Result]]:
    processed: list[Result] = []
    # technically, we can detect whether result is below or above threshold per
    # individual result. However, we can't repeat benchmark runs with that
    # granularity. So we just reject all results and try again.
    valid_results = True # above stddev threshold

    for label, rlist in remove_outliers(results).items():
        if (len(rlist) == 0):
            continue

        if len(rlist) == 1:
            processed.append(rlist[0])
            continue

        values = [r.value for r in rlist]

        mean_value = statistics.mean(values)
        stddev = statistics.stdev(values)

        threshold = options.stddev_threshold * mean_value

        if stddev > threshold:
            print(f"stddev {stddev} above the threshold {threshold} for {label}")
            valid_results = False

        rlist.sort(key=lambda res: res.value)
        median_index = len(rlist) // 2
        median_result = rlist[median_index]
        median_result.stddev = stddev

        processed.append(median_result)

    return valid_results, processed

def main(directory, additional_env_vars, save_name, compare_names, filter):
    prepare_workdir(directory, INTERNAL_WORKDIR_VERSION)

    suites = [
        # ComputeBench(directory),
        # VelocityBench(directory),
        # SyclBench(directory),
        # LlamaCppBench(directory),
        UMFSuite(directory),
        #TestSuite()
    ] if not options.dry_run else []

    benchmarks = []

    for s in suites:
        print(f"Setting up {type(s).__name__}")
        s.setup()
        print(f"{type(s).__name__} setup complete.")

    for s in suites:
        benchmarks += s.benchmarks()

    if filter:
        benchmarks = [benchmark for benchmark in benchmarks if filter.search(benchmark.name())]

    for b in benchmarks:
        print(b.name())

    for benchmark in benchmarks:
        try:
            print(f"Setting up {benchmark.name()}... ")
            benchmark.setup()
            print(f"{benchmark.name()} setup complete.")

        except Exception as e:
            if options.exit_on_failure:
                raise e
            else:
                print(f"failed: {e}")

    results = []
    for benchmark in benchmarks:
        try:
            merged_env_vars = {**additional_env_vars}
            intermediate_results: dict[str, list[Result]] = {}
            processed: list[Result] = []
            for _ in range(5):
                run_iterations(benchmark, merged_env_vars, options.iterations, intermediate_results)
                valid, processed = process_results(intermediate_results)
                if valid:
                    break
            results += processed

        except Exception as e:
            if options.exit_on_failure:
                raise e
            else:
                print(f"failed: {e}")

    for benchmark in benchmarks:
        print(f"tearing down {benchmark.name()}... ", end='', flush=True)
        benchmark.teardown()
        print("complete.")

    this_name = "This PR"

    chart_data = {this_name : results}

    history = BenchmarkHistory(directory)
    # limit how many files we load.
    # should this be configurable?
    history.load(1000)

    for name in compare_names:
        compare_result = history.get_compare(name)
        if compare_result:
            chart_data[name] = compare_result.results

    if options.output_markdown:
        markdown_content = generate_markdown(this_name, chart_data)

        with open('benchmark_results.md', 'w') as file:
            file.write(markdown_content)

        print(f"Markdown with benchmark results has been written to {os.getcwd()}/benchmark_results.md")

    saved_name = save_name if save_name is not None else this_name

    # It's important we don't save the current results into history before
    # we calculate historical averages or get latest results for compare.
    # Otherwise we might be comparing the results to themselves.
    if not options.dry_run:
        history.save(saved_name, results, save_name is not None)
        compare_names.append(saved_name)

    if options.output_html:
        html_content = generate_html(history.runs, 'oneapi-src/unified-runtime', compare_names)

        with open('benchmark_results.html', 'w') as file:
            file.write(html_content)

        print(f"HTML with benchmark results has been written to {os.getcwd()}/benchmark_results.html")

def validate_and_parse_env_args(env_args):
    env_vars = {}
    for arg in env_args:
        if '=' not in arg:
            raise ValueError(f"Environment variable argument '{arg}' is not in the form Variable=Value.")
        key, value = arg.split('=', 1)
        env_vars[key] = value
    return env_vars

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Unified Runtime Benchmark Runner')
    parser.add_argument('benchmark_directory', type=str, help='Working directory to setup benchmarks.')
    parser.add_argument('--sycl', type=str, help='Root directory of the SYCL compiler.', default=None)
    parser.add_argument('--ur', type=str, help='UR install prefix path', default=None)
    parser.add_argument('--umf', type=str, help='UMF install prefix path', default=None)
    parser.add_argument('--adapter', type=str, help='Options to build the Unified Runtime as part of the benchmark', default="level_zero")
    parser.add_argument("--no-rebuild", help='Rebuild the benchmarks from scratch.', action="store_true")
    parser.add_argument("--env", type=str, help='Use env variable for a benchmark run.', action="append", default=[])
    parser.add_argument("--save", type=str, help='Save the results for comparison under a specified name.')
    parser.add_argument("--compare", type=str, help='Compare results against previously saved data.', action="append", default=["baseline"])
    parser.add_argument("--iterations", type=int, help='Number of times to run each benchmark to select a median value.', default=options.iterations)
    parser.add_argument("--stddev-threshold", type=float, help='If stddev % is above this threshold, rerun all iterations', default=options.stddev_threshold)
    parser.add_argument("--timeout", type=int, help='Timeout for individual benchmarks in seconds.', default=options.timeout)
    parser.add_argument("--filter", type=str, help='Regex pattern to filter benchmarks by name.', default=None)
    parser.add_argument("--epsilon", type=float, help='Threshold to consider change of performance significant', default=options.epsilon)
    parser.add_argument("--verbose", help='Print output of all the commands.', action="store_true")
    parser.add_argument("--exit-on-failure", help='Exit on first failure.', action="store_true")
    parser.add_argument("--compare-type", type=str, choices=[e.value for e in Compare], help='Compare results against previously saved data.', default=Compare.LATEST.value)
    parser.add_argument("--compare-max", type=int, help='How many results to read for comparisions', default=options.compare_max)
    parser.add_argument("--output-html", help='Create HTML output', action="store_true", default=False)
    parser.add_argument("--output-markdown", help='Create Markdown output', action="store_true", default=True)
    parser.add_argument("--dry-run", help='Do not run any actual benchmarks', action="store_true", default=False)

    args = parser.parse_args()
    additional_env_vars = validate_and_parse_env_args(args.env)

    options.workdir = args.benchmark_directory
    options.verbose = args.verbose
    options.rebuild = not args.no_rebuild
    options.sycl = args.sycl
    options.iterations = args.iterations
    options.timeout = args.timeout
    options.epsilon = args.epsilon
    options.ur = args.ur
    options.umf = args.umf
    options.ur_adapter = args.adapter
    options.exit_on_failure = args.exit_on_failure
    options.compare = Compare(args.compare_type)
    options.compare_max = args.compare_max
    options.output_html = args.output_html
    options.output_markdown = args.output_markdown
    options.dry_run = args.dry_run

    benchmark_filter = re.compile(args.filter) if args.filter else None

    main(args.benchmark_directory, additional_env_vars, args.save, args.compare, benchmark_filter)
