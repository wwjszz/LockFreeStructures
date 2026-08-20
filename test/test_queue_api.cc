#include "ConcurrentQueue/ConcurrentQueue.h"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <numeric>
#include <type_traits>
#include <utility>

namespace {

using queue_type = hakle::ConcurrentQueue<int>;

class single_block_manager {
public:
  static constexpr std::size_t BlockSize = 32;
  using BlockType = hakle::HakleFlagsBlock<int, 32>;
  using ValueType = int;
  using AllocMode = hakle::AllocMode;

  BlockType *RequisitionBlock(AllocMode) noexcept {
    if (!available_) {
      return nullptr;
    }
    available_ = false;
    return &block_;
  }

  void ReturnBlock(BlockType *block) noexcept {
    (void)block;
    available_ = true;
  }

  void ReturnBlocks(BlockType *block) noexcept {
    while (block != nullptr) {
      BlockType *next = block->Next;
      ReturnBlock(block);
      block = next == block ? nullptr : next;
    }
  }

private:
  BlockType block_{};
  bool available_{true};
};

class batch_counter_block_manager {
public:
  static constexpr std::size_t BlockSize = 32;
  using BlockType = hakle::HakleFlagsBlock<int, BlockSize>;
  using ValueType = int;
  using AllocMode = hakle::AllocMode;

  batch_counter_block_manager() { available_.fill(true); }

  BlockType *RequisitionBlock(AllocMode) noexcept {
    ++scalar_requisition_calls;
    return take_one();
  }

  hakle::BlockBatch<BlockType> RequisitionBlocks(std::size_t max_count,
                                                  AllocMode) noexcept {
    ++batch_requisition_calls;
    hakle::BlockBatch<BlockType> batch{};
    BlockType *last = nullptr;
    for (std::size_t index = 0;
         index != blocks_.size() && batch.Count != max_count; ++index) {
      if (!available_[index]) {
        continue;
      }
      available_[index] = false;
      BlockType *block = &blocks_[index];
      block->Next = nullptr;
      if (last == nullptr) {
        batch.First = block;
      } else {
        last->Next = block;
      }
      last = block;
      ++batch.Count;
    }
    return batch;
  }

  void ReturnBlock(BlockType *block) noexcept {
    const auto index = static_cast<std::size_t>(block - blocks_.data());
    available_[index] = true;
    ++returned_blocks;
  }

  void ReturnBlocks(BlockType *block) noexcept {
    while (block != nullptr) {
      BlockType *next = block->Next;
      block->Next = nullptr;
      ReturnBlock(block);
      block = next;
    }
  }

  std::size_t available_blocks() const noexcept {
    return static_cast<std::size_t>(
        std::count(available_.begin(), available_.end(), true));
  }

  std::size_t scalar_requisition_calls{0};
  std::size_t batch_requisition_calls{0};
  std::size_t returned_blocks{0};

private:
  BlockType *take_one() noexcept {
    for (std::size_t index = 0; index != blocks_.size(); ++index) {
      if (available_[index]) {
        available_[index] = false;
        return &blocks_[index];
      }
    }
    return nullptr;
  }

  std::array<BlockType, 8> blocks_{};
  std::array<bool, 8> available_{};
};

class scalar_counter_block_manager {
public:
  static constexpr std::size_t BlockSize = 32;
  using BlockType = hakle::HakleCounterBlock<int, BlockSize>;
  using ValueType = int;
  using AllocMode = hakle::AllocMode;

  scalar_counter_block_manager() { available_.fill(true); }

  BlockType *RequisitionBlock(AllocMode) noexcept {
    ++requisition_calls;
    for (std::size_t index = 0; index != blocks_.size(); ++index) {
      if (available_[index]) {
        available_[index] = false;
        return &blocks_[index];
      }
    }
    return nullptr;
  }

  void ReturnBlock(BlockType *block) noexcept {
    const auto index = static_cast<std::size_t>(block - blocks_.data());
    available_[index] = true;
  }

  void ReturnBlocks(BlockType *block) noexcept {
    while (block != nullptr) {
      BlockType *next = block->Next;
      ReturnBlock(block);
      block = next;
    }
  }

  std::size_t requisition_calls{0};

private:
  std::array<BlockType, 4> blocks_{};
  std::array<bool, 4> available_{};
};

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

void test_implicit_caches_handle_queue_address_reuse() {
  alignas(queue_type) std::byte queue_storage[sizeof(queue_type)];

  for (int iteration = 0; iteration != 8; ++iteration) {
    auto *queue = std::construct_at(reinterpret_cast<queue_type *>(queue_storage));
    CHECK(queue->Enqueue(iteration));

    int value = -1;
    CHECK(queue->TryDequeue(value));
    CHECK(value == iteration);
    std::destroy_at(queue);
  }
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

void test_zero_length_bulk_enqueue_is_a_no_op() {
  queue_type queue;
  auto producer = queue.GetProducerToken();
  int value = 7;

  CHECK(queue.EnqueueBulk(&value, 0));
  CHECK(queue.EnqueueBulk(producer, &value, 0));
  CHECK(queue.TryEnqueueBulk(&value, 0));
  CHECK(queue.TryEnqueueBulk(producer, &value, 0));
  CHECK(queue.Size() == 0);
}

void test_fast_queue_bulk_failure_keeps_preallocated_block_reusable() {
  single_block_manager manager;
  using fast_queue_type =
      hakle::FastQueue<int, 32, hakle::HakleAllocator<int>,
                       single_block_manager::BlockType, single_block_manager>;
  fast_queue_type queue(2, &manager);
  std::array<int, 33> values{};

  CHECK(!queue.EnqueueBulk<hakle::AllocMode::CannotAlloc>(values.begin(), values.size()));
  CHECK(queue.Enqueue<hakle::AllocMode::CannotAlloc>(42));

  int output = 0;
  CHECK(queue.Dequeue(output));
  CHECK(output == 42);
}

void test_fast_queue_bulk_requisition_uses_batch_extension() {
  using manager_type = batch_counter_block_manager;
  using fast_queue_type =
      hakle::FastQueue<int, manager_type::BlockSize,
                       hakle::HakleAllocator<int>, manager_type::BlockType,
                       manager_type, true>;

  manager_type manager;
  fast_queue_type queue(4, &manager);
  std::array<int, 64> input{};
  std::array<int, 64> output{};
  std::iota(input.begin(), input.end(), 3000);

  CHECK(queue.EnqueueBulk<hakle::AllocMode::CannotAlloc>(input.begin(),
                                                          input.size()));
  CHECK(manager.batch_requisition_calls == 1);
  CHECK(manager.scalar_requisition_calls == 0);
  CHECK(queue.DequeueBulk(output.begin(), output.size()) == output.size());
  CHECK(output == input);
}

void test_fast_queue_batch_mode_falls_back_to_scalar_manager() {
  using manager_type = scalar_counter_block_manager;
  using fast_queue_type =
      hakle::FastQueue<int, manager_type::BlockSize,
                       hakle::HakleAllocator<int>, manager_type::BlockType,
                       manager_type, true>;

  manager_type manager;
  fast_queue_type queue(4, &manager);
  std::array<int, 64> input{};
  std::array<int, 64> output{};
  std::iota(input.begin(), input.end(), 4000);

  CHECK(queue.EnqueueBulk<hakle::AllocMode::CannotAlloc>(input.begin(),
                                                          input.size()));
  CHECK(manager.requisition_calls == 2);
  CHECK(queue.DequeueBulk(output.begin(), output.size()) == output.size());
  CHECK(output == input);
}

void test_fast_queue_batch_failure_returns_unused_reservation() {
  using manager_type = batch_counter_block_manager;
  using fast_queue_type =
      hakle::FastQueue<int, manager_type::BlockSize,
                       hakle::HakleAllocator<int>, manager_type::BlockType,
                       manager_type, true>;

  manager_type manager;
  fast_queue_type queue(2, &manager);
  std::array<int, 160> too_many{};
  std::iota(too_many.begin(), too_many.end(), 5000);

  CHECK(!queue.EnqueueBulk<hakle::AllocMode::CannotAlloc>(
      too_many.begin(), too_many.size()));
  CHECK(queue.Size() == 0);
  CHECK(manager.batch_requisition_calls == 1);
  CHECK(manager.returned_blocks == 1);
  CHECK(manager.available_blocks() == 4);

  std::array<int, 128> input{};
  std::array<int, 128> output{};
  std::iota(input.begin(), input.end(), 6000);
  CHECK(queue.EnqueueBulk<hakle::AllocMode::CannotAlloc>(input.begin(),
                                                          input.size()));
  CHECK(manager.batch_requisition_calls == 1);
  CHECK(queue.DequeueBulk(output.begin(), output.size()) == output.size());
  CHECK(output == input);
}

void test_slow_queue_bulk_failure_returns_entire_batch() {
  using block_type = hakle::HakleCounterBlock<int, 32>;
  using manager_type = hakle::HakleBlockManager<block_type>;
  using slow_queue_type =
      hakle::SlowQueue<int, 32, hakle::HakleAllocator<int>, block_type,
                       manager_type>;

  manager_type manager(2);
  slow_queue_type queue(2, &manager);
  std::array<int, 65> too_many{};
  std::iota(too_many.begin(), too_many.end(), 100);

  CHECK(!queue.EnqueueBulk<hakle::AllocMode::CannotAlloc>(
      too_many.begin(), too_many.size()));
  CHECK(queue.Size() == 0);

  std::array<int, 64> input{};
  std::array<int, 64> output{};
  std::iota(input.begin(), input.end(), 1000);
  CHECK(queue.EnqueueBulk<hakle::AllocMode::CannotAlloc>(input.begin(),
                                                         input.size()));
  CHECK(queue.DequeueBulk(output.begin(), output.size()) == output.size());
  CHECK(output == input);
}

void test_slow_queue_falls_back_for_scalar_custom_manager() {
  using manager_type = scalar_counter_block_manager;
  using slow_queue_type =
      hakle::SlowQueue<int, manager_type::BlockSize,
                       hakle::HakleAllocator<int>, manager_type::BlockType,
                       manager_type>;

  static_assert(!hakle::HasRequisitionBlocks<manager_type>::value);

  manager_type manager;
  slow_queue_type queue(2, &manager);
  std::array<int, 64> input{};
  std::array<int, 64> output{};
  std::iota(input.begin(), input.end(), 2000);

  CHECK(queue.EnqueueBulk<hakle::AllocMode::CannotAlloc>(input.begin(),
                                                         input.size()));
  CHECK(manager.requisition_calls == 2);
  CHECK(queue.DequeueBulk(output.begin(), output.size()) == output.size());
  CHECK(output == input);
}

void test_word_flags_policy_fast_queue() {
    using block_type   = hakle::HakleWordFlagsBlock<int, 32>;
    using manager_type = hakle::HakleBlockManager<block_type>;
    manager_type                                                                    manager( 4 );
    hakle::FastQueue<int, 32, hakle::HakleAllocator<int>, block_type, manager_type> queue( 2, &manager );

    for ( int value = 1; value <= 64; ++value ) {
        CHECK( queue.template Enqueue<hakle::AllocMode::CanAlloc>( value ) );
    }

    int output = 0;
    for ( int expected = 1; expected <= 64; ++expected ) {
        CHECK( queue.Dequeue( output ) );
        CHECK( output == expected );
    }
    CHECK( !queue.Dequeue( output ) );

    std::array<int, 73> bulk_input{};
    std::array<int, 73> bulk_output{};
    std::iota( bulk_input.begin(), bulk_input.end(), 100 );
    CHECK( queue.template EnqueueBulk<hakle::AllocMode::CanAlloc>( bulk_input.begin(), bulk_input.size() ) );
    CHECK( queue.DequeueBulk( bulk_output.begin(), bulk_output.size() ) == bulk_output.size() );
    CHECK( bulk_output == bulk_input );
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
  CHECK(source.Enqueue(0));
  int value = -1;
  CHECK(source.TryDequeue(value));
  CHECK(value == 0);
  CHECK(source.Enqueue(1));
  CHECK(source.Enqueue(2));

  queue_type moved(std::move(source));
  CHECK(source.Size() == 0);
  CHECK(moved.Size() == 2);

  queue_type other;
  CHECK(other.Enqueue(98));
  CHECK(other.TryDequeue(value));
  CHECK(value == 98);
  CHECK(other.Enqueue(99));
  moved.swap(other);

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
  runner.run("implicit caches handle queue address reuse",
             test_implicit_caches_handle_queue_address_reuse);
  runner.run("explicit producer and consumer tokens", test_explicit_tokens);
  runner.run("bulk enqueue and dequeue", test_bulk_operations);
  runner.run("zero-length bulk enqueue", test_zero_length_bulk_enqueue_is_a_no_op);
  runner.run("FastQueue bulk failure rollback",
             test_fast_queue_bulk_failure_keeps_preallocated_block_reusable);
  runner.run("FastQueue batch requisition",
             test_fast_queue_bulk_requisition_uses_batch_extension);
  runner.run("FastQueue batch scalar fallback",
             test_fast_queue_batch_mode_falls_back_to_scalar_manager);
  runner.run("FastQueue batch failure returns unused reservation",
             test_fast_queue_batch_failure_returns_unused_reservation);
  runner.run("SlowQueue batch failure rollback",
             test_slow_queue_bulk_failure_returns_entire_batch);
  runner.run("SlowQueue scalar custom manager fallback",
             test_slow_queue_falls_back_for_scalar_custom_manager);
  runner.run( "WordFlags policy FastQueue", test_word_flags_policy_fast_queue );
  runner.run("token bulk enqueue and dequeue", test_bulk_operations_with_producer_token);
  runner.run("move-only values", test_move_only_values);
  runner.run("move construction and swap", test_move_construction_and_swap_preserve_contents);
  runner.run("move assignment releases existing contents",
             test_move_assignment_releases_existing_contents);
  return runner.finish("queue API");
}
