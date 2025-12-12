// ExplicitQueueExceptionTest.cpp
#include "ConcurrentQueue/Block.h"

#include <gtest/gtest.h>
#include <iostream>
#include <thread>
#include <vector>

#include "ConcurrentQueue/ConcurrentQueue.h"  // 浣犵殑 ExplicitQueue 瀹氫箟
#include "Debug/ThrowOnAssign.h"
#include "Debug/ThrowOnCtor.h"

using namespace hakle;

// 这里假设你已经有一个 HakleBlockManager<BlockType>，并且 ExplicitQueue 的模板参数是 <BLOCK_TYPE, BLOCK_MANAGER_TYPE>
// 如果你用的是 HakleFlagsBlock + HakleBlockManager，就这样定义：

constexpr std::size_t kBlockSize = 64;

using Block        = HakleFlagsBlock<ThrowOnCtor, kBlockSize>;
using BlockManager = HakleBlockManager<Block>;
using Queue        = FastQueue<Block, BlockManager>;
using AllocMode    = Queue::AllocMode;

// 如果你的 BlockManager 类型名不同，改一下上面的 using 即可。

// ========== 测试 1: Enqueue 构造异常回滚 ==========

TEST( ExplicitQueueExceptionTest, Enqueue_ExceptionRollback ) {
    constexpr std::size_t POOL_SIZE = 16;
    BlockManager          mgr( POOL_SIZE );
    Queue                 q( 128, mgr );

    // 先让一次构造成功，第二次构造抛
    ThrowOnCtor::Reset( /*throwOn=*/1 );

    bool caught = false;
    try {
        q.Enqueue<AllocMode::CanAlloc>( 42 );  // 这里内部可能会构造 1~多个 ThrowOnCtor
    }
    catch ( std::exception const& ) {
        caught = true;
    }
    EXPECT_TRUE( caught );

    // 队列应为空
    EXPECT_EQ( q.Size(), 0u );

    // 至少能保证：所有“构造成功”的对象都已析构完
    EXPECT_EQ( ThrowOnCtor::liveCount.load(), 1 );
    EXPECT_EQ( ThrowOnCtor::ctorCount.load(), ThrowOnCtor::dtorCount.load() + 1 );
}

// ========== 测试 2: EnqueueBulk 中途异常回滚 ==========

TEST( ExplicitQueueExceptionTest, EnqueueBulk_ExceptionRollback ) {
    constexpr std::size_t POOL_SIZE = 16;
    BlockManager          mgr( POOL_SIZE );
    Queue                 q( 128, mgr );

    constexpr std::size_t    COUNT = kBlockSize * 3 + 10;  // 跨 3+ 块
    std::vector<ThrowOnCtor> src;
    src.reserve( COUNT );
    for ( std::size_t i = 0; i < COUNT; ++i ) {
        src.emplace_back( static_cast<int>( i ) );
    }

    // 在 bulk 构造过程中，第 (COUNT / 2) 个构造时抛异常
    ThrowOnCtor::Reset( /*throwOn=*/static_cast<int>( COUNT / 2 ) );

    bool caught = false;
    try {
        q.EnqueueBulk<AllocMode::CanAlloc>( src.begin(), COUNT );
    }
    catch ( std::exception const& ) {
        caught = true;
    }
    EXPECT_TRUE( caught );

    // bulk 视为整个失败，队列应仍为空
    EXPECT_EQ( q.Size(), 0u );

    // 由队列内部为该 bulk 构造的所有对象必须全部析构
    EXPECT_EQ( ThrowOnCtor::liveCount.load(), 1 );
    EXPECT_EQ( ThrowOnCtor::ctorCount.load(), ThrowOnCtor::dtorCount.load() + 1 );
}

// ========== 测试 3: 异常后再成功 EnqueueBulk ==========

TEST( ExplicitQueueExceptionTest, EnqueueBulk_FailThenSuccess ) {
    constexpr std::size_t POOL_SIZE = 16;
    BlockManager          mgr( POOL_SIZE );
    Queue                 q( 128, mgr );

    constexpr std::size_t    COUNT = kBlockSize * 2 + 5;
    std::vector<ThrowOnCtor> src;
    src.reserve( COUNT );
    for ( std::size_t i = 0; i < COUNT; ++i ) {
        src.emplace_back( static_cast<int>( i ) );
    }

    // 第一次：设定中途抛异常
    ThrowOnCtor::Reset( /*throwOn=*/static_cast<int>( COUNT / 2 ) );
    bool caught = false;
    try {
        q.EnqueueBulk<AllocMode::CanAlloc>( src.begin(), COUNT );
    }
    catch ( ... ) {
        caught = true;
    }
    EXPECT_TRUE( caught );
    EXPECT_EQ( q.Size(), 0u );
    EXPECT_EQ( ThrowOnCtor::liveCount.load(), 1 );
    EXPECT_EQ( ThrowOnCtor::ctorCount.load(), ThrowOnCtor::dtorCount.load() + 1 );

    // 第二次：不抛异常，应该完全成功
    ThrowOnCtor::Reset( /*throwOn=*/-1 );
    EXPECT_NO_THROW( q.EnqueueBulk<AllocMode::CanAlloc>( src.begin(), COUNT ) );
    EXPECT_EQ( q.Size(), COUNT );

    // 全部 Dequeue 出来
    std::vector<ThrowOnCtor>* out = new std::vector<ThrowOnCtor>( COUNT );
    auto                      got = q.DequeueBulk( out->begin(), COUNT );
    EXPECT_EQ( got, COUNT );
    EXPECT_EQ( q.Size(), 0u );

    long long sum = 0;
    for ( auto const& x : *out )
        sum += x.value;
    long long expected = ( long long )( COUNT - 1 ) * ( long long )COUNT / 2;
    EXPECT_EQ( sum, expected );

    delete out;

    // 至此不应有泄漏
    EXPECT_EQ( ThrowOnCtor::liveCount.load(), 0 );
    EXPECT_EQ( ThrowOnCtor::ctorCount.load(), ThrowOnCtor::dtorCount.load() );
}

// ========== 测试 4: DequeueBulk 中赋值异常回滚 ==========

TEST( ExplicitQueueExceptionTest, DequeueBulk_AssignExceptionRollback ) {
    constexpr std::size_t POOL_SIZE = 16;
    BlockManager          mgr( POOL_SIZE );
    Queue                 q( 128, mgr );

    constexpr std::size_t    COUNT = kBlockSize * 2;
    std::vector<ThrowOnCtor> src;
    src.reserve( COUNT );
    for ( std::size_t i = 0; i < COUNT; ++i ) {
        src.emplace_back( static_cast<int>( i ) );
    }

    // enqueue 正常
    ThrowOnCtor::Reset( -1 );
    EXPECT_NO_THROW( q.EnqueueBulk<AllocMode::CanAlloc>( src.begin(), COUNT ) );
    EXPECT_EQ( q.Size(), COUNT );

    auto* out                    = new std::vector<ThrowOnAssign>( COUNT );
    ThrowOnAssign::assignCount   = 0;
    ThrowOnAssign::throwOnAssign = static_cast<int>( COUNT / 2 );  // 第 mid 次赋值抛异常

    bool caught = false;
    try {
        auto got = q.DequeueBulk( out->begin(), COUNT );
        ( void )got;
    }
    catch ( ... ) {
        caught = true;
    }
    EXPECT_TRUE( caught );

    // 你的 DequeueBulk 实现中，catch 分支会销毁所有剩余 Value，并对相应 slot 调 SetSomeEmpty
    // 所以已参与本次 DequeueBulk 的那一批元素应不再留在队列中。
    // 简化起见：这里要求队列为空（如果你希望队列还保留没有被尝试出队的元素，可以改成 EXPECT_LE 等）
    EXPECT_EQ( q.Size(), 0u );

    delete out;

    // 不应有漏掉未 Destroy 的 ThrowOnCtor
    EXPECT_EQ( ThrowOnCtor::liveCount.load(), 0 );
    EXPECT_EQ( ThrowOnCtor::ctorCount.load(), ThrowOnCtor::dtorCount.load() );
}

// ========== （选做）测试 5: 多消费者压力 + 构造异常注入 ==========

TEST( ExplicitQueueExceptionTest, MultiConsumerStress_ThrowOnCtor ) {
    constexpr std::size_t POOL_SIZE = 256;
    BlockManager          mgr( POOL_SIZE );
    Queue                 q( 1024, mgr );

    const unsigned long long N             = 10000;
    const int                NUM_CONSUMERS = 8;

    std::atomic<unsigned long long> total_sum{ 0 };
    std::atomic<unsigned long long> count{ 0 };
    std::atomic<int>                failed{ 0 };
    std::atomic<int>                dequeue_failed{ 0 };

    ThrowOnCtor::Reset( -1 );

    // 生产者：每隔一定频率在构造时注入异常
    std::thread producer( [ & ]() {
        for ( unsigned long long i = 0; i < N; ++i ) {
            // 每 500 次，向 ThrowOnCtor 注入一次异常
            if ( i % 500 == 0 ) {
                ThrowOnCtor::SetThrowOnCtor( 1 );  // 下一次构造第1个就抛
            }
            else {
                ThrowOnCtor::SetThrowOnCtor( -1 );
            }
            try {
                q.Enqueue<AllocMode::CanAlloc>( static_cast<int>( i ) );
            }
            catch ( ... ) {
                failed.fetch_add( 1, std::memory_order_relaxed );
            }
        }
    } );

    std::vector<std::thread> consumers;
    for ( int c = 0; c < NUM_CONSUMERS; ++c ) {
        consumers.emplace_back( [ & ]() {
            unsigned long long local_sum = 0;
            ThrowOnCtor        x;

            while ( count.load( std::memory_order_relaxed ) < N - failed.load() ) {
                if ( q.Dequeue( x ) ) {
                    local_sum += x.value;
                    count.fetch_add( 1, std::memory_order_relaxed );
                }
            }
            total_sum.fetch_add( local_sum, std::memory_order_relaxed );
        } );
    }

    producer.join();
    for ( auto& t : consumers )
        t.join();

    std::cout << "count=" << count.load() << " failed=" << failed.load() << " live=" << ThrowOnCtor::liveCount.load()
              << " ctor=" << ThrowOnCtor::ctorCount.load() << " dtor=" << ThrowOnCtor::dtorCount.load() << std::endl;

    EXPECT_EQ( ThrowOnCtor::liveCount.load(), dequeue_failed.load() + failed.load() );
    EXPECT_EQ( ThrowOnCtor::ctorCount.load(), ThrowOnCtor::dtorCount.load() + dequeue_failed.load() + failed.load() );
}

// test/queue_test.cpp 最后加上：
int main( int argc, char** argv ) {
    ::testing::InitGoogleTest( &argc, argv );

    // 打印所有测试开始和结束（可选）
    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    // 注意：不要删除默认的 listener，否则看不到输出

    return RUN_ALL_TESTS();
}