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
constexpr std::size_t kBlockSize = 64;

constexpr std::size_t POOL_SIZE = 100;
using TestFlagsBlock            = HakleFlagsBlock<int, kBlockSize>;
using TestCounterBlock          = HakleCounterBlock<int, kBlockSize>;
using TestCounterAllocator      = HakleAllocator<TestCounterBlock>;
using TestCounterBlockManager   = HakleBlockManager<TestCounterBlock, TestCounterAllocator>;
using TestFlagsAllocator        = HakleAllocator<TestFlagsBlock>;
using TestFlagsBlockManager     = HakleBlockManager<TestFlagsBlock, TestFlagsAllocator>;

// 定义测试队列类型
using TestFlagsQueue   = FastQueue<TestFlagsBlock, TestFlagsBlockManager>;
using TestCounterQueue = FastQueue<TestCounterBlock, TestCounterBlockManager>;

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
    queue.Enqueue<AllocMode::CanAlloc>( 3 );

    EXPECT_EQ( queue.Size(), 3 );

    int v;
    queue.Dequeue( v );
    EXPECT_EQ( queue.Size(), 2 );
}

// === 测试 3: 单生产者多消费者线程安全 ===
TEST( ConcurrentQueueTest, SingleProducerMultipleConsumer ) {
    TestFlagsBlockManager blockManager( POOL_SIZE );
    TestFlagsQueue        queue( 100, blockManager );
    using AllocMode         = TestFlagsQueue::AllocMode;
    const int num_items     = 1000;
    const int num_consumers = 4;

    std::atomic<int>         consumed_count{ 0 };
    std::vector<std::thread> consumers;

    // 启动多个消费者
    for ( int i = 0; i < num_consumers; ++i ) {
        consumers.emplace_back( [ &queue, &consumed_count ]() {
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
using ThrowingBlock        = HakleFlagsBlock<ThrowingType, kBlockSize>;
using ThrowingBlockManager = HakleBlockManager<ThrowingBlock>;
using ThrowingQueue        = FastQueue<ThrowingBlock>;

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

// === 测试 7: 多消费者压力测试（Multi-Consumer Stress Test）===
TEST( ConcurrentQueueTest, MultiConsumerStressTest ) {
    TestCounterBlockManager blockManager( POOL_SIZE );
    TestCounterQueue        queue( 100, blockManager );
    using AllocMode = TestCounterQueue::AllocMode;

    const unsigned long long        N             = 800000;  // 每个数从 0 到 N-1
    const int                       NUM_CONSUMERS = 68;      // 3 个消费者
    std::atomic<unsigned long long> total_sum{ 0 };          // 所有消费者结果累加
    std::vector<std::thread>        consumers;
    std::atomic<unsigned long long> count{ 0 };

    int* a = new int[ 100 ]{};
    for ( int i = 0; i < 100; ++i ) {
        a[ i ] = i;
    }

    // 生产者：生产 0 ~ N-1
    std::thread producer( [ &queue, N, a ]() {
        for ( int i = 0; i < N; ++i ) {
            while ( !queue.EnqueueBulk<AllocMode::CanAlloc>( a, 100 ) ) {
                printf( "enqueue failed\n" );
            }
        }
    } );

    // 创建多个消费者
    for ( int c = 0; c < NUM_CONSUMERS; ++c ) {
        consumers.emplace_back( [ &queue, N, &total_sum, &count ]() {
            unsigned long long local_sum = 0;
            int                value;

            // 每个消费者一直取，直到取到 N 个元素为止
            while ( count < N * 100 ) {
                int buffer[ 10 ]{};
                if ( std::size_t get_count = queue.DequeueBulk( &buffer[ 0 ], 10 ) ) {
                    for ( int i = 0; i < get_count; ++i ) {
                        local_sum += buffer[ i ];
                        ++count;
                    }
                }
            }

            // 累加到总和（使用原子操作）
            total_sum.fetch_add( local_sum, std::memory_order_relaxed );
        } );
    }

    // 等待生产者和所有消费者完成
    producer.join();
    for ( auto& t : consumers ) {
        t.join();
    }

    // 验证：总和是否正确
    unsigned long long expected_sum = 99 * 50 * N;
    EXPECT_EQ( total_sum.load(), expected_sum );

    // 验证队列为空
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

// === 测试 7: 多消费者压力测试（Multi-Consumer Stress Test）===
TEST( ConcurrentQueueTest, MultiConsumerStressTestWithException ) {
    using ExceptionBlock       = HakleFlagsBlock<ExceptionTest, kBlockSize>;
    using ExceptionBlockManger = HakleBlockManager<ExceptionBlock>;
    using ExceptionQueue       = FastQueue<ExceptionBlock>;
    ExceptionBlockManger blockManager( POOL_SIZE );
    ExceptionQueue       queue( 100, blockManager );
    using AllocMode = ExceptionQueue::AllocMode;

    const unsigned long long        N             = 800;  // 每个数从 0 到 N-1
    const int                       NUM_CONSUMERS = 68;   // 3 个消费者
    std::atomic<unsigned long long> total_sum{ 0 };       // 所有消费者结果累加
    std::vector<std::thread>        consumers;
    std::atomic<unsigned long long> count{ 0 };
    std::atomic<int>                failed{ 0 };

    int* a = new int[ 100 ]{};
    for ( int i = 0; i < 100; ++i ) {
        a[ i ] = i;
    }

    // 生产者：生产 0 ~ N-1
    std::thread producer( [ &queue, N, a ]() {
        for ( std::size_t i = 1; i <= N; ++i ) {
            a[ 0 ] = i % 100;
            try {
                if ( !queue.EnqueueBulk<AllocMode::CanAlloc>( a, 1 ) ) {
                    printf( "enqueue failed\n" );
                }
            }
            catch ( ... ) {
                // printf("find a exception\n");
                // do nothing
            }
        }
    } );

    // 创建多个消费者
    for ( int c = 0; c < NUM_CONSUMERS; ++c ) {
        consumers.emplace_back( [ &queue, N, &total_sum, &count, &failed ]() {
            unsigned long long local_sum = 0;
            int                value;

            // 每个消费者一直取，直到取到 N 个元素为止
            while ( count < N - (N / 100) ) {
                ExceptionTest buffer[ 1 ]{};
                // std::size_t   random_index = rand() % 100;
                // printf("random_index: %zu\n", random_index);
                try {
                    std::size_t get_count = queue.DequeueBulk( &buffer[ 0 ], 1 );
                    count.fetch_add( get_count, std::memory_order_relaxed );
                    for ( std::size_t i = 0; i < get_count; ++i ) {
                        local_sum += buffer[ i ].value;
                    }
                }
                catch ( ... ) {
                    count.fetch_add( 1, std::memory_order_relaxed );
                    failed.fetch_add( 1, std::memory_order_relaxed );
                }
            }

            // 累加到总和（使用原子操作）
            total_sum.fetch_add( local_sum, std::memory_order_relaxed );
        } );
    }

    // 等待生产者和所有消费者完成
    producer.join();
    for ( auto& t : consumers ) {
        t.join();
    }

    // 验证：总和是否正确
    // unsigned long long expected_sum = 101 * 50 * N;
    // EXPECT_EQ( total_sum.load(), expected_sum );
    EXPECT_EQ( failed.load(), 0 );
    // EXPECT_EQ( count.load(), N * 100 );
    printf( "count: %llu\n", count.load() );
    printf( "failed: %d\n", failed.load() );
    printf( "total_sum: %llu\n", total_sum.load() );
    printf( "expected_sum: %llu\n", 101 * 50 * N );

    // 验证队列为空
    EXPECT_EQ( queue.Size(), 0 );
}

// test/queue_test.cpp 最后加上：
int main( int argc, char** argv ) {
    ::testing::InitGoogleTest( &argc, argv );

    // 打印所有测试开始和结束（可选）
    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    // 注意：不要删除默认的 listener，否则看不到输出

    return RUN_ALL_TESTS();
}