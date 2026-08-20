#include "ConcurrentQueue/ConcurrentQueue.h"
#include "moodycamel/concurrentqueue.h"

#include <benchmark/benchmark.h>
#include <boost/lockfree/queue.hpp>
#include <oneapi/tbb/concurrent_queue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <tuple>
#include <vector>

namespace {

constexpr std::size_t benchmark_block_size = hakle::ConcurrentQueue<int>::BlockSize;
static_assert(benchmark_block_size == moodycamel::ConcurrentQueue<int>::BLOCK_SIZE,
              "Equal-block benchmarks require matching queue block sizes");

using slab_benchmark_allocator = hakle::HakleAllocator<int>;

struct original_hakle_benchmark_traits
    : hakle::ConcurrentQueueDefaultTraits<int, slab_benchmark_allocator> {
  static constexpr bool UseImplicitConsumerCache = false;
  static constexpr bool UseDirectProducerTokenDispatch = false;
};

using original_hakle_benchmark_queue =
    hakle::ConcurrentQueue<int, slab_benchmark_allocator,
                           original_hakle_benchmark_traits>;

template <bool ProducerCache, bool ConsumerCache>
struct cache_ablation_benchmark_traits
    : hakle::ConcurrentQueueDefaultTraits<int, slab_benchmark_allocator> {
  static constexpr bool UseImplicitProducerCache = ProducerCache;
  static constexpr bool UseImplicitConsumerCache = ConsumerCache;
  static constexpr bool UseDirectProducerTokenDispatch = false;
};

template <bool ProducerCache, bool ConsumerCache>
using cache_ablation_benchmark_queue =
    hakle::ConcurrentQueue<
        int, slab_benchmark_allocator,
        cache_ablation_benchmark_traits<ProducerCache, ConsumerCache>>;

template <bool BulkRequisition>
struct block_batch_ablation_benchmark_traits
    : hakle::ConcurrentQueueDefaultTraits<int, slab_benchmark_allocator> {
  static constexpr bool UseBulkBlockRequisition = BulkRequisition;
  static constexpr bool UseFastQueueBulkBlockRequisition = BulkRequisition;
};

template <bool BulkRequisition>
using block_batch_ablation_benchmark_queue =
    hakle::ConcurrentQueue<
        int, slab_benchmark_allocator,
        block_batch_ablation_benchmark_traits<BulkRequisition>>;

struct slab_benchmark_traits
    : hakle::ConcurrentQueueDefaultTraits<int, slab_benchmark_allocator> {
  using Base = hakle::ConcurrentQueueDefaultTraits<int, slab_benchmark_allocator>;
  using typename Base::ExplicitAllocatorType;
  using typename Base::ExplicitBlockType;
  using typename Base::ImplicitAllocatorType;
  using typename Base::ImplicitBlockType;

  static constexpr std::size_t SlabBlockCount = 32;
  using ExplicitBlockManagerType =
      hakle::SlabBlockManager<ExplicitBlockType, ExplicitAllocatorType, SlabBlockCount>;
  using ImplicitBlockManagerType =
      hakle::SlabBlockManager<ImplicitBlockType, ImplicitAllocatorType, SlabBlockCount>;

  static ExplicitBlockManagerType
  MakeDefaultExplicitBlockManager(const ExplicitAllocatorType &allocator) {
    return ExplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
  }

  static ImplicitBlockManagerType
  MakeDefaultImplicitBlockManager(const ImplicitAllocatorType &allocator) {
    return ImplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
  }

  static ExplicitBlockManagerType
  MakeExplicitBlockManager(const ExplicitAllocatorType &allocator,
                           std::size_t block_pool_size) {
    return ExplicitBlockManagerType(block_pool_size, allocator);
  }

  static ImplicitBlockManagerType
  MakeImplicitBlockManager(const ImplicitAllocatorType &allocator,
                           std::size_t block_pool_size) {
    return ImplicitBlockManagerType(block_pool_size, allocator);
  }
};

using slab_benchmark_queue =
    hakle::ConcurrentQueue<int, slab_benchmark_allocator, slab_benchmark_traits>;

template <bool BulkRequisition>
struct slab_batch_ablation_benchmark_traits : slab_benchmark_traits {
  static constexpr bool UseBulkBlockRequisition = BulkRequisition;
  static constexpr bool UseFastQueueBulkBlockRequisition = BulkRequisition;
};

template <bool BulkRequisition>
using slab_batch_ablation_benchmark_queue =
    hakle::ConcurrentQueue<
        int, slab_benchmark_allocator,
        slab_batch_ablation_benchmark_traits<BulkRequisition>>;

template <std::size_t ArenaBytes>
struct arena_benchmark_traits
    : hakle::ConcurrentQueueDefaultTraits<int, slab_benchmark_allocator> {
  using Base = hakle::ConcurrentQueueDefaultTraits<int, slab_benchmark_allocator>;
  using typename Base::ExplicitAllocatorType;
  using typename Base::ExplicitBlockType;
  using typename Base::ImplicitAllocatorType;
  using typename Base::ImplicitBlockType;

  static constexpr std::size_t SlabBlockCount = 32;
  using ExplicitBlockManagerType =
      hakle::ArenaBlockManager<ExplicitBlockType, ExplicitAllocatorType,
                               SlabBlockCount, ArenaBytes>;
  using ImplicitBlockManagerType =
      hakle::ArenaBlockManager<ImplicitBlockType, ImplicitAllocatorType,
                               SlabBlockCount, ArenaBytes>;

  static ExplicitBlockManagerType
  MakeDefaultExplicitBlockManager(const ExplicitAllocatorType &allocator) {
    return ExplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
  }

  static ImplicitBlockManagerType
  MakeDefaultImplicitBlockManager(const ImplicitAllocatorType &allocator) {
    return ImplicitBlockManagerType(Base::InitialBlockPoolSize, allocator);
  }

  static ExplicitBlockManagerType
  MakeExplicitBlockManager(const ExplicitAllocatorType &allocator,
                           std::size_t block_pool_size) {
    return ExplicitBlockManagerType(block_pool_size, allocator);
  }

  static ImplicitBlockManagerType
  MakeImplicitBlockManager(const ImplicitAllocatorType &allocator,
                           std::size_t block_pool_size) {
    return ImplicitBlockManagerType(block_pool_size, allocator);
  }
};

template <std::size_t ArenaBytes>
using arena_benchmark_queue =
    hakle::ConcurrentQueue<int, slab_benchmark_allocator,
                           arena_benchmark_traits<ArenaBytes>>;

enum class initial_pool_mode {
  equal_blocks,
  zero_initial_pool,
};

template <initial_pool_mode Mode>
constexpr std::size_t initial_blocks_for(std::size_t producer_count,
                                         std::size_t items_per_producer) {
  if constexpr (Mode == initial_pool_mode::zero_initial_pool) {
    return 0;
  } else {
    const auto blocks_per_producer =
        (items_per_producer + benchmark_block_size - 1) / benchmark_block_size;
    return producer_count * blocks_per_producer;
  }
}

template <class Queue> class hakle_cache_ablation_implicit_queue {
public:
  hakle_cache_ablation_implicit_queue(std::size_t initial_block_count,
                                      std::size_t, std::size_t)
      : queue_(std::piecewise_construct, std::make_tuple(std::size_t{0}),
               std::make_tuple(initial_block_count), {}) {}

  bool enqueue(std::size_t, int value) { return queue_.Enqueue(value); }
  bool try_dequeue(std::size_t, int &value) { return queue_.TryDequeue(value); }

private:
  Queue queue_;
};

template <class Queue> class hakle_batch_ablation_implicit_queue {
public:
  hakle_batch_ablation_implicit_queue(std::size_t initial_block_count,
                                      std::size_t, std::size_t)
      : queue_(std::piecewise_construct, std::make_tuple(std::size_t{0}),
               std::make_tuple(initial_block_count), {}) {}

  bool enqueue_bulk(std::size_t, const int *values, std::size_t count) {
    return queue_.EnqueueBulk(values, count);
  }

  std::size_t try_dequeue_bulk(std::size_t, int *values,
                               std::size_t count) {
    return queue_.TryDequeueBulk(values, count);
  }

private:
  Queue queue_;
};

template <class Queue> class hakle_batch_ablation_token_queue {
public:
  hakle_batch_ablation_token_queue(std::size_t initial_block_count,
                                   std::size_t producer_count,
                                   std::size_t consumer_count)
      : queue_(std::piecewise_construct, std::make_tuple(initial_block_count),
               std::make_tuple(std::size_t{0}), {}) {
    producers_.reserve(producer_count);
    for (std::size_t index = 0; index < producer_count; ++index) {
      producers_.emplace_back(queue_.GetProducerToken());
    }
    consumers_.reserve(consumer_count);
    for (std::size_t index = 0; index < consumer_count; ++index) {
      consumers_.emplace_back(queue_.GetConsumerToken());
    }
  }

  bool enqueue_bulk(std::size_t producer, const int *values,
                    std::size_t count) {
    return queue_.EnqueueBulk(producers_[producer], values, count);
  }

  std::size_t try_dequeue_bulk(std::size_t consumer, int *values,
                               std::size_t count) {
    return queue_.TryDequeueBulk(consumers_[consumer], values, count);
  }

private:
  Queue queue_;
  std::vector<typename Queue::ProducerToken> producers_;
  std::vector<typename Queue::ConsumerToken> consumers_;
};

using hakle_block_batch_off_queue = hakle_batch_ablation_implicit_queue<
    block_batch_ablation_benchmark_queue<false>>;
using hakle_block_batch_on_queue = hakle_batch_ablation_implicit_queue<
    block_batch_ablation_benchmark_queue<true>>;
using hakle_slab_block_batch_off_queue = hakle_batch_ablation_implicit_queue<
    slab_batch_ablation_benchmark_queue<false>>;
using hakle_slab_block_batch_on_queue = hakle_batch_ablation_implicit_queue<
    slab_batch_ablation_benchmark_queue<true>>;

using hakle_fast_block_batch_off_queue = hakle_batch_ablation_token_queue<
    block_batch_ablation_benchmark_queue<false>>;
using hakle_fast_block_batch_on_queue = hakle_batch_ablation_token_queue<
    block_batch_ablation_benchmark_queue<true>>;
using hakle_slab_fast_block_batch_off_queue =
    hakle_batch_ablation_token_queue<
        slab_batch_ablation_benchmark_queue<false>>;
using hakle_slab_fast_block_batch_on_queue =
    hakle_batch_ablation_token_queue<
        slab_batch_ablation_benchmark_queue<true>>;

using hakle_implicit_no_caches_queue =
    hakle_cache_ablation_implicit_queue<
        cache_ablation_benchmark_queue<false, false>>;
using hakle_implicit_producer_cache_queue =
    hakle_cache_ablation_implicit_queue<
        cache_ablation_benchmark_queue<true, false>>;
using hakle_implicit_consumer_cache_queue =
    hakle_cache_ablation_implicit_queue<
        cache_ablation_benchmark_queue<false, true>>;
using hakle_implicit_both_caches_queue =
    hakle_cache_ablation_implicit_queue<
        cache_ablation_benchmark_queue<true, true>>;

class original_hakle_implicit_queue {
public:
  original_hakle_implicit_queue(std::size_t initial_block_count, std::size_t,
                                std::size_t)
      : queue_(std::piecewise_construct, std::make_tuple(std::size_t{0}),
               std::make_tuple(initial_block_count), {}) {}

  bool enqueue(std::size_t, int value) { return queue_.Enqueue(value); }
  bool try_dequeue(std::size_t, int &value) { return queue_.TryDequeue(value); }

private:
  original_hakle_benchmark_queue queue_;
};

class original_hakle_token_queue {
public:
  original_hakle_token_queue(std::size_t initial_block_count,
                             std::size_t producer_count,
                             std::size_t consumer_count)
      : queue_(std::piecewise_construct, std::make_tuple(initial_block_count),
               std::make_tuple(std::size_t{0}), {}) {
    producers_.reserve(producer_count);
    for (std::size_t index = 0; index < producer_count; ++index) {
      producers_.emplace_back(queue_.GetProducerToken());
    }

    consumers_.reserve(consumer_count);
    for (std::size_t index = 0; index < consumer_count; ++index) {
      consumers_.emplace_back(queue_.GetConsumerToken());
    }
  }

  bool enqueue(std::size_t producer, int value) {
    return queue_.EnqueueWithToken(producers_[producer], value);
  }

  bool try_dequeue(std::size_t consumer, int &value) {
    return queue_.TryDequeue(consumers_[consumer], value);
  }

  bool enqueue_bulk(std::size_t producer, const int *values, std::size_t count) {
    return queue_.EnqueueBulk(producers_[producer], values, count);
  }

  std::size_t try_dequeue_bulk(std::size_t consumer, int *values,
                               std::size_t count) {
    return queue_.TryDequeueBulk(consumers_[consumer], values, count);
  }

private:
  original_hakle_benchmark_queue queue_;
  std::vector<original_hakle_benchmark_queue::ProducerToken> producers_;
  std::vector<original_hakle_benchmark_queue::ConsumerToken> consumers_;
};

class hakle_implicit_queue {
public:
  hakle_implicit_queue(std::size_t initial_block_count, std::size_t, std::size_t)
      : queue_(std::piecewise_construct, std::make_tuple(std::size_t{0}),
               std::make_tuple(initial_block_count), {}) {}

  bool enqueue(std::size_t, int value) { return queue_.Enqueue(value); }
  bool try_dequeue(std::size_t, int &value) { return queue_.TryDequeue(value); }

private:
  hakle::ConcurrentQueue<int> queue_;
};

class hakle_token_queue {
public:
  hakle_token_queue(std::size_t initial_block_count, std::size_t producer_count,
                    std::size_t consumer_count)
      : queue_(std::piecewise_construct, std::make_tuple(initial_block_count),
               std::make_tuple(std::size_t{0}), {}) {
    producers_.reserve(producer_count);
    for (std::size_t index = 0; index < producer_count; ++index) {
      producers_.emplace_back(queue_.GetProducerToken());
    }

    consumers_.reserve(consumer_count);
    for (std::size_t index = 0; index < consumer_count; ++index) {
      consumers_.emplace_back(queue_.GetConsumerToken());
    }
  }

  bool enqueue(std::size_t producer, int value) {
    return queue_.EnqueueWithToken(producers_[producer], value);
  }

  bool try_dequeue(std::size_t consumer, int &value) {
    return queue_.TryDequeue(consumers_[consumer], value);
  }

  bool enqueue_bulk(std::size_t producer, const int *values, std::size_t count) {
    return queue_.EnqueueBulk(producers_[producer], values, count);
  }

  std::size_t try_dequeue_bulk(std::size_t consumer, int *values, std::size_t count) {
    return queue_.TryDequeueBulk(consumers_[consumer], values, count);
  }

private:
  hakle::ConcurrentQueue<int> queue_;
  std::vector<hakle::ConcurrentQueue<int>::ProducerToken> producers_;
  std::vector<hakle::ConcurrentQueue<int>::ConsumerToken> consumers_;
};

class hakle_slab_implicit_queue {
public:
  hakle_slab_implicit_queue(std::size_t initial_block_count, std::size_t,
                            std::size_t)
      : queue_(std::piecewise_construct, std::make_tuple(std::size_t{0}),
               std::make_tuple(initial_block_count), {}) {}

  bool enqueue(std::size_t, int value) { return queue_.Enqueue(value); }
  bool try_dequeue(std::size_t, int &value) { return queue_.TryDequeue(value); }

private:
  slab_benchmark_queue queue_;
};

template <std::size_t ArenaBytes> class hakle_arena_implicit_queue {
public:
  hakle_arena_implicit_queue(std::size_t initial_block_count, std::size_t,
                             std::size_t)
      : queue_(std::piecewise_construct, std::make_tuple(std::size_t{0}),
               std::make_tuple(initial_block_count), {}) {}

  bool enqueue(std::size_t, int value) { return queue_.Enqueue(value); }
  bool try_dequeue(std::size_t, int &value) { return queue_.TryDequeue(value); }

private:
  arena_benchmark_queue<ArenaBytes> queue_;
};

class hakle_slab_token_queue {
public:
  hakle_slab_token_queue(std::size_t initial_block_count,
                         std::size_t producer_count,
                         std::size_t consumer_count)
      : queue_(std::piecewise_construct, std::make_tuple(initial_block_count),
               std::make_tuple(std::size_t{0}), {}) {
    producers_.reserve(producer_count);
    for (std::size_t index = 0; index < producer_count; ++index) {
      producers_.emplace_back(queue_.GetProducerToken());
    }

    consumers_.reserve(consumer_count);
    for (std::size_t index = 0; index < consumer_count; ++index) {
      consumers_.emplace_back(queue_.GetConsumerToken());
    }
  }

  bool enqueue(std::size_t producer, int value) {
    return queue_.EnqueueWithToken(producers_[producer], value);
  }

  bool try_dequeue(std::size_t consumer, int &value) {
    return queue_.TryDequeue(consumers_[consumer], value);
  }

private:
  slab_benchmark_queue queue_;
  std::vector<slab_benchmark_queue::ProducerToken> producers_;
  std::vector<slab_benchmark_queue::ConsumerToken> consumers_;
};

class moodycamel_implicit_queue {
public:
  moodycamel_implicit_queue(std::size_t initial_block_count, std::size_t, std::size_t)
      : queue_(initial_block_count * benchmark_block_size) {}

  bool enqueue(std::size_t, int value) { return queue_.enqueue(value); }
  bool try_dequeue(std::size_t, int &value) { return queue_.try_dequeue(value); }

private:
  moodycamel::ConcurrentQueue<int> queue_;
};

class moodycamel_token_queue {
public:
  moodycamel_token_queue(std::size_t initial_block_count, std::size_t producer_count,
                         std::size_t consumer_count)
      : queue_(initial_block_count * benchmark_block_size) {
    producers_.reserve(producer_count);
    for (std::size_t index = 0; index < producer_count; ++index) {
      producers_.emplace_back(queue_);
    }

    consumers_.reserve(consumer_count);
    for (std::size_t index = 0; index < consumer_count; ++index) {
      consumers_.emplace_back(queue_);
    }
  }

  bool enqueue(std::size_t producer, int value) {
    return queue_.enqueue(producers_[producer], value);
  }

  bool try_dequeue(std::size_t consumer, int &value) {
    return queue_.try_dequeue(consumers_[consumer], value);
  }

  bool enqueue_bulk(std::size_t producer, const int *values, std::size_t count) {
    return queue_.enqueue_bulk(producers_[producer], values, count);
  }

  std::size_t try_dequeue_bulk(std::size_t consumer, int *values, std::size_t count) {
    return queue_.try_dequeue_bulk(consumers_[consumer], values, count);
  }

private:
  moodycamel::ConcurrentQueue<int> queue_;
  std::vector<moodycamel::ProducerToken> producers_;
  std::vector<moodycamel::ConsumerToken> consumers_;
};

class boost_lockfree_queue {
public:
  boost_lockfree_queue(std::size_t, std::size_t, std::size_t) : queue_(0) {}

  bool enqueue(std::size_t, int value) { return queue_.push(value); }
  bool try_dequeue(std::size_t, int &value) { return queue_.pop(value); }

private:
  boost::lockfree::queue<int> queue_;
};

class tbb_concurrent_queue {
public:
  tbb_concurrent_queue(std::size_t, std::size_t, std::size_t) {}

  bool enqueue(std::size_t, int value) {
    queue_.push(value);
    return true;
  }

  bool try_dequeue(std::size_t, int &value) { return queue_.try_pop(value); }

private:
  oneapi::tbb::concurrent_queue<int> queue_;
};

class mutex_queue {
public:
  mutex_queue(std::size_t, std::size_t, std::size_t) {}

  bool enqueue(std::size_t, int value) {
    std::lock_guard lock(mutex_);
    queue_.push(value);
    return true;
  }

  bool try_dequeue(std::size_t, int &value) {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    value = queue_.front();
    queue_.pop();
    return true;
  }

private:
  std::mutex mutex_;
  std::queue<int> queue_;
};

template <typename Queue, initial_pool_mode PoolMode = initial_pool_mode::equal_blocks>
void run_mpmc(benchmark::State &state) {
  const auto producer_count = static_cast<std::size_t>(state.range(0));
  const auto consumer_count = static_cast<std::size_t>(state.range(1));
  const auto items_per_producer = static_cast<std::size_t>(state.range(2));
  const auto total = producer_count * items_per_producer;
  const auto initial_block_count = initial_blocks_for<PoolMode>(producer_count, items_per_producer);
  const auto expected_sum = static_cast<std::uint64_t>(total) * (total - 1) / 2;

  for (auto _ : state) {
    static_cast<void>(_);
    bool iteration_ok = true;
    state.PauseTiming();
    {
      Queue queue(initial_block_count, producer_count, consumer_count);
      std::atomic<std::size_t> producers_remaining{producer_count};
      std::vector<std::size_t> consumed(consumer_count, 0);
      std::vector<std::uint64_t> checksums(consumer_count, 0);
      std::latch ready_gate(producer_count + consumer_count);
      std::latch start_gate(1);

      std::vector<std::thread> consumers;
      consumers.reserve(consumer_count);
      for (std::size_t consumer = 0; consumer < consumer_count; ++consumer) {
        consumers.emplace_back([&, consumer] {
          ready_gate.count_down();
          start_gate.wait();
          std::size_t local_count = 0;
          std::uint64_t local_sum = 0;

          for (;;) {
            int value = 0;
            bool dequeued = queue.try_dequeue(consumer, value);
            if (!dequeued) {
              if (producers_remaining.load(std::memory_order_acquire) == 0) {
                // The acquire observes every enqueue before the producers' release
                // decrement. Retry once after that synchronization before declaring
                // this consumer drained.
                dequeued = queue.try_dequeue(consumer, value);
                if (!dequeued) {
                  break;
                }
              } else {
                std::this_thread::yield();
                continue;
              }
            }

            local_sum += static_cast<std::uint64_t>(value);
            ++local_count;
          }
          consumed[consumer] = local_count;
          checksums[consumer] = local_sum;
        });
      }

      std::vector<std::thread> producers;
      producers.reserve(producer_count);
      for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
          ready_gate.count_down();
          start_gate.wait();
          for (std::size_t sequence = 0; sequence < items_per_producer; ++sequence) {
            const auto value = static_cast<int>(producer * items_per_producer + sequence);
            while (!queue.enqueue(producer, value)) {
              std::this_thread::yield();
            }
          }
          producers_remaining.fetch_sub(1, std::memory_order_release);
        });
      }

      ready_gate.wait();
      state.ResumeTiming();
      start_gate.count_down();

      for (auto &thread : producers) {
        thread.join();
      }
      for (auto &thread : consumers) {
        thread.join();
      }

      // A concurrent dequeue may transiently report empty while another consumer
      // is completing an operation. Once all worker threads have joined, perform
      // one serial drain so termination cannot leave valid items unaccounted for.
      std::size_t final_count = 0;
      std::uint64_t final_sum = 0;
      int final_value = 0;
      while (queue.try_dequeue(0, final_value)) {
        ++final_count;
        final_sum += static_cast<std::uint64_t>(final_value);
      }

      state.PauseTiming();
      const auto consumed_count =
          final_count + std::accumulate(consumed.begin(), consumed.end(), std::size_t{0});
      const auto checksum =
          final_sum + std::accumulate(checksums.begin(), checksums.end(), std::uint64_t{0});
      iteration_ok = consumed_count == total && checksum == expected_sum;
      if (!iteration_ok) {
        state.SkipWithError("queue lost, duplicated, or corrupted values");
      }
    }
    state.ResumeTiming();
    if (!iteration_ok) {
      break;
    }
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * total));
  state.counters["RequestedInitialBlocks"] = static_cast<double>(initial_block_count);
}

template <typename Queue, initial_pool_mode PoolMode = initial_pool_mode::equal_blocks,
          std::size_t BulkSize = 64>
void run_bulk_mpmc(benchmark::State &state) {
  const auto producer_count = static_cast<std::size_t>(state.range(0));
  const auto consumer_count = static_cast<std::size_t>(state.range(1));
  const auto items_per_producer = static_cast<std::size_t>(state.range(2));
  const auto total = producer_count * items_per_producer;
  const auto initial_block_count = initial_blocks_for<PoolMode>(producer_count, items_per_producer);
  const auto expected_sum = static_cast<std::uint64_t>(total) * (total - 1) / 2;

  for (auto _ : state) {
    static_cast<void>(_);
    bool iteration_ok = true;
    state.PauseTiming();
    {
      Queue queue(initial_block_count, producer_count, consumer_count);
      std::atomic<std::size_t> producers_remaining{producer_count};
      std::vector<std::size_t> consumed(consumer_count, 0);
      std::vector<std::uint64_t> checksums(consumer_count, 0);
      std::latch ready_gate(producer_count + consumer_count);
      std::latch start_gate(1);

      std::vector<std::thread> consumers;
      consumers.reserve(consumer_count);
      for (std::size_t consumer = 0; consumer < consumer_count; ++consumer) {
        consumers.emplace_back([&, consumer] {
          std::array<int, BulkSize> values{};
          ready_gate.count_down();
          start_gate.wait();
          std::size_t local_count = 0;
          std::uint64_t local_sum = 0;

          for (;;) {
            auto count = queue.try_dequeue_bulk(consumer, values.data(), values.size());
            if (count == 0) {
              if (producers_remaining.load(std::memory_order_acquire) == 0) {
                count = queue.try_dequeue_bulk(consumer, values.data(), values.size());
                if (count == 0) {
                  break;
                }
              } else {
                std::this_thread::yield();
                continue;
              }
            }

            for (std::size_t index = 0; index < count; ++index) {
              local_sum += static_cast<std::uint64_t>(values[index]);
            }
            local_count += count;
          }
          consumed[consumer] = local_count;
          checksums[consumer] = local_sum;
        });
      }

      std::vector<std::thread> producers;
      producers.reserve(producer_count);
      for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
          std::array<int, BulkSize> values{};
          ready_gate.count_down();
          start_gate.wait();
          std::size_t sequence = 0;
          while (sequence != items_per_producer) {
            const auto count = std::min(BulkSize, items_per_producer - sequence);
            for (std::size_t index = 0; index < count; ++index) {
              values[index] = static_cast<int>(producer * items_per_producer + sequence + index);
            }
            while (!queue.enqueue_bulk(producer, values.data(), count)) {
              std::this_thread::yield();
            }
            sequence += count;
          }
          producers_remaining.fetch_sub(1, std::memory_order_release);
        });
      }

      ready_gate.wait();
      state.ResumeTiming();
      start_gate.count_down();

      for (auto &thread : producers) {
        thread.join();
      }
      for (auto &thread : consumers) {
        thread.join();
      }

      std::size_t final_count = 0;
      std::uint64_t final_sum = 0;
      std::array<int, BulkSize> final_values{};
      for (;;) {
        const auto count =
            queue.try_dequeue_bulk(0, final_values.data(), final_values.size());
        if (count == 0) {
          break;
        }
        final_count += count;
        for (std::size_t index = 0; index < count; ++index) {
          final_sum += static_cast<std::uint64_t>(final_values[index]);
        }
      }

      state.PauseTiming();
      const auto consumed_count =
          final_count + std::accumulate(consumed.begin(), consumed.end(), std::size_t{0});
      const auto checksum =
          final_sum + std::accumulate(checksums.begin(), checksums.end(), std::uint64_t{0});
      iteration_ok = consumed_count == total && checksum == expected_sum;
      if (!iteration_ok) {
        state.SkipWithError("bulk queue lost, duplicated, or corrupted values");
      }
    }
    state.ResumeTiming();
    if (!iteration_ok) {
      break;
    }
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * total));
  state.counters["RequestedInitialBlocks"] = static_cast<double>(initial_block_count);
}

template <typename Queue, initial_pool_mode PoolMode = initial_pool_mode::equal_blocks>
void run_producer_only(benchmark::State &state) {
  const auto producer_count = static_cast<std::size_t>(state.range(0));
  const auto items_per_producer = static_cast<std::size_t>(state.range(1));
  const auto total = producer_count * items_per_producer;
  const auto initial_block_count =
      initial_blocks_for<PoolMode>(producer_count, items_per_producer);
  const auto expected_sum = static_cast<std::uint64_t>(total) * (total - 1) / 2;

  for (auto _ : state) {
    static_cast<void>(_);
    bool iteration_ok = true;
    state.PauseTiming();
    {
      Queue queue(initial_block_count, producer_count, 1);
      std::latch ready_gate(producer_count);
      std::latch start_gate(1);
      std::vector<std::thread> producers;
      producers.reserve(producer_count);

      for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
          ready_gate.count_down();
          start_gate.wait();
          for (std::size_t sequence = 0; sequence < items_per_producer; ++sequence) {
            const auto value =
                static_cast<int>(producer * items_per_producer + sequence);
            while (!queue.enqueue(producer, value)) {
              std::this_thread::yield();
            }
          }
        });
      }

      ready_gate.wait();
      state.ResumeTiming();
      start_gate.count_down();
      for (auto &thread : producers) {
        thread.join();
      }
      state.PauseTiming();

      std::size_t consumed = 0;
      std::uint64_t checksum = 0;
      int value = 0;
      while (queue.try_dequeue(0, value)) {
        ++consumed;
        checksum += static_cast<std::uint64_t>(value);
      }
      iteration_ok = consumed == total && checksum == expected_sum;
      if (!iteration_ok) {
        state.SkipWithError("producer-only queue lost, duplicated, or corrupted values");
      }
    }
    state.ResumeTiming();
    if (!iteration_ok) {
      break;
    }
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * total));
  state.counters["RequestedInitialBlocks"] = static_cast<double>(initial_block_count);
}

void mpmc_arguments(benchmark::internal::Benchmark *benchmark) {
  benchmark->Args({1, 1, 100000})
      ->Args({2, 2, 100000})
      ->Args({4, 4, 100000})
      ->Args({8, 8, 50000})
      ->Args({12, 12, 50000})
      ->Args({16, 16, 50000})
      ->Args({20, 20, 50000})
      ->Args({24, 24, 50000})
      ->ArgNames({"producers", "consumers", "items_per_producer"})
      ->UseRealTime()
      ->Unit(benchmark::kMillisecond)
      ->MinTime(0.25);
}

void block_batch_arguments(benchmark::internal::Benchmark *benchmark) {
  benchmark->Args({1, 1, 100000})
      ->Args({2, 2, 100000})
      ->Args({4, 4, 100000})
      ->Args({8, 8, 50000})
      ->Args({16, 16, 50000})
      ->ArgNames({"producers", "consumers", "items_per_producer"})
      ->UseRealTime()
      ->Unit(benchmark::kMillisecond)
      ->MinTime(0.15);
}

void producer_only_arguments(benchmark::internal::Benchmark *benchmark) {
  benchmark->Args({1, 100000})
      ->Args({2, 100000})
      ->Args({4, 100000})
      ->Args({8, 50000})
      ->Args({12, 50000})
      ->Args({16, 50000})
      ->Args({20, 50000})
      ->Args({24, 50000})
      ->ArgNames({"producers", "items_per_producer"})
      ->UseRealTime()
      ->Unit(benchmark::kMillisecond)
      ->Iterations(1);
}

void BM_HakleImplicitEqualBlocks(benchmark::State &state) {
  run_mpmc<original_hakle_implicit_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_HakleImplicitNoCachesEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_implicit_no_caches_queue,
           initial_pool_mode::equal_blocks>(state);
}
void BM_HakleImplicitProducerCacheOnlyEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_implicit_producer_cache_queue,
           initial_pool_mode::equal_blocks>(state);
}
void BM_HakleImplicitConsumerCacheOnlyEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_implicit_consumer_cache_queue,
           initial_pool_mode::equal_blocks>(state);
}
void BM_HakleImplicitBothCachesEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_implicit_both_caches_queue,
           initial_pool_mode::equal_blocks>(state);
}
void BM_HakleImplicitZeroInitialPool(benchmark::State &state) {
  run_mpmc<original_hakle_implicit_queue,
           initial_pool_mode::zero_initial_pool>(state);
}
void BM_HakleTokensEqualBlocks(benchmark::State &state) {
  run_mpmc<original_hakle_token_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_HakleTokensZeroInitialPool(benchmark::State &state) {
  run_mpmc<original_hakle_token_queue,
           initial_pool_mode::zero_initial_pool>(state);
}
void BM_HakleOptimizedImplicitEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_implicit_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_HakleOptimizedImplicitZeroInitialPool(benchmark::State &state) {
  run_mpmc<hakle_implicit_queue, initial_pool_mode::zero_initial_pool>(state);
}
void BM_HakleOptimizedTokensEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_token_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_HakleOptimizedTokensZeroInitialPool(benchmark::State &state) {
  run_mpmc<hakle_token_queue, initial_pool_mode::zero_initial_pool>(state);
}
void BM_HakleSlabImplicitEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_slab_implicit_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_HakleSlabImplicitZeroInitialPool(benchmark::State &state) {
  run_mpmc<hakle_slab_implicit_queue, initial_pool_mode::zero_initial_pool>(state);
}
void BM_HakleArena256KImplicitEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_arena_implicit_queue<256 * 1024>,
           initial_pool_mode::equal_blocks>(state);
}
void BM_HakleArena256KImplicitZeroInitialPool(benchmark::State &state) {
  run_mpmc<hakle_arena_implicit_queue<256 * 1024>,
           initial_pool_mode::zero_initial_pool>(state);
}
void BM_HakleArena1MImplicitEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_arena_implicit_queue<1024 * 1024>,
           initial_pool_mode::equal_blocks>(state);
}
void BM_HakleArena1MImplicitZeroInitialPool(benchmark::State &state) {
  run_mpmc<hakle_arena_implicit_queue<1024 * 1024>,
           initial_pool_mode::zero_initial_pool>(state);
}
void BM_HakleArena2MImplicitEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_arena_implicit_queue<2 * 1024 * 1024>,
           initial_pool_mode::equal_blocks>(state);
}
void BM_HakleArena2MImplicitZeroInitialPool(benchmark::State &state) {
  run_mpmc<hakle_arena_implicit_queue<2 * 1024 * 1024>,
           initial_pool_mode::zero_initial_pool>(state);
}
void BM_HakleSlabTokensEqualBlocks(benchmark::State &state) {
  run_mpmc<hakle_slab_token_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_HakleSlabTokensZeroInitialPool(benchmark::State &state) {
  run_mpmc<hakle_slab_token_queue, initial_pool_mode::zero_initial_pool>(state);
}
void BM_MoodycamelImplicitEqualBlocks(benchmark::State &state) {
  run_mpmc<moodycamel_implicit_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_MoodycamelImplicitZeroInitialPool(benchmark::State &state) {
  run_mpmc<moodycamel_implicit_queue, initial_pool_mode::zero_initial_pool>(state);
}
void BM_MoodycamelTokensEqualBlocks(benchmark::State &state) {
  run_mpmc<moodycamel_token_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_MoodycamelTokensZeroInitialPool(benchmark::State &state) {
  run_mpmc<moodycamel_token_queue, initial_pool_mode::zero_initial_pool>(state);
}
void BM_BoostLockfree(benchmark::State &state) { run_mpmc<boost_lockfree_queue>(state); }
void BM_OneTBB(benchmark::State &state) { run_mpmc<tbb_concurrent_queue>(state); }
void BM_MutexQueue(benchmark::State &state) { run_mpmc<mutex_queue>(state); }

void BM_BlockBatchHakleOffEqualBlocksBulk64(benchmark::State &state) {
  run_bulk_mpmc<hakle_block_batch_off_queue,
                initial_pool_mode::equal_blocks, 64>(state);
}
void BM_BlockBatchHakleOnEqualBlocksBulk64(benchmark::State &state) {
  run_bulk_mpmc<hakle_block_batch_on_queue,
                initial_pool_mode::equal_blocks, 64>(state);
}
void BM_BlockBatchHakleOffEqualBlocksBulk256(benchmark::State &state) {
  run_bulk_mpmc<hakle_block_batch_off_queue,
                initial_pool_mode::equal_blocks, 256>(state);
}
void BM_BlockBatchHakleOnEqualBlocksBulk256(benchmark::State &state) {
  run_bulk_mpmc<hakle_block_batch_on_queue,
                initial_pool_mode::equal_blocks, 256>(state);
}
void BM_BlockBatchSlabOffZeroPoolBulk64(benchmark::State &state) {
  run_bulk_mpmc<hakle_slab_block_batch_off_queue,
                initial_pool_mode::zero_initial_pool, 64>(state);
}
void BM_BlockBatchSlabOnZeroPoolBulk64(benchmark::State &state) {
  run_bulk_mpmc<hakle_slab_block_batch_on_queue,
                initial_pool_mode::zero_initial_pool, 64>(state);
}
void BM_BlockBatchSlabOffZeroPoolBulk256(benchmark::State &state) {
  run_bulk_mpmc<hakle_slab_block_batch_off_queue,
                initial_pool_mode::zero_initial_pool, 256>(state);
}
void BM_BlockBatchSlabOnZeroPoolBulk256(benchmark::State &state) {
  run_bulk_mpmc<hakle_slab_block_batch_on_queue,
                initial_pool_mode::zero_initial_pool, 256>(state);
}

void BM_FastBlockBatchHakleOffEqualBlocksBulk64(benchmark::State &state) {
  run_bulk_mpmc<hakle_fast_block_batch_off_queue,
                initial_pool_mode::equal_blocks, 64>(state);
}
void BM_FastBlockBatchHakleOnEqualBlocksBulk64(benchmark::State &state) {
  run_bulk_mpmc<hakle_fast_block_batch_on_queue,
                initial_pool_mode::equal_blocks, 64>(state);
}
void BM_FastBlockBatchHakleOffEqualBlocksBulk256(benchmark::State &state) {
  run_bulk_mpmc<hakle_fast_block_batch_off_queue,
                initial_pool_mode::equal_blocks, 256>(state);
}
void BM_FastBlockBatchHakleOnEqualBlocksBulk256(benchmark::State &state) {
  run_bulk_mpmc<hakle_fast_block_batch_on_queue,
                initial_pool_mode::equal_blocks, 256>(state);
}
void BM_FastBlockBatchSlabOffZeroPoolBulk64(benchmark::State &state) {
  run_bulk_mpmc<hakle_slab_fast_block_batch_off_queue,
                initial_pool_mode::zero_initial_pool, 64>(state);
}
void BM_FastBlockBatchSlabOnZeroPoolBulk64(benchmark::State &state) {
  run_bulk_mpmc<hakle_slab_fast_block_batch_on_queue,
                initial_pool_mode::zero_initial_pool, 64>(state);
}
void BM_FastBlockBatchSlabOffZeroPoolBulk256(benchmark::State &state) {
  run_bulk_mpmc<hakle_slab_fast_block_batch_off_queue,
                initial_pool_mode::zero_initial_pool, 256>(state);
}
void BM_FastBlockBatchSlabOnZeroPoolBulk256(benchmark::State &state) {
  run_bulk_mpmc<hakle_slab_fast_block_batch_on_queue,
                initial_pool_mode::zero_initial_pool, 256>(state);
}

void BM_HakleTokenBulkEqualBlocks(benchmark::State &state) {
  run_bulk_mpmc<original_hakle_token_queue,
                initial_pool_mode::equal_blocks>(state);
}
void BM_HakleTokenBulkZeroInitialPool(benchmark::State &state) {
  run_bulk_mpmc<original_hakle_token_queue,
                initial_pool_mode::zero_initial_pool>(state);
}
void BM_HakleOptimizedTokenBulkEqualBlocks(benchmark::State &state) {
  run_bulk_mpmc<hakle_token_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_HakleOptimizedTokenBulkZeroInitialPool(benchmark::State &state) {
  run_bulk_mpmc<hakle_token_queue,
                initial_pool_mode::zero_initial_pool>(state);
}
void BM_MoodycamelTokenBulkEqualBlocks(benchmark::State &state) {
  run_bulk_mpmc<moodycamel_token_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_MoodycamelTokenBulkZeroInitialPool(benchmark::State &state) {
  run_bulk_mpmc<moodycamel_token_queue, initial_pool_mode::zero_initial_pool>(state);
}
void BM_ProducerOnlyHakleImplicitEqualBlocks(benchmark::State &state) {
  run_producer_only<original_hakle_implicit_queue,
                    initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyHakleImplicitNoProducerCacheEqualBlocks(
    benchmark::State &state) {
  run_producer_only<hakle_implicit_no_caches_queue,
                    initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyHakleImplicitProducerCacheEqualBlocks(
    benchmark::State &state) {
  run_producer_only<hakle_implicit_producer_cache_queue,
                    initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyHakleImplicitZeroInitialPool(benchmark::State &state) {
  run_producer_only<original_hakle_implicit_queue,
                    initial_pool_mode::zero_initial_pool>(state);
}
void BM_ProducerOnlyHakleTokensEqualBlocks(benchmark::State &state) {
  run_producer_only<original_hakle_token_queue,
                    initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyHakleTokensZeroInitialPool(benchmark::State &state) {
  run_producer_only<original_hakle_token_queue,
                    initial_pool_mode::zero_initial_pool>(state);
}
void BM_ProducerOnlyHakleSlabImplicitEqualBlocks(benchmark::State &state) {
  run_producer_only<hakle_slab_implicit_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyHakleSlabImplicitZeroInitialPool(benchmark::State &state) {
  run_producer_only<hakle_slab_implicit_queue,
                    initial_pool_mode::zero_initial_pool>(state);
}
void BM_ProducerOnlyHakleArena256KImplicitEqualBlocks(benchmark::State &state) {
  run_producer_only<hakle_arena_implicit_queue<256 * 1024>,
                    initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyHakleArena256KImplicitZeroInitialPool(
    benchmark::State &state) {
  run_producer_only<hakle_arena_implicit_queue<256 * 1024>,
                    initial_pool_mode::zero_initial_pool>(state);
}
void BM_ProducerOnlyHakleArena1MImplicitEqualBlocks(benchmark::State &state) {
  run_producer_only<hakle_arena_implicit_queue<1024 * 1024>,
                    initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyHakleArena1MImplicitZeroInitialPool(
    benchmark::State &state) {
  run_producer_only<hakle_arena_implicit_queue<1024 * 1024>,
                    initial_pool_mode::zero_initial_pool>(state);
}
void BM_ProducerOnlyHakleArena2MImplicitEqualBlocks(benchmark::State &state) {
  run_producer_only<hakle_arena_implicit_queue<2 * 1024 * 1024>,
                    initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyHakleArena2MImplicitZeroInitialPool(
    benchmark::State &state) {
  run_producer_only<hakle_arena_implicit_queue<2 * 1024 * 1024>,
                    initial_pool_mode::zero_initial_pool>(state);
}
void BM_ProducerOnlyHakleSlabTokensEqualBlocks(benchmark::State &state) {
  run_producer_only<hakle_slab_token_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyHakleSlabTokensZeroInitialPool(benchmark::State &state) {
  run_producer_only<hakle_slab_token_queue,
                    initial_pool_mode::zero_initial_pool>(state);
}
void BM_ProducerOnlyMoodycamelImplicitEqualBlocks(benchmark::State &state) {
  run_producer_only<moodycamel_implicit_queue,
                    initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyMoodycamelImplicitZeroInitialPool(benchmark::State &state) {
  run_producer_only<moodycamel_implicit_queue,
                    initial_pool_mode::zero_initial_pool>(state);
}
void BM_ProducerOnlyMoodycamelTokensEqualBlocks(benchmark::State &state) {
  run_producer_only<moodycamel_token_queue, initial_pool_mode::equal_blocks>(state);
}
void BM_ProducerOnlyMoodycamelTokensZeroInitialPool(benchmark::State &state) {
  run_producer_only<moodycamel_token_queue,
                    initial_pool_mode::zero_initial_pool>(state);
}

BENCHMARK(BM_HakleImplicitEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleImplicitNoCachesEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleImplicitProducerCacheOnlyEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleImplicitConsumerCacheOnlyEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleImplicitBothCachesEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleImplicitZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleTokensEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleTokensZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleOptimizedImplicitEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleOptimizedImplicitZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleOptimizedTokensEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleOptimizedTokensZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleSlabImplicitEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleSlabImplicitZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleArena256KImplicitEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleArena256KImplicitZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleArena1MImplicitEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleArena1MImplicitZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleArena2MImplicitEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleArena2MImplicitZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleSlabTokensEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleSlabTokensZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_MoodycamelImplicitEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_MoodycamelImplicitZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_MoodycamelTokensEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_MoodycamelTokensZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_BoostLockfree)->Apply(mpmc_arguments);
BENCHMARK(BM_OneTBB)->Apply(mpmc_arguments);
BENCHMARK(BM_MutexQueue)->Apply(mpmc_arguments);
BENCHMARK(BM_BlockBatchHakleOffEqualBlocksBulk64)->Apply(block_batch_arguments);
BENCHMARK(BM_BlockBatchHakleOnEqualBlocksBulk64)->Apply(block_batch_arguments);
BENCHMARK(BM_BlockBatchHakleOffEqualBlocksBulk256)->Apply(block_batch_arguments);
BENCHMARK(BM_BlockBatchHakleOnEqualBlocksBulk256)->Apply(block_batch_arguments);
BENCHMARK(BM_BlockBatchSlabOffZeroPoolBulk64)->Apply(block_batch_arguments);
BENCHMARK(BM_BlockBatchSlabOnZeroPoolBulk64)->Apply(block_batch_arguments);
BENCHMARK(BM_BlockBatchSlabOffZeroPoolBulk256)->Apply(block_batch_arguments);
BENCHMARK(BM_BlockBatchSlabOnZeroPoolBulk256)->Apply(block_batch_arguments);
BENCHMARK(BM_FastBlockBatchHakleOffEqualBlocksBulk64)->Apply(block_batch_arguments);
BENCHMARK(BM_FastBlockBatchHakleOnEqualBlocksBulk64)->Apply(block_batch_arguments);
BENCHMARK(BM_FastBlockBatchHakleOffEqualBlocksBulk256)->Apply(block_batch_arguments);
BENCHMARK(BM_FastBlockBatchHakleOnEqualBlocksBulk256)->Apply(block_batch_arguments);
BENCHMARK(BM_FastBlockBatchSlabOffZeroPoolBulk64)->Apply(block_batch_arguments);
BENCHMARK(BM_FastBlockBatchSlabOnZeroPoolBulk64)->Apply(block_batch_arguments);
BENCHMARK(BM_FastBlockBatchSlabOffZeroPoolBulk256)->Apply(block_batch_arguments);
BENCHMARK(BM_FastBlockBatchSlabOnZeroPoolBulk256)->Apply(block_batch_arguments);
BENCHMARK(BM_HakleTokenBulkEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleTokenBulkZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleOptimizedTokenBulkEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleOptimizedTokenBulkZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_MoodycamelTokenBulkEqualBlocks)->Apply(mpmc_arguments);
BENCHMARK(BM_MoodycamelTokenBulkZeroInitialPool)->Apply(mpmc_arguments);
BENCHMARK(BM_ProducerOnlyHakleImplicitEqualBlocks)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleImplicitNoProducerCacheEqualBlocks)
    ->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleImplicitProducerCacheEqualBlocks)
    ->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleImplicitZeroInitialPool)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleTokensEqualBlocks)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleTokensZeroInitialPool)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleSlabImplicitEqualBlocks)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleSlabImplicitZeroInitialPool)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleArena256KImplicitEqualBlocks)
    ->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleArena256KImplicitZeroInitialPool)
    ->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleArena1MImplicitEqualBlocks)
    ->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleArena1MImplicitZeroInitialPool)
    ->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleArena2MImplicitEqualBlocks)
    ->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleArena2MImplicitZeroInitialPool)
    ->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleSlabTokensEqualBlocks)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyHakleSlabTokensZeroInitialPool)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyMoodycamelImplicitEqualBlocks)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyMoodycamelImplicitZeroInitialPool)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyMoodycamelTokensEqualBlocks)->Apply(producer_only_arguments);
BENCHMARK(BM_ProducerOnlyMoodycamelTokensZeroInitialPool)->Apply(producer_only_arguments);

} // namespace

BENCHMARK_MAIN();
