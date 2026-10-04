# Optimization of listener selection and result collection

Measured on 2026-10-04: Windows-11-10.0.26200-SP0, AMD Ryzen Threadripper 2970WX 24-Core Processor, MSVC 193833145, 64-bit Release, logical CPU 0.

Before: fork `7e8fd0e991981f5435c62de2291fec4cfe0b37ee`. After: the same header base with the optimization patch (header diff SHA-256 `eb0dcab28f4fecc630875dcc0173361c95dfedfd9b72a76716acf64d6bd3a698`). The extended workload is compiled for both binaries. Each reported time is the median of 20 samples over 4 rounds with balanced execution order. Timing uses the normal allocator; separate binaries count C++ heap allocations. The workload and memory accounting follow the [original comparison](fork_overhead.md#environment-and-method).

## Changes

- Reserve the listener snapshot once, avoiding repeated metadata copies when the vector grows.
- Replace per-listener validation tree nodes with one sorted array of weak owners and used flags. Control-block identity, duplicate suppression and removed-listener checks are preserved.
- Reserve result and handle vector capacities from listener/selection size hints. Nested and concurrent dispatches still own separate buffers; ordinary calls do not count listeners or allocate result buffers.

## Eight listeners

Times are microseconds per event; allocations are C++ heap allocation calls per event. The queue uses batches of 64 and includes future retrieval.

| Operation | Before, us | After, us | Speedup | Allocations before | Allocations after |
| --- | ---: | ---: | ---: | ---: | ---: |
| `dispatch` | 0.294 | 0.294 | 1.00x | 0.000 | 0.000 |
| `enqueue_process` | 0.380 | 0.382 | 1.00x | 1.016 | 1.016 |
| `ordering` | 4.135 | 1.999 | 2.07x | 56.000 | 27.000 |
| `ordering_metadata` | 8.441 | 4.341 | 1.94x | 120.000 | 59.000 |
| `ordering_compact_metadata` | 1.676 | 0.936 | 1.79x | 16.000 | 3.000 |
| `plan_original` | 4.438 | 2.393 | 1.85x | 61.000 | 32.000 |
| `plan_arguments` | 4.974 | 2.771 | 1.79x | 69.000 | 40.000 |
| `plan_results` | 5.774 | 3.236 | 1.78x | 81.000 | 42.000 |
| `results` | 1.255 | 0.664 | 1.89x | 12.000 | 2.000 |
| `results_aggregate` | 1.318 | 0.710 | 1.86x | 13.000 | 3.000 |
| `enqueue_results_process` | 1.619 | 1.038 | 1.56x | 16.016 | 6.016 |

## Scaling

Each cell is before -> after in microseconds per event.

| Listeners | Ordering, dictionary | Plan, replaced args | Result collection | Queue with futures |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.713 -> 0.675 | 0.524 -> 0.474 | 0.209 -> 0.230 | 0.571 -> 0.609 |
| 8 | 8.441 -> 4.341 | 4.974 -> 2.771 | 1.255 -> 0.664 | 1.619 -> 1.038 |
| 32 | 38.660 -> 17.905 | 19.790 -> 11.498 | 3.389 -> 2.160 | 3.826 -> 2.764 |
| 128 | 140.363 -> 72.411 | 69.989 -> 42.407 | 10.319 -> 8.237 | 11.184 -> 9.060 |

## Memory and limits

The five measured object layout sizes are unchanged. The following rows measure eight-listener steady-state calls. Requested totals exclude allocator bookkeeping; peaks exclude preexisting setup storage.

| Operation | Requested bytes before -> after | New live peak before -> after, bytes |
| --- | ---: | ---: |
| `ordering` | 5272.0 -> 2880.0 | 1920 -> 1344 |
| `ordering_metadata` | 11416.0 -> 5952.0 | 4608 -> 2944 |
| `plan_arguments` | 5872.0 -> 3480.0 | 1920 -> 1560 |
| `results` | 500.0 -> 160.0 | 276 -> 160 |
| `enqueue_results_process` | 868.8 -> 528.8 | 28664 -> 27288 |

Reserving results introduces an extra count traversal for ordinary collection. A result vector can retain unused capacity when callbacks are removed, selected handles are invalid or invocation stops early. The full-dispatch benchmark does not measure those cases. Timing improvements for tiny counts can be small or overlap sample noise; saved min/max values accompany every median. This is a one-thread microbenchmark with small handlers, not a measurement of contention or application throughput.

Run the project's unit tests and tutorials separately to validate correctness. Coverage includes nested dispatch, concurrent configuration, snapshot stability, mutations during callbacks, duplicate and foreign handles, move-only results, cancellation and exceptions. Added regression cases verify distinct owners of the same node address and subscription mutation during result collection.

## Reproduce

```sh
python tests/benchmark/overhead/run.py --baseline 7e8fd0e --baseline-features --build-dir build/overhead-optimized
python tests/benchmark/overhead/report.py build/overhead-optimized build/overhead-optimized/report.md
```

Saved measurements: [summary CSV](benchmark_results/optimization_summary.csv), [environment](benchmark_results/optimization_environment.json). Raw allocation and timing samples remain in the chosen build directory. The `upstream` CSV label denotes the pre-optimization fork in this comparison.
