// test/queue_test.cpp
#include "ConcurrentQueue/Block.h"

#include <gtest/gtest.h>

#include "ConcurrentQueue/ConcurrentQueue.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace hakle;

// 使用的 block size
constexpr std::size_t kBlockSize = 2;

constexpr std::size_t POOL_SIZE = 100;
using TestCounterBlock          = HakleCounterBlock<int, kBlockSize>;
using TestFlagsBlock            = TestCounterBlock;
using TestCounterAllocator      = HakleAllocator<TestCounterBlock>;
using TestCounterBlockManager   = HakleBlockManager<TestCounterBlock, TestCounterAllocator>;
using TestFlagsAllocator        = HakleAllocator<TestFlagsBlock>;
using TestFlagsBlockManager     = HakleBlockManager<TestFlagsBlock, TestFlagsAllocator>;

// 定义测试队列类型
using TestFlagsQueue   = SlowQueue<TestFlagsBlock, TestFlagsBlockManager>;
using TestCounterQueue = SlowQueue<TestCounterBlock, TestCounterBlockManager>;

// 辅助：等待一段时间让操作完成
void SleepFor( std::int64_t ms ) { std::this_thread::sleep_for( std::chrono::milliseconds( ms ) ); }

// === 测试 1: 基本 Enqueue/Dequeue ===
TEST( ConcurrentQueueTest, BasicEnqueueDequeue ) {
    TestFlagsBlockManager blockManager( POOL_SIZE );
    TestFlagsQueue        queue( 10, blockManager );
    using AllocMode = TestFlagsQueue::AllocMode;

    EXPECT_TRUE( queue.Enqueue<AllocMode::CanAlloc>( 100 ) );
    EXPECT_TRUE( queue.Enqueue<AllocMode::CanAlloc>( 200 ) );

    int value = 0;
    EXPECT_TRUE( queue.Dequeue( value ) );
    EXPECT_EQ( value, 100 );

    EXPECT_TRUE( queue.Dequeue( value ) );
    EXPECT_EQ( value, 200 );

    EXPECT_FALSE( queue.Dequeue( value ) );  // 空队列
}

// === 测试 2: 队列大小 ===
TEST( ConcurrentQueueTest, Size ) {
    TestCounterBlockManager blockManager( POOL_SIZE );
    TestCounterQueue        queue( 5, blockManager );
    using AllocMode = TestCounterQueue::AllocMode;
    EXPECT_EQ( queue.Size(), 0 );

    queue.Enqueue<AllocMode::CanAlloc>( 1 );
    queue.Enqueue<AllocMode::CanAlloc>( 2 );
    EXPECT_EQ( queue.Enqueue<AllocMode::CanAlloc>( 3 ), true );


    EXPECT_EQ( queue.Size(), 3 );

    int v;
    queue.Dequeue( v );
    EXPECT_EQ( v, 1 );
    EXPECT_EQ( queue.Size(), 2 );
    queue.Dequeue( v );
    EXPECT_EQ( v, 2 );
    // queue.Dequeue( v );
    // EXPECT_EQ( v, 3 );
}

// === 测试 3: 单生产者多消费者线程安全 ===
TEST( ConcurrentQueueTest, SingleProducerMultipleConsumer ) {
    TestFlagsBlockManager blockManager( POOL_SIZE );
    TestFlagsQueue        queue( 100, blockManager );
    using AllocMode         = TestFlagsQueue::AllocMode;
    const int num_items     = 10000000;
    const int num_consumers = 400;

    std::atomic<int>         consumed_count{ 0 };
    std::vector<std::thread> consumers;

    // 启动多个消费者
    for ( int i = 0; i < num_consumers; ++i ) {
        consumers.emplace_back( [ num_items, &queue, &consumed_count ]() {
            int value;
            while ( consumed_count.load() < num_items ) {
                if ( queue.Dequeue( value ) ) {
                    ++consumed_count;
                    // 模拟处理
                    std::this_thread::yield();
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    // 生产者线程
    std::thread producer( [ &queue, num_items ]() {
        for ( int i = 0; i < num_items; ++i ) {
            while ( !queue.Enqueue<AllocMode::CanAlloc>( i ) ) {
                std::this_thread::yield();  // 如果失败（理论上不会），重试
            }
        }
    } );

    producer.join();
    for ( auto& t : consumers ) {
        t.join();
    }

    EXPECT_EQ( consumed_count.load(), num_items );
    EXPECT_EQ( queue.Size(), 0 );  // 所有都被消费
}

// === 测试 4: 异常安全（构造函数抛出）===
struct ThrowingType {
    int value;

    explicit ThrowingType( int v ) : value( v ) {
        if ( v == 999 ) {
            throw std::runtime_error( "Simulated construction failure" );
        }
    }

    ThrowingType( const ThrowingType& )            = default;
    ThrowingType& operator=( const ThrowingType& ) = default;
};
using ThrowingBlock        = HakleCounterBlock<ThrowingType, kBlockSize>;
using ThrowingBlockManager = HakleBlockManager<ThrowingBlock>;
using ThrowingQueue        = SlowQueue<ThrowingBlock>;

TEST( ConcurrentQueueTest, ExceptionSafety ) {
    ThrowingBlockManager blockManager( POOL_SIZE );
    ThrowingQueue        queue( 10, blockManager );
    using AllocMode = ThrowingQueue::AllocMode;

    // 正常插入
    EXPECT_TRUE( queue.Enqueue<AllocMode::CanAlloc>( ThrowingType( 10 ) ) );

    // 抛出异常的插入
    EXPECT_THROW( { queue.Enqueue<AllocMode::CanAlloc>( ThrowingType( 999 ) ); }, std::runtime_error );

    // 队列仍可用
    ThrowingType val( 0 );
    EXPECT_TRUE( queue.Dequeue( val ) );
    EXPECT_EQ( val.value, 10 );

    // 再次插入正常值
    EXPECT_TRUE( queue.Enqueue<AllocMode::CanAlloc>( ThrowingType( 20 ) ) );
    EXPECT_TRUE( queue.Dequeue( val ) );
    EXPECT_EQ( val.value, 20 );
}

// === 测试 5: 使用 CounterCheckPolicy 的行为一致性 ===
TEST( ConcurrentQueueTest, CounterPolicyBasic ) {
    TestCounterBlockManager blockManager( POOL_SIZE );
    TestCounterQueue        queue( 10, blockManager );
    using AllocMode = TestCounterQueue::AllocMode;

    EXPECT_TRUE( queue.Enqueue<AllocMode::CanAlloc>( 42 ) );
    int value = 0;
    EXPECT_TRUE( queue.Dequeue( value ) );
    EXPECT_EQ( value, 42 );
    EXPECT_FALSE( queue.Dequeue( value ) );
}

// === 测试 6: 大量数据压测 ===
TEST( ConcurrentQueueTest, HighVolumeStressTest ) {
    TestCounterBlockManager blockManager( POOL_SIZE );
    TestCounterQueue        queue( 100, blockManager );
    using AllocMode = TestCounterQueue::AllocMode;
    const int N     = 10000;

    std::thread producer( [ &queue, N ]() {
        for ( int i = 0; i < N; ++i ) {
            while ( !queue.Enqueue<AllocMode::CanAlloc>( i ) ) {
                std::this_thread::yield();
            }
        }
    } );

    std::thread consumer( [ &queue, N ]() {
        int sum          = 0;
        int expected_sum = N * ( N - 1 ) / 2;
        int value;
        for ( int i = 0; i < N; ++i ) {
            while ( !queue.Dequeue( value ) ) {
                std::this_thread::yield();
            }
            sum += value;
        }
        EXPECT_EQ( sum, expected_sum );
    } );

    producer.join();
    consumer.join();
    EXPECT_EQ( queue.Size(), 0 );
}

struct ExceptionTest {
    ExceptionTest( int v = 0 ) : value( v ) {
        if ( v == 5 ) {
            throw std::runtime_error( "ExceptionTest" );
        }
    }

    ExceptionTest& operator=( ExceptionTest other ) {
        if ( other.value == 5 ) {
            throw std::runtime_error( "ExceptionTest" );
        }
        value = other.value;
        return *this;
    }

    ~ExceptionTest() = default;
    int value;
};

// test/queue_test.cpp 最后加上：
int main( int argc, char** argv ) {
    ::testing::InitGoogleTest( &argc, argv );

    // 打印所有测试开始和结束（可选）
    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    // 注意：不要删除默认的 listener，否则看不到输出

    return RUN_ALL_TESTS();
}