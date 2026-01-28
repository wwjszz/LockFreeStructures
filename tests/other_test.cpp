#include "ConcurrentQueue/ConcurrentQueue.h"
#include "concurrentqueue.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#define USE_MY 1

using Clock = std::chrono::steady_clock;

struct BenchmarkConfig {
    std::size_t prodThreads  = 15;
    std::size_t consThreads  = 30;
    std::size_t itemsPerProd = 1600000;
};

struct Result {
    std::string name;
    double      seconds;
    double      throughput;  // items/s
};

template <typename F>
double MeasureSeconds( F&& f ) {
    auto start = Clock::now();
    f();
    auto end = Clock::now();
    return std::chrono::duration<double>( end - start ).count();
}

// 打印单个测试结果
void PrintResult( const Result& r, std::size_t totalItems ) {
    std::cout << "[" << r.name << "] time=" << r.seconds << "s  throughput=" << r.throughput << " items/s"
              << "  totalItems=" << totalItems << "\n";
}

#ifdef USE_MY
// 2. 普通 Enqueue / TryDequeue
Result TestCQ_NormalEnqDeq( const BenchmarkConfig& cfg ) {
    hakle::ConcurrentQueue<int> queue;
    const std::size_t           totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                    int v = static_cast<int>( p * cfg.itemsPerProd + i );
                    queue.Enqueue( v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
            } );
        }

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                int value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( queue.TryDequeue( value ) ) {
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_NormalEnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 3. 普通 Bulk Enqueue / TryDequeueBulk
Result TestCQ_BulkEnqDeq( const BenchmarkConfig& cfg ) {
    constexpr std::size_t BULK = 256;

    hakle::ConcurrentQueue<int> queue;
    const std::size_t           totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                std::vector<int> buf( BULK );
                std::size_t      sent = 0;
                while ( sent < cfg.itemsPerProd ) {
                    std::size_t n = std::min( BULK, cfg.itemsPerProd - sent );
                    for ( std::size_t i = 0; i < n; ++i ) {
                        buf[ i ] = static_cast<int>( p * cfg.itemsPerProd + sent + i );
                    }
                    if ( !queue.EnqueueBulk( buf.data(), n ) ) {
                        exit( 0 );
                    }
                    produced.fetch_add( n, std::memory_order_relaxed );
                    sent += n;
                }
            } );
        }

        // consumers
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                std::vector<int> buf( BULK );
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t got = queue.TryDequeueBulk( buf.data(), BULK );
                    if ( got > 0 ) {
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_BulkEnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 4. ProducerToken + 单元素 Enq / TryDequeueFromProducer
Result TestCQ_ProdToken_EnqDeq( const BenchmarkConfig& cfg ) {
    hakle::ConcurrentQueue<int> queue;
    const std::size_t           totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
    prodTokens.reserve( cfg.prodThreads );
    for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };
    std::atomic<std::size_t> nextProducerForConsumer{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                auto& token = prodTokens[ p ];
                for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                    int v = static_cast<int>( p * cfg.itemsPerProd + i );
                    queue.EnqueueWithToken( token, v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
            } );
        }

        // consumers：轮询不同 producer token
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                int value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t idx = nextProducerForConsumer.fetch_add( 1, std::memory_order_relaxed ) % cfg.prodThreads;
                    if ( queue.TryDequeueFromProducer( prodTokens[ idx ], value ) ) {
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_ProdToken_EnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 5. ProducerToken + Bulk Enq / TryDequeueBulkFromProducer
Result TestCQ_ProdToken_BulkEnqDeq( const BenchmarkConfig& cfg ) {
    constexpr std::size_t BULK = 256;

    hakle::ConcurrentQueue<int> queue;
    const std::size_t           totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
    prodTokens.reserve( cfg.prodThreads );
    for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };
    std::atomic<std::size_t> nextProducerForConsumer{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                auto&            token = prodTokens[ p ];
                std::vector<int> buf( BULK );
                std::size_t      sent = 0;
                while ( sent < cfg.itemsPerProd ) {
                    std::size_t n = std::min( BULK, cfg.itemsPerProd - sent );
                    for ( std::size_t i = 0; i < n; ++i ) {
                        buf[ i ] = static_cast<int>( p * cfg.itemsPerProd + sent + i );
                    }
                    queue.EnqueueBulk( token, buf.data(), n );
                    produced.fetch_add( n, std::memory_order_relaxed );
                    sent += n;
                }
            } );
        }

        // consumers
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                std::vector<int> buf( BULK );
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t idx = nextProducerForConsumer.fetch_add( 1, std::memory_order_relaxed ) % cfg.prodThreads;
                    std::size_t got = queue.TryDequeueBulkFromProducer( prodTokens[ idx ], buf.data(), BULK );
                    if ( got > 0 ) {
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_ProdToken_BulkEnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 6. ProducerToken Enq / ConsumerToken Deq（单元素）
Result TestCQ_ProdTokenEnq_ConsTokenDeq( const BenchmarkConfig& cfg ) {
    hakle::ConcurrentQueue<int> queue;
    const std::size_t           totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
    prodTokens.reserve( cfg.prodThreads );
    for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                auto& token = prodTokens[ p ];
                for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                    int v = static_cast<int>( p * cfg.itemsPerProd + i );
                    queue.EnqueueWithToken( token, v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
            } );
        }

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
                int                                                 value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( queue.TryDequeue( token, value ) ) {
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_ProdTokenEnq_ConsTokenDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 7. 普通 Enq / ConsumerToken Deq（单元素）
Result TestCQ_NormalEnq_ConsTokenDeq( const BenchmarkConfig& cfg ) {
    hakle::ConcurrentQueue<int> queue;
    const std::size_t           totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                    int v = static_cast<int>( p * cfg.itemsPerProd + i );
                    queue.Enqueue( v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
            } );
        }

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
                int                                                 value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( queue.TryDequeue( token, value ) ) {
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_NormalEnq_ConsTokenDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 8. ProducerToken BulkEnq / ConsumerToken BulkDeq
Result TestCQ_ProdTokenBulkEnq_ConsTokenBulkDeq( const BenchmarkConfig& cfg ) {
    constexpr std::size_t BULK = 256;

    hakle::ConcurrentQueue<int> queue;
    const std::size_t           totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
    prodTokens.reserve( cfg.prodThreads );
    for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                auto&            token = prodTokens[ p ];
                std::vector<int> buf( BULK );
                std::size_t      sent = 0;
                while ( sent < cfg.itemsPerProd ) {
                    std::size_t n = std::min( BULK, cfg.itemsPerProd - sent );
                    for ( std::size_t i = 0; i < n; ++i ) {
                        buf[ i ] = static_cast<int>( p * cfg.itemsPerProd + sent + i );
                    }
                    queue.EnqueueBulk( token, buf.data(), n );
                    produced.fetch_add( n, std::memory_order_relaxed );
                    sent += n;
                }
            } );
        }

        // consumers
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
                std::vector<int>                                    buf( BULK );
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t got = queue.TryDequeueBulk( token, buf.data(), BULK );
                    if ( got > 0 ) {
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_ProdTokenBulkEnq_ConsTokenBulkDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 9. 普通 BulkEnq / ConsumerToken BulkDeq
Result TestCQ_NormalBulkEnq_ConsTokenBulkDeq( const BenchmarkConfig& cfg ) {
    constexpr std::size_t BULK = 256;

    hakle::ConcurrentQueue<int> queue;
    const std::size_t           totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                std::vector<int> buf( BULK );
                std::size_t      sent = 0;
                while ( sent < cfg.itemsPerProd ) {
                    std::size_t n = std::min( BULK, cfg.itemsPerProd - sent );
                    for ( std::size_t i = 0; i < n; ++i ) {
                        buf[ i ] = static_cast<int>( p * cfg.itemsPerProd + sent + i );
                    }
                    queue.EnqueueBulk( buf.data(), n );
                    produced.fetch_add( n, std::memory_order_relaxed );
                    sent += n;
                }
            } );
        }

        // consumers
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
                std::vector<int>                                    buf( BULK );
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t got = queue.TryDequeueBulk( token, buf.data(), BULK );
                    if ( got > 0 ) {
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_NormalBulkEnq_ConsTokenBulkDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

#endif

// 1. 基准：std::queue + std::mutex
Result TestMutexQueue( const BenchmarkConfig& cfg ) {
    std::mutex              mtx;
    std::condition_variable cv;
    std::queue<int>         q;

    const std::size_t        totalItems = cfg.prodThreads * cfg.itemsPerProd;
    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };
    bool                     done = false;

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                    int v = static_cast<int>( p * cfg.itemsPerProd + i );
                    {
                        std::lock_guard<std::mutex> lock( mtx );
                        q.push( v );
                    }
                    produced.fetch_add( 1, std::memory_order_relaxed );
                    cv.notify_one();
                }
            } );
        }

        // consumers
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                while ( true ) {
                    int v;
                    {
                        std::unique_lock<std::mutex> lock( mtx );
                        cv.wait( lock, [ & ] { return !q.empty() || done; } );
                        if ( q.empty() ) {  // done 且队列空
                            break;
                        }
                        v = q.front();
                        q.pop();
                    }
                    ( void )v;
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        {
            std::lock_guard<std::mutex> lock( mtx );
            done = true;
        }
        cv.notify_all();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "MutexQueue", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 2. 普通 enqueue / try_dequeue
Result TestCQ_MOODY_NormalEnqDeq( const BenchmarkConfig& cfg ) {
    moodycamel::ConcurrentQueue<int> queue;
    const std::size_t                totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                    int v = static_cast<int>( p * cfg.itemsPerProd + i );
                    queue.enqueue( v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
            } );
        }

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                int value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( queue.try_dequeue( value ) ) {
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_MOODY_NormalEnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 3. 普通 Bulk enqueue / try_dequeue_bulk
Result TestCQ_MOODY_BulkEnqDeq( const BenchmarkConfig& cfg ) {
    constexpr std::size_t BULK = 256;

    moodycamel::ConcurrentQueue<int> queue;
    const std::size_t                totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                std::vector<int> buf( BULK );
                std::size_t      sent = 0;
                while ( sent < cfg.itemsPerProd ) {
                    std::size_t n = std::min( BULK, cfg.itemsPerProd - sent );
                    for ( std::size_t i = 0; i < n; ++i ) {
                        buf[ i ] = static_cast<int>( p * cfg.itemsPerProd + sent + i );
                    }
                    if ( !queue.enqueue_bulk( buf.data(), n ) ) {
                        exit( 0 );
                        // printf( "wetd" );
                    }
                    produced.fetch_add( n, std::memory_order_relaxed );
                    sent += n;
                }
            } );
        }

        // consumers
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                std::vector<int> buf( BULK );
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t got = queue.try_dequeue_bulk( buf.data(), BULK );
                    if ( got > 0 ) {
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_MOODY_BulkEnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 4. ProducerToken + 单元素 Enq / try_dequeue_from_producer
Result TestCQ_MOODY_ProdToken_EnqDeq( const BenchmarkConfig& cfg ) {
    moodycamel::ConcurrentQueue<int> queue;
    const std::size_t                totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::vector<moodycamel::ProducerToken> prodTokens;
    prodTokens.reserve( cfg.prodThreads );
    for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
        prodTokens.emplace_back( queue );
    }

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };
    std::atomic<std::size_t> nextProducerForConsumer{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                auto& token = prodTokens[ p ];
                for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                    int v = static_cast<int>( p * cfg.itemsPerProd + i );
                    queue.enqueue( token, v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
            } );
        }

        // consumers：轮询不同 producer token
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                int value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t idx = nextProducerForConsumer.fetch_add( 1, std::memory_order_relaxed ) % cfg.prodThreads;
                    if ( queue.try_dequeue_from_producer( prodTokens[ idx ], value ) ) {
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_MOODY_ProdToken_EnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 5. ProducerToken + Bulk Enq / try_dequeue_bulk_from_producer
Result TestCQ_MOODY_ProdToken_BulkEnqDeq( const BenchmarkConfig& cfg ) {
    constexpr std::size_t BULK = 256;

    moodycamel::ConcurrentQueue<int> queue;
    const std::size_t                totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::vector<moodycamel::ProducerToken> prodTokens;
    prodTokens.reserve( cfg.prodThreads );
    for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
        prodTokens.emplace_back( queue );
    }

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };
    std::atomic<std::size_t> nextProducerForConsumer{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                auto&            token = prodTokens[ p ];
                std::vector<int> buf( BULK );
                std::size_t      sent = 0;
                while ( sent < cfg.itemsPerProd ) {
                    std::size_t n = std::min( BULK, cfg.itemsPerProd - sent );
                    for ( std::size_t i = 0; i < n; ++i ) {
                        buf[ i ] = static_cast<int>( p * cfg.itemsPerProd + sent + i );
                    }
                    queue.enqueue_bulk( token, buf.data(), n );
                    produced.fetch_add( n, std::memory_order_relaxed );
                    sent += n;
                }
            } );
        }

        // consumers
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                std::vector<int> buf( BULK );
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t idx = nextProducerForConsumer.fetch_add( 1, std::memory_order_relaxed ) % cfg.prodThreads;
                    std::size_t got = queue.try_dequeue_bulk_from_producer( prodTokens[ idx ], buf.data(), BULK );
                    if ( got > 0 ) {
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_MOODY_ProdToken_BulkEnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 6. ProducerToken Enq / ConsumerToken Deq（单元素）
Result TestCQ_MOODY_ProdTokenEnq_ConsTokenDeq( const BenchmarkConfig& cfg ) {
    moodycamel::ConcurrentQueue<int> queue;
    const std::size_t                totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::vector<moodycamel::ProducerToken> prodTokens;
    prodTokens.reserve( cfg.prodThreads );
    for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
        prodTokens.emplace_back( queue );
    }

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                auto& token = prodTokens[ p ];
                for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                    int v = static_cast<int>( p * cfg.itemsPerProd + i );
                    queue.enqueue( token, v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
            } );
        }

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                moodycamel::ConsumerToken token( queue );
                int                       value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( queue.try_dequeue( token, value ) ) {
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_MOODY_ProdTokenEnq_ConsTokenDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 7. 普通 Enq / ConsumerToken Deq（单元素）
Result TestCQ_MOODY_NormalEnq_ConsTokenDeq( const BenchmarkConfig& cfg ) {
    moodycamel::ConcurrentQueue<int> queue;
    const std::size_t                totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                    int v = static_cast<int>( p * cfg.itemsPerProd + i );
                    queue.enqueue( v );
                    produced.fetch_add( 1, std::memory_order_relaxed );
                }
            } );
        }

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                moodycamel::ConsumerToken token( queue );
                int                       value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( queue.try_dequeue( token, value ) ) {
                        consumed.fetch_add( 1, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_MOODY_NormalEnq_ConsTokenDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 8. ProducerToken BulkEnq / ConsumerToken BulkDeq
Result TestCQ_MOODY_ProdTokenBulkEnq_ConsTokenBulkDeq( const BenchmarkConfig& cfg ) {
    constexpr std::size_t BULK = 256;

    moodycamel::ConcurrentQueue<int> queue;
    const std::size_t                totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::vector<moodycamel::ProducerToken> prodTokens;
    prodTokens.reserve( cfg.prodThreads );
    for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
        prodTokens.emplace_back( queue );
    }

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                auto&            token = prodTokens[ p ];
                std::vector<int> buf( BULK );
                std::size_t      sent = 0;
                while ( sent < cfg.itemsPerProd ) {
                    std::size_t n = std::min( BULK, cfg.itemsPerProd - sent );
                    for ( std::size_t i = 0; i < n; ++i ) {
                        buf[ i ] = static_cast<int>( p * cfg.itemsPerProd + sent + i );
                    }
                    queue.enqueue_bulk( token, buf.data(), n );
                    produced.fetch_add( n, std::memory_order_relaxed );
                    sent += n;
                }
            } );
        }

        // consumers
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                moodycamel::ConsumerToken token( queue );
                std::vector<int>          buf( BULK );
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t got = queue.try_dequeue_bulk( token, buf.data(), BULK );
                    if ( got > 0 ) {
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_MOODY_ProdTokenBulkEnq_ConsTokenBulkDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 9. 普通 BulkEnq / ConsumerToken BulkDeq
Result TestCQ_MOODY_NormalBulkEnq_ConsTokenBulkDeq( const BenchmarkConfig& cfg ) {
    constexpr std::size_t BULK = 256;

    moodycamel::ConcurrentQueue<int> queue;
    const std::size_t                totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        // producers
        for ( std::size_t p = 0; p < cfg.prodThreads; ++p ) {
            producers.emplace_back( [ &, p ] {
                std::vector<int> buf( BULK );
                std::size_t      sent = 0;
                while ( sent < cfg.itemsPerProd ) {
                    std::size_t n = std::min( BULK, cfg.itemsPerProd - sent );
                    for ( std::size_t i = 0; i < n; ++i ) {
                        buf[ i ] = static_cast<int>( p * cfg.itemsPerProd + sent + i );
                    }
                    queue.enqueue_bulk( buf.data(), n );
                    produced.fetch_add( n, std::memory_order_relaxed );
                    sent += n;
                }
            } );
        }

        // consumers
        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                moodycamel::ConsumerToken token( queue );
                std::vector<int>          buf( BULK );
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    std::size_t got = queue.try_dequeue_bulk( token, buf.data(), BULK );
                    if ( got > 0 ) {
                        consumed.fetch_add( got, std::memory_order_relaxed );
                    }
                }
            } );
        }

        for ( auto& t : producers )
            t.join();
        for ( auto& t : consumers )
            t.join();
    } );

    double thr = ( double )totalItems / seconds;
    Result r{ "CQ_MOODY_NormalBulkEnq_ConsTokenBulkDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 打印“排行榜”和 ASCII 柱状图
void PrintRanking( const std::vector<Result>& results ) {
    if ( results.empty() )
        return;

    std::vector<Result> sorted = results;
    std::sort( sorted.begin(), sorted.end(), []( const Result& a, const Result& b ) {
        return a.throughput > b.throughput;  // 降序
    } );

    double maxThr   = sorted.front().throughput;
    int    barWidth = 50;  // 最大柱宽

    std::cout << "\n=== Ranking by throughput (items/s) ===\n";
    for ( const auto& r : sorted ) {
        double ratio  = r.throughput / maxThr;
        int    barLen = static_cast<int>( ratio * barWidth );
        std::cout << std::left << std::setw( 45 ) << r.name << " | " << std::setw( barWidth ) << std::string( barLen, '#' ) << " | " << std::fixed << std::setprecision( 0 ) << r.throughput << "\n";
    }
}

int main() {
    BenchmarkConfig cfg;
    cfg.prodThreads  = 20;
    cfg.consThreads  = 20;
    cfg.itemsPerProd = 800000;

    const std::size_t totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::cout << "Benchmark: prodThreads=" << cfg.prodThreads << " consThreads=" << cfg.consThreads << " itemsPerProd=" << cfg.itemsPerProd << " totalItems=" << totalItems << "\n\n";

    std::vector<Result> results;
    results.reserve( 10 );

#ifdef USE_MY
    results.push_back( TestCQ_NormalEnqDeq( cfg ) );
    results.push_back( TestCQ_BulkEnqDeq( cfg ) );
    results.push_back( TestCQ_ProdToken_EnqDeq( cfg ) );
    results.push_back( TestCQ_ProdToken_BulkEnqDeq( cfg ) );
    results.push_back( TestCQ_ProdTokenEnq_ConsTokenDeq( cfg ) );
    results.push_back( TestCQ_NormalEnq_ConsTokenDeq( cfg ) );
    results.push_back( TestCQ_ProdTokenBulkEnq_ConsTokenBulkDeq( cfg ) );
    results.push_back( TestCQ_NormalBulkEnq_ConsTokenBulkDeq( cfg ) );
#endif

    // results.push_back( TestMutexQueue( cfg ) );
    results.push_back( TestCQ_MOODY_NormalEnqDeq( cfg ) );
    results.push_back( TestCQ_MOODY_BulkEnqDeq( cfg ) );
    results.push_back( TestCQ_MOODY_ProdToken_EnqDeq( cfg ) );
    results.push_back( TestCQ_MOODY_ProdToken_BulkEnqDeq( cfg ) );
    results.push_back( TestCQ_MOODY_ProdTokenEnq_ConsTokenDeq( cfg ) );
    results.push_back( TestCQ_MOODY_NormalEnq_ConsTokenDeq( cfg ) );
    results.push_back( TestCQ_MOODY_ProdTokenBulkEnq_ConsTokenBulkDeq( cfg ) );
    results.push_back( TestCQ_MOODY_NormalBulkEnq_ConsTokenBulkDeq( cfg ) );
    PrintRanking( results );
}