#include "ConcurrentQueue/ConcurrentQueue.h"
#include <cstdio>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

struct Obj {
    std::uint64_t value;

    Obj() : value( 0 ) {}
    Obj( std::uint64_t value ) : value( value ) {
        if ( value == 4 ) {
            throw std::runtime_error( "4" );
        }
    }
    ~Obj() {}

    Obj( const Obj& other ) : value( other.value ) {}
    Obj( Obj&& other ) : value( std::move( other.value ) ) {}

    Obj& operator=( const Obj& other ) {
        value = other.value;
        return *this;
    }

    Obj& operator=( Obj&& other ) {
        if ( other.value == 70 )
            throw std::runtime_error( "70" );
        value = other.value;
        return *this;
    }
};

// 为了避免一次测试时间太长，这里规模用小一点的数
static constexpr std::size_t kProdThreadsSmall = 10;
static constexpr std::size_t kConsThreadsSmall = 20;
static constexpr std::size_t kItemsPerProducer = 1000;
static constexpr std::size_t PRODUCER_BULK     = 10;
static constexpr std::size_t CONSUMER_BULK     = 5;

// 工具：根据 prodThreads / itemsPerProd 计算理论总和
std::uint64_t CalcExpectedSum( std::size_t prodThreads, std::size_t itemsPerProd ) {
    std::uint64_t sum = 0;
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
            if ( i == 4 )
                continue;

            if ( i == 70 )
                continue;

            sum += i;
        }
    }
    return sum;
}

std::uint64_t CalcExpectedSumBulk( std::size_t prodThreads, std::size_t itemsPerProd ) {
    std::uint64_t sum = 0;
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        for ( std::size_t i = 10; i < itemsPerProd; ++i ) {
            if ( i >= 70 && i < 75 )
                continue;
            sum += i;
        }
    }
    return sum;
}

// ---------------------------------------------------------------------
// 1. 多生产者 / 多消费者，普通 Enqueue / TryDequeue
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, MultiProducerMultiConsumer_NormalEnqDeq ) {
    hakle::ConcurrentQueue<Obj> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd - prodThreads - prodThreads;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers, consumers;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
                int v = static_cast<int>( i );
                try {
                    if ( queue.Enqueue( v ) ) {
                        produced.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
                catch ( ... ) {
                }
            }
        } );
    }

    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            Obj value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                try {
                    if ( queue.TryDequeue( value ) ) {
                        sum.fetch_add( static_cast<std::uint64_t>( value.value ), std::memory_order_relaxed );
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
                catch ( ... ) {
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    EXPECT_EQ( produced.load() - prodThreads, totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 2. 多生产者 / 多消费者，Bulk Enqueue / TryDequeueBulk
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, MultiProducerMultiConsumer_BulkEnqDeq ) {

    hakle::ConcurrentQueue<Obj> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd - prodThreads * PRODUCER_BULK - prodThreads * CONSUMER_BULK;
    const std::uint64_t   expectedSum  = CalcExpectedSumBulk( prodThreads, itemsPerProd );

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers, consumers;
    std::vector<int>         buf( itemsPerProd );
    for ( int i = 0; i < itemsPerProd; ++i )
        buf[ i ] = i;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            int val = 0;

            std::size_t sent = 0;
            while ( sent < itemsPerProd ) {
                try {
                    queue.EnqueueBulk( buf.data() + sent, PRODUCER_BULK );
                    produced.fetch_add( PRODUCER_BULK, std::memory_order_relaxed );
                }
                catch ( ... ) {
                }
                sent += PRODUCER_BULK;
            }
        } );
    }

    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            std::vector<Obj> buf( CONSUMER_BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                try {
                    std::size_t got = queue.TryDequeueBulk( buf.data(), CONSUMER_BULK );
                    if ( got > 0 ) {
                        std::uint64_t localSum = 0;
                        for ( std::size_t i = 0; i < got; ++i ) {
                            localSum += static_cast<std::uint64_t>( buf[ i ].value );
                        }
                        sum.fetch_add( localSum, std::memory_order_relaxed );
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
                catch ( ... ) {
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    EXPECT_EQ( produced.load() - prodThreads * CONSUMER_BULK, totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 3. ProducerToken + 单元素 Enq / TryDequeueFromProducer
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, ProducerToken_Enq_TryDequeueFromProducer ) {
    hakle::ConcurrentQueue<Obj> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd - prodThreads - prodThreads;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    std::vector<hakle::ConcurrentQueue<Obj>::ProducerToken> prodTokens;
    prodTokens.reserve( prodThreads );
    for ( std::size_t i = 0; i < prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::size_t>   nextProducer{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers, consumers;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            auto& token = prodTokens[ p ];
            for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
                try {
                    int v = static_cast<int>( i );
                    queue.EnqueueWithToken( token, v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
                catch ( ... ) {
                }
            }
        } );
    }

    // consumers：轮询不同 producer token
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            Obj value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                try {
                    std::size_t idx = nextProducer.fetch_add( 1, std::memory_order_relaxed ) % prodThreads;
                    if ( queue.TryDequeueFromProducer( prodTokens[ idx ], value ) ) {
                        sum.fetch_add( static_cast<std::uint64_t>( value.value ), std::memory_order_relaxed );
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
                catch ( ... ) {
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    EXPECT_EQ( produced.load() - prodThreads, totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 4. ProducerToken BulkEnq / ConsumerToken BulkDeq
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, ProducerTokenBulkEnq_ConsumerTokenBulkDeq ) {
    hakle::HakleAllocator<int> t;

    hakle::ConcurrentQueue<Obj> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd - prodThreads * PRODUCER_BULK - prodThreads * CONSUMER_BULK;
    const std::uint64_t   expectedSum  = CalcExpectedSumBulk( prodThreads, itemsPerProd );

    std::vector<hakle::ConcurrentQueue<Obj>::ProducerToken> prodTokens;
    prodTokens.reserve( prodThreads );
    for ( std::size_t i = 0; i < prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers, consumers;
    std::vector<int>         buf( itemsPerProd );
    for ( int i = 0; i < itemsPerProd; ++i )
        buf[ i ] = i;
    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            auto& token = prodTokens[ p ];
            int   val   = 0;

            std::size_t sent = 0;
            while ( sent < itemsPerProd ) {
                try {
                    queue.EnqueueBulk( token, buf.data() + sent, PRODUCER_BULK );
                    produced.fetch_add( PRODUCER_BULK, std::memory_order_relaxed );
                }
                catch ( ... ) {
                }
                sent += PRODUCER_BULK;
            }
        } );
    }

    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<Obj>::ConsumerToken token( queue );
            std::vector<Obj>                                    buf( CONSUMER_BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                try {
                    std::size_t got = queue.TryDequeueBulk( token, buf.data(), CONSUMER_BULK );
                    if ( got > 0 ) {
                        std::uint64_t localSum = 0;
                        for ( std::size_t i = 0; i < got; ++i ) {
                            localSum += static_cast<std::uint64_t>( buf[ i ].value );
                        }
                        sum.fetch_add( localSum, std::memory_order_relaxed );
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
                catch ( ... ) {
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    EXPECT_EQ( produced.load() - prodThreads * CONSUMER_BULK, totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 5. 普通 Enq + ConsumerToken Deq（单元素）
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, NormalEnq_ConsumerTokenDeq_SingleElement ) {
    hakle::ConcurrentQueue<Obj> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd - prodThreads - prodThreads;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
                int v = static_cast<int>( i );
                try {
                    queue.Enqueue( v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
                catch ( ... ) {
                }
            }
        } );
    }

    // consumers: 使用 ConsumerToken + TryDequeue(token, value)
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<Obj>::ConsumerToken token( queue );
            Obj                                                 value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                try {
                    if ( queue.TryDequeue( token, value ) ) {
                        sum.fetch_add( static_cast<std::uint64_t>( value.value ), std::memory_order_relaxed );
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
                catch ( ... ) {
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    EXPECT_EQ( produced.load() - prodThreads, totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 6. 普通 BulkEnq + ConsumerToken BulkDeq
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, NormalBulkEnq_ConsumerTokenBulkDeq ) {

    hakle::ConcurrentQueue<Obj> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd - prodThreads * PRODUCER_BULK - prodThreads * CONSUMER_BULK;
    const std::uint64_t   expectedSum  = CalcExpectedSumBulk( prodThreads, itemsPerProd );

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    std::vector<int> buf( itemsPerProd );
    for ( int i = 0; i < itemsPerProd; ++i )
        buf[ i ] = i;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            int val = 0;

            std::size_t sent = 0;
            while ( sent < itemsPerProd ) {
                try {
                    queue.EnqueueBulk( buf.data() + sent, PRODUCER_BULK );
                    produced.fetch_add( PRODUCER_BULK, std::memory_order_relaxed );
                }
                catch ( ... ) {
                }
                sent += PRODUCER_BULK;
            }
        } );
    }

    // consumers: ConsumerToken + TryDequeueBulk(token, buf, BULK)
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<Obj>::ConsumerToken token( queue );
            std::vector<Obj>                                    buf( CONSUMER_BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                try {
                    std::size_t got = queue.TryDequeueBulk( token, buf.data(), CONSUMER_BULK );
                    if ( got > 0 ) {
                        std::uint64_t localSum = 0;
                        for ( std::size_t i = 0; i < got; ++i ) {
                            localSum += static_cast<std::uint64_t>( buf[ i ].value );
                        }
                        sum.fetch_add( localSum, std::memory_order_relaxed );
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
                catch ( ... ) {
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    EXPECT_EQ( produced.load() - prodThreads * CONSUMER_BULK, totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 7. ProducerToken Enq / ConsumerToken Deq（单元素）
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, ProducerTokenEnq_ConsumerTokenDeq_SingleElement ) {
    hakle::ConcurrentQueue<Obj> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd - prodThreads - prodThreads;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    // 为每个生产线程创建一个 ProducerToken
    std::vector<hakle::ConcurrentQueue<Obj>::ProducerToken> prodTokens;
    prodTokens.reserve( prodThreads );
    for ( std::size_t i = 0; i < prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            auto& token = prodTokens[ p ];
            for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
                try {
                    int v = static_cast<int>( i );
                    queue.EnqueueWithToken( token, v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
                catch ( ... ) {
                }
            }
        } );
    }

    // consumers：为每个线程创建一个 ConsumerToken + TryDequeue(token, value)
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<Obj>::ConsumerToken token( queue );
            Obj                                                 value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                try {
                    if ( queue.TryDequeue( token, value ) ) {
                        sum.fetch_add( static_cast<std::uint64_t>( value.value ), std::memory_order_relaxed );
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
                catch ( ... ) {
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    EXPECT_EQ( produced.load() - prodThreads, totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// 还可以继续加：
// - 普通 Enq + ConsumerToken Deq（单元素）
// - 普通 BulkEnq + ConsumerToken BulkDeq
// 等，方式和上面类似。

int main( int argc, char** argv ) {
    ::testing::InitGoogleTest( &argc, argv );

    // 打印所有测试开始和结束（可选）
    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    // 注意：不要删除默认的 listener，否则看不到输出

    return RUN_ALL_TESTS();
}