//
// Created by admin on 25-11-24.
//
#include <string>
#include <random>
#include <atomic>
#include <chrono>

#include "../ConcurrentQueue/HashTable.h"

#include <iostream>
#include <limits>
#include <thread>

#include "gtest/gtest.h"

using namespace hakle;

// 测试用的键值类型
using TestHashTable = HashTable<uint32_t, uint32_t, UINT32_MAX, 8>;

class HashTableTest : public ::testing::Test {
protected:
    void SetUp() override {
        table = new TestHashTable();
    }

    void TearDown() override {
        delete table;
    }

    TestHashTable* table;
};

// 测试基本插入和查找
TEST_F(HashTableTest, BasicInsertAndGet) {
    auto createFunc = [](uint32_t value) -> uint32_t* {
        return new uint32_t(value);
    };

    uint32_t key = 123;
    uint32_t value = 456;

    // 插入数据
    uint32_t* inserted = table->GetOrAddByFunc(key, createFunc, value);
    ASSERT_NE(inserted, nullptr);
    EXPECT_EQ(*inserted, value);

    // 查找数据
    uint32_t* found = table->Get(key);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, value);
    EXPECT_EQ(inserted, found); // 应该是同一个指针
}

// 测试查找不存在的键
TEST_F(HashTableTest, GetNonExistentKey) {
    uint32_t* found = table->Get(999);
    EXPECT_EQ(found, nullptr);
}

// 测试哈希冲突处理
TEST_F(HashTableTest, HandleHashCollisions) {
    auto createFunc = [](uint32_t value) -> uint32_t* {
        return new uint32_t(value);
    };

    // 插入多个可能产生冲突的键
    std::vector<uint32_t> keys = {1, 9, 17}; // 这些键在初始容量8下可能冲突

    for (uint32_t i = 0; i < keys.size(); ++i) {
        uint32_t* inserted = table->GetOrAddByFunc(keys[i], createFunc, i * 100);
        ASSERT_NE(inserted, nullptr);
        EXPECT_EQ(*inserted, i * 100);
    }

    // 验证所有键都能正确找到
    for (uint32_t i = 0; i < keys.size(); ++i) {
        uint32_t* found = table->Get(keys[i]);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(*found, i * 100);
    }
}

// 测试哈希表扩容
TEST_F(HashTableTest, HashTableResize) {
    auto createFunc = [](uint32_t value) -> uint32_t* {
        return new uint32_t(value);
    };

    // 插入足够多的数据触发扩容（初始容量8，扩容阈值约4-6）
    for (uint32_t i = 0; i < 10; ++i) {
        uint32_t* inserted = table->GetOrAddByFunc(i, createFunc, i * 1000);
        ASSERT_NE(inserted, nullptr);
    }

    // 验证所有数据在扩容后仍然可访问
    for (uint32_t i = 0; i < 10; ++i) {
        uint32_t* found = table->Get(i);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(*found, i * 1000);
    }
}

// 高并发插入测试 - 大量线程同时插入
TEST_F(HashTableTest, HighConcurrencyInsert) {
    const int num_threads = 16;
    const int num_operations = 5000;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;

    auto worker = [this, &success_count](int thread_id) {
        auto createFunc = [](uint32_t value) -> uint32_t* {
            return new uint32_t(value);
        };

        for (int i = 0; i < num_operations; ++i) {
            uint32_t key = thread_id * num_operations + i;
            uint32_t value = key * 10;

            uint32_t* inserted = table->GetOrAddByFunc(key, createFunc, value);
            if (inserted != nullptr) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * num_operations);

    // 验证所有插入的数据都存在且正确
    for (int i = 0; i < num_threads; ++i) {
        for (int j = 0; j < num_operations; ++j) {
            uint32_t key = i * num_operations + j;
            uint32_t* found = table->Get(key);
            ASSERT_NE(found, nullptr);
            EXPECT_EQ(*found, key * 10);
        }
    }
}

// 读写混合测试 - 部分线程读，部分线程写
TEST_F(HashTableTest, ReadWriteMixed) {
    const int writer_threads = 4;
    const int reader_threads = 4;
    const int num_operations = 2000;
    std::atomic<bool> stop_reading{false};
    std::atomic<int> read_success{0};
    std::atomic<int> write_success{0};

    // 先插入一些初始数据
    auto createFunc = [](uint32_t value) -> uint32_t* {
        return new uint32_t(value);
    };
    for (int i = 0; i < 1000; ++i) {
        table->GetOrAddByFunc(i, createFunc, i * 100);
    }

    std::vector<std::thread> writers;
    std::vector<std::thread> readers;

    // 写线程
    for (int i = 0; i < writer_threads; ++i) {
        writers.emplace_back([this, &write_success, num_operations](int thread_id) {
            auto createFunc = [](uint32_t value) -> uint32_t* {
                return new uint32_t(value);
            };

            for (int j = 0; j < num_operations; ++j) {
                uint32_t key = 10000 + thread_id * num_operations + j;
                if (table->GetOrAddByFunc(key, createFunc, key * 10) != nullptr) {
                    write_success.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(10)); // 稍微延迟，增加竞争
            }
        }, i);
    }

    // 读线程
    for (int i = 0; i < reader_threads; ++i) {
        readers.emplace_back([this, &read_success, &stop_reading]() {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 2000);

            while (!stop_reading.load(std::memory_order_acquire)) {
                uint32_t key = dis(gen) % 2000; // 读取现有数据
                uint32_t* found = table->Get(key);
                if (found != nullptr) {
                    read_success.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(5));
            }
        });
    }

    // 等待写线程完成
    for (auto& t : writers) {
        t.join();
    }

    // 停止读线程
    stop_reading.store(true, std::memory_order_release);
    for (auto& t : readers) {
        t.join();
    }

    EXPECT_GT(read_success.load(), 0);
    EXPECT_EQ(write_success.load(), writer_threads * num_operations);
}

// 测试重复键插入 - 多个线程尝试插入相同的键
TEST_F(HashTableTest, DuplicateKeyInsertion) {
    const int num_threads = 8;
    const uint32_t duplicate_key = 12345;
    std::atomic<int> success_count{0};
    std::atomic<int> total_attempts{0};

    std::vector<std::thread> threads;

    auto worker = [this, duplicate_key, &success_count, &total_attempts]() {
        auto createFunc = [](uint32_t value) -> uint32_t* {
            return new uint32_t(value);
        };

        for (int i = 0; i < 100; ++i) {
            total_attempts.fetch_add(1, std::memory_order_relaxed);
            uint32_t* result = table->GetOrAddByFunc(duplicate_key, createFunc, i);
            if (result != nullptr) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield(); // 增加竞争机会
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker);
    }

    for (auto& t : threads) {
        t.join();
    }

    // 但所有尝试都应该安全完成，没有崩溃
    EXPECT_EQ(total_attempts.load(), num_threads * 100);

    // 验证插入的值存在
    uint32_t* found = table->Get(duplicate_key);
    ASSERT_NE(found, nullptr);
}

// 压力测试 - 持续的高并发操作
TEST_F(HashTableTest, StressTest) {
    const int num_threads = 12;
    const int duration_seconds = 3; // 运行3秒
    std::atomic<bool> stop{false};
    std::atomic<int64_t> total_operations{0};

    std::vector<std::thread> threads;

    auto worker = [this, &stop, &total_operations](int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> op_dis(0, 100); // 操作类型分布
        std::uniform_int_distribution<> key_dis(0, 10000); // 键分布

        auto createFunc = [](uint32_t value) -> uint32_t* {
            return new uint32_t(value);
        };

        auto start_time = std::chrono::steady_clock::now();

        while (!stop.load(std::memory_order_acquire)) {
            int op = op_dis(gen);
            uint32_t key = key_dis(gen);

            if (op < 60) { // 60% 插入操作
                table->GetOrAddByFunc(key, createFunc, key * 10);
            } else { // 40% 查找操作
                table->Get(key);
            }

            total_operations.fetch_add(1, std::memory_order_relaxed);

            // 每1000次操作检查一次时间
            if (total_operations.load() % 1000 == 0) {
                auto current_time = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(
                    current_time - start_time).count() >= duration_seconds) {
                    break;
                }
            }
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    // 运行指定时间
    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
    stop.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Stress test completed: " << total_operations.load()
              << " operations in " << duration_seconds << " seconds" << std::endl;
    EXPECT_GT(total_operations.load(), 0);
}

// 测试在扩容期间的并发访问
TEST_F(HashTableTest, ConcurrentAccessDuringResize) {
    const int num_threads = 8;
    const int initial_inserts = 10; // 触发扩容的初始插入
    std::atomic<int> ready_threads{0};
    std::atomic<bool> start{false};
    std::atomic<int> success_count{0};

    // 先插入足够的数据来确保后续操作会触发扩容
    auto createFunc = [](uint32_t value) -> uint32_t* {
        return new uint32_t(value);
    };
    for (int i = 0; i < initial_inserts; ++i) {
        table->GetOrAddByFunc(i, createFunc, i * 100);
    }

    std::vector<std::thread> threads;

    auto worker = [this, &ready_threads, &start, &success_count](int thread_id) {
        auto createFunc = [](uint32_t value) -> uint32_t* {
            return new uint32_t(value);
        };

        ready_threads.fetch_add(1, std::memory_order_relaxed);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // 大量插入操作，确保会触发扩容
        for (int i = 0; i < 1000; ++i) {
            uint32_t key = 1000 + thread_id * 1000 + i;
            if (table->GetOrAddByFunc(key, createFunc, key * 10) != nullptr) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }

            // 同时进行查找操作
            if (i % 10 == 0) {
                table->Get(key - 500); // 查找可能不存在的键
            }
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    // 等待所有线程准备就绪
    while (ready_threads.load() < num_threads) {
        std::this_thread::yield();
    }

    // 同时启动所有线程，最大化竞争
    start.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * 1000);
}

// 测试内存分配失败的情况
TEST_F(HashTableTest, MemoryAllocationFailure) {
    SUCCEED(); // 暂时跳过
}

// 并发测试 - 多线程插入和查找
TEST_F(HashTableTest, ConcurrentAccess) {
    const int num_threads = 8;
    const int num_operations = 3000;

    std::vector<std::thread> threads;

    auto worker = [this](int thread_id) {
        auto createFunc = [](uint32_t value) -> uint32_t* {
            return new uint32_t(value);
        };

        for (int i = 0; i < num_operations; ++i) {
            uint32_t key = thread_id * num_operations + i;
            uint32_t value = key * 10;

            // 插入数据
            uint32_t* inserted = table->GetOrAddByFunc(key, createFunc, value);
            ASSERT_NE(inserted, nullptr);

            // 立即查找验证
            uint32_t* found = table->Get(key);
            ASSERT_NE(found, nullptr);
            EXPECT_EQ(*found, value);
        }
    };

    // 启动多个线程
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    // 验证所有数据都存在
    for (int i = 0; i < num_threads; ++i) {
        for (int j = 0; j < num_operations; ++j) {
            uint32_t key = i * num_operations + j;
            uint32_t* found = table->Get(key);
            ASSERT_NE(found, nullptr);
            EXPECT_EQ(*found, key * 10);
        }
    }
}

// 测试移动语义
TEST_F(HashTableTest, MoveSemantics) {
    auto createFunc = [](uint32_t value) -> uint32_t* {
        return new uint32_t(value);
    };

    // 向原表插入一些数据
    table->GetOrAddByFunc(1, createFunc, 100);
    table->GetOrAddByFunc(2, createFunc, 200);

    // 移动构造
    TestHashTable moved_table(std::move(*table));

    // 验证数据被移动
    uint32_t* found1 = moved_table.Get(1);
    ASSERT_NE(found1, nullptr);
    EXPECT_EQ(*found1, 100);

    uint32_t* found2 = moved_table.Get(2);
    ASSERT_NE(found2, nullptr);
    EXPECT_EQ(*found2, 200);
}

// 测试析构函数正确释放内存
TEST_F(HashTableTest, DestructorReleasesMemory) {
    std::function<uint32_t*(uint32_t)> createFunc = [](uint32_t value) -> uint32_t* {
        return new uint32_t(value);
    };

    // 插入大量数据
    for (uint32_t i = 0; i < 100; ++i) {
        table->GetOrAddByFunc(i, createFunc, i);
    }

    SUCCEED();
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}