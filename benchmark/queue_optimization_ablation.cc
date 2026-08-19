// Standalone ablation benchmark using the same implicit, token, and token-bulk
// workloads as benchmark/queue_benchmark.cc.
//
// Each executable is compiled with exactly one HAKLE_ABLATION_VARIANT:
//   0: original queue
//   1: token-less consumer cache only
//   2: direct ProducerToken dispatch + packed word flags
//   3: all optimizations
//   4: moodycamel

#ifndef HAKLE_ABLATION_VARIANT
#error "HAKLE_ABLATION_VARIANT must be defined"
#endif

#include "ConcurrentQueue/ConcurrentQueue.h"
#include "moodycamel/concurrentqueue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <latch>
#include <numeric>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using Clock          = std::chrono::steady_clock;
using BenchAllocator = hakle::HakleAllocator<int>;

constexpr std::size_t BlockSize = hakle::ConcurrentQueue<int>::BlockSize;
constexpr std::size_t BulkSize  = 64;
static_assert( BlockSize == moodycamel::ConcurrentQueue<int>::BLOCK_SIZE );

#if HAKLE_ABLATION_VARIANT == 2 || HAKLE_ABLATION_VARIANT == 3
struct WordFlagsTraits : hakle::ConcurrentQueueDefaultTraits<int, BenchAllocator> {
    using Base = hakle::ConcurrentQueueDefaultTraits<int, BenchAllocator>;

    static constexpr bool UseImplicitConsumerCache = HAKLE_ABLATION_VARIANT == 3;

    using ExplicitBlockType     = hakle::HakleWordFlagsBlock<int, Base::BlockSize>;
    using ExplicitAllocatorType = typename hakle::HakeAllocatorTraits<BenchAllocator>::template RebindAlloc<ExplicitBlockType>;
    using ImplicitBlockType     = typename Base::ImplicitBlockType;
    using ImplicitAllocatorType = typename Base::ImplicitAllocatorType;

    using ExplicitBlockManagerType = hakle::HakleBlockManager<ExplicitBlockType, ExplicitAllocatorType>;
    using ImplicitBlockManagerType = typename Base::ImplicitBlockManagerType;

    static ExplicitBlockManagerType MakeDefaultExplicitBlockManager( const ExplicitAllocatorType& Allocator ) { return ExplicitBlockManagerType( Base::InitialBlockPoolSize, Allocator ); }
    static ImplicitBlockManagerType MakeDefaultImplicitBlockManager( const ImplicitAllocatorType& Allocator ) { return ImplicitBlockManagerType( Base::InitialBlockPoolSize, Allocator ); }
    static ExplicitBlockManagerType MakeExplicitBlockManager( const ExplicitAllocatorType& Allocator, std::size_t BlockPoolSize ) { return ExplicitBlockManagerType( BlockPoolSize, Allocator ); }
    static ImplicitBlockManagerType MakeImplicitBlockManager( const ImplicitAllocatorType& Allocator, std::size_t BlockPoolSize ) { return ImplicitBlockManagerType( BlockPoolSize, Allocator ); }
};

using SelectedHakleQueue = hakle::ConcurrentQueue<int, BenchAllocator, WordFlagsTraits>;
#else
using SelectedHakleQueue = hakle::ConcurrentQueue<int>;
#endif

template <class QueueType>
class HakleImplicitQueue {
public:
    HakleImplicitQueue( std::size_t InitialBlockCount, std::size_t, std::size_t ) : Queue( std::piecewise_construct, std::make_tuple( std::size_t{ 0 } ), std::make_tuple( InitialBlockCount ), BenchAllocator{} ) {}

    bool        Enqueue( std::size_t, int Value ) { return Queue.Enqueue( Value ); }
    bool        TryDequeue( std::size_t, int& Value ) { return Queue.TryDequeue( Value ); }
    bool        EnqueueBulk( std::size_t, const int* Values, std::size_t Count ) { return Queue.EnqueueBulk( Values, Count ); }
    std::size_t TryDequeueBulk( std::size_t, int* Values, std::size_t Count ) { return Queue.TryDequeueBulk( Values, Count ); }

private:
    QueueType Queue;
};

template <class QueueType>
class HakleTokenQueue {
public:
    HakleTokenQueue( std::size_t InitialBlockCount, std::size_t ProducerCount, std::size_t ConsumerCount ) : Queue( std::piecewise_construct, std::make_tuple( InitialBlockCount ), std::make_tuple( std::size_t{ 0 } ), BenchAllocator{} ) {
        Producers.reserve( ProducerCount );
        for ( std::size_t Index = 0; Index < ProducerCount; ++Index ) {
            Producers.emplace_back( Queue.GetProducerToken() );
        }
        Consumers.reserve( ConsumerCount );
        for ( std::size_t Index = 0; Index < ConsumerCount; ++Index ) {
            Consumers.emplace_back( Queue.GetConsumerToken() );
        }
    }

    bool        Enqueue( std::size_t Producer, int Value ) { return Queue.EnqueueWithToken( Producers[ Producer ], Value ); }
    bool        TryDequeue( std::size_t Consumer, int& Value ) { return Queue.TryDequeue( Consumers[ Consumer ], Value ); }
    bool        EnqueueBulk( std::size_t Producer, const int* Values, std::size_t Count ) { return Queue.EnqueueBulk( Producers[ Producer ], Values, Count ); }
    std::size_t TryDequeueBulk( std::size_t Consumer, int* Values, std::size_t Count ) { return Queue.TryDequeueBulk( Consumers[ Consumer ], Values, Count ); }

private:
    QueueType                                      Queue;
    std::vector<typename QueueType::ProducerToken> Producers;
    std::vector<typename QueueType::ConsumerToken> Consumers;
};

class MoodycamelImplicitQueue {
public:
    MoodycamelImplicitQueue( std::size_t InitialBlockCount, std::size_t, std::size_t ) : Queue( InitialBlockCount * BlockSize ) {}
    bool        Enqueue( std::size_t, int Value ) { return Queue.enqueue( Value ); }
    bool        TryDequeue( std::size_t, int& Value ) { return Queue.try_dequeue( Value ); }
    bool        EnqueueBulk( std::size_t, const int* Values, std::size_t Count ) { return Queue.enqueue_bulk( Values, Count ); }
    std::size_t TryDequeueBulk( std::size_t, int* Values, std::size_t Count ) { return Queue.try_dequeue_bulk( Values, Count ); }

private:
    moodycamel::ConcurrentQueue<int> Queue;
};

class MoodycamelTokenQueue {
public:
    MoodycamelTokenQueue( std::size_t InitialBlockCount, std::size_t ProducerCount, std::size_t ConsumerCount ) : Queue( InitialBlockCount * BlockSize ) {
        Producers.reserve( ProducerCount );
        for ( std::size_t Index = 0; Index < ProducerCount; ++Index ) {
            Producers.emplace_back( Queue );
        }
        Consumers.reserve( ConsumerCount );
        for ( std::size_t Index = 0; Index < ConsumerCount; ++Index ) {
            Consumers.emplace_back( Queue );
        }
    }

    bool        Enqueue( std::size_t Producer, int Value ) { return Queue.enqueue( Producers[ Producer ], Value ); }
    bool        TryDequeue( std::size_t Consumer, int& Value ) { return Queue.try_dequeue( Consumers[ Consumer ], Value ); }
    bool        EnqueueBulk( std::size_t Producer, const int* Values, std::size_t Count ) { return Queue.enqueue_bulk( Producers[ Producer ], Values, Count ); }
    std::size_t TryDequeueBulk( std::size_t Consumer, int* Values, std::size_t Count ) { return Queue.try_dequeue_bulk( Consumers[ Consumer ], Values, Count ); }

private:
    moodycamel::ConcurrentQueue<int>       Queue;
    std::vector<moodycamel::ProducerToken> Producers;
    std::vector<moodycamel::ConsumerToken> Consumers;
};

#if HAKLE_ABLATION_VARIANT == 4
using ImplicitQueue = MoodycamelImplicitQueue;
using TokenQueue    = MoodycamelTokenQueue;
#else
using ImplicitQueue = HakleImplicitQueue<SelectedHakleQueue>;
using TokenQueue    = HakleTokenQueue<SelectedHakleQueue>;
#endif

struct Result {
    double      Nanoseconds;
    std::size_t Items;
};

template <class QueueType>
Result RunSingleOnce( std::size_t ProducerCount, std::size_t ConsumerCount, std::size_t ItemsPerProducer ) {
    const std::size_t          Total             = ProducerCount * ItemsPerProducer;
    const std::size_t          BlocksPerProducer = ( ItemsPerProducer + BlockSize - 1 ) / BlockSize;
    QueueType                  Queue( ProducerCount * BlocksPerProducer, ProducerCount, ConsumerCount );
    std::atomic<std::size_t>   ProducersRemaining{ ProducerCount };
    std::vector<std::size_t>   Consumed( ConsumerCount );
    std::vector<std::uint64_t> Checksums( ConsumerCount );
    std::latch                 Ready( ProducerCount + ConsumerCount );
    std::latch                 Start( 1 );

    std::vector<std::thread> Consumers;
    Consumers.reserve( ConsumerCount );
    for ( std::size_t Consumer = 0; Consumer < ConsumerCount; ++Consumer ) {
        Consumers.emplace_back( [ &, Consumer ] {
            Ready.count_down();
            Start.wait();
            std::size_t   LocalCount = 0;
            std::uint64_t LocalSum   = 0;
            for ( ;; ) {
                int  Value    = 0;
                bool Dequeued = Queue.TryDequeue( Consumer, Value );
                if ( !Dequeued ) {
                    if ( ProducersRemaining.load( std::memory_order_acquire ) == 0 ) {
                        Dequeued = Queue.TryDequeue( Consumer, Value );
                        if ( !Dequeued ) {
                            break;
                        }
                    }
                    else {
                        std::this_thread::yield();
                        continue;
                    }
                }
                ++LocalCount;
                LocalSum += static_cast<std::uint64_t>( Value );
            }
            Consumed[ Consumer ]  = LocalCount;
            Checksums[ Consumer ] = LocalSum;
        } );
    }

    std::vector<std::thread> Producers;
    Producers.reserve( ProducerCount );
    for ( std::size_t Producer = 0; Producer < ProducerCount; ++Producer ) {
        Producers.emplace_back( [ &, Producer ] {
            Ready.count_down();
            Start.wait();
            for ( std::size_t Sequence = 0; Sequence < ItemsPerProducer; ++Sequence ) {
                const int Value = static_cast<int>( Producer * ItemsPerProducer + Sequence );
                while ( !Queue.Enqueue( Producer, Value ) ) {
                    std::this_thread::yield();
                }
            }
            ProducersRemaining.fetch_sub( 1, std::memory_order_release );
        } );
    }

    Ready.wait();
    const auto Begin = Clock::now();
    Start.count_down();
    for ( std::thread& Producer : Producers ) {
        Producer.join();
    }
    for ( std::thread& Consumer : Consumers ) {
        Consumer.join();
    }

    std::size_t   FinalCount = 0;
    std::uint64_t FinalSum   = 0;
    int           FinalValue = 0;
    while ( Queue.TryDequeue( 0, FinalValue ) ) {
        ++FinalCount;
        FinalSum += static_cast<std::uint64_t>( FinalValue );
    }
    const auto End = Clock::now();

    const std::size_t   ActualCount = FinalCount + std::accumulate( Consumed.begin(), Consumed.end(), std::size_t{ 0 } );
    const std::uint64_t ActualSum   = FinalSum + std::accumulate( Checksums.begin(), Checksums.end(), std::uint64_t{ 0 } );
    const std::uint64_t ExpectedSum = static_cast<std::uint64_t>( Total ) * ( Total - 1 ) / 2;
    if ( ActualCount != Total || ActualSum != ExpectedSum ) {
        std::cerr << "verification failed: count=" << ActualCount << "/" << Total << ", sum=" << ActualSum << "/" << ExpectedSum << '\n';
        std::abort();
    }
    return { std::chrono::duration<double, std::nano>( End - Begin ).count(), Total };
}

template <class QueueType>
Result RunBulkOnce( std::size_t ProducerCount, std::size_t ConsumerCount, std::size_t ItemsPerProducer ) {
    const std::size_t          Total             = ProducerCount * ItemsPerProducer;
    const std::size_t          BlocksPerProducer = ( ItemsPerProducer + BlockSize - 1 ) / BlockSize;
    QueueType                  Queue( ProducerCount * BlocksPerProducer, ProducerCount, ConsumerCount );
    std::atomic<std::size_t>   ProducersRemaining{ ProducerCount };
    std::vector<std::size_t>   Consumed( ConsumerCount );
    std::vector<std::uint64_t> Checksums( ConsumerCount );
    std::latch                 Ready( ProducerCount + ConsumerCount );
    std::latch                 Start( 1 );

    std::vector<std::thread> Consumers;
    Consumers.reserve( ConsumerCount );
    for ( std::size_t Consumer = 0; Consumer < ConsumerCount; ++Consumer ) {
        Consumers.emplace_back( [ &, Consumer ] {
            std::array<int, BulkSize> Values{};
            Ready.count_down();
            Start.wait();
            std::size_t   LocalCount = 0;
            std::uint64_t LocalSum   = 0;
            for ( ;; ) {
                std::size_t Count = Queue.TryDequeueBulk( Consumer, Values.data(), Values.size() );
                if ( Count == 0 ) {
                    if ( ProducersRemaining.load( std::memory_order_acquire ) == 0 ) {
                        Count = Queue.TryDequeueBulk( Consumer, Values.data(), Values.size() );
                        if ( Count == 0 ) {
                            break;
                        }
                    }
                    else {
                        std::this_thread::yield();
                        continue;
                    }
                }
                LocalCount += Count;
                for ( std::size_t Index = 0; Index < Count; ++Index ) {
                    LocalSum += static_cast<std::uint64_t>( Values[ Index ] );
                }
            }
            Consumed[ Consumer ]  = LocalCount;
            Checksums[ Consumer ] = LocalSum;
        } );
    }

    std::vector<std::thread> Producers;
    Producers.reserve( ProducerCount );
    for ( std::size_t Producer = 0; Producer < ProducerCount; ++Producer ) {
        Producers.emplace_back( [ &, Producer ] {
            std::array<int, BulkSize> Values{};
            Ready.count_down();
            Start.wait();
            std::size_t Sequence = 0;
            while ( Sequence != ItemsPerProducer ) {
                const std::size_t Count = std::min( BulkSize, ItemsPerProducer - Sequence );
                for ( std::size_t Index = 0; Index < Count; ++Index ) {
                    Values[ Index ] = static_cast<int>( Producer * ItemsPerProducer + Sequence + Index );
                }
                while ( !Queue.EnqueueBulk( Producer, Values.data(), Count ) ) {
                    std::this_thread::yield();
                }
                Sequence += Count;
            }
            ProducersRemaining.fetch_sub( 1, std::memory_order_release );
        } );
    }

    Ready.wait();
    const auto Begin = Clock::now();
    Start.count_down();
    for ( std::thread& Producer : Producers ) {
        Producer.join();
    }
    for ( std::thread& Consumer : Consumers ) {
        Consumer.join();
    }

    std::size_t               FinalCount = 0;
    std::uint64_t             FinalSum   = 0;
    std::array<int, BulkSize> FinalValues{};
    for ( ;; ) {
        const std::size_t Count = Queue.TryDequeueBulk( 0, FinalValues.data(), FinalValues.size() );
        if ( Count == 0 ) {
            break;
        }
        FinalCount += Count;
        for ( std::size_t Index = 0; Index < Count; ++Index ) {
            FinalSum += static_cast<std::uint64_t>( FinalValues[ Index ] );
        }
    }
    const auto End = Clock::now();

    const std::size_t   ActualCount = FinalCount + std::accumulate( Consumed.begin(), Consumed.end(), std::size_t{ 0 } );
    const std::uint64_t ActualSum   = FinalSum + std::accumulate( Checksums.begin(), Checksums.end(), std::uint64_t{ 0 } );
    const std::uint64_t ExpectedSum = static_cast<std::uint64_t>( Total ) * ( Total - 1 ) / 2;
    if ( ActualCount != Total || ActualSum != ExpectedSum ) {
        std::cerr << "verification failed: count=" << ActualCount << "/" << Total << ", sum=" << ActualSum << "/" << ExpectedSum << '\n';
        std::abort();
    }
    return { std::chrono::duration<double, std::nano>( End - Begin ).count(), Total };
}

Result RunOnce( std::string_view Path, std::size_t ProducerCount, std::size_t ConsumerCount, std::size_t ItemsPerProducer ) {
    if ( Path == "implicit" ) {
        return RunSingleOnce<ImplicitQueue>( ProducerCount, ConsumerCount, ItemsPerProducer );
    }
    if ( Path == "implicit_bulk" ) {
        return RunBulkOnce<ImplicitQueue>( ProducerCount, ConsumerCount, ItemsPerProducer );
    }
    if ( Path == "token" ) {
        return RunSingleOnce<TokenQueue>( ProducerCount, ConsumerCount, ItemsPerProducer );
    }
    if ( Path == "bulk" ) {
        return RunBulkOnce<TokenQueue>( ProducerCount, ConsumerCount, ItemsPerProducer );
    }
    std::cerr << "path must be implicit, implicit_bulk, token, or bulk\n";
    std::exit( 2 );
}

constexpr std::string_view VariantName() {
#if HAKLE_ABLATION_VARIANT == 0
    return "original";
#elif HAKLE_ABLATION_VARIANT == 1
    return "consumer_cache";
#elif HAKLE_ABLATION_VARIANT == 2
    return "producer_path";
#elif HAKLE_ABLATION_VARIANT == 3
    return "all_optimizations";
#elif HAKLE_ABLATION_VARIANT == 4
    return "moodycamel";
#else
#error "Unsupported HAKLE_ABLATION_VARIANT"
#endif
}

}  // namespace

int main( int argc, char** argv ) {
    if ( argc != 6 ) {
        std::cerr << "usage: queue_optimization_ablation PATH PRODUCERS CONSUMERS ITEMS_PER_PRODUCER MIN_SECONDS\n";
        return 2;
    }

    const std::string_view Path             = argv[ 1 ];
    const std::size_t      ProducerCount    = std::strtoull( argv[ 2 ], nullptr, 10 );
    const std::size_t      ConsumerCount    = std::strtoull( argv[ 3 ], nullptr, 10 );
    const std::size_t      ItemsPerProducer = std::strtoull( argv[ 4 ], nullptr, 10 );
    const double           MinimumSeconds   = std::strtod( argv[ 5 ], nullptr );
    if ( ProducerCount == 0 || ConsumerCount == 0 || ItemsPerProducer == 0 || MinimumSeconds <= 0.0 ) {
        std::cerr << "numeric arguments must be positive\n";
        return 2;
    }

    static_cast<void>( RunOnce( Path, ProducerCount, ConsumerCount, ItemsPerProducer ) );

    double      TotalNanoseconds = 0.0;
    std::size_t TotalItems       = 0;
    std::size_t Iterations       = 0;
    while ( TotalNanoseconds < MinimumSeconds * 1'000'000'000.0 ) {
        const Result Current = RunOnce( Path, ProducerCount, ConsumerCount, ItemsPerProducer );
        TotalNanoseconds += Current.Nanoseconds;
        TotalItems += Current.Items;
        ++Iterations;
    }

    const double NanosecondsPerItem = TotalNanoseconds / static_cast<double>( TotalItems );
    std::cout << std::setprecision( 12 ) << "{\"name\":\"" << VariantName() << "\",\"path\":\"" << Path << "\",\"producers\":" << ProducerCount << ",\"consumers\":" << ConsumerCount << ",\"items_per_producer\":" << ItemsPerProducer
              << ",\"iterations\":" << Iterations << ",\"ns_per_item\":" << NanosecondsPerItem << ",\"million_items_per_second\":" << 1000.0 / NanosecondsPerItem << "}\n";
}
