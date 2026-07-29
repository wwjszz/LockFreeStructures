#include "ConcurrentQueue/ConcurrentQueue.h"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <memory>
#include <numeric>
#include <type_traits>
#include <utility>

namespace {

using queue_type = hakle::ConcurrentQueue<int>;

static_assert(std::movable<queue_type>);
static_assert(!std::copy_constructible<queue_type>);
static_assert(!std::is_copy_assignable_v<queue_type>);

void test_empty_queue() {
  queue_type queue;
  int value = -1;

  CHECK(queue.Size() == 0);
  CHECK(!queue.TryDequeue(value));
  CHECK(!queue.TryDequeueNonInterleaved(value));
}

void test_implicit_producer_fifo() {
  queue_type queue;

  CHECK(queue.Enqueue(11));
  CHECK(queue.Enqueue(22));
  CHECK(queue.Enqueue(33));
  CHECK(queue.Size() == 3);

  int value = 0;
  CHECK(queue.TryDequeue(value));
  CHECK(value == 11);
  CHECK(queue.TryDequeue(value));
  CHECK(value == 22);
  CHECK(queue.TryDequeue(value));
  CHECK(value == 33);
  CHECK(!queue.TryDequeue(value));
  CHECK(queue.Size() == 0);
}

void test_explicit_tokens() {
  queue_type queue;
  auto producer = queue.GetProducerToken();
  auto consumer = queue.GetConsumerToken();

  CHECK(producer.Valid());
  CHECK(queue.EnqueueWithToken(producer, 7));
  CHECK(queue.EnqueueWithToken(producer, 9));

  int value = 0;
  CHECK(queue.TryDequeueFromProducer(producer, value));
  CHECK(value == 7);
  CHECK(queue.TryDequeue(consumer, value));
  CHECK(value == 9);
  CHECK(!queue.TryDequeue(consumer, value));
}

void test_bulk_operations() {
  queue_type queue;
  std::array<int, 257> input{};
  std::array<int, 257> output{};
  std::iota(input.begin(), input.end(), 1000);

  CHECK(queue.EnqueueBulk(input.begin(), input.size()));
  CHECK(queue.Size() == input.size());

  std::size_t drained = 0;
  while (drained != output.size()) {
    drained += queue.TryDequeueBulk(output.begin() + static_cast<std::ptrdiff_t>(drained),
                                     std::min<std::size_t>(41, output.size() - drained));
  }

  CHECK(output == input);
  CHECK(queue.Size() == 0);
}

void test_bulk_operations_with_producer_token() {
  queue_type queue;
  auto producer = queue.GetProducerToken();
  std::array<int, 73> input{};
  std::array<int, 73> output{};
  std::iota(input.begin(), input.end(), 2000);

  CHECK(queue.EnqueueBulk(producer, input.begin(), input.size()));
  const auto count = queue.TryDequeueBulkFromProducer(producer, output.begin(), output.size());

  CHECK(count == input.size());
  CHECK(output == input);
  CHECK(queue.Size() == 0);
}

void test_move_only_values() {
  hakle::ConcurrentQueue<std::unique_ptr<int>> queue;
  CHECK(queue.Enqueue(std::make_unique<int>(42)));

  std::unique_ptr<int> value;
  CHECK(queue.TryDequeue(value));
  CHECK(value != nullptr);
  CHECK(*value == 42);
  CHECK(queue.Size() == 0);
}

void test_move_construction_and_swap_preserve_contents() {
  queue_type source;
  CHECK(source.Enqueue(1));
  CHECK(source.Enqueue(2));

  queue_type moved(std::move(source));
  CHECK(source.Size() == 0);
  CHECK(moved.Size() == 2);

  queue_type other;
  CHECK(other.Enqueue(99));
  moved.swap(other);

  int value = 0;
  CHECK(moved.TryDequeue(value));
  CHECK(value == 99);
  CHECK(other.TryDequeue(value));
  CHECK(value == 1);
  CHECK(other.TryDequeue(value));
  CHECK(value == 2);

}

void test_move_assignment_releases_existing_contents() {
  queue_type target;
  for (int value = 0; value != 96; ++value) {
    CHECK(target.Enqueue(value));
  }
  {
    auto producer = target.GetProducerToken();
    for (int value = 0; value != 96; ++value) {
      CHECK(target.EnqueueWithToken(producer, 1000 + value));
    }
  }

  queue_type source;
  CHECK(source.Enqueue(7001));
  CHECK(source.Enqueue(7002));
  CHECK(source.Enqueue(7003));

  target = std::move(source);

  CHECK(source.Size() == 0);
  CHECK(target.Size() == 3);

  target = std::move(target);
  CHECK(target.Size() == 3);

  int value = 0;
  CHECK(target.TryDequeue(value));
  CHECK(value == 7001);
  CHECK(target.TryDequeue(value));
  CHECK(value == 7002);
  CHECK(target.TryDequeue(value));
  CHECK(value == 7003);
  CHECK(!target.TryDequeue(value));
}
} // namespace

int main() {
  lockfree_test::test_runner runner;
  runner.run("empty queue", test_empty_queue);
  runner.run("implicit producer FIFO", test_implicit_producer_fifo);
  runner.run("explicit producer and consumer tokens", test_explicit_tokens);
  runner.run("bulk enqueue and dequeue", test_bulk_operations);
  runner.run("token bulk enqueue and dequeue", test_bulk_operations_with_producer_token);
  runner.run("move-only values", test_move_only_values);
  runner.run("move construction and swap", test_move_construction_and_swap_preserve_contents);
  runner.run("move assignment releases existing contents",
             test_move_assignment_releases_existing_contents);
  return runner.finish("queue API");
}
