// Standalone Hakle-vs-moodycamel benchmark.
//
// This is intentionally independent from Google Benchmark, Boost.Lockfree and
// oneTBB so it can be built on machines that only have the vendored moodycamel
// queue and a C++20 compiler.
//
// Build:
//   c++ -std=c++20 -O3 -DNDEBUG -I. -Ibenchmark/third_party \
//       benchmark/queue_comparison_benchmark.cc -o benchmark/queue_comparison_benchmark
#include "ConcurrentQueue/ConcurrentQueue.h"
#include "moodycamel/concurrentqueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <numeric>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t benchmark_block_size = hakle::ConcurrentQueue<int>::BlockSize;
static_assert(benchmark_block_size == moodycamel::ConcurrentQueue<int>::BLOCK_SIZE,
              "Equal-block benchmarks require matching queue block sizes");

using bench_allocator = hakle::HakleAllocator<int>;

// ---------------------------------------------------------------------------
// Existing customization points exercised by the benchmark.
// ---------------------------------------------------------------------------
struct slab_benchmark_traits : hakle::ConcurrentQueueDefaultTraits<int, bench_allocator> {
    using Base = hakle::ConcurrentQueueDefaultTraits<int, bench_allocator>;
    using typename Base::ExplicitAllocatorType;
    using typename Base::ExplicitBlockType;
    using typename Base::ImplicitAllocatorType;
    using typename Base::ImplicitBlockType;

    static constexpr std::size_t SlabBlockCount = 32;
    using ExplicitBlockManagerType =
        hakle::SlabBlockManager<ExplicitBlockType, ExplicitAllocatorType, SlabBlockCount>;
    using ImplicitBlockManagerType =
        hakle::SlabBlockManager<ImplicitBlockType, ImplicitAllocatorType, SlabBlockCount>;

    static ExplicitBlockManagerType MakeDefaultExplicitBlockManager(const ExplicitAllocatorType& allocator) {
        return ExplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
    }
    static ImplicitBlockManagerType MakeDefaultImplicitBlockManager(const ImplicitAllocatorType& allocator) {
        return ImplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
    }
    static ExplicitBlockManagerType MakeExplicitBlockManager(const ExplicitAllocatorType& allocator, std::size_t block_pool_size) {
        return ExplicitBlockManagerType(block_pool_size, allocator);
    }
    static ImplicitBlockManagerType MakeImplicitBlockManager(const ImplicitAllocatorType& allocator, std::size_t block_pool_size) {
        return ImplicitBlockManagerType(block_pool_size, allocator);
    }
};

using slab_benchmark_queue =
    hakle::ConcurrentQueue<int, bench_allocator, slab_benchmark_traits>;

template <std::size_t ArenaBytes>
struct arena_benchmark_traits : hakle::ConcurrentQueueDefaultTraits<int, bench_allocator> {
    using Base = hakle::ConcurrentQueueDefaultTraits<int, bench_allocator>;
    using typename Base::ExplicitAllocatorType;
    using typename Base::ExplicitBlockType;
    using typename Base::ImplicitAllocatorType;
    using typename Base::ImplicitBlockType;

    static constexpr std::size_t SlabBlockCount = 32;
    using ExplicitBlockManagerType =
        hakle::ArenaBlockManager<ExplicitBlockType, ExplicitAllocatorType, SlabBlockCount, ArenaBytes>;
    using ImplicitBlockManagerType =
        hakle::ArenaBlockManager<ImplicitBlockType, ImplicitAllocatorType, SlabBlockCount, ArenaBytes>;

    static ExplicitBlockManagerType MakeDefaultExplicitBlockManager(const ExplicitAllocatorType& allocator) {
        return ExplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
    }
    static ImplicitBlockManagerType MakeDefaultImplicitBlockManager(const ImplicitAllocatorType& allocator) {
        return ImplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
    }
    static ExplicitBlockManagerType MakeExplicitBlockManager(const ExplicitAllocatorType& allocator, std::size_t block_pool_size) {
        return ExplicitBlockManagerType(block_pool_size, allocator);
    }
    static ImplicitBlockManagerType MakeImplicitBlockManager(const ImplicitAllocatorType& allocator, std::size_t block_pool_size) {
        return ImplicitBlockManagerType(block_pool_size, allocator);
    }
};

// Uses the new packed-word empty-flags policy for explicit token producers.
struct word_flags_benchmark_traits : hakle::ConcurrentQueueDefaultTraits<int, bench_allocator> {
    using Base = hakle::ConcurrentQueueDefaultTraits<int, bench_allocator>;

    using ExplicitBlockType = hakle::HakleWordFlagsBlock<int, Base::BlockSize>;
    using ExplicitAllocatorType =
        typename hakle::HakeAllocatorTraits<bench_allocator>::template RebindAlloc<ExplicitBlockType>;
    using ImplicitBlockType  = typename Base::ImplicitBlockType;
    using ImplicitAllocatorType = typename Base::ImplicitAllocatorType;

    using ExplicitBlockManagerType = hakle::HakleBlockManager<ExplicitBlockType, ExplicitAllocatorType>;
    using ImplicitBlockManagerType = typename Base::ImplicitBlockManagerType;

    static ExplicitBlockManagerType MakeDefaultExplicitBlockManager(const ExplicitAllocatorType& allocator) {
        return ExplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
    }
    static ImplicitBlockManagerType MakeDefaultImplicitBlockManager(const ImplicitAllocatorType& allocator) {
        return ImplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
    }
    static ExplicitBlockManagerType MakeExplicitBlockManager(const ExplicitAllocatorType& allocator, std::size_t block_pool_size) {
        return ExplicitBlockManagerType(block_pool_size, allocator);
    }
    static ImplicitBlockManagerType MakeImplicitBlockManager(const ImplicitAllocatorType& allocator, std::size_t block_pool_size) {
        return ImplicitBlockManagerType(block_pool_size, allocator);
    }
};

using word_flags_benchmark_queue =
    hakle::ConcurrentQueue<int, bench_allocator, word_flags_benchmark_traits>;

// ---------------------------------------------------------------------------
// Queue wrappers.  All wrappers receive the requested number of initial
// blocks; equal_blocks mode gives every queue the same number of preallocated
// blocks, zero_pool mode forces every queue through its allocation slow path.
// ---------------------------------------------------------------------------
class hakle_default_implicit_queue {
public:
    hakle_default_implicit_queue(std::size_t initial_block_count, std::size_t, std::size_t)
        : queue_(std::piecewise_construct, std::make_tuple(std::size_t{0}),
                 std::make_tuple(initial_block_count), {}) {}
    bool enqueue(std::size_t, int value) { return queue_.Enqueue(value); }
    bool try_dequeue(std::size_t, int& value) { return queue_.TryDequeue(value); }

private:
    hakle::ConcurrentQueue<int> queue_;
};

class hakle_default_token_queue {
public:
    hakle_default_token_queue(std::size_t initial_block_count, std::size_t producer_count, std::size_t consumer_count)
        : queue_(std::piecewise_construct, std::make_tuple(initial_block_count),
                 std::make_tuple(std::size_t{0}), {}) {
        producers_.reserve(producer_count);
        for (std::size_t i = 0; i < producer_count; ++i) {
            producers_.emplace_back(queue_.GetProducerToken());
        }
        consumers_.reserve(consumer_count);
        for (std::size_t i = 0; i < consumer_count; ++i) {
            consumers_.emplace_back(queue_.GetConsumerToken());
        }
    }
    bool enqueue(std::size_t producer, int value) { return queue_.EnqueueWithToken(producers_[producer], value); }
    bool try_dequeue(std::size_t consumer, int& value) { return queue_.TryDequeue(consumers_[consumer], value); }

private:
    hakle::ConcurrentQueue<int> queue_;
    std::vector<hakle::ConcurrentQueue<int>::ProducerToken> producers_;
    std::vector<hakle::ConcurrentQueue<int>::ConsumerToken> consumers_;
};

class hakle_word_flags_token_queue {
public:
    hakle_word_flags_token_queue(std::size_t initial_block_count, std::size_t producer_count, std::size_t consumer_count)
        : queue_(std::piecewise_construct, std::make_tuple(initial_block_count),
                 std::make_tuple(std::size_t{0}), {}) {
        producers_.reserve(producer_count);
        for (std::size_t i = 0; i < producer_count; ++i) {
            producers_.emplace_back(queue_.GetProducerToken());
        }
        consumers_.reserve(consumer_count);
        for (std::size_t i = 0; i < consumer_count; ++i) {
            consumers_.emplace_back(queue_.GetConsumerToken());
        }
    }
    bool enqueue(std::size_t producer, int value) { return queue_.EnqueueWithToken(producers_[producer], value); }
    bool try_dequeue(std::size_t consumer, int& value) { return queue_.TryDequeue(consumers_[consumer], value); }

private:
    word_flags_benchmark_queue queue_;
    std::vector<word_flags_benchmark_queue::ProducerToken> producers_;
    std::vector<word_flags_benchmark_queue::ConsumerToken> consumers_;
};

class hakle_slab_implicit_queue {
public:
    hakle_slab_implicit_queue(std::size_t initial_block_count, std::size_t, std::size_t)
        : queue_(std::piecewise_construct, std::make_tuple(std::size_t{0}),
                 std::make_tuple(initial_block_count), {}) {}
    bool enqueue(std::size_t, int value) { return queue_.Enqueue(value); }
    bool try_dequeue(std::size_t, int& value) { return queue_.TryDequeue(value); }

private:
    slab_benchmark_queue queue_;
};

class hakle_slab_token_queue {
public:
    hakle_slab_token_queue(std::size_t initial_block_count, std::size_t producer_count, std::size_t consumer_count)
        : queue_(std::piecewise_construct, std::make_tuple(initial_block_count),
                 std::make_tuple(std::size_t{0}), {}) {
        producers_.reserve(producer_count);
        for (std::size_t i = 0; i < producer_count; ++i) {
            producers_.emplace_back(queue_.GetProducerToken());
        }
        consumers_.reserve(consumer_count);
        for (std::size_t i = 0; i < consumer_count; ++i) {
            consumers_.emplace_back(queue_.GetConsumerToken());
        }
    }
    bool enqueue(std::size_t producer, int value) { return queue_.EnqueueWithToken(producers_[producer], value); }
    bool try_dequeue(std::size_t consumer, int& value) { return queue_.TryDequeue(consumers_[consumer], value); }

private:
    slab_benchmark_queue queue_;
    std::vector<slab_benchmark_queue::ProducerToken> producers_;
    std::vector<slab_benchmark_queue::ConsumerToken> consumers_;
};

template <std::size_t ArenaBytes>
class hakle_arena_implicit_queue {
public:
    hakle_arena_implicit_queue(std::size_t initial_block_count, std::size_t, std::size_t)
        : queue_(std::piecewise_construct, std::make_tuple(std::size_t{0}),
                 std::make_tuple(initial_block_count), {}) {}
    bool enqueue(std::size_t, int value) { return queue_.Enqueue(value); }
    bool try_dequeue(std::size_t, int& value) { return queue_.TryDequeue(value); }

private:
    hakle::ConcurrentQueue<int, bench_allocator, arena_benchmark_traits<ArenaBytes>> queue_;
};

class moodycamel_implicit_queue {
public:
    moodycamel_implicit_queue(std::size_t initial_block_count, std::size_t, std::size_t)
        : queue_(initial_block_count * benchmark_block_size) {}
    bool enqueue(std::size_t, int value) { return queue_.enqueue(value); }
    bool try_dequeue(std::size_t, int& value) { return queue_.try_dequeue(value); }

private:
    moodycamel::ConcurrentQueue<int> queue_;
};

class moodycamel_token_queue {
public:
    moodycamel_token_queue(std::size_t initial_block_count, std::size_t producer_count, std::size_t consumer_count)
        : queue_(initial_block_count * benchmark_block_size) {
        producers_.reserve(producer_count);
        for (std::size_t i = 0; i < producer_count; ++i) {
            producers_.emplace_back(queue_);
        }
        consumers_.reserve(consumer_count);
        for (std::size_t i = 0; i < consumer_count; ++i) {
            consumers_.emplace_back(queue_);
        }
    }
    bool enqueue(std::size_t producer, int value) { return queue_.enqueue(producers_[producer], value); }
    bool try_dequeue(std::size_t consumer, int& value) { return queue_.try_dequeue(consumers_[consumer], value); }

private:
    moodycamel::ConcurrentQueue<int> queue_;
    std::vector<moodycamel::ProducerToken> producers_;
    std::vector<moodycamel::ConsumerToken> consumers_;
};

// ---------------------------------------------------------------------------
// Benchmark driver.
// ---------------------------------------------------------------------------
struct Scenario {
    std::size_t producers;
    std::size_t consumers;
    std::size_t items_per_producer;
};

struct Result {
    double ns_per_item;
    double million_items_per_second;
};

void verify(std::size_t got, std::uint64_t sum, std::size_t expected) {
    const std::uint64_t expected_sum = static_cast<std::uint64_t>(expected) * (expected - 1) / 2;
    if (got != expected || sum != expected_sum) {
        std::cerr << "verification failed: got=" << got << " expected=" << expected
                  << " sum=" << sum << " expected_sum=" << expected_sum << "\n";
        std::abort();
    }
}

inline std::size_t required_initial_blocks(const Scenario& scenario) {
    const std::size_t blocks_per_producer =
        (scenario.items_per_producer + benchmark_block_size - 1) / benchmark_block_size;
    return scenario.producers * blocks_per_producer;
}

template <class Queue>
Result run_mpmc_once(const Scenario& scenario) {
    const std::size_t total = scenario.producers * scenario.items_per_producer;
    Queue queue(required_initial_blocks(scenario), scenario.producers, scenario.consumers);

    std::atomic<std::size_t> producers_remaining{scenario.producers};
    std::vector<std::uint64_t> sums(scenario.consumers, 0);
    std::vector<std::size_t> counts(scenario.consumers, 0);
    std::latch ready_gate(scenario.producers + scenario.consumers);
    std::latch start_gate(1);

    std::vector<std::thread> consumers;
    consumers.reserve(scenario.consumers);
    for (std::size_t c = 0; c < scenario.consumers; ++c) {
        consumers.emplace_back([&, c] {
            ready_gate.count_down();
            start_gate.wait();
            std::size_t local_count = 0;
            std::uint64_t local_sum = 0;
            for (;;) {
                int value = 0;
                if (queue.try_dequeue(c, value)) {
                    local_sum += static_cast<std::uint64_t>(value);
                    ++local_count;
                    continue;
                }
                if (producers_remaining.load(std::memory_order_acquire) == 0) {
                    if (!queue.try_dequeue(c, value)) {
                        break;
                    }
                    local_sum += static_cast<std::uint64_t>(value);
                    ++local_count;
                } else {
                    std::this_thread::yield();
                }
            }
            sums[c] = local_sum;
            counts[c] = local_count;
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(scenario.producers);
    for (std::size_t p = 0; p < scenario.producers; ++p) {
        producers.emplace_back([&, p] {
            ready_gate.count_down();
            start_gate.wait();
            for (std::size_t sequence = 0; sequence < scenario.items_per_producer; ++sequence) {
                const int value = static_cast<int>(p * scenario.items_per_producer + sequence);
                while (!queue.enqueue(p, value)) {
                    std::this_thread::yield();
                }
            }
            producers_remaining.fetch_sub(1, std::memory_order_release);
        });
    }

    ready_gate.wait();
    const auto start = Clock::now();
    start_gate.count_down();
    for (auto& thread : producers) {
        thread.join();
    }
    for (auto& thread : consumers) {
        thread.join();
    }
    const auto finish = Clock::now();

    std::size_t final_count = 0;
    std::uint64_t final_sum = 0;
    int value = 0;
    while (queue.try_dequeue(0, value)) {
        ++final_count;
        final_sum += static_cast<std::uint64_t>(value);
    }

    const std::size_t got = final_count + std::accumulate(counts.begin(), counts.end(), std::size_t{0});
    const std::uint64_t sum = final_sum + std::accumulate(sums.begin(), sums.end(), std::uint64_t{0});
    verify(got, sum, total);

    const double ns = std::chrono::duration<double, std::nano>(finish - start).count() / static_cast<double>(total);
    return Result{ns, 1000.0 / ns};
}

template <class Queue>
Result run_producer_only_once(const Scenario& scenario) {
    const std::size_t total = scenario.producers * scenario.items_per_producer;
    Queue queue(required_initial_blocks(scenario), scenario.producers, 1);

    std::latch ready_gate(scenario.producers);
    std::latch start_gate(1);
    std::vector<std::thread> producers;
    producers.reserve(scenario.producers);
    for (std::size_t p = 0; p < scenario.producers; ++p) {
        producers.emplace_back([&, p] {
            ready_gate.count_down();
            start_gate.wait();
            for (std::size_t sequence = 0; sequence < scenario.items_per_producer; ++sequence) {
                const int value = static_cast<int>(p * scenario.items_per_producer + sequence);
                while (!queue.enqueue(p, value)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    ready_gate.wait();
    const auto start = Clock::now();
    start_gate.count_down();
    for (auto& thread : producers) {
        thread.join();
    }
    const auto finish = Clock::now();

    std::size_t got = 0;
    std::uint64_t sum = 0;
    int value = 0;
    while (queue.try_dequeue(0, value)) {
        ++got;
        sum += static_cast<std::uint64_t>(value);
    }
    verify(got, sum, total);

    const double ns = std::chrono::duration<double, std::nano>(finish - start).count() / static_cast<double>(total);
    return Result{ns, 1000.0 / ns};
}

template <class Queue>
Result run_mpmc(const Scenario& scenario, int reps) {
    Result best{1e100, 0.0};
    for (int rep = 0; rep < reps; ++rep) {
        const Result current = run_mpmc_once<Queue>(scenario);
#ifdef QUEUE_BENCH_PRINT_REPS
        std::cerr << "rep " << scenario.producers << "P" << scenario.consumers << "C "
                  << rep << ": " << current.ns_per_item << " ns/item\n";
#endif
        if (current.ns_per_item < best.ns_per_item) {
            best = current;
        }
    }
    return best;
}

template <class Queue>
Result run_producer_only(const Scenario& scenario, int reps) {
    Result best{1e100, 0.0};
    for (int rep = 0; rep < reps; ++rep) {
        const Result current = run_producer_only_once<Queue>(scenario);
        if (current.ns_per_item < best.ns_per_item) {
            best = current;
        }
    }
    return best;
}

inline bool g_first_result = true;

template <class Queue>
void print_row(std::string_view name, const Scenario& scenario, Result result) {
    if (g_first_result) {
        g_first_result = false;
    }
    else {
        std::cout << ",\n";
    }
    std::cout << "{\"name\":\"" << name << "\",\"producers\":" << scenario.producers
              << ",\"consumers\":" << scenario.consumers
              << ",\"items_per_producer\":" << scenario.items_per_producer
              << ",\"ns_per_item\":" << result.ns_per_item
              << ",\"million_items_per_second\":" << result.million_items_per_second << "}";
}

template <class Queue>
void run_variant(std::string_view name, const std::vector<Scenario>& scenarios, int reps) {
    for (const Scenario& scenario : scenarios) {
        Result result = scenario.consumers == 0
                            ? run_producer_only<Queue>(scenario, reps)
                            : run_mpmc<Queue>(scenario, reps);
        print_row<Queue>(name, scenario, result);
        std::cout << std::flush;
    }
}

} // namespace

int main(int argc, char** argv) {
    const int reps = argc > 1 ? std::atoi(argv[1]) : 3;
    const bool include_slow = argc > 2 && std::string_view(argv[2]) == "all";

    std::cout << "{\"host\":\""
#ifdef __APPLE__
              << "apple"
#else
              << "unknown"
#endif
              << "\",\"compiler\":\""
#ifdef __clang__
              << "clang-" << __clang_major__
#elif defined(__GNUC__)
              << "gcc-" << __GNUC__
#else
              << "unknown"
#endif
              << "\",\"cpus\":" << std::thread::hardware_concurrency()
              << ",\"results\":[\n";

    const std::vector<Scenario> mpmc = {
        {1, 1, 200000},
        {2, 2, 200000},
        {4, 4, 200000},
        {8, 8, 100000},
        {16, 16, 50000},
    };
    const std::vector<Scenario> producer_only = {
        {1, 0, 1000000},
        {4, 0, 500000},
        {8, 0, 500000},
        {16, 0, 250000},
    };

    run_variant<hakle_default_implicit_queue>("hakle_default_implicit", mpmc, reps);
    run_variant<hakle_default_token_queue>("hakle_default_token", mpmc, reps);
    run_variant<hakle_word_flags_token_queue>("hakle_word_flags_token", mpmc, reps);
    run_variant<hakle_slab_implicit_queue>("hakle_slab_implicit", mpmc, reps);
    run_variant<hakle_slab_token_queue>("hakle_slab_token", mpmc, reps);
    run_variant<moodycamel_implicit_queue>("moodycamel_implicit", mpmc, reps);
    run_variant<moodycamel_token_queue>("moodycamel_token", mpmc, reps);

    run_variant<hakle_default_implicit_queue>("producer_only_hakle_default_implicit", producer_only, reps);
    run_variant<hakle_default_token_queue>("producer_only_hakle_default_token", producer_only, reps);
    run_variant<hakle_word_flags_token_queue>("producer_only_hakle_word_flags_token", producer_only, reps);
    run_variant<hakle_slab_implicit_queue>("producer_only_hakle_slab_implicit", producer_only, reps);
    run_variant<hakle_slab_token_queue>("producer_only_hakle_slab_token", producer_only, reps);
    run_variant<moodycamel_implicit_queue>("producer_only_moodycamel_implicit", producer_only, reps);
    run_variant<moodycamel_token_queue>("producer_only_moodycamel_token", producer_only, reps);

    if (include_slow) {
        run_variant<hakle_arena_implicit_queue<256 * 1024>>("hakle_arena256k_implicit", mpmc, reps);
        run_variant<hakle_arena_implicit_queue<1024 * 1024>>("hakle_arena1m_implicit", mpmc, reps);
        run_variant<hakle_arena_implicit_queue<256 * 1024>>("producer_only_hakle_arena256k_implicit", producer_only, reps);
        run_variant<hakle_arena_implicit_queue<1024 * 1024>>("producer_only_hakle_arena1m_implicit", producer_only, reps);
    }

    std::cout << "]}\n";
    return 0;
}
