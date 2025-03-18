benchmarkRuns = [
  {
    "results": [],
    "name": "baseline",
    "hostname": "gkdse-pre-dnp-02",
    "git_hash": "bce874e",
    "github_repo": "EuphoricThinking/unified-memory-framework",
    "date": "2025-03-18T18:09:58.148710+00:00"
  },
  {
    "results": [],
    "name": "baseline",
    "hostname": "gkdse-pre-dnp-02",
    "git_hash": "bce874e",
    "github_repo": "EuphoricThinking/unified-memory-framework",
    "date": "2025-03-18T18:05:58.711084+00:00"
  },
  {
    "results": [],
    "name": "baseline",
    "hostname": "gkdse-pre-dnp-02",
    "git_hash": "bce874e",
    "github_repo": "EuphoricThinking/unified-memory-framework",
    "date": "2025-03-18T18:05:22.677353+00:00"
  },
  {
    "results": [],
    "name": "baseline",
    "hostname": "gkdse-pre-dnp-02",
    "git_hash": "bce874e",
    "github_repo": "EuphoricThinking/unified-memory-framework",
    "date": "2025-03-18T18:03:52.343596+00:00"
  },
  {
    "results": [],
    "name": "baseline",
    "hostname": "gkdse-pre-dnp-02",
    "git_hash": "bce874e",
    "github_repo": "EuphoricThinking/unified-memory-framework",
    "date": "2025-03-18T18:03:13.308787+00:00"
  },
  {
    "results": [],
    "name": "baseline",
    "hostname": "gkdse-pre-dnp-02",
    "git_hash": "bce874e",
    "github_repo": "EuphoricThinking/unified-memory-framework",
    "date": "2025-03-18T18:01:08.648722+00:00"
  },
  {
    "results": [],
    "name": "baseline",
    "hostname": "gkdse-pre-dnp-02",
    "git_hash": "bce874e",
    "github_repo": "EuphoricThinking/unified-memory-framework",
    "date": "2025-03-18T18:00:52.005919+00:00"
  }
];

benchmarkMetadata = {
  "SubmitKernel": {
    "type": "group",
    "description": "Measures CPU time overhead of submitting kernels through different APIs.",
    "notes": "Each layer builds on top of the previous layer, adding functionality and overhead.\nThe first layer is the Level Zero API, the second is the Unified Runtime API, and the third is the SYCL API.\nThe UR v2 adapter noticeably reduces UR layer overhead, also improving SYCL performance.\nWork is ongoing to reduce the overhead of the SYCL API\n",
    "unstable": null
  },
  "SinKernelGraph": {
    "type": "group",
    "description": null,
    "notes": null,
    "unstable": "This benchmark combines both eager and graph execution, and may not be representative of real use cases."
  },
  "umf-benchmark": {
    "type": "benchmark",
    "description": "No description provided.",
    "notes": null,
    "unstable": null
  },
  "Foo Group": {
    "type": "group",
    "description": "This is a test benchmark for Foo Group.",
    "notes": "This is a test note for Foo Group.\nLook, multiple lines!",
    "unstable": null
  },
  "Bar Group": {
    "type": "group",
    "description": "This is a test benchmark for Bar Group.",
    "notes": null,
    "unstable": "This is an unstable note for Bar Group."
  },
  "Memory Bandwidth 1": {
    "type": "benchmark",
    "description": "This is a test benchmark for Memory Bandwidth 1.",
    "notes": null,
    "unstable": null
  },
  "Memory Bandwidth 2": {
    "type": "benchmark",
    "description": "This is a test benchmark for Memory Bandwidth 2.",
    "notes": null,
    "unstable": null
  },
  "Memory Bandwidth 3": {
    "type": "benchmark",
    "description": "This is a test benchmark for Memory Bandwidth 3.",
    "notes": null,
    "unstable": null
  },
  "Memory Bandwidth 4": {
    "type": "benchmark",
    "description": "This is a test benchmark for Memory Bandwidth 4.",
    "notes": null,
    "unstable": null
  },
  "Memory Bandwidth 5": {
    "type": "benchmark",
    "description": "This is a test benchmark for Memory Bandwidth 5.",
    "notes": null,
    "unstable": null
  },
  "Memory Bandwidth 6": {
    "type": "benchmark",
    "description": "This is a test benchmark for Memory Bandwidth 6.",
    "notes": null,
    "unstable": null
  },
  "Latency 1": {
    "type": "benchmark",
    "description": "This is a test benchmark for Latency 1.",
    "notes": "A Latency test note!",
    "unstable": null
  },
  "Latency 2": {
    "type": "benchmark",
    "description": "This is a test benchmark for Latency 2.",
    "notes": "A Latency test note!",
    "unstable": null
  },
  "Latency 3": {
    "type": "benchmark",
    "description": "This is a test benchmark for Latency 3.",
    "notes": "A Latency test note!",
    "unstable": null
  },
  "Latency 4": {
    "type": "benchmark",
    "description": "This is a test benchmark for Latency 4.",
    "notes": "A Latency test note!",
    "unstable": null
  },
  "Latency 5": {
    "type": "benchmark",
    "description": "This is a test benchmark for Latency 5.",
    "notes": "A Latency test note!",
    "unstable": null
  },
  "Latency 6": {
    "type": "benchmark",
    "description": "This is a test benchmark for Latency 6.",
    "notes": "A Latency test note!",
    "unstable": null
  },
  "Throughput 1": {
    "type": "benchmark",
    "description": "This is a test benchmark for Throughput 1.",
    "notes": null,
    "unstable": null
  },
  "Throughput 2": {
    "type": "benchmark",
    "description": "This is a test benchmark for Throughput 2.",
    "notes": null,
    "unstable": null
  },
  "Throughput 3": {
    "type": "benchmark",
    "description": "This is a test benchmark for Throughput 3.",
    "notes": null,
    "unstable": null
  },
  "Throughput 4": {
    "type": "benchmark",
    "description": "This is a test benchmark for Throughput 4.",
    "notes": null,
    "unstable": null
  },
  "Throughput 5": {
    "type": "benchmark",
    "description": "This is a test benchmark for Throughput 5.",
    "notes": null,
    "unstable": null
  },
  "Throughput 6": {
    "type": "benchmark",
    "description": "This is a test benchmark for Throughput 6.",
    "notes": null,
    "unstable": null
  },
  "FLOPS 1": {
    "type": "benchmark",
    "description": "This is a test benchmark for FLOPS 1.",
    "notes": null,
    "unstable": "Unstable FLOPS test!"
  },
  "FLOPS 2": {
    "type": "benchmark",
    "description": "This is a test benchmark for FLOPS 2.",
    "notes": null,
    "unstable": "Unstable FLOPS test!"
  },
  "FLOPS 3": {
    "type": "benchmark",
    "description": "This is a test benchmark for FLOPS 3.",
    "notes": null,
    "unstable": "Unstable FLOPS test!"
  },
  "FLOPS 4": {
    "type": "benchmark",
    "description": "This is a test benchmark for FLOPS 4.",
    "notes": null,
    "unstable": "Unstable FLOPS test!"
  },
  "FLOPS 5": {
    "type": "benchmark",
    "description": "This is a test benchmark for FLOPS 5.",
    "notes": null,
    "unstable": "Unstable FLOPS test!"
  },
  "FLOPS 6": {
    "type": "benchmark",
    "description": "This is a test benchmark for FLOPS 6.",
    "notes": null,
    "unstable": "Unstable FLOPS test!"
  },
  "Cache Miss Rate 1": {
    "type": "benchmark",
    "description": "This is a test benchmark for Cache Miss Rate 1.",
    "notes": "Test Note",
    "unstable": "And another note!"
  },
  "Cache Miss Rate 2": {
    "type": "benchmark",
    "description": "This is a test benchmark for Cache Miss Rate 2.",
    "notes": "Test Note",
    "unstable": "And another note!"
  },
  "Cache Miss Rate 3": {
    "type": "benchmark",
    "description": "This is a test benchmark for Cache Miss Rate 3.",
    "notes": "Test Note",
    "unstable": "And another note!"
  },
  "Cache Miss Rate 4": {
    "type": "benchmark",
    "description": "This is a test benchmark for Cache Miss Rate 4.",
    "notes": "Test Note",
    "unstable": "And another note!"
  },
  "Cache Miss Rate 5": {
    "type": "benchmark",
    "description": "This is a test benchmark for Cache Miss Rate 5.",
    "notes": "Test Note",
    "unstable": "And another note!"
  },
  "Cache Miss Rate 6": {
    "type": "benchmark",
    "description": "This is a test benchmark for Cache Miss Rate 6.",
    "notes": "Test Note",
    "unstable": "And another note!"
  }
};

defaultCompareNames = [
  "baseline"
];
