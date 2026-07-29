#include <bit>

#include "ConcurrentQueue/ConcurrentQueue.h"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <numeric>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class value_verifier {
public:
  explicit value_verifier(std::size_t total) : seen_(total) {
    for (auto &count : seen_) {
      count.store(0, std::memory_order_relaxed);
    }
  }

  void record(int value) noexcept {
    if (value < 0 || static_cast<std::size_t>(value) >= seen_.size()) {
      out_of_range_.fetch_add(1, std::memory_order_relaxed);
    } else if (seen_[static_cast<std::size_t>(value)].fetch_add(
                   1, std::memory_order_relaxed) != 0) {
      duplicates_.fetch_add(1, std::memory_order_relaxed);
    }

    sum_.fetch_add(static_cast<std::uint64_t>(value), std::memory_order_relaxed);
    consumed_.fetch_add(1, std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t consumed() const noexcept {
    return consumed_.load(std::memory_order_relaxed);
  }

  void check_complete() const {
    const auto total = seen_.size();
    const auto expected_sum = static_cast<std::uint64_t>(total) * (total - 1) / 2;

    CHECK(consumed() == total);
    CHECK(duplicates_.load(std::memory_order_relaxed) == 0);
    CHECK(out_of_range_.load(std::memory_order_relaxed) == 0);
    CHECK(sum_.load(std::memory_order_relaxed) == expected_sum);
    CHECK(std::all_of(seen_.begin(), seen_.end(), [](const auto &count) {
      return count.load(std::memory_order_relaxed) == 1;
    }));
  }

private:
  std::vector<std::atomic<unsigned int>> seen_;
  std::atomic<std::size_t> consumed_{0};
  std::atomic<std::size_t> duplicates_{0};
  std::atomic<std::size_t> out_of_range_{0};
  std::atomic<std::uint64_t> sum_{0};
};

void test_implicit_mpmc() {
  constexpr std::size_t producer_count = 4;
  constexpr std::size_t consumer_count = 4;
  constexpr std::size_t items_per_producer = 5000;
  constexpr std::size_t total = producer_count * items_per_producer;

  hakle::ConcurrentQueue<int> queue;
  value_verifier verifier(total);
  std::atomic<bool> producers_done{false};
  std::atomic<bool> timed_out{false};
  std::atomic<std::size_t> enqueue_failures{0};
  std::latch start_gate(producer_count + consumer_count + 1);
  const auto deadline = std::chrono::steady_clock::now() + 15s;

  std::vector<std::thread> consumers;
  for (std::size_t index = 0; index < consumer_count; ++index) {
    consumers.emplace_back([&] {
      start_gate.arrive_and_wait();
      for (;;) {
        int value = 0;
        if (queue.TryDequeue(value)) {
          verifier.record(value);
          continue;
        }
        if (producers_done.load(std::memory_order_acquire) && verifier.consumed() == total) {
          return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          timed_out.store(true, std::memory_order_relaxed);
          return;
        }
        std::this_thread::yield();
      }
    });
  }

  std::vector<std::thread> producers;
  for (std::size_t producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([&, producer] {
      start_gate.arrive_and_wait();
      for (std::size_t sequence = 0; sequence < items_per_producer; ++sequence) {
        const auto value = static_cast<int>(producer * items_per_producer + sequence);
        if (!queue.Enqueue(value)) {
          enqueue_failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  start_gate.arrive_and_wait();
  for (auto &thread : producers) {
    thread.join();
  }
  producers_done.store(true, std::memory_order_release);
  for (auto &thread : consumers) {
    thread.join();
  }

  CHECK(!timed_out.load(std::memory_order_relaxed));
  CHECK(enqueue_failures.load(std::memory_order_relaxed) == 0);
  CHECK(queue.Size() == 0);
  verifier.check_complete();
}

void test_explicit_token_mpmc() {
  constexpr std::size_t producer_count = 4;
  constexpr std::size_t consumer_count = 4;
  constexpr std::size_t items_per_producer = 5000;
  constexpr std::size_t total = producer_count * items_per_producer;

  hakle::ConcurrentQueue<int> queue;
  std::vector<hakle::ConcurrentQueue<int>::ProducerToken> producer_tokens;
  producer_tokens.reserve(producer_count);
  for (std::size_t index = 0; index < producer_count; ++index) {
    producer_tokens.emplace_back(queue.GetProducerToken());
  }

  value_verifier verifier(total);
  std::atomic<bool> producers_done{false};
  std::atomic<bool> timed_out{false};
  std::atomic<std::size_t> enqueue_failures{0};
  std::latch start_gate(producer_count + consumer_count + 1);
  const auto deadline = std::chrono::steady_clock::now() + 15s;

  std::vector<std::thread> consumers;
  for (std::size_t index = 0; index < consumer_count; ++index) {
    consumers.emplace_back([&] {
      auto token = queue.GetConsumerToken();
      start_gate.arrive_and_wait();
      for (;;) {
        int value = 0;
        if (queue.TryDequeue(token, value)) {
          verifier.record(value);
          continue;
        }
        if (producers_done.load(std::memory_order_acquire) && verifier.consumed() == total) {
          return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          timed_out.store(true, std::memory_order_relaxed);
          return;
        }
        std::this_thread::yield();
      }
    });
  }

  std::vector<std::thread> producers;
  for (std::size_t producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([&, producer] {
      start_gate.arrive_and_wait();
      auto &token = producer_tokens[producer];
      for (std::size_t sequence = 0; sequence < items_per_producer; ++sequence) {
        const auto value = static_cast<int>(producer * items_per_producer + sequence);
        if (!queue.EnqueueWithToken(token, value)) {
          enqueue_failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  start_gate.arrive_and_wait();
  for (auto &thread : producers) {
    thread.join();
  }
  producers_done.store(true, std::memory_order_release);
  for (auto &thread : consumers) {
    thread.join();
  }

  CHECK(!timed_out.load(std::memory_order_relaxed));
  CHECK(enqueue_failures.load(std::memory_order_relaxed) == 0);
  CHECK(queue.Size() == 0);
  verifier.check_complete();
}

void test_bulk_mpmc() {
  constexpr std::size_t producer_count = 4;
  constexpr std::size_t consumer_count = 4;
  constexpr std::size_t items_per_producer = 4097;
  constexpr std::size_t producer_bulk = 37;
  constexpr std::size_t consumer_bulk = 53;
  constexpr std::size_t total = producer_count * items_per_producer;

  hakle::ConcurrentQueue<int> queue;
  value_verifier verifier(total);
  std::atomic<bool> producers_done{false};
  std::atomic<bool> timed_out{false};
  std::atomic<std::size_t> enqueue_failures{0};
  std::latch start_gate(producer_count + consumer_count + 1);
  const auto deadline = std::chrono::steady_clock::now() + 15s;

  std::vector<std::thread> consumers;
  for (std::size_t index = 0; index < consumer_count; ++index) {
    consumers.emplace_back([&] {
      std::array<int, consumer_bulk> values{};
      start_gate.arrive_and_wait();
      for (;;) {
        const auto count = queue.TryDequeueBulk(values.begin(), values.size());
        if (count != 0) {
          for (std::size_t item = 0; item < count; ++item) {
            verifier.record(values[item]);
          }
          continue;
        }
        if (producers_done.load(std::memory_order_acquire) && verifier.consumed() == total) {
          return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          timed_out.store(true, std::memory_order_relaxed);
          return;
        }
        std::this_thread::yield();
      }
    });
  }

  std::vector<std::thread> producers;
  for (std::size_t producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([&, producer] {
      std::array<int, producer_bulk> values{};
      start_gate.arrive_and_wait();
      std::size_t sequence = 0;
      while (sequence != items_per_producer) {
        const auto count = std::min(producer_bulk, items_per_producer - sequence);
        for (std::size_t item = 0; item < count; ++item) {
          values[item] = static_cast<int>(producer * items_per_producer + sequence + item);
        }
        if (!queue.EnqueueBulk(values.begin(), count)) {
          enqueue_failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        sequence += count;
      }
    });
  }

  start_gate.arrive_and_wait();
  for (auto &thread : producers) {
    thread.join();
  }
  producers_done.store(true, std::memory_order_release);
  for (auto &thread : consumers) {
    thread.join();
  }

  CHECK(!timed_out.load(std::memory_order_relaxed));
  CHECK(enqueue_failures.load(std::memory_order_relaxed) == 0);
  CHECK(queue.Size() == 0);
  verifier.check_complete();
}

void test_per_producer_fifo() {
  constexpr std::size_t producer_count = 4;
  constexpr std::size_t items_per_producer = 4000;
  constexpr std::size_t total = producer_count * items_per_producer;

  hakle::ConcurrentQueue<int> queue;
  std::vector<hakle::ConcurrentQueue<int>::ProducerToken> tokens;
  tokens.reserve(producer_count);
  for (std::size_t index = 0; index < producer_count; ++index) {
    tokens.emplace_back(queue.GetProducerToken());
  }

  std::atomic<bool> producers_done{false};
  std::atomic<bool> timed_out{false};
  std::atomic<std::size_t> enqueue_failures{0};
  std::size_t ordering_errors = 0;
  std::array<std::size_t, producer_count> next_sequence{};
  std::latch start_gate(producer_count + 2);
  const auto deadline = std::chrono::steady_clock::now() + 15s;

  std::thread consumer([&] {
    start_gate.arrive_and_wait();
    std::size_t consumed = 0;
    while (consumed != total) {
      int value = 0;
      if (queue.TryDequeue(value)) {
        if (value < 0 || static_cast<std::size_t>(value) >= total) {
          ++ordering_errors;
        } else {
          const auto producer = static_cast<std::size_t>(value) / items_per_producer;
          const auto sequence = static_cast<std::size_t>(value) % items_per_producer;
          if (sequence != next_sequence[producer]) {
            ++ordering_errors;
          }
          ++next_sequence[producer];
        }
        ++consumed;
        continue;
      }
      if (producers_done.load(std::memory_order_acquire) &&
          std::chrono::steady_clock::now() >= deadline) {
        timed_out.store(true, std::memory_order_relaxed);
        return;
      }
      std::this_thread::yield();
    }
  });

  std::vector<std::thread> producers;
  for (std::size_t producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([&, producer] {
      start_gate.arrive_and_wait();
      for (std::size_t sequence = 0; sequence < items_per_producer; ++sequence) {
        const auto value = static_cast<int>(producer * items_per_producer + sequence);
        if (!queue.EnqueueWithToken(tokens[producer], value)) {
          enqueue_failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  start_gate.arrive_and_wait();
  for (auto &thread : producers) {
    thread.join();
  }
  producers_done.store(true, std::memory_order_release);
  consumer.join();

  CHECK(!timed_out.load(std::memory_order_relaxed));
  CHECK(enqueue_failures.load(std::memory_order_relaxed) == 0);
  CHECK(ordering_errors == 0);
  CHECK(std::all_of(next_sequence.begin(), next_sequence.end(), [](std::size_t count) {
    return count == items_per_producer;
  }));
  CHECK(queue.Size() == 0);
}

} // namespace

int main() {
  lockfree_test::test_runner runner;
  runner.run("implicit MPMC delivers each value once", test_implicit_mpmc);
  runner.run("explicit token MPMC delivers each value once", test_explicit_token_mpmc);
  runner.run("bulk MPMC delivers each value once", test_bulk_mpmc);
  runner.run("per-producer FIFO order", test_per_producer_fifo);
  return runner.finish("concurrent queue");
}