#include "ConcurrentQueue/ConcurrentQueue.h"
#include "moodycamel/concurrentqueue.h"

#include <benchmark/benchmark.h>
#include <boost/lockfree/queue.hpp>
#include <oneapi/tbb/concurrent_queue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class hakle_implicit_queue {
public:
  hakle_implicit_queue(std::size_t, std::size_t, std::size_t) {}

  bool enqueue(std::size_t, int value) { return queue_.Enqueue(value); }
  bool try_dequeue(std::size_t, int &value) { return queue_.TryDequeue(value); }

private:
  hakle::ConcurrentQueue<int> queue_;
};

class hakle_token_queue {
public:
  hakle_token_queue(std::size_t, std::size_t producer_count, std::size_t consumer_count) {
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

class moodycamel_implicit_queue {
public:
  moodycamel_implicit_queue(std::size_t, std::size_t, std::size_t) {}

  bool enqueue(std::size_t, int value) { return queue_.enqueue(value); }
  bool try_dequeue(std::size_t, int &value) { return queue_.try_dequeue(value); }

private:
  moodycamel::ConcurrentQueue<int> queue_;
};

class moodycamel_token_queue {
public:
  moodycamel_token_queue(std::size_t, std::size_t producer_count,
                         std::size_t consumer_count) {
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

template <typename Queue> void run_mpmc(benchmark::State &state) {
  const auto producer_count = static_cast<std::size_t>(state.range(0));
  const auto consumer_count = static_cast<std::size_t>(state.range(1));
  const auto items_per_producer = static_cast<std::size_t>(state.range(2));
  const auto total = producer_count * items_per_producer;
  const auto expected_sum = static_cast<std::uint64_t>(total) * (total - 1) / 2;

  for (auto _ : state) {
    static_cast<void>(_);
    bool iteration_ok = true;
    state.PauseTiming();
    {
      Queue queue(total, producer_count, consumer_count);
      std::atomic<std::size_t> consumed{0};
      std::atomic<std::size_t> producers_remaining{producer_count};
      std::atomic<bool> stalled{false};
      std::vector<std::uint64_t> checksums(consumer_count, 0);
      std::latch ready_gate(producer_count + consumer_count);
      std::latch start_gate(1);
      const auto failure_deadline = std::chrono::steady_clock::now() + 30s;

      std::vector<std::thread> consumers;
      consumers.reserve(consumer_count);
      for (std::size_t consumer = 0; consumer < consumer_count; ++consumer) {
        consumers.emplace_back([&, consumer] {
          ready_gate.count_down();
          start_gate.wait();
          std::uint64_t local_sum = 0;

          while (consumed.load(std::memory_order_relaxed) != total) {
            int value = 0;
            if (queue.try_dequeue(consumer, value)) {
              local_sum += static_cast<std::uint64_t>(value);
              consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
              if (producers_remaining.load(std::memory_order_acquire) == 0 &&
                  std::chrono::steady_clock::now() >= failure_deadline) {
                stalled.store(true, std::memory_order_relaxed);
                break;
              }
              std::this_thread::yield();
            }
          }
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

      state.PauseTiming();
      const auto checksum = std::accumulate(checksums.begin(), checksums.end(), std::uint64_t{0});
      iteration_ok = !stalled.load(std::memory_order_relaxed) && consumed == total &&
                     checksum == expected_sum;
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
}

template <typename Queue> void run_bulk_mpmc(benchmark::State &state) {
  constexpr std::size_t bulk_size = 64;
  const auto producer_count = static_cast<std::size_t>(state.range(0));
  const auto consumer_count = static_cast<std::size_t>(state.range(1));
  const auto items_per_producer = static_cast<std::size_t>(state.range(2));
  const auto total = producer_count * items_per_producer;
  const auto expected_sum = static_cast<std::uint64_t>(total) * (total - 1) / 2;

  for (auto _ : state) {
    static_cast<void>(_);
    bool iteration_ok = true;
    state.PauseTiming();
    {
      Queue queue(total, producer_count, consumer_count);
      std::atomic<std::size_t> consumed{0};
      std::atomic<std::size_t> producers_remaining{producer_count};
      std::atomic<bool> stalled{false};
      std::vector<std::uint64_t> checksums(consumer_count, 0);
      std::latch ready_gate(producer_count + consumer_count);
      std::latch start_gate(1);
      const auto failure_deadline = std::chrono::steady_clock::now() + 30s;

      std::vector<std::thread> consumers;
      consumers.reserve(consumer_count);
      for (std::size_t consumer = 0; consumer < consumer_count; ++consumer) {
        consumers.emplace_back([&, consumer] {
          std::array<int, bulk_size> values{};
          ready_gate.count_down();
          start_gate.wait();
          std::uint64_t local_sum = 0;

          while (consumed.load(std::memory_order_relaxed) != total) {
            const auto count = queue.try_dequeue_bulk(consumer, values.data(), values.size());
            if (count != 0) {
              for (std::size_t index = 0; index < count; ++index) {
                local_sum += static_cast<std::uint64_t>(values[index]);
              }
              consumed.fetch_add(count, std::memory_order_relaxed);
            } else {
              if (producers_remaining.load(std::memory_order_acquire) == 0 &&
                  std::chrono::steady_clock::now() >= failure_deadline) {
                stalled.store(true, std::memory_order_relaxed);
                break;
              }
              std::this_thread::yield();
            }
          }
          checksums[consumer] = local_sum;
        });
      }

      std::vector<std::thread> producers;
      producers.reserve(producer_count);
      for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
          std::array<int, bulk_size> values{};
          ready_gate.count_down();
          start_gate.wait();
          std::size_t sequence = 0;
          while (sequence != items_per_producer) {
            const auto count = std::min(bulk_size, items_per_producer - sequence);
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

      state.PauseTiming();
      const auto checksum = std::accumulate(checksums.begin(), checksums.end(), std::uint64_t{0});
      iteration_ok = !stalled.load(std::memory_order_relaxed) && consumed == total &&
                     checksum == expected_sum;
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

void BM_HakleImplicit(benchmark::State &state) { run_mpmc<hakle_implicit_queue>(state); }
void BM_HakleTokens(benchmark::State &state) { run_mpmc<hakle_token_queue>(state); }
void BM_MoodycamelImplicit(benchmark::State &state) {
  run_mpmc<moodycamel_implicit_queue>(state);
}
void BM_MoodycamelTokens(benchmark::State &state) { run_mpmc<moodycamel_token_queue>(state); }
void BM_BoostLockfree(benchmark::State &state) { run_mpmc<boost_lockfree_queue>(state); }
void BM_OneTBB(benchmark::State &state) { run_mpmc<tbb_concurrent_queue>(state); }
void BM_MutexQueue(benchmark::State &state) { run_mpmc<mutex_queue>(state); }
void BM_HakleTokenBulk(benchmark::State &state) { run_bulk_mpmc<hakle_token_queue>(state); }
void BM_MoodycamelTokenBulk(benchmark::State &state) {
  run_bulk_mpmc<moodycamel_token_queue>(state);
}

BENCHMARK(BM_HakleImplicit)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleTokens)->Apply(mpmc_arguments);
BENCHMARK(BM_MoodycamelImplicit)->Apply(mpmc_arguments);
BENCHMARK(BM_MoodycamelTokens)->Apply(mpmc_arguments);
BENCHMARK(BM_BoostLockfree)->Apply(mpmc_arguments);
BENCHMARK(BM_OneTBB)->Apply(mpmc_arguments);
BENCHMARK(BM_MutexQueue)->Apply(mpmc_arguments);
BENCHMARK(BM_HakleTokenBulk)->Apply(mpmc_arguments);
BENCHMARK(BM_MoodycamelTokenBulk)->Apply(mpmc_arguments);

} // namespace

BENCHMARK_MAIN();
