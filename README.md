# LockFreeStructures

LockFreeStructures is a header-only C++20 library for concurrent data
structures, providing a wrapper based on `moodycamel::ConcurrentQueue`. Its
primary interface is `hakle::ConcurrentQueue<T>`, an MPMC queue built around
moodycamel's per-producer subqueue design.

## Features

- Implicit producers and explicit `ProducerToken` / `ConsumerToken` APIs
- Single-item and bulk enqueue/dequeue operations
- Customizable allocators, blocks, block managers, and queue traits
- Per-slot block policies: byte flags, packed word flags, and a counter
- Sharded slab/arena block managers for producer-affine recycling
- Thread-local cached implicit dequeue path, configurable through traits
- Header-only integration

The queue preserves FIFO order within each producer, but it does not define a
single global order across producers. Tokens belong to the queue that created
them, and a producer token must not be used concurrently by multiple producer
threads.

## Quick start

```cpp
#include "ConcurrentQueue/ConcurrentQueue.h"

hakle::ConcurrentQueue<int> queue;
queue.Enqueue(42);

int value = 0;
if (queue.TryDequeue(value)) {
  // value == 42
}
```

Explicit tokens can reduce repeated producer and consumer lookup:

```cpp
#include "ConcurrentQueue/ConcurrentQueue.h"

hakle::ConcurrentQueue<int> queue;
auto producer = queue.GetProducerToken();
auto consumer = queue.GetConsumerToken();

queue.EnqueueWithToken(producer, 7);

int value = 0;
queue.TryDequeue(consumer, value);
```

## Build and test

The project is tested with CMake and CTest. Each `test/test_*.cc` source is
built as a separate test executable.

```sh
cmake -S . -B build \
  -DBUILD_TESTING=ON \
  -DLOCKFREESTRUCTURES_BUILD_BENCHMARKS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For multi-config generators such as Visual Studio, add `--config Debug` to the
build and test commands. The suite covers the queue API, tokens, bulk
operations, MPMC behavior, per-producer FIFO ordering, move semantics, object
lifetime, exception recovery, and allocator propagation.

## Benchmarks

A lightweight standalone benchmark compares Hakle default/slab/arena/word-flag
configurations against the vendored moodycamel queue. It only needs a C++20
compiler and `Threads`, so it can be run without Boost or oneTBB:

```sh
cmake -S . -B build-standalone \
  -DCMAKE_BUILD_TYPE=Release \
  -DLOCKFREESTRUCTURES_BUILD_QUEUE_COMPARISON_BENCHMARK=ON
cmake --build build-standalone --parallel
build-standalone/benchmark/bin/queue_comparison_benchmark 10 \
  > benchmark/results/local-queue-comparison.json
```

Recent Apple M5 Pro / Clang 21 results are in
`benchmark/results/macos-m5pro-clang21-baseline-2026-08-19.json` and
`benchmark/results/macos-m5pro-clang21-optimized-2026-08-19.json`; the
three-process min/median aggregation used in the comparison table is in
`benchmark/results/macos-m5pro-clang21-summary-2026-08-19.json`.

The optional full benchmark compares Hakle's implicit, token, and token-bulk paths
with moodycamel, Boost.Lockfree, oneTBB, and a mutex-protected `std::queue`.
Google Benchmark and oneTBB are loaded from `thirdparty/` when available, or
fetched at configure time. Boost 1.74 or newer must be installed or supplied
through `BOOST_ROOT`.

```sh
cmake -S . -B build \
  -DLOCKFREESTRUCTURES_BUILD_BENCHMARKS=ON \
  -DBOOST_ROOT=/path/to/boost
cmake --build build --config Release --target queue_benchmark --parallel
```

The reference results below were collected on Windows with MSVC 19.44 in
Release mode on a 32-logical-processor host. The vertical axis is logarithmic
because bulk operations are substantially faster than single-item operations.

![Queue throughput benchmark](docs/benchmark-throughput.svg)

Raw results are available in
[`benchmark/results/windows-msvc-19.44-2026-07-29.json`](benchmark/results/windows-msvc-19.44-2026-07-29.json).
Regenerate the chart with:

```sh
python -m pip install matplotlib
python benchmark/plot_benchmark.py \
  benchmark/results/windows-msvc-19.44-2026-07-29.json \
  --output docs/benchmark-throughput.svg
```

## Custom allocators

Allocators use the project's `HakeAllocatorTraits` protocol rather than
`std::allocator_traits`. An allocator supplies its value, pointer, reference,
and size types; `Allocate`, `Deallocate`, `Construct`, and `Destroy`; and
rebind support. See
[`test/test_allocator_and_lifetime.cc`](test/test_allocator_and_lifetime.cc)
for a complete example.

The default queue traits propagate the rebound allocator to both explicit and
implicit block managers, so their block pools and free lists use the same
allocator family.

## License

LockFreeStructures is licensed under the Apache License 2.0. See
[`NOTICE`](NOTICE) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for design attribution and
vendored dependency notices.
