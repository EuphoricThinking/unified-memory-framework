
# Summary
No diffs to calculate performance change

(<ins>result</ins> is better)


## Performance change in benchmark groups

<details>
<summary> Relative perf in group umf-benchmark (1): cannot calculate </summary>

| Benchmark | This PR | Relative perf | Change | - |
|---|---|---|---|---|
| umf-benchmark | <ins>510.000000</ins> ns | |   |

</details>


# Details

<details>
<summary>Benchmark details - environment, command, output...</summary>


<details>
<summary>umf-benchmark</summary>

#### Environment Variables:


#### Command:
/home/amomot/gits/unified-memory-framework/build/benchmark/umf-benchmark --benchmark_format=csv

#### Output:
name,iterations,real_time,cpu_time,time_unit,bytes_per_second,items_per_second,label,error_occurred,error_message
"alloc_benchmark<fix_alloc_size>/stdmalloc/1000/4096",510,1.28642e+06,1.2864e+06,ns,,,,,
"alloc_benchmark<fix_alloc_size>/stdmalloc/1000/40960",473,1.66544e+06,1.66541e+06,ns,,,,,
"provider_benchmark<os_provider, fix_alloc_size>/bench/1000/4096",963,753468,753456,ns,,,,,
"provider_benchmark<os_provider, fix_alloc_size>/bench/1000/409600",739,802722,802636,ns,,,,,
"pool_benchmark<proxy_pool<os_provider>, fix_alloc_size>/bench/1000/4096",712,1.02554e+06,1.02543e+06,ns,,,,,
"pool_benchmark<scalable_pool<os_provider>, uniform_alloc_size>/scalable_pool_size/1000/1/5/4096",4381,163218,163193,ns,,,,,


</details>


</details>

