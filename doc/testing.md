# Compatibility and sanitizer checks

The GitHub Actions workflow runs these jobs on pushes, pull requests and manual
dispatches:

| Job | Coverage |
| --- | --- |
| MSVC / Windows | Full unit suite, tutorials and compatibility executable |
| Apple Clang / macOS | Same tests |
| GCC 13 / Ubuntu 24.04 | Same tests |
| Clang 18 / Ubuntu 24.04 | Same tests |
| Clang / AddressSanitizer | Full suite, tutorials, compatibility executable; leak detection enabled |
| Clang / ThreadSanitizer | Separate run of all unit cases tagged `[thread]` |

The unit suite uses C++17 for test fixtures. Tutorials and
`tests/compatibility/fork_cxx11.cpp` use C++11 without language extensions on GCC
and Clang. The compatibility executable exercises metadata reads, atomic updates,
listener snapshots, ordering, argument plans, aggregation, errors, queued results,
void reports and move-only returns. MSVC uses its available default language mode;
the GCC/Clang jobs provide the strict C++11 check.

Thread cases include callback-list registration/removal, dispatch and queue
processing, selection configuration changes, independent argument plans and
result vectors, concurrent metadata readers/writers, exception-policy changes,
and competing dispatches of a queued report. Regression cases tagged
`[race-regression]` additionally overlap ordinary traversal with removal, query
list emptiness during mutation, and race queue insertion/recycling against all
processing modes, peek/take, cancellation and wait predicates. They cover both
ordinary and heterogeneous queues, in-flight emptiness and callback reentry. Assertions run after worker joins;
test-owned shared state uses explicit synchronization. The thread job fails on
the first reported race and fails if no tests match its CTest label.

Sanitizers are separate builds. Instrumentation applies to test executables and
the header-only library they instantiate, without changing flags for downstream
users or benchmarks. See the official
[AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html) and
[ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html) documentation.

## Run locally

Configure from `tests`, not the root installation project. The test project
requires CMake 3.13 or newer. These commands use `cmake -E chdir` so they also work
with versions predating `ctest --test-dir`:

```sh
CXX=g++-13 cmake -S tests -B build/gcc -DCMAKE_BUILD_TYPE=Release
cmake --build build/gcc --target unittest tutorial compatibility_cxx11 --parallel 2
cmake -E chdir build/gcc ctest --output-on-failure
```

For Clang use another build directory and `CXX=clang++-18`. On Windows omit `CXX`
and add `--config Release` to the build command and `--build-config Release` to
CTest.

On Linux, select exactly one sanitizer with `EVENTPP_TEST_SANITIZER`:

```sh
CXX=clang++-18 cmake -S tests -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEVENTPP_TEST_SANITIZER=address
cmake --build build/asan --target unittest tutorial compatibility_cxx11 --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 cmake -E chdir build/asan ctest --output-on-failure

CXX=clang++-18 cmake -S tests -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEVENTPP_TEST_SANITIZER=thread
cmake --build build/tsan --target unittest --parallel 2
TSAN_OPTIONS=halt_on_error=1 cmake -E chdir build/tsan ctest --label-regex threads --output-on-failure
```

Valid values are `none` (default), `address` and `thread`. Unsupported values,
compilers or platforms fail configuration rather than silently dropping
instrumentation. The sanitizer runtime must be installed with the compiler.
Use distinct build directories when changing compilers or sanitizer modes.

Sanitized builds use 8 workers with 256 subscriptions per worker (128 for the
insert case) for the largest legacy stress tests. Ordinary builds retain 256
workers with 4096 subscriptions per worker (1024 for insert). Operations and
assertions are unchanged; smaller sanitizer workloads keep resource use bounded.
Feature-specific concurrent tests retain their existing iteration counts.

New CI jobs being configured is not evidence that their runtime checks passed.
Inspect the corresponding workflow run after publishing changes; sanitizer
startup failures also fail the job and are not suppressed.
