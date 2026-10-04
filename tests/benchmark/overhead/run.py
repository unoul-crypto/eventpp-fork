"""Build the same C++11 workload against the local fork and its upstream baseline."""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import statistics
import struct
import subprocess
import tarfile
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(__file__).resolve().parent


def command(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def git(*args):
    return command(["git", "-C", ROOT, *args], capture_output=True, text=True).stdout.strip()


def write_csv(path, rows, columns):
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def pin_cpu():
    """Pin measurement children to one available logical CPU; builds are already done."""
    if hasattr(os, "sched_getaffinity"):
        original = os.sched_getaffinity(0)
        selected = min(original)
        os.sched_setaffinity(0, {selected})
        return selected
    if os.name == "nt":
        import ctypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        kernel.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t),
                                                 ctypes.POINTER(ctypes.c_size_t)]
        kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        process = kernel.GetCurrentProcess()
        allowed, system = ctypes.c_size_t(), ctypes.c_size_t()
        if not kernel.GetProcessAffinityMask(process, ctypes.byref(allowed), ctypes.byref(system)):
            raise ctypes.WinError(ctypes.get_last_error())
        selected = allowed.value & -allowed.value
        if not selected or not kernel.SetProcessAffinityMask(process, selected):
            raise ctypes.WinError(ctypes.get_last_error())
        return selected.bit_length() - 1
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default="1224dd6", help="Local Git ref containing the upstream headers")
    parser.add_argument("--baseline-features", action="store_true",
                        help="The baseline is an earlier fork with the same extended API")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--cpu", default=platform.processor())
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "overhead")
    args = parser.parse_args()
    if args.rounds < 2 or args.rounds % 2:
        parser.error("Use a positive even number of rounds to balance execution order")
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    baseline = git("rev-parse", args.baseline)
    headers = build / "baseline" / baseline
    archive = build / "baseline.tar"
    command(["git", "-C", ROOT, "archive", "--format=tar", "--output", archive, baseline, "include"])
    # Extract regular include files only. No shell redirection, symlinks, or path traversal.
    with tarfile.open(archive) as contents:
        for member in contents.getmembers():
            if not member.isfile():
                continue
            destination = (headers / member.name).resolve()
            if headers.resolve() not in destination.parents:
                raise ValueError("Unexpected archive path")
            destination.parent.mkdir(parents=True, exist_ok=True)
            with contents.extractfile(member) as input_file:
                destination.write_bytes(input_file.read())
    command([args.cmake, "-S", SOURCE, "-B", build,
             "-DCMAKE_BUILD_TYPE=Release", f"-DUPSTREAM_INCLUDE_DIR={headers / 'include'}",
             f"-DUPSTREAM_HAS_FORK_API={'ON' if args.baseline_features else 'OFF'}"])
    command([args.cmake, "--build", build, "--config", "Release", "--parallel", "2"])
    selected_cpu = pin_cpu()

    def executable(variant, mode):
        name = f"overhead_{variant}_{mode}" + (".exe" if os.name == "nt" else "")
        candidates = [build / "Release" / name, build / name]
        return next(path for path in candidates if path.is_file())

    rows = []
    compiler = None

    def measure(variant, mode, round_index):
        nonlocal compiler
        print(f"Measuring {variant} {mode}, round {round_index + 1}", flush=True)
        result = command([executable(variant, mode)], capture_output=True, text=True)
        compiler = result.stderr.splitlines()[0]
        # Preserve complete samples for inspection; the summary contains no local paths.
        (build / f"{variant}_{mode}_{round_index}.csv").write_text(result.stdout, encoding="utf-8")
        for row in csv.DictReader(io.StringIO(result.stdout)):
            row.update(variant=variant, round=round_index)
            rows.append(row)

    for round_index in range(args.rounds):
        order = ("upstream", "fork") if round_index % 2 == 0 else ("fork", "upstream")
        for variant in order:
            measure(variant, "timing", round_index)
    for variant in ("upstream", "fork"):
        measure(variant, "allocations", 0)

    groups = {}
    for row in rows:
        key = (row["variant"], row["kind"], row["scenario"], int(row["listeners"]))
        groups.setdefault(key, []).append(row)
    summary = []
    for (variant, kind, scenario, listeners), samples in groups.items():
        record = dict(variant=variant, kind=kind, scenario=scenario, listeners=listeners,
                      samples=len(samples), median_ns="", min_ns="", max_ns="",
                      allocations_per_op="", bytes_per_op="", peak_bytes="")
        if kind == "timing":
            values = [float(sample["ns_per_op"]) for sample in samples]
            record.update(median_ns=round(statistics.median(values), 3),
                          min_ns=min(values), max_ns=max(values))
        else:
            for field in ("allocations_per_op", "bytes_per_op", "peak_bytes"):
                record[field] = samples[0][field]
        summary.append(record)
    columns = list(summary[0])
    write_csv(build / "summary.csv", summary, columns)
    metadata = dict(date_utc=datetime.now(timezone.utc).isoformat(), platform=platform.platform(),
                    cpu=args.cpu, compiler=compiler, baseline=baseline,
                    baseline_has_fork_api=args.baseline_features,
                    fork_commit=git("rev-parse", "HEAD"),
                    fork_headers_dirty=bool(git("status", "--porcelain", "--", "include")),
                    fork_header_diff_sha256=hashlib.sha256(git("diff", "--", "include").encode()).hexdigest(),
                    logical_cpu=selected_cpu,
                    rounds=args.rounds, samples_per_case=args.rounds * 5,
                    build_type="Release", pointer_bits=8 * struct.calcsize("P"))
    (build / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print("Summary:", build / "summary.csv")
    print("Environment:", build / "environment.json")


if __name__ == "__main__":
    main()
