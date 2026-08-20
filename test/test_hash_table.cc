#include "ConcurrentQueue/HashTable.h"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

namespace {

using table_type = hakle::HashTable<std::uint64_t, std::uint64_t, 2>;

void test_repeated_growth_preserves_entries() {
  constexpr std::uint64_t entry_count = 4096;
  table_type table;

  for (std::uint64_t key = 1; key <= entry_count; ++key) {
    CHECK(table.Set(key, key * 3));
  }

  CHECK(table.GetSize() == entry_count);
  for (std::uint64_t key = 1; key <= entry_count; ++key) {
    std::uint64_t value = 0;
    CHECK(table.Get(key, value));
    CHECK(value == key * 3);
  }
}

void test_concurrent_growth_preserves_entries() {
  constexpr std::size_t thread_count = 16;
  constexpr std::size_t entries_per_thread = 256;
  constexpr std::size_t entry_count = thread_count * entries_per_thread;

  table_type table;
  std::latch start_gate(thread_count + 1);
  std::vector<std::thread> threads;
  threads.reserve(thread_count);

  for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
    threads.emplace_back([&, thread_index] {
      start_gate.arrive_and_wait();
      const auto first_key = thread_index * entries_per_thread + 1;
      const auto last_key = first_key + entries_per_thread;
      for (std::size_t key = first_key; key < last_key; ++key) {
        CHECK(table.Set(static_cast<std::uint64_t>(key),
                        static_cast<std::uint64_t>(key * 7)));
      }
    });
  }

  start_gate.arrive_and_wait();
  for (auto &thread : threads) {
    thread.join();
  }

  CHECK(table.GetSize() == entry_count);
  for (std::uint64_t key = 1; key <= entry_count; ++key) {
    std::uint64_t value = 0;
    CHECK(table.Get(key, value));
    CHECK(value == key * 7);
  }
}

} // namespace

int main() {
  lockfree_test::test_runner runner;
  runner.run("repeated growth preserves entries", test_repeated_growth_preserves_entries);
  runner.run("concurrent growth preserves entries", test_concurrent_growth_preserves_entries);
  return runner.finish("hash table");
}
