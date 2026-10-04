"""Render saved measurements without rerunning the benchmark."""
import argparse
import csv
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, help="Directory containing summary.csv and environment.json")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    with (args.results / "summary.csv").open(encoding="utf-8", newline="") as source:
        rows = list(csv.DictReader(source))
    environment = json.loads((args.results / "environment.json").read_text(encoding="utf-8"))
    index = {(row["variant"], row["kind"], row["scenario"], int(row["listeners"])): row for row in rows}

    def record(variant, kind, scenario, count):
        return index[variant, kind, scenario, count]

    def ns(variant, scenario, count):
        return float(record(variant, "timing", scenario, count)["median_ns"])

    text = ["# Fork overhead versus upstream", "",
            "Generated from saved measurements by `tests/benchmark/overhead/report.py`.", "",
            "## Environment and method", "",
            f"Measured on {environment['date_utc'][:10]}: {environment['platform']}, "
            f"{environment['cpu']}, {environment['compiler'].removeprefix('compiler=')}, "
            f"{environment['pointer_bits']}-bit Release build. "
            f"Measurement processes were pinned to logical CPU {environment['logical_cpu']}.", "",
            f"Baseline: upstream `{environment['baseline']}`. "
            f"Fork headers: `{environment['fork_commit']}` "
            f"({'modified' if environment['fork_headers_dirty'] else 'unmodified'}). "
            "Both use the same standalone C++11 workload and compiler options, without LTO.", "",
            f"Times are medians of {environment['samples_per_case']} samples per case across "
            f"{environment['rounds']} rounds, alternating which binary runs first. "
            "Each sample is calibrated to at least 10 ms (subject to the iteration cap); "
            "64 warm-up iterations precede calibration. The default threading policy is used, "
            "but all processing happens on one thread. One event key and a small `int(int)` "
            "callback with an observable checksum are used; all listeners execute.", "",
            "Queue measurements enqueue/process batches of 64 and report time per event. "
            "The future case also gets and destroys every result. Handler setup, metadata "
            "registration and configuration are outside steady-state timing. Metadata uses "
            "the default `map<string, string>` with two short entries, unless the case explicitly "
            "uses no metadata or a compact structure of two integers. Read/write tests target "
            "the last listener. No concurrent producer, worker scheduling or contention is measured.", "",
            "A separate executable instruments global C++ `new`/`delete`; timing binaries use "
            "the normal allocator. Allocation totals count requested bytes over 64 iterations, "
            "normalized per event/operation. Peak bytes count simultaneously live allocations "
            "created inside the measurement window, excluding preexisting setup allocations. "
            "Object sizes use `sizeof`. Allocator bookkeeping, instrumentation headers, RSS, "
            "stack frames and allocations made directly through `malloc` are excluded.", "",
            "## Ordinary calls compared with upstream", "",
            "Small changes overlap the observed sample ranges; they are not evidence of a "
            "stable improvement or regression. Raw median/min/max values are in "
            "[the saved CSV](benchmark_results/overhead_summary.csv).", "",
            "| Operation | Listeners | Upstream, ns | Fork, ns | Median change |",
            "| --- | ---: | ---: | ---: | ---: |"]
    for scenario in ("callback_list", "dispatch", "enqueue_process"):
        for count in (1, 8, 32, 128):
            old, new = ns("upstream", scenario, count), ns("fork", scenario, count)
            text.append(f"| `{scenario}` | {count} | {old:.1f} | {new:.1f} | {(new / old - 1) * 100:+.1f}% |")

    text.extend(["", "## Feature costs with eight listeners", "",
                 "Multipliers compare each synchronous call with the fork's ordinary `dispatch`; "
                 "queued futures compare with the fork's ordinary `enqueue_process`. They describe "
                 "this tiny-handler workload, not applications where handler work dominates.", "",
                 "| Operation | Time, us | Relative cost | Heap allocations/op | Requested bytes/op | New live peak, bytes |",
                 "| --- | ---: | ---: | ---: | ---: | ---: |"])
    scenarios = [row["scenario"] for row in rows
                 if row["variant"] == "fork" and row["kind"] == "timing" and row["listeners"] == "8"]
    for scenario in scenarios:
        memory = record("fork", "steady", scenario, 8)
        reference = "enqueue_process" if scenario.startswith("enqueue") else "dispatch"
        text.append(f"| `{scenario}` | {ns('fork', scenario, 8) / 1000:.3f} | "
                    f"{ns('fork', scenario, 8) / ns('fork', reference, 8):.2f}x | "
                    f"{float(memory['allocations_per_op']):.3f} | {float(memory['bytes_per_op']):.1f} | "
                    f"{float(memory['peak_bytes']):.0f} |")

    text.extend(["", "`ordering` passes every handle in registration order and has no stored metadata. "
                 "`ordering_metadata` does the same with two dictionary entries. "
                 "`ordering_compact_metadata` uses a custom two-integer metadata structure. "
                 "None performs sorting: the measurements include snapshot and handle-validation costs. "
                 "`plan_original` reuses event arguments; `plan_arguments` adds one owned replacement "
                 "argument per listener. `plan_results` combines replacement plans with result collection. "
                 "`results` and `results_metadata` have no selection function; `results_aggregate` sums "
                 "the returned integers into an additional integer result.", "",
                 "## Scaling of optional features", "",
                 "All times below are microseconds per event.", "",
                 "| Listeners | Ordering, no metadata | Ordering, dictionary | Ordering, compact | Plan, original args | Plan, replaced args | Plan + results |",
                 "| ---: | ---: | ---: | ---: | ---: | ---: | ---: |"])
    scaling = ("ordering", "ordering_metadata", "ordering_compact_metadata", "plan_original", "plan_arguments", "plan_results")
    for count in (1, 8, 32, 128):
        text.append(f"| {count} | " + " | ".join(f"{ns('fork', scenario, count) / 1000:.3f}" for scenario in scaling) + " |")
    text.extend(["", "| Listeners | Results | Results + sum | Queue with futures |",
                 "| ---: | ---: | ---: | ---: |"])
    for count in (1, 8, 32, 128):
        text.append(f"| {count} | " + " | ".join(f"{ns('fork', scenario, count) / 1000:.3f}"
                    for scenario in ("results", "results_aggregate", "enqueue_results_process")) + " |")

    text.extend(["", "## Memory", "",
                 "These layout sizes are specific to this compiler and standard library. "
                 "Container objects are stack-resident here; their heap storage is counted separately.", "",
                 "| Object | Upstream, bytes | Fork, bytes | Change |", "| --- | ---: | ---: | ---: |"])
    for scenario in ("callback_list_object", "dispatcher_object", "queue_object", "queued_event", "callback_node"):
        old = int(record("upstream", "size", scenario, 0)["bytes_per_op"])
        new = int(record("fork", "size", scenario, 0)["bytes_per_op"])
        text.append(f"| `{scenario}` | {old} | {new} | {new - old:+d} |")
    text.extend(["", "Registration measurements include construction and teardown of one container "
                 "with eight listeners on one event, with no stored metadata. `pending_queue_64` also "
                 "adds 64 unprocessed events. Requested totals include transient allocations; peaks "
                 "show simultaneous live heap bytes. Stack-resident container sizes are excluded.", "",
                 "| Setup | Version | Allocation calls | Requested heap bytes | Peak heap bytes |",
                 "| --- | --- | ---: | ---: | ---: |"])
    for scenario in ("register_list", "register_dispatcher", "register_queue", "pending_queue_64"):
        for variant in ("upstream", "fork"):
            row = record(variant, "setup", scenario, 8)
            text.append(f"| `{scenario}` | {variant} | {row['allocations_per_op']} | {row['bytes_per_op']} | {row['peak_bytes']} |")
    text.extend(["", "Dictionary registration and a complete cold queue/future lifecycle are also recorded "
                 "in the saved CSV (`register_metadata`, `queue_results_64_lifecycle`). The former "
                 "includes the temporary input dictionary; the latter includes 64 futures, processing "
                 "and result retrieval, so it is not directly equivalent to pending ordinary events.", "",
                 "## Interpretation", "",
                 "- Ordinary callback/dispatcher invocation adds no per-call heap allocations. "
                 "Registration without metadata still adds 16 bytes per callback node in this build. "
                 "Dispatcher/queue objects grow by 128 bytes, and queued records by 16 bytes.",
                 "- The ordinary queue uses the same number of steady-state allocations as upstream, "
                 "but requests more bytes because its records and list sentinel nodes are larger. "
                 "MSVC's temporary `std::list` sentinel allocations remain visible even after warming "
                 "the recycled queue-node pool.",
                 "- Planning pays for snapshot construction, a returned order/plan and validation storage. "
                 "Dictionary copies dominate these tiny-handler examples. MSVC allocates map sentinels "
                 "even for empty metadata; compact application-defined metadata substantially reduces this cost.",
                 "- Plain result collection skips metadata snapshots when no selection function is set. "
                 "It pays for growing result/handle vectors. The integer sum aggregator adds one allocation; "
                 "future results additionally allocate promise state and retain outputs until retrieved.",
                 "- Potential follow-up optimizations are snapshot/vector capacity reuse and less "
                 "allocation-heavy handle validation. They need their own measurements and must preserve "
                 "nested dispatch, concurrent calls and snapshot lifetime guarantees.", "",
                 "## Reproduce", "",
                 "From the repository root, with Python 3.9+, CMake and a C++ compiler:", "",
                 "```sh", "python tests/benchmark/overhead/run.py",
                 "python tests/benchmark/overhead/report.py build/overhead build/overhead/report.md", "```", "",
                 "Use `--cmake` to select a CMake executable and `--baseline` to select a local Git ref. "
                 "The default baseline is `1224dd6`; no network request is needed. The runner extracts "
                 "only baseline headers into the build directory, builds four independent binaries, "
                 "and saves raw samples, `summary.csv` and `environment.json`. Allocation binaries use "
                 "a single-threaded instrumented allocator and are intended only for this benchmark.", "",
                 "The optional benchmark is separate from the existing test suite and requires no "
                 "third-party benchmark framework. Saved measurements: "
                 "[summary CSV](benchmark_results/overhead_summary.csv), "
                 "[environment](benchmark_results/overhead_environment.json).", ""])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(text), encoding="utf-8")


if __name__ == "__main__":
    main()
