#include <bit>

#include "ConcurrentQueue/ConcurrentQueue.h"
#include "test_support.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace {

struct allocation_counters {
  std::atomic<std::size_t> allocation_calls{0};
  std::atomic<std::size_t> deallocation_calls{0};
  std::atomic<std::int64_t> live_slots{0};
  std::atomic<std::size_t> constructions{0};
  std::atomic<std::size_t> destructions{0};

  void reset() noexcept {
    allocation_calls.store(0, std::memory_order_relaxed);
    deallocation_calls.store(0, std::memory_order_relaxed);
    live_slots.store(0, std::memory_order_relaxed);
    constructions.store(0, std::memory_order_relaxed);
    destructions.store(0, std::memory_order_relaxed);
  }
};

std::shared_ptr<allocation_counters> shared_allocation_counters() {
  static auto counters = std::make_shared<allocation_counters>();
  return counters;
}

template <typename T> class counting_allocator {
public:
  using ValueType = T;
  using Pointer = T *;
  using ConstPointer = const T *;
  using Reference = T &;
  using ConstReference = const T &;
  using SizeType = std::size_t;
  using DifferenceType = std::ptrdiff_t;

  template <typename U> struct rebind {
    using other = counting_allocator<U>;
  };

  counting_allocator() noexcept : counters_(shared_allocation_counters()) {}

  explicit counting_allocator(std::shared_ptr<allocation_counters> counters) noexcept
      : counters_(std::move(counters)) {}

  template <typename U>
  explicit counting_allocator(const counting_allocator<U> &other) noexcept
      : counters_(other.counters_) {}

  template <typename U>
  explicit counting_allocator(counting_allocator<U> &&other) noexcept
      : counters_(std::move(other.counters_)) {}

  template <typename U>
  counting_allocator &operator=(const counting_allocator<U> &other) noexcept {
    counters_ = other.counters_;
    return *this;
  }

  template <typename U>
  counting_allocator &operator=(counting_allocator<U> &&other) noexcept {
    counters_ = std::move(other.counters_);
    return *this;
  }

  [[nodiscard]] Pointer Allocate() { return Allocate(1); }

  [[nodiscard]] Pointer Allocate(SizeType count) {
    auto *memory = static_cast<Pointer>(::operator new(sizeof(T) * count));
    counters_->allocation_calls.fetch_add(1, std::memory_order_relaxed);
    counters_->live_slots.fetch_add(static_cast<std::int64_t>(count),
                                    std::memory_order_relaxed);
    return memory;
  }

  void Deallocate(Pointer pointer) noexcept { Deallocate(pointer, 1); }

  void Deallocate(Pointer pointer, SizeType count) noexcept {
    if (pointer == nullptr) {
      return;
    }
    ::operator delete(pointer);
    counters_->deallocation_calls.fetch_add(1, std::memory_order_relaxed);
    counters_->live_slots.fetch_sub(static_cast<std::int64_t>(count),
                                    std::memory_order_relaxed);
  }

  template <typename... Arguments> void Construct(Pointer pointer, Arguments &&...arguments) {
    std::construct_at(pointer, std::forward<Arguments>(arguments)...);
    counters_->constructions.fetch_add(1, std::memory_order_relaxed);
  }

  void Destroy(Pointer pointer) noexcept {
    std::destroy_at(pointer);
    counters_->destructions.fetch_add(1, std::memory_order_relaxed);
  }

  void Destroy(Pointer pointer, SizeType count) noexcept {
    for (SizeType index = 0; index < count; ++index) {
      Destroy(pointer + index);
    }
  }

  void Destroy(Pointer first, Pointer last) noexcept {
    Destroy(first, static_cast<SizeType>(last - first));
  }

  void swap(counting_allocator &other) noexcept { counters_.swap(other.counters_); }

  template <typename U>
  [[nodiscard]] bool operator==(const counting_allocator<U> &other) const noexcept {
    return counters_ == other.counters_;
  }

  template <typename U>
  [[nodiscard]] bool operator!=(const counting_allocator<U> &other) const noexcept {
    return !(*this == other);
  }

private:
  template <typename> friend class counting_allocator;
  std::shared_ptr<allocation_counters> counters_;
};

template <typename T> void swap(counting_allocator<T> &left, counting_allocator<T> &right) noexcept {
  left.swap(right);
}

template <typename T, typename Allocator> struct counting_queue_traits {
  static constexpr std::size_t BlockSize = 32;
  static constexpr std::size_t InitialBlockPoolSize = 32 * BlockSize;
  static constexpr std::size_t InitialHashSize = 32;
  static constexpr std::size_t InitialExplicitQueueSize = 32;
  static constexpr std::size_t InitialImplicitQueueSize = 32;

  using AllocatorType = Allocator;
  using ExplicitBlockType = hakle::HakleFlagsBlock<T, BlockSize>;
  using ImplicitBlockType = hakle::HakleCounterBlock<T, BlockSize>;
  using ExplicitAllocatorType =
      typename hakle::HakeAllocatorTraits<AllocatorType>::template RebindAlloc<ExplicitBlockType>;
  using ImplicitAllocatorType =
      typename hakle::HakeAllocatorTraits<AllocatorType>::template RebindAlloc<ImplicitBlockType>;
  using ExplicitBlockManagerType =
      hakle::HakleBlockManager<ExplicitBlockType, ExplicitAllocatorType>;
  using ImplicitBlockManagerType =
      hakle::HakleBlockManager<ImplicitBlockType, ImplicitAllocatorType>;

  static ExplicitBlockManagerType
  MakeDefaultExplicitBlockManager(const ExplicitAllocatorType &allocator) {
    return ExplicitBlockManagerType(InitialBlockPoolSize, allocator);
  }

  static ImplicitBlockManagerType
  MakeDefaultImplicitBlockManager(const ImplicitAllocatorType &allocator) {
    return ImplicitBlockManagerType(InitialBlockPoolSize, allocator);
  }
};
struct lifetime_probe {
  static inline std::atomic<int> alive{0};
  static inline std::atomic<int> constructed{0};
  static inline std::atomic<int> destroyed{0};

  int value = 0;

  lifetime_probe() noexcept { on_construct(); }
  explicit lifetime_probe(int input) noexcept : value(input) { on_construct(); }
  lifetime_probe(const lifetime_probe &other) noexcept : value(other.value) { on_construct(); }
  lifetime_probe(lifetime_probe &&other) noexcept : value(other.value) {
    other.value = -1;
    on_construct();
  }

  lifetime_probe &operator=(const lifetime_probe &other) noexcept {
    value = other.value;
    return *this;
  }

  lifetime_probe &operator=(lifetime_probe &&other) noexcept {
    value = other.value;
    other.value = -1;
    return *this;
  }

  ~lifetime_probe() {
    alive.fetch_sub(1, std::memory_order_relaxed);
    destroyed.fetch_add(1, std::memory_order_relaxed);
  }

  static void reset() noexcept {
    alive.store(0, std::memory_order_relaxed);
    constructed.store(0, std::memory_order_relaxed);
    destroyed.store(0, std::memory_order_relaxed);
  }

private:
  static void on_construct() noexcept {
    alive.fetch_add(1, std::memory_order_relaxed);
    constructed.fetch_add(1, std::memory_order_relaxed);
  }
};

void test_non_trivial_value_lifetime() {
  lifetime_probe::reset();

  {
    hakle::ConcurrentQueue<lifetime_probe> queue;
    CHECK(queue.Enqueue(10));
    CHECK(queue.Enqueue(20));

    lifetime_probe output;
    CHECK(queue.TryDequeue(output));
    CHECK(output.value == 10);
    CHECK(queue.Size() == 1);
  }

  CHECK(lifetime_probe::alive.load(std::memory_order_relaxed) == 0);
  CHECK(lifetime_probe::constructed.load(std::memory_order_relaxed) ==
        lifetime_probe::destroyed.load(std::memory_order_relaxed));
}


struct throwing_probe {
  static inline std::atomic<int> alive{0};
  int value = 0;

  explicit throwing_probe(int input = 0) : value(input) {
    if (input == 42) {
      throw std::runtime_error("requested construction failure");
    }
    alive.fetch_add(1, std::memory_order_relaxed);
  }

  throwing_probe(const throwing_probe &other) : throwing_probe(other.value) {}

  throwing_probe(throwing_probe &&other) noexcept : value(other.value) {
    other.value = -1;
    alive.fetch_add(1, std::memory_order_relaxed);
  }

  throwing_probe &operator=(throwing_probe &&other) noexcept {
    value = other.value;
    other.value = -1;
    return *this;
  }

  throwing_probe &operator=(const throwing_probe &other) noexcept {
    value = other.value;
    return *this;
  }

  ~throwing_probe() { alive.fetch_sub(1, std::memory_order_relaxed); }
};

void test_enqueue_constructor_exception_keeps_queue_usable() {
  throwing_probe::alive.store(0, std::memory_order_relaxed);

  {
    hakle::ConcurrentQueue<throwing_probe> queue;
    CHECK(queue.Enqueue(1));
    lockfree_test::check_throws<std::runtime_error>([&] { static_cast<void>(queue.Enqueue(42)); });
    CHECK(queue.Enqueue(3));

    throwing_probe output;
    CHECK(queue.TryDequeue(output));
    CHECK(output.value == 1);
    CHECK(queue.TryDequeue(output));
    CHECK(output.value == 3);
    CHECK(!queue.TryDequeue(output));
    CHECK(queue.Size() == 0);
  }

  CHECK(throwing_probe::alive.load(std::memory_order_relaxed) == 0);
}
void test_custom_allocator_is_used_and_balanced() {
  const auto counters = shared_allocation_counters();
  counters->reset();
  lifetime_probe::reset();

  {
    counting_allocator<lifetime_probe> allocator(counters);
    using allocator_type = counting_allocator<lifetime_probe>;
    using traits_type = counting_queue_traits<lifetime_probe, allocator_type>;
    hakle::ConcurrentQueue<lifetime_probe, allocator_type, traits_type> queue(allocator);

    CHECK(queue.Enqueue(31));
    CHECK(queue.Enqueue(32));
    CHECK(queue.Enqueue(33));

    lifetime_probe output;
    CHECK(queue.TryDequeue(output));
    CHECK(output.value == 31);
    CHECK(counters->allocation_calls.load(std::memory_order_relaxed) != 0);
    CHECK(counters->live_slots.load(std::memory_order_relaxed) > 0);
  }

  CHECK(lifetime_probe::alive.load(std::memory_order_relaxed) == 0);
  CHECK(counters->live_slots.load(std::memory_order_relaxed) == 0);
  CHECK(counters->allocation_calls.load(std::memory_order_relaxed) ==
        counters->deallocation_calls.load(std::memory_order_relaxed));
  CHECK(counters->constructions.load(std::memory_order_relaxed) ==
        counters->destructions.load(std::memory_order_relaxed));
}

} // namespace

int main() {
  lockfree_test::test_runner runner;
  runner.run("non-trivial value lifetime", test_non_trivial_value_lifetime);
  runner.run("enqueue constructor exception", test_enqueue_constructor_exception_keeps_queue_usable);
  runner.run("custom allocator usage and balance", test_custom_allocator_is_used_and_balanced);
  return runner.finish("allocator and lifetime");
}