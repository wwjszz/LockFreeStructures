#include "ConcurrentQueue/Block.h"
#include "ConcurrentQueue/BlockManager.h"
#include "ConcurrentQueue/ConcurrentQueue.h"
#include "common/allocator.h"

#include <gtest/gtest.h>

#include <assert.h>
#include <cstdio>
#include <thread>
#include <vector>

using namespace hakle;

constexpr std::size_t kBlockSize = 4;
constexpr std::size_t POOL_SIZE  = 40;

using TestFlagsBlock          = HakleFlagsBlock<int, kBlockSize>;
using TestCounterBlock        = HakleCounterBlock<int, kBlockSize>;
using TestCounterBlockManager = HakleCounterBlockManager<int, kBlockSize>;
using TestFlagsBlockManager   = HakleFlagsBlockManager<int, kBlockSize>;
using TestFlagsAllocator      = HakleAllocator<TestFlagsBlock>;

using TestSlowQueue = SlowQueue<int, kBlockSize>;
using TestSlowQueue = SlowQueue<int, kBlockSize>;

using FastAllocMode = TestSlowQueue::AllocMode;
using SlowAllocMode = TestSlowQueue::AllocMode;

struct ExceptionTest {
    ExceptionTest( int v = 0 ) : value( v ) {
        if ( v == 5 ) {
            throw std::runtime_error( "ExceptionTest" );
        }
    }

    ExceptionTest& operator=( ExceptionTest other ) {
        // if ( other.value == 5 ) {
        //     throw std::runtime_error( "ExceptionTest" );
        // }
        value = other.value;
        return *this;
    }

    ~ExceptionTest() = default;
    int value;
};

struct ExceptionTest2 {
    ExceptionTest2( int v = 0 ) : value( v ) {
        // if ( v == 5 ) {
        //     throw std::runtime_error( "ExceptionTest" );
        // }
    }

    ExceptionTest2& operator=( ExceptionTest2 other ) {
        if ( other.value == 5 ) {
            throw std::runtime_error( "ExceptionTest" );
        }
        value = other.value;
        return *this;
    }

    ~ExceptionTest2() = default;
    int value;
};

TEST( SlowQueueLeaks, StressTest ) {

    const unsigned long long        N             = 900000;  // 每个数从 0 到 N-1
    const int                       NUM_CONSUMERS = 68;    // 3 个消费者
    std::atomic<unsigned long long> total_sum{ 0 };        // 所有消费者结果累加
    std::vector<std::thread>        consumers;
    std::atomic<unsigned long long> count{ 0 };

    int* a = new int[ 100 ]{};
    for ( int i = 0; i < 100; ++i ) {
        a[ i ] = i;
    }

    HakleCounterBlockManager<int, kBlockSize> blockManager( POOL_SIZE );
    SlowQueue<int, kBlockSize>                queue( 2, blockManager );

    // 生产者：生产 0 ~ N-1
    std::thread producer( [ &queue, N, a ]() {
        for ( int i = 0; i < N; ++i ) {
            while ( !queue.EnqueueBulk<FastAllocMode::CanAlloc>( a, 100 ) ) {
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
            while ( count < N * 10 ) {
                int buffer[ 100 ]{};
                if ( std::size_t get_count = queue.DequeueBulk( &buffer[ 0 ], 10 ) ) {
                    int sum = 0;
                    for ( int i = 0; i < get_count; ++i ) {
                        sum += buffer[ i ];
                        ++count;
                    }
                    local_sum += sum;
                }
            }

            total_sum.fetch_add( local_sum, std::memory_order_relaxed );
        } );
    }

    // 等待生产者和所有消费者完成
    producer.join();
    delete[] a;
    for ( auto& t : consumers ) {
        t.join();
    }
}

TEST(SlowQueueLeaks, EnqueueExceptionTest) {
    using ExceptionBlock       = HakleCounterBlock<ExceptionTest, kBlockSize>;
    using ExceptionBlockManger = HakleBlockManager<ExceptionBlock>;
    using ExceptionQueue       = SlowQueue<ExceptionTest, kBlockSize, HakleAllocator<ExceptionTest>, ExceptionBlock>;
    ExceptionBlockManger blockManager( POOL_SIZE );
    ExceptionQueue       queue( 20, blockManager );
    using AllocMode = ExceptionQueue::AllocMode;

    const unsigned long long        N             = 900000;  // 每个数从 0 到 N-1
    const int                       NUM_CONSUMERS = 32;     // 3 个消费者
    std::atomic<unsigned long long> total_sum{ 0 };        // 所有消费者结果累加
    std::vector<std::thread>        consumers;
    std::atomic<unsigned long long> count{ 0 };
    std::atomic<int>                failed{ 0 };

    int* a = new int[ 100 ]{};
    for ( int i = 0; i < 100; ++i ) {
        a[ i ] = i;
    }

    // 生产者：生产 0 ~ N-1
    std::thread producer( [ &queue, N, a ]() {
        for ( std::size_t i = 0; i < N; ++i ) {
            // a[ 0 ] = i % 100;
            try {
                if ( !queue.EnqueueBulk<AllocMode::CanAlloc>( a + 10 * (i % 10), 10 ) ) {
                   printf( "enqueue failed\n" );
               }
                // if ( !queue.Enqueue<AllocMode::CanAlloc>( a[ 0 ] ) ) {
                //     printf( "enqueue failed\n" );
                // }
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
            while ( count < N * 9 ) {
                ExceptionTest buffer[ 100 ]{};
                // std::size_t   random_index = rand() % 100;
                // printf("random_index: %zu\n", random_index);
                try {
                    std::size_t get_count = queue.DequeueBulk( &buffer[ 0 ], 100 );
                    count.fetch_add( get_count, std::memory_order_relaxed );
                    for ( std::size_t i = 0; i < get_count; ++i ) {
                        local_sum += buffer[ i ].value;
                    }
                    // ExceptionTest value;
                    // if ( !queue.Dequeue( value ) ) {
                    //     ++count;
                    // }
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

    EXPECT_EQ( total_sum, N * (109) * 45 / 10 );
}

TEST(SlowQueueLeaks, DequeueExceptionTest) {
    using ExceptionBlock       = HakleCounterBlock<ExceptionTest2, kBlockSize>;
    using ExceptionBlockManger = HakleBlockManager<ExceptionBlock>;
    using ExceptionQueue       = SlowQueue<ExceptionTest2, kBlockSize>;
    ExceptionBlockManger blockManager( POOL_SIZE );
    ExceptionQueue       queue( 2, blockManager );
    using AllocMode = ExceptionQueue::AllocMode;

    const unsigned long long        N             = 80000;  // 每个数从 0 到 N-1
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
        for ( std::size_t i = 0; i < N; ++i ) {
            try {
                if ( !queue.EnqueueBulk<AllocMode::CanAlloc>( a, 100 ) ) {
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
            while ( count < N * 90 ) {
                ExceptionTest2 buffer[ 10 ]{};
                // std::size_t   random_index = rand() % 100;
                // printf("random_index: %zu\n", random_index);
                try {
                    std::size_t get_count = queue.DequeueBulk( &buffer[ 0 ], 10 );
                    count.fetch_add( get_count, std::memory_order_relaxed );
                    for ( std::size_t i = 0; i < get_count; ++i ) {
                        local_sum += buffer[ i ].value;
                    }
                }
                catch ( ... ) {
                    // count.fetch_add( 1, std::memory_order_relaxed );
                    // failed.fetch_add( 1, std::memory_order_relaxed );
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
    EXPECT_EQ( failed.load(), 0 );
    EXPECT_EQ(  total_sum.load(), expected_sum - N * 45 );

    // 验证队列为空
    EXPECT_EQ( queue.Size(), 0 );
}


int main(int argc, char** argv) {
    ::testing::InitGoogleTest( &argc, argv );

    // 打印所有测试开始和结束（可选）
    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    // 注意：不要删除默认的 listener，否则看不到输出

    return RUN_ALL_TESTS();
}