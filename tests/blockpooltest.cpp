//
// Created by admin on 25-11-26.
//
#include "ConcurrentQueue/Block.h"
#include "ConcurrentQueue/BlockManager.h"
#include "ConcurrentQueue/ConcurrentQueue.h"
#include "common/allocator.h"
#include "common/memory.h"

#include <atomic>
#include <gtest/gtest.h>
#include <iostream>
#include <thread>
#include <vector>

using namespace hakle;

// 测试用的基础块类型
struct TestBlock : public HakleFlagsBlock<int, 64> {
    // 可以添加额外的成员变量或方法
    int custom_data{ 0 };
};

// 自定义块类型用于测试继承约束
struct CustomBlock : public HakleFlagsBlock<double, 32> {
    std::atomic<bool> initialized{ false };

    CustomBlock() { initialized.store( true, std::memory_order_relaxed ); }
};

class BlockPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试前可以初始化一些资源
    }

    void TearDown() override {
        // 每个测试后清理资源
    }
};

// 测试基本功能 - 获取块
TEST_F( BlockPoolTest, BasicBlockAllocation ) {
    constexpr size_t POOL_SIZE  = 10;
    constexpr size_t BLOCK_SIZE = 64;

    BlockPool<HakleFlagsBlock<int, BLOCK_SIZE>> pool( POOL_SIZE );

    // 获取所有块
    std::vector<HakleFlagsBlock<int, BLOCK_SIZE>*> blocks;
    for ( size_t i = 0; i < POOL_SIZE; ++i ) {
        auto* block = pool.GetBlock();
        ASSERT_NE( block, nullptr );
        blocks.push_back( block );

        // 验证块的基本属性
        EXPECT_TRUE( block->HasOwner );
        EXPECT_EQ( block->Elements.size(), BLOCK_SIZE * sizeof( int ) );
    }

    // 尝试获取超出范围的块
    auto* should_be_null = pool.GetBlock();
    EXPECT_EQ( should_be_null, nullptr );
}

// 测试自定义块类型
TEST_F( BlockPoolTest, CustomBlockType ) {
    constexpr size_t POOL_SIZE  = 5;
    constexpr size_t BLOCK_SIZE = 32;

    BlockPool<CustomBlock> pool( POOL_SIZE );

    for ( size_t i = 0; i < POOL_SIZE; ++i ) {
        auto* block = pool.GetBlock();
        ASSERT_NE( block, nullptr );

        // 验证自定义属性
        EXPECT_TRUE( block->initialized.load( std::memory_order_relaxed ) );
        EXPECT_TRUE( block->HasOwner );
        EXPECT_EQ( block->Elements.size(), BLOCK_SIZE * sizeof( double ) );
    }
}

// 测试元素访问和修改
TEST_F( BlockPoolTest, ElementAccess ) {
    constexpr size_t POOL_SIZE  = 3;
    constexpr size_t BLOCK_SIZE = 8;

    BlockPool<HakleFlagsBlock<int, BLOCK_SIZE>> pool( POOL_SIZE );

    auto* block = pool.GetBlock();
    ASSERT_NE( block, nullptr );

    // 测试元素访问
    for ( size_t i = 0; i < BLOCK_SIZE; ++i ) {
        HakleAllocator<int>::Construct( ( *block )[ i ], i * 10 );
    }

    // 验证元素值
    for ( size_t i = 0; i < BLOCK_SIZE; ++i ) {
        EXPECT_EQ( *( *block )[ i ], i * 10 );
    }
}

// 测试缓存行对齐
TEST_F( BlockPoolTest, CacheLineAlignment ) {
    constexpr size_t POOL_SIZE  = 2;
    constexpr size_t BLOCK_SIZE = 16;

    BlockPool<HakleFlagsBlock<int, BLOCK_SIZE>> pool( POOL_SIZE );

    auto* block1 = pool.GetBlock();
    auto* block2 = pool.GetBlock();

    ASSERT_NE( block1, nullptr );
    ASSERT_NE( block2, nullptr );

    // 验证缓存行对齐（基本检查）
    auto addr1 = reinterpret_cast<uintptr_t>( block1 );
    auto addr2 = reinterpret_cast<uintptr_t>( block2 );

    EXPECT_EQ( addr1 % alignof( int ), 0 );
    EXPECT_EQ( addr2 % alignof( int ), 0 );

    // 块之间应该有足够的间距（至少缓存行大小）
    auto distance = std::abs( static_cast<long>( addr2 - addr1 ) );
    EXPECT_GE( distance, sizeof( HakleFlagsBlock<int, BLOCK_SIZE> ) );
}

// 测试多线程安全性
TEST_F( BlockPoolTest, ThreadSafety ) {
    constexpr size_t POOL_SIZE         = 100;
    constexpr size_t BLOCK_SIZE        = 64;
    constexpr size_t NUM_THREADS       = 8;
    constexpr size_t BLOCKS_PER_THREAD = 20;  // 总共需要160个，但池只有100个

    BlockPool<HakleFlagsBlock<int, BLOCK_SIZE>> pool( POOL_SIZE );
    std::atomic<size_t>                         successful_allocations{ 0 };
    std::atomic<size_t>                         failed_allocations{ 0 };
    std::vector<std::thread>                    threads;

    // 用于记录分配的块，确保没有重复分配
    std::array<std::atomic<HakleFlagsBlock<int, BLOCK_SIZE>*>, POOL_SIZE> allocated_blocks;
    for ( auto& ptr : allocated_blocks ) {
        ptr.store( nullptr, std::memory_order_relaxed );
    }

    auto worker = [ & ]( int thread_id ) {
        for ( size_t i = 0; i < BLOCKS_PER_THREAD; ++i ) {
            auto* block = pool.GetBlock();
            if ( block != nullptr ) {
                // 记录成功的分配
                successful_allocations.fetch_add( 1, std::memory_order_relaxed );

                // 验证块的唯一性（检查是否重复分配）
                bool found_slot = false;
                for ( auto& allocated : allocated_blocks ) {
                    HakleFlagsBlock<int, BLOCK_SIZE>* expected = nullptr;
                    if ( allocated.compare_exchange_strong( expected, block, std::memory_order_acq_rel, std::memory_order_relaxed ) ) {
                        found_slot = true;
                        break;
                    }
                }
                EXPECT_TRUE( found_slot ) << "Duplicate block allocation detected!";

                // 使用块（模拟一些工作）
                for ( int i = 0; i < BLOCK_SIZE; ++i ) {
                    HakleAllocator<int>::Construct( ( *block )[ i ], thread_id * 1000 + i );
                }
            }
            else {
                failed_allocations.fetch_add( 1, std::memory_order_relaxed );
            }
        }
    };

    // 启动多个线程
    for ( size_t t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( worker, static_cast<int>( t ) );
    }

    // 等待所有线程完成
    for ( auto& thread : threads ) {
        thread.join();
    }

    // 验证结果
    EXPECT_EQ( successful_allocations.load(), POOL_SIZE );  // 应该正好分配了池大小的块数
    EXPECT_GT( failed_allocations.load(), 0 );              // 应该有一些分配失败（因为请求数 > 池大小）

    std::cout << "Thread safety test: " << successful_allocations.load() << " successful, " << failed_allocations.load() << " failed" << std::endl;
}

// 测试边界情况：大小为0的池
TEST_F( BlockPoolTest, ZeroSizedPool ) {
    BlockPool<HakleFlagsBlock<int, 64>> pool( 0 );

    auto* block = pool.GetBlock();
    EXPECT_EQ( block, nullptr );
}

// 测试边界情况：大小为1的池
TEST_F( BlockPoolTest, SingleBlockPool ) {
    constexpr size_t                            BLOCK_SIZE = 16;
    BlockPool<HakleFlagsBlock<int, BLOCK_SIZE>> pool( 1 );

    auto* block1 = pool.GetBlock();
    ASSERT_NE( block1, nullptr );

    auto* block2 = pool.GetBlock();
    EXPECT_EQ( block2, nullptr );

    // 验证块可用
    HakleAllocator<int>::Construct( ( *block1 )[ 0 ], 42 );
    EXPECT_EQ( *( *block1 )[ 0 ], 42 );
}

// 测试内存布局
TEST_F( BlockPoolTest, MemoryLayout ) {
    constexpr size_t POOL_SIZE  = 3;
    constexpr size_t BLOCK_SIZE = 4;

    BlockPool<HakleFlagsBlock<int, BLOCK_SIZE>> pool( POOL_SIZE );

    std::vector<HakleFlagsBlock<int, BLOCK_SIZE>*> blocks;
    for ( size_t i = 0; i < POOL_SIZE; ++i ) {
        blocks.push_back( pool.GetBlock() );
    }

    // 验证块在内存中是连续的（或至少按顺序分配）
    for ( size_t i = 1; i < blocks.size(); ++i ) {
        auto* prev = blocks[ i - 1 ];
        auto* curr = blocks[ i ];

        // 块应该在内存中连续（或至少顺序正确）
        EXPECT_LT( reinterpret_cast<uintptr_t>( prev ), reinterpret_cast<uintptr_t>( curr ) );

        // 检查间距（应该至少是一个块的大小）
        auto distance = reinterpret_cast<uintptr_t>( curr ) - reinterpret_cast<uintptr_t>( prev );
        EXPECT_GE( distance, sizeof( HakleFlagsBlock<int, BLOCK_SIZE> ) );
    }
}

// 测试类型约束
TEST_F( BlockPoolTest, TypeConstraints ) {
    // 应该能正常编译（正确继承）
    struct GoodBlock : public HakleFlagsBlock<float, 16> {
        // 可以添加自定义成员
    };

    BlockPool<GoodBlock> good_pool( 5 );
    EXPECT_TRUE( true );  // 如果能编译到这里就说明类型约束正确

    // 下面这行应该导致编译错误（取消注释测试）
    /*
    struct BadBlock {
        // 没有继承HakleFlagsBlock
        std::array<float, 16> Elements;
    };
    BlockPool<float, 16, BadBlock> bad_pool(5); // 应该static_assert失败
    */
}

// 性能测试：大量分配
TEST_F( BlockPoolTest, PerformanceLargePool ) {
    constexpr size_t POOL_SIZE  = 10000;
    constexpr size_t BLOCK_SIZE = 32;

    auto start = std::chrono::high_resolution_clock::now();

    BlockPool<HakleFlagsBlock<int, BLOCK_SIZE>> pool( POOL_SIZE );

    // 分配所有块
    for ( size_t i = 0; i < POOL_SIZE; ++i ) {
        auto* block = pool.GetBlock();
        ASSERT_NE( block, nullptr );

        // 简单操作
        HakleAllocator<int>::Construct( ( *block )[ 0 ], i );
    }

    auto end      = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>( end - start );

    std::cout << "Allocated " << POOL_SIZE << " blocks in " << duration.count() << " microseconds" << std::endl;

    // 性能检查：应该很快完成
    EXPECT_LT( duration.count(), 1000000 );  // 少于1秒
}

// 测试析构函数正确性（通过Valgrind或ASan检查内存泄漏）
TEST_F( BlockPoolTest, Destruction ) {
    constexpr size_t POOL_SIZE  = 100;
    constexpr size_t BLOCK_SIZE = 64;

    // 在作用域内创建和销毁池
    {
        BlockPool<HakleFlagsBlock<int, BLOCK_SIZE>> pool( POOL_SIZE );

        // 分配一些块
        for ( size_t i = 0; i < POOL_SIZE / 2; ++i ) {
            auto* block = pool.GetBlock();
            ASSERT_NE( block, nullptr );
        }
    }  // 池应该在这里正确销毁

    EXPECT_TRUE( true );  // 如果能到达这里且没有内存泄漏，测试通过
}

int main( int argc, char** argv ) {
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}