//
// Created by admin on 25-11-25.
//
#include "../ConcurrentQueue/BlockManager.h"
#include <atomic>
#include <gtest/gtest.h>
#include <iostream>
#include <thread>
#include <vector>

using namespace hakle;

// 测试节点类型：继承 FreeListNode，HasOwner 默认为 false
struct TestNode : public FreeListNode<TestNode> {
    int               value;
    std::atomic<bool> in_use{ false };

    explicit TestNode( int v = 0 ) : value( v ) {}
};

class FreeListTest : public ::testing::Test {
protected:
    void SetUp() override { list = new FreeList<TestNode>(); }

    void TearDown() override {
        // FreeList 析构时会自动 delete 所有 HasOwner==false 的节点
        delete list;
    }

    FreeList<TestNode>* list;
};

// 测试基本添加和获取
TEST_F( FreeListTest, BasicAddAndGet ) {
    auto* node1 = new TestNode( 42 );
    auto* node2 = new TestNode( 100 );

    list->Add( node1 );
    list->Add( node2 );

    TestNode* retrieved1 = list->TryGet();
    ASSERT_NE( retrieved1, nullptr );

    TestNode* retrieved2 = list->TryGet();
    ASSERT_NE( retrieved2, nullptr );

    EXPECT_TRUE( ( retrieved1 == node1 && retrieved2 == node2 ) || ( retrieved1 == node2 && retrieved2 == node1 ) );

    TestNode* should_be_null = list->TryGet();
    EXPECT_EQ( should_be_null, nullptr );
}

// 测试重复添加同一个节点（应被忽略）
TEST_F( FreeListTest, DuplicateAdd ) {
    auto* node = new TestNode( 42 );
    list->Add( node );
    list->Add( node );  // 第二次添加应被忽略

    TestNode* retrieved1 = list->TryGet();
    ASSERT_NE( retrieved1, nullptr );
    EXPECT_EQ( retrieved1, node );

    TestNode* retrieved2 = list->TryGet();
    EXPECT_EQ( retrieved2, nullptr );
}

// 测试从空列表获取
TEST_F( FreeListTest, GetFromEmptyList ) {
    TestNode* result = list->TryGet();
    EXPECT_EQ( result, nullptr );
}

// 测试节点重用
TEST_F( FreeListTest, NodeReuse ) {
    auto* node = new TestNode( 42 );

    list->Add( node );
    TestNode* first_get = list->TryGet();
    ASSERT_EQ( first_get, node );

    list->Add( node );  // 重新加入
    TestNode* second_get = list->TryGet();
    ASSERT_EQ( second_get, node );
}

// 单线程压力测试
TEST_F( FreeListTest, SingleThreadStressTest ) {
    const int              NUM_NODES = 1000;
    std::vector<TestNode*> nodes;
    nodes.reserve( NUM_NODES );

    for ( int i = 0; i < NUM_NODES; ++i ) {
        nodes.push_back( new TestNode( i ) );
        list->Add( nodes[ i ] );
    }

    std::vector<TestNode*> retrieved;
    for ( int i = 0; i < NUM_NODES; ++i ) {
        TestNode* node = list->TryGet();
        ASSERT_NE( node, nullptr );
        retrieved.push_back( node );
    }

    EXPECT_EQ( retrieved.size(), NUM_NODES );
    EXPECT_EQ( list->TryGet(), nullptr );
}

// 多线程并发添加
TEST_F( FreeListTest, ConcurrentAdd ) {
    const int NUM_THREADS      = 4;
    const int NODES_PER_THREAD = 100;
    const int TOTAL_NODES      = NUM_THREADS * NODES_PER_THREAD;

    std::vector<TestNode*> nodes;
    nodes.reserve( TOTAL_NODES );
    for ( int i = 0; i < TOTAL_NODES; ++i ) {
        nodes.push_back( new TestNode( i ) );
    }

    std::vector<std::thread> threads;
    std::atomic<int>         add_count{ 0 };

    for ( int t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( [ this, t, &nodes, &add_count, NODES_PER_THREAD ]() {
            for ( int i = 0; i < NODES_PER_THREAD; ++i ) {
                int idx = t * NODES_PER_THREAD + i;
                list->Add( nodes[ idx ] );
                add_count.fetch_add( 1, std::memory_order_relaxed );
            }
        } );
    }

    for ( auto& th : threads )
        th.join();

    EXPECT_EQ( add_count.load(), TOTAL_NODES );

    int get_count = 0;
    while ( list->TryGet() != nullptr ) {
        get_count++;
    }
    EXPECT_EQ( get_count, TOTAL_NODES );
}

// 多线程同时添加和获取（生产者-消费者模型）
TEST_F( FreeListTest, ConcurrentAddAndGet ) {
    const int NUM_PRODUCERS      = 2;
    const int NUM_CONSUMERS      = 2;
    const int NODES_PER_PRODUCER = 500;

    std::vector<TestNode*> initial_nodes;
    initial_nodes.reserve( NUM_PRODUCERS * NODES_PER_PRODUCER );
    for ( int i = 0; i < NUM_PRODUCERS * NODES_PER_PRODUCER; ++i ) {
        initial_nodes.push_back( new TestNode( i ) );
    }

    std::vector<std::thread> threads;
    std::atomic<int>         produced{ 0 };
    std::atomic<int>         consumed{ 0 };
    std::atomic<bool>        stop{ false };

    // 生产者线程
    for ( int t = 0; t < NUM_PRODUCERS; ++t ) {
        threads.emplace_back( [ this, t, &initial_nodes, &produced, NODES_PER_PRODUCER ]() {
            for ( int i = 0; i < NODES_PER_PRODUCER; ++i ) {
                int idx = t * NODES_PER_PRODUCER + i;
                list->Add( initial_nodes[ idx ] );
                produced.fetch_add( 1, std::memory_order_relaxed );
            }
        } );
    }

    // 消费者线程
    for ( int t = 0; t < NUM_CONSUMERS; ++t ) {
        threads.emplace_back( [ this, &consumed, &stop ]() {
            while ( !stop.load( std::memory_order_acquire ) ) {
                TestNode* node = list->TryGet();
                if ( node != nullptr ) {
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                    // 模拟使用
                    std::this_thread::sleep_for( std::chrono::microseconds( 1 ) );
                    // 重新放回供其他线程使用
                    list->Add( node );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    std::this_thread::sleep_for( std::chrono::seconds( 2 ) );
    stop.store( true, std::memory_order_release );

    for ( auto& th : threads )
        th.join();

    std::cout << "Concurrent test: " << produced.load() << " produced, " << consumed.load() << " consumed (with reuse)" << std::endl;

    EXPECT_GT( produced.load(), 0 );
    EXPECT_GT( consumed.load(), 0 );
}

// 内存序基本正确性测试
TEST_F( FreeListTest, MemoryOrderSanity ) {
    auto* n1 = new TestNode( 1 );
    auto* n2 = new TestNode( 2 );
    auto* n3 = new TestNode( 3 );

    list->Add( n1 );
    list->Add( n2 );

    auto* r1 = list->TryGet();
    auto* r2 = list->TryGet();
    ASSERT_NE( r1, nullptr );
    ASSERT_NE( r2, nullptr );

    list->Add( r1 );
    list->Add( r2 );
    list->Add( n3 );

    auto* r3 = list->TryGet();
    auto* r4 = list->TryGet();
    auto* r5 = list->TryGet();

    EXPECT_NE( r3, nullptr );
    EXPECT_NE( r4, nullptr );
    EXPECT_NE( r5, nullptr );
}

// 大量节点测试
TEST_F( FreeListTest, LargeNumberOfNodes ) {
    const int              LARGE_NUMBER = 10000;
    std::vector<TestNode*> nodes;
    nodes.reserve( LARGE_NUMBER );
    for ( int i = 0; i < LARGE_NUMBER; ++i ) {
        nodes.push_back( new TestNode( i ) );
        list->Add( nodes[ i ] );
    }

    int count = 0;
    while ( list->TryGet() != nullptr ) {
        count++;
    }
    EXPECT_EQ( count, LARGE_NUMBER );
}

// 类型约束测试（编译期）
TEST_F( FreeListTest, TypeConstraints ) {
    struct GoodNode : FreeListNode<GoodNode> {
        int data;
    };
    // 应该能编译通过
    FreeList<GoodNode> good_list;
    auto*              node = new GoodNode{};
    good_list.Add( node );
    delete good_list.TryGet();  // 手动删（因为 HasOwner=false，但这里只测构造）

    // 下面取消注释会导致编译错误：
    /*
    struct BadNode { int x; };
    FreeList<BadNode> bad; // static_assert 失败
    */
}

// 主函数
int main( int argc, char** argv ) {
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}