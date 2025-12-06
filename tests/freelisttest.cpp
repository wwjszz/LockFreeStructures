//
// Created by admin on 25-11-25.
//
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include "../ConcurrentQueue/BlockPool.h"

using namespace hakle;

// 测试节点类型
struct TestNode : public FreeListNode<TestNode> {
    int value;
    std::atomic<bool> in_use{false};

    TestNode(int v = 0) : value(v) {}
};

class FreeListTest : public ::testing::Test {
protected:
    void SetUp() override {
        list = new FreeList<TestNode>();
    }

    void TearDown() override {
        delete list;
    }

    FreeList<TestNode>* list;
};

// 测试基本添加和获取
TEST_F(FreeListTest, BasicAddAndGet) {
    TestNode node1(42);
    TestNode node2(100);

    // 添加节点到空闲列表
    list->Add(&node1);
    list->Add(&node2);

    // 获取节点
    TestNode* retrieved1 = list->TryGet();
    ASSERT_NE(retrieved1, nullptr);

    TestNode* retrieved2 = list->TryGet();
    ASSERT_NE(retrieved2, nullptr);

    // 验证获取的节点是之前添加的（顺序可能不同）
    EXPECT_TRUE((retrieved1 == &node1 && retrieved2 == &node2) ||
                (retrieved1 == &node2 && retrieved2 == &node1));

    // 列表应该为空
    TestNode* should_be_null = list->TryGet();
    EXPECT_EQ(should_be_null, nullptr);
}

// 测试重复添加同一个节点
TEST_F(FreeListTest, DuplicateAdd) {
    TestNode node(42);

    // 多次添加同一个节点
    list->Add(&node);
    list->Add(&node); // 应该被忽略

    // 只能获取一次
    TestNode* retrieved1 = list->TryGet();
    ASSERT_NE(retrieved1, nullptr);
    EXPECT_EQ(retrieved1, &node);

    TestNode* retrieved2 = list->TryGet();
    EXPECT_EQ(retrieved2, nullptr);
}

// 测试获取空列表
TEST_F(FreeListTest, GetFromEmptyList) {
    TestNode* result = list->TryGet();
    EXPECT_EQ(result, nullptr);
}

// // 测试引用计数逻辑
// TEST_F(FreeListTest, ReferenceCounting) {
//     TestNode node(42);
//
//     // 初始引用计数应为0
//     EXPECT_EQ(node.Refs.load() & list->RefsMask, 0);
//
//     // 添加节点
//     list->Add(&node);
//
//     // 添加后引用计数应该有AddFlag
//     EXPECT_TRUE(node.Refs.load() & list->AddFlag);
//
//     // 获取节点
//     TestNode* retrieved = list->TryGet();
//     ASSERT_EQ(retrieved, &node);
//
//     // 获取后引用计数应该清除AddFlag
//     EXPECT_EQ(node.Refs.load() & list->AddFlag, 0);
// }

// 测试节点重用
TEST_F(FreeListTest, NodeReuse) {
    TestNode node(42);

    // 添加->获取->再添加->再获取
    list->Add(&node);

    TestNode* first_get = list->TryGet();
    ASSERT_EQ(first_get, &node);

    // 再次添加
    list->Add(&node);

    TestNode* second_get = list->TryGet();
    ASSERT_EQ(second_get, &node);
}

// 单线程压力测试
TEST_F(FreeListTest, SingleThreadStressTest) {
    const int NUM_NODES = 1000;
    std::vector<TestNode> nodes(NUM_NODES);

    // 添加所有节点
    for (int i = 0; i < NUM_NODES; ++i) {
        nodes[i].value = i;
        list->Add(&nodes[i]);
    }

    // 获取所有节点
    std::vector<TestNode*> retrieved;
    for (int i = 0; i < NUM_NODES; ++i) {
        TestNode* node = list->TryGet();
        ASSERT_NE(node, nullptr);
        retrieved.push_back(node);
    }

    // 验证获取了所有节点
    EXPECT_EQ(retrieved.size(), NUM_NODES);

    // 应该没有更多节点
    TestNode* should_be_null = list->TryGet();
    EXPECT_EQ(should_be_null, nullptr);
}

// 多线程添加测试
TEST_F(FreeListTest, ConcurrentAdd) {
    const int NUM_THREADS = 4;
    const int NODES_PER_THREAD = 100;
    const int TOTAL_NODES = NUM_THREADS * NODES_PER_THREAD;

    std::vector<TestNode> nodes(TOTAL_NODES);
    std::vector<std::thread> threads;
    std::atomic<int> add_count{0};

    for (int i = 0; i < TOTAL_NODES; ++i) {
        nodes[i].value = i;
    }

    // 多个线程同时添加节点
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, t, &nodes, &add_count]() {
            for (int i = 0; i < NODES_PER_THREAD; ++i) {
                int index = t * NODES_PER_THREAD + i;
                list->Add(&nodes[index]);
                add_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(add_count.load(), TOTAL_NODES);

    // 验证所有节点都在列表中
    int get_count = 0;
    while (list->TryGet() != nullptr) {
        get_count++;
    }

    EXPECT_EQ(get_count, TOTAL_NODES);
}

// 多线程同时添加和获取测试
TEST_F(FreeListTest, ConcurrentAddAndGet) {
    const int NUM_THREADS = 4;
    const int OPERATIONS_PER_THREAD = 500;

    std::vector<TestNode> nodes(OPERATIONS_PER_THREAD * 2);
    std::vector<std::thread> threads;
    std::atomic<int> add_count{0};
    std::atomic<int> get_count{0};
    std::atomic<bool> stop{false};

    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].value = static_cast<int>(i);
    }

    // 生产者线程：添加节点
    for (int t = 0; t < NUM_THREADS / 2; ++t) {
        threads.emplace_back([this, t, &nodes, &add_count]() {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                int index = t * OPERATIONS_PER_THREAD + i;
                if (index < nodes.size()) {
                    list->Add(&nodes[index]);
                    add_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // 消费者线程：获取节点
    for (int t = 0; t < NUM_THREADS / 2; ++t) {
        threads.emplace_back([this, &get_count, &stop]() {
            while (!stop.load(std::memory_order_acquire)) {
                TestNode* node = list->TryGet();
                if (node != nullptr) {
                    get_count.fetch_add(1, std::memory_order_relaxed);
                    // 模拟使用节点
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                    // 重新添加节点以便重用
                    list->Add(node);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    // 运行一段时间
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    std::cout << "Concurrent test: " << add_count.load() << " adds, "
              << get_count.load() << " gets" << std::endl;

    EXPECT_GT(add_count.load(), 0);
    EXPECT_GT(get_count.load(), 0);
}

// 测试内存序的正确性（通过TSAN检测）
TEST_F(FreeListTest, MemoryOrderSanity) {
    TestNode node1(1), node2(2), node3(3);

    // 基本操作序列
    list->Add(&node1);
    list->Add(&node2);

    TestNode* n1 = list->TryGet();
    TestNode* n2 = list->TryGet();

    EXPECT_TRUE(n1 != nullptr && n2 != nullptr);

    // 重用节点
    list->Add(n1);
    list->Add(n2);
    list->Add(&node3);

    // 再次获取
    TestNode* n3 = list->TryGet();
    TestNode* n4 = list->TryGet();
    TestNode* n5 = list->TryGet();

    EXPECT_TRUE(n3 != nullptr && n4 != nullptr && n5 != nullptr);
}

// 测试边界情况：大量节点
TEST_F(FreeListTest, LargeNumberOfNodes) {
    const int LARGE_NUMBER = 10000;
    std::vector<TestNode> nodes(LARGE_NUMBER);

    for (int i = 0; i < LARGE_NUMBER; ++i) {
        nodes[i].value = i;
        list->Add(&nodes[i]);
    }

    int count = 0;
    while (list->TryGet() != nullptr) {
        count++;
    }

    EXPECT_EQ(count, LARGE_NUMBER);
}

// 测试类型约束
TEST_F(FreeListTest, TypeConstraints) {
    // 应该能正常编译（正确继承）
    struct GoodNode : public FreeListNode<GoodNode> {
        int data;
    };

    FreeList<GoodNode> good_list;
    GoodNode good_node;
    good_list.Add(&good_node);

    // 下面这行应该导致编译错误（取消注释测试）
    /*
    struct BadNode {
        int data;
    };
    FreeList<BadNode> bad_list; // 应该static_assert失败
    */
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}