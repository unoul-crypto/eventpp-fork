# Fork overhead versus upstream

Generated from saved measurements by `tests/benchmark/overhead/report.py`.

These saved measurements precede the optimization patch. See
[subsequent optimization results](optimization_results.md) for its before/after comparison.

## Environment and method

Measured on 2026-10-04: Windows-11-10.0.26200-SP0, AMD Ryzen Threadripper 2970WX 24-Core Processor, MSVC 193833145, 64-bit Release build. Measurement processes were pinned to logical CPU 0.

Baseline: upstream `1224dd6c9bd4577d686ac42334fc545997f5ece1`. Fork headers: `72ed309381f1ee848d1e89108ec41cbee5f6b3f2` (unmodified). Both use the same standalone C++11 workload and compiler options, without LTO.

Times are medians of 20 samples per case across 4 rounds, alternating which binary runs first. Each sample is calibrated to at least 10 ms (subject to the iteration cap); 64 warm-up iterations precede calibration. The default threading policy is used, but all processing happens on one thread. One event key and a small `int(int)` callback with an observable checksum are used; all listeners execute.

Queue measurements enqueue/process batches of 64 and report time per event. The future case also gets and destroys every result. Handler setup, metadata registration and configuration are outside steady-state timing. Metadata uses the default `map<string, string>` with two short entries, unless the case explicitly uses no metadata or a compact structure of two integers. Read/write tests target the last listener. No concurrent producer, worker scheduling or contention is measured.

A separate executable instruments global C++ `new`/`delete`; timing binaries use the normal allocator. Allocation totals count requested bytes over 64 iterations, normalized per event/operation. Peak bytes count simultaneously live allocations created inside the measurement window, excluding preexisting setup allocations. Object sizes use `sizeof`. Allocator bookkeeping, instrumentation headers, RSS, stack frames and allocations made directly through `malloc` are excluded.

## Ordinary calls compared with upstream

Small changes overlap the observed sample ranges; they are not evidence of a stable improvement or regression. Raw median/min/max values are in [the saved CSV](benchmark_results/overhead_summary.csv).

| Operation | Listeners | Upstream, ns | Fork, ns | Median change |
| --- | ---: | ---: | ---: | ---: |
| `callback_list` | 1 | 48.8 | 49.2 | +0.9% |
| `callback_list` | 8 | 256.1 | 259.0 | +1.1% |
| `callback_list` | 32 | 964.9 | 947.2 | -1.8% |
| `callback_list` | 128 | 3778.5 | 3801.1 | +0.6% |
| `dispatch` | 1 | 67.5 | 67.9 | +0.6% |
| `dispatch` | 8 | 270.0 | 284.5 | +5.3% |
| `dispatch` | 32 | 976.1 | 950.5 | -2.6% |
| `dispatch` | 128 | 3809.5 | 3785.5 | -0.6% |
| `enqueue_process` | 1 | 146.5 | 149.2 | +1.8% |
| `enqueue_process` | 8 | 353.9 | 355.1 | +0.3% |
| `enqueue_process` | 32 | 1031.7 | 1031.2 | -0.0% |
| `enqueue_process` | 128 | 3856.7 | 3885.2 | +0.7% |

## Feature costs with eight listeners

Multipliers compare each synchronous call with the fork's ordinary `dispatch`; queued futures compare with the fork's ordinary `enqueue_process`. They describe this tiny-handler workload, not applications where handler work dominates.

| Operation | Time, us | Relative cost | Heap allocations/op | Requested bytes/op | New live peak, bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `callback_list` | 0.259 | 0.91x | 0.000 | 0.0 | 0 |
| `dispatch` | 0.284 | 1.00x | 0.000 | 0.0 | 0 |
| `enqueue_process` | 0.355 | 1.00x | 1.016 | 48.8 | 96 |
| `dispatch_metadata` | 0.275 | 0.97x | 0.000 | 0.0 | 0 |
| `get_metadata` | 0.274 | 0.96x | 2.000 | 192.0 | 192 |
| `set_metadata` | 0.357 | 1.26x | 4.000 | 320.0 | 640 |
| `ordering` | 3.849 | 13.53x | 56.000 | 5272.0 | 1920 |
| `ordering_metadata` | 7.796 | 27.40x | 120.000 | 11416.0 | 4608 |
| `ordering_compact_metadata` | 1.593 | 5.60x | 16.000 | 1232.0 | 848 |
| `plan_original` | 4.182 | 14.70x | 61.000 | 5744.0 | 1920 |
| `plan_arguments` | 4.612 | 16.21x | 69.000 | 5872.0 | 1920 |
| `plan_results` | 5.426 | 19.07x | 81.000 | 6372.0 | 1920 |
| `results` | 1.168 | 4.11x | 12.000 | 500.0 | 276 |
| `results_metadata` | 1.164 | 4.09x | 12.000 | 500.0 | 276 |
| `results_aggregate` | 1.203 | 4.23x | 13.000 | 504.0 | 276 |
| `enqueue_results_process` | 1.497 | 4.22x | 16.016 | 868.8 | 28664 |

`ordering` passes every handle in registration order and has no stored metadata. `ordering_metadata` does the same with two dictionary entries. `ordering_compact_metadata` uses a custom two-integer metadata structure. None performs sorting: the measurements include snapshot and handle-validation costs. `plan_original` reuses event arguments; `plan_arguments` adds one owned replacement argument per listener. `plan_results` combines replacement plans with result collection. `results` and `results_metadata` have no selection function; `results_aggregate` sums the returned integers into an additional integer result.

## Scaling of optional features

All times below are microseconds per event.

| Listeners | Ordering, no metadata | Ordering, dictionary | Ordering, compact | Plan, original args | Plan, replaced args | Plan + results |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.425 | 0.665 | 0.291 | 0.430 | 0.482 | 0.614 |
| 8 | 3.849 | 7.796 | 1.593 | 4.182 | 4.612 | 5.426 |
| 32 | 15.764 | 35.920 | 6.401 | 16.714 | 18.318 | 20.311 |
| 128 | 57.830 | 129.053 | 26.391 | 60.347 | 64.590 | 69.673 |

| Listeners | Results | Results + sum | Queue with futures |
| ---: | ---: | ---: | ---: |
| 1 | 0.193 | 0.239 | 0.527 |
| 8 | 1.168 | 1.203 | 1.497 |
| 32 | 3.200 | 3.263 | 3.501 |
| 128 | 9.603 | 9.726 | 10.762 |

## Memory

These layout sizes are specific to this compiler and standard library. Container objects are stack-resident here; their heap storage is counted separately.

| Object | Upstream, bytes | Fork, bytes | Change |
| --- | ---: | ---: | ---: |
| `callback_list_object` | 120 | 120 | +0 |
| `dispatcher_object` | 144 | 272 | +128 |
| `queue_object` | 424 | 552 | +128 |
| `queued_event` | 8 | 24 | +16 |
| `callback_node` | 104 | 120 | +16 |

Registration measurements include construction and teardown of one container with eight listeners on one event, with no stored metadata. `pending_queue_64` also adds 64 unprocessed events. Requested totals include transient allocations; peaks show simultaneous live heap bytes. Stack-resident container sizes are excluded.

| Setup | Version | Allocation calls | Requested heap bytes | Peak heap bytes |
| --- | --- | ---: | ---: | ---: |
| `register_list` | upstream | 8 | 960 | 960 |
| `register_list` | fork | 8 | 1088 | 1088 |
| `register_dispatcher` | upstream | 11 | 1376 | 1376 |
| `register_dispatcher` | fork | 15 | 1840 | 1840 |
| `register_queue` | upstream | 13 | 1440 | 1440 |
| `register_queue` | fork | 17 | 1936 | 1936 |
| `pending_queue_64` | upstream | 141 | 5536 | 3520 |
| `pending_queue_64` | fork | 145 | 8080 | 5056 |

Dictionary registration and a complete cold queue/future lifecycle are also recorded in the saved CSV (`register_metadata`, `queue_results_64_lifecycle`). The former includes the temporary input dictionary; the latter includes 64 futures, processing and result retrieval, so it is not directly equivalent to pending ordinary events.

## Interpretation

- Ordinary callback/dispatcher invocation adds no per-call heap allocations. Registration without metadata still adds 16 bytes per callback node in this build. Dispatcher/queue objects grow by 128 bytes, and queued records by 16 bytes.
- The ordinary queue uses the same number of steady-state allocations as upstream, but requests more bytes because its records and list sentinel nodes are larger. MSVC's temporary `std::list` sentinel allocations remain visible even after warming the recycled queue-node pool.
- Planning pays for snapshot construction, a returned order/plan and validation storage. Dictionary copies dominate these tiny-handler examples. MSVC allocates map sentinels even for empty metadata; compact application-defined metadata substantially reduces this cost.
- Plain result collection skips metadata snapshots when no selection function is set. It pays for growing result/handle vectors. The integer sum aggregator adds one allocation; future results additionally allocate promise state and retain outputs until retrieved.
- Potential follow-up optimizations are snapshot/vector capacity reuse and less allocation-heavy handle validation. They need their own measurements and must preserve nested dispatch, concurrent calls and snapshot lifetime guarantees.

## Reproduce

From the repository root, with Python 3.9+, CMake and a C++ compiler:

```sh
python tests/benchmark/overhead/run.py
python tests/benchmark/overhead/report.py build/overhead build/overhead/report.md
```

Use `--cmake` to select a CMake executable and `--baseline` to select a local Git ref. The default baseline is `1224dd6`; no network request is needed. The runner extracts only baseline headers into the build directory, builds four independent binaries, and saves raw samples, `summary.csv` and `environment.json`. Allocation binaries use a single-threaded instrumented allocator and are intended only for this benchmark.

The optional benchmark is separate from the existing test suite and requires no third-party benchmark framework. Saved measurements: [summary CSV](benchmark_results/overhead_summary.csv), [environment](benchmark_results/overhead_environment.json).
