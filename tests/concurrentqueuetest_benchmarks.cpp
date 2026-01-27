#include "ConcurrentQueue/Block.h"
#include "ConcurrentQueue/ConcurrentQueue.h"
#include "common/allocator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#define SPMC

using Clock = std::chrono::steady_clock;

struct BenchmarkConfig {
    std::size_t prodThreads  = 30;
    std::size_t consThreads  = 30;
    std::size_t itemsPerProd = 1000000;
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
                    queue.EnqueueBulk( buf.data(), n );
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

#ifdef SPMC
// 2. 普通 Enqueue / TryDequeue
Result TestFastQueue_EnqDeq( const BenchmarkConfig& cfg ) {
    hakle::HakleCounterBlockManager<int, 32>                                                 block_manager( 32 );
    hakle::FastQueue<int, 32, hakle::HakleAllocator<int>, hakle::HakleCounterBlock<int, 32>> queue( 32, block_manager );
    const std::size_t                                                                        totalItems = cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        producers.emplace_back( [ & ] {
            for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                int v = static_cast<int>( i );
                queue.Enqueue<hakle::AllocMode::CanAlloc>( v );
                produced.fetch_add( 1, std::memory_order_relaxed );
            }
        } );

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                int value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( queue.Dequeue( value ) ) {
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
    Result r{ "FastQueue_EnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

Result TestSlowQueue_EnqDeq( const BenchmarkConfig& cfg ) {
    hakle::HakleCounterBlockManager<int, 32> block_manager( 32 );
    hakle::SlowQueue<int, 32>                queue( 32, block_manager );
    const std::size_t                        totalItems = cfg.itemsPerProd;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        producers.emplace_back( [ & ] {
            for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
                int v = static_cast<int>( i );
                queue.Enqueue<hakle::AllocMode::CanAlloc>( v );
                produced.fetch_add( 1, std::memory_order_relaxed );
            }
        } );

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                int value;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( queue.Dequeue( value ) ) {
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
    Result r{ "SlowQueue_EnqDeq", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 2. 普通 Enqueue / TryDequeue
Result TestFastQueue_EnqDeqBulk( const BenchmarkConfig& cfg ) {
    hakle::HakleCounterBlockManager<int, 32>                                                 block_manager( 32 );
    hakle::FastQueue<int, 32, hakle::HakleAllocator<int>, hakle::HakleCounterBlock<int, 32>> queue( 32, block_manager );
    const std::size_t                                                                        totalItems = cfg.itemsPerProd * cfg.prodThreads;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    int* values = new int[ cfg.itemsPerProd ];
    for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
        values[ i ] = i;
    }

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        producers.emplace_back( [ & ] {
            for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
                queue.EnqueueBulk<hakle::AllocMode::CanAlloc>( values, cfg.itemsPerProd );
                produced.fetch_add( cfg.itemsPerProd, std::memory_order_relaxed );
            }
        } );

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                int*        values = new int[ cfg.itemsPerProd ];
                std::size_t n;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( ( n = queue.DequeueBulk( values, cfg.itemsPerProd ) ) ) {
                        consumed.fetch_add( n, std::memory_order_relaxed );
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
    Result r{ "FastQueue_EnqDeqBulk", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}

// 2. 普通 Enqueue / TryDequeue
Result TestSlowQueue_EnqDeqBulk( const BenchmarkConfig& cfg ) {
    hakle::HakleCounterBlockManager<int, 32>                                                 block_manager( 32 );
    hakle::SlowQueue<int, 32, hakle::HakleAllocator<int>, hakle::HakleCounterBlock<int, 32>> queue( 32, block_manager );
    const std::size_t                                                                        totalItems = cfg.itemsPerProd * cfg.prodThreads;

    std::atomic<std::size_t> produced{ 0 };
    std::atomic<std::size_t> consumed{ 0 };

    int* values = new int[ cfg.itemsPerProd ];
    for ( std::size_t i = 0; i < cfg.itemsPerProd; ++i ) {
        values[ i ] = i;
    }

    double seconds = MeasureSeconds( [ & ] {
        std::vector<std::thread> producers, consumers;

        producers.emplace_back( [ & ] {
            for ( std::size_t i = 0; i < cfg.prodThreads; ++i ) {
                queue.EnqueueBulk<hakle::AllocMode::CanAlloc>( values, cfg.itemsPerProd );
                produced.fetch_add( cfg.itemsPerProd, std::memory_order_relaxed );
            }
        } );

        for ( std::size_t c = 0; c < cfg.consThreads; ++c ) {
            consumers.emplace_back( [ & ] {
                int*        values = new int[ cfg.itemsPerProd ];
                std::size_t n;
                while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                    if ( ( n = queue.DequeueBulk( values, cfg.itemsPerProd ) ) ) {
                        consumed.fetch_add( n, std::memory_order_relaxed );
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
    Result r{ "SlowQueue_EnqDeqBulk", seconds, thr };
    PrintResult( r, totalItems );
    return r;
}
#endif

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
        std::cout << std::left << std::setw( 36 ) << r.name << " | " << std::setw( barWidth ) << std::string( barLen, '#' ) << " | " << std::fixed << std::setprecision( 0 ) << r.throughput << "\n";
    }
}

int main() {
    BenchmarkConfig cfg;
    cfg.prodThreads  = 10;
    cfg.consThreads  = 10;
    cfg.itemsPerProd = 200000;

    const std::size_t totalItems = cfg.prodThreads * cfg.itemsPerProd;

    std::cout << "Benchmark: prodThreads=" << cfg.prodThreads << " consThreads=" << cfg.consThreads << " itemsPerProd=" << cfg.itemsPerProd << " totalItems=" << totalItems << "\n\n";

    std::vector<Result> results;
    results.reserve( 10 );

    results.push_back( TestMutexQueue( cfg ) );
    results.push_back( TestCQ_NormalEnqDeq( cfg ) );
    results.push_back( TestCQ_BulkEnqDeq( cfg ) );
    results.push_back( TestCQ_ProdToken_EnqDeq( cfg ) );
    results.push_back( TestCQ_ProdToken_BulkEnqDeq( cfg ) );
    results.push_back( TestCQ_ProdTokenEnq_ConsTokenDeq( cfg ) );
    results.push_back( TestCQ_NormalEnq_ConsTokenDeq( cfg ) );
    results.push_back( TestCQ_ProdTokenBulkEnq_ConsTokenBulkDeq( cfg ) );
    results.push_back( TestCQ_NormalBulkEnq_ConsTokenBulkDeq( cfg ) );
#ifdef SPMC
    results.push_back( TestFastQueue_EnqDeq( cfg ) );
    results.push_back( TestSlowQueue_EnqDeq( cfg ) );
    results.push_back( TestFastQueue_EnqDeqBulk( cfg ) );
    results.push_back( TestSlowQueue_EnqDeqBulk( cfg ) );
#endif
    PrintRanking( results );
}