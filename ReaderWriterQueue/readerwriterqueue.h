//
// Created by wwjszz on 25-11-5.
//

#ifndef READERWRITERQUEUE_H
#define READERWRITERQUEUE_H

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>

#include "common/memory.h"

#if HAKLE_CPP_VERSION >= 20
#include <semaphore>
#elif defined( _WIN32 )
#include <windows.h>
#undef min
#undef max
#elif defined( __MACH__ )
#include <mach/mach.h>
#elif defined( __unix__ )
#include <semaphore.h>
#else
#error Unsupported platform!
#endif

namespace hakle {

#if HAKLE_CPP_VERSION >= 20
template <class T>
    requires std::is_integral_v<T> || std::is_floating_point_v<T> || std::is_pointer_v<T>
#else
template <class T,
          typename std::enable_if<std::is_integral<T>::value || std::is_floating_point<T>::value || std::is_pointer<T>::value, int>::type = 0>
#endif
class WeakAtomic {
public:
    WeakAtomic() = default;

    ~WeakAtomic() = default;

    WeakAtomic( const WeakAtomic& Other ) noexcept( std::is_nothrow_copy_constructible<T>::value ) : Value( Other.Load() ) {}

    WeakAtomic( WeakAtomic&& Other ) noexcept : Value( std::move( Other.Load() ) ) {}

#if HAKLE_CPP_VERSION >= 20
    template <std::convertible_to<T> U>
#else
    template <class U, typename std::enable_if<std::is_convertible<U, T>::value, int>::type = 0>
#endif
    explicit WeakAtomic( U&& Other ) noexcept( std::is_nothrow_constructible<T, U>::value ) : Value( std::forward<U>( Other ) ) {
    }

    // ReSharper disable once CppNonExplicitConversionOperator
    operator T() const noexcept {  // NOLINT(*-explicit-constructor)
        return Load();
    }

    template <class U>
    WeakAtomic& operator=( U&& Other ) noexcept {
        Store( std::forward<U>( Other ) );
        return *this;
    }

    WeakAtomic& operator=( const WeakAtomic& Other ) noexcept {
        Store( Other.Load() );
        return *this;
    }

    [[nodiscard]] T Load() const noexcept { return Value.load( std::memory_order_relaxed ); }

    void Store( const T& Val ) noexcept { Value.store( Val, std::memory_order_relaxed ); }

    T FetchAdd( T Increment ) noexcept { return Value.fetch_add( Increment, std::memory_order_relaxed ); }

    T FetchAddRelease( T Increment ) noexcept { return Value.fetch_add( Increment, std::memory_order_release ); }

    T FetchAddAcquire( T Increment ) noexcept { return Value.fetch_add( Increment, std::memory_order_acquire ); }

    bool CompareExchangeStrong( T& Expected, T Desired ) noexcept {
        return Value.compare_exchange_strong( Expected, Desired, std::memory_order_relaxed );
    }

private:
    std::atomic<T> Value;
};

template <class T, std::size_t EXPECTED_BLOCK_SIZE = 512>
class ReaderWriterQueue {
public:
    explicit ReaderWriterQueue( std::size_t ReservedSize = 15 )
        : BlockSize( EXPECTED_BLOCK_SIZE )
#ifndef NDEBUG
          ,
          DequeueStatus( false ), EnqueueStatus( false )
#endif
    {
        assert( EXPECTED_BLOCK_SIZE == CeilToPow2( EXPECTED_BLOCK_SIZE ) && "EXPECTED_BLOCK_SIZE must be a power of 2" );
        assert( EXPECTED_BLOCK_SIZE >= 2 && "EXPECTED_BLOCK_SIZE must be at least 2" );

        Block* FirstBlock = nullptr;
        BlockSize         = CeilToPow2( ReservedSize + 1 );
        if ( BlockSize > EXPECTED_BLOCK_SIZE ) {
            Block* LastBlock = nullptr;
            BlockSize        = EXPECTED_BLOCK_SIZE;

            const std::size_t InitialCount = ( ReservedSize + ( EXPECTED_BLOCK_SIZE << 1 ) - 3 ) / ( EXPECTED_BLOCK_SIZE - 1 );
            for ( std::size_t i = 0; i != InitialCount; ++i ) {
                Block* CurrentBlock = MakeBlock( BlockSize );
                if ( CurrentBlock == nullptr ) {
                    throw std::bad_alloc();
                }
                if ( FirstBlock == nullptr ) {
                    FirstBlock = CurrentBlock;
                }
                else {
                    LastBlock->Next = CurrentBlock;
                }
                LastBlock          = CurrentBlock;
                CurrentBlock->Next = FirstBlock;
            }
        }
        else {
            FirstBlock = MakeBlock( BlockSize );
            if ( FirstBlock == nullptr ) {
                throw std::bad_alloc();
            }
            FirstBlock->Next = FirstBlock;
        }

        FrontBlock = FirstBlock;
        TailBlock  = FirstBlock;

        std::atomic_thread_fence( std::memory_order_release );
    }

    ReaderWriterQueue( ReaderWriterQueue&& Other ) noexcept
        : FrontBlock( Other.FrontBlock ), TailBlock( Other.TailBlock ), BlockSize( Other.BlockSize )
#ifndef NDEBUG
          ,
          DequeueStatus( false ), EnqueueStatus( false )
#endif
    {
        Other.FrontBlock = nullptr;
        Other.TailBlock  = nullptr;
        Other.BlockSize  = 0;
    }

    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    void Clear() noexcept {
        Block* CurrentBlock = FrontBlock;

        if ( !CurrentBlock ) {
            return;
        }

        do {
            Block* NextBlock = CurrentBlock->Next;

#if HAKLE_CPP_VERSION >= 17
            CurrentBlock->~Block();
            HAKLE_OPERATOR_DELETE( CurrentBlock );
#else
            char* RawBlock = CurrentBlock->RawThis;
            CurrentBlock->~Block();
            ::operator delete( RawBlock );
#endif

            CurrentBlock = NextBlock;
        } while ( CurrentBlock != FrontBlock );
    }

    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    ReaderWriterQueue& operator=( ReaderWriterQueue&& Other ) noexcept {
        if ( this != &Other ) {
            Clear();
            FrontBlock       = Other.FrontBlock;
            TailBlock        = Other.TailBlock;
            BlockSize        = Other.BlockSize;
            Other.FrontBlock = nullptr;
            Other.TailBlock  = nullptr;
            Other.BlockSize  = 0;
        }
        return *this;
    }

    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    ~ReaderWriterQueue() { Clear(); }

    ReaderWriterQueue( const ReaderWriterQueue& ) = delete;

    ReaderWriterQueue& operator=( const ReaderWriterQueue& ) = delete;

    [[nodiscard]] bool TryEnqueue( const T& Item ) { return InnerEnqueue<AllocMode::CannotAlloc>( Item ); }

    [[nodiscard]] bool TryEnqueue( T&& Item ) { return InnerEnqueue<AllocMode::CannotAlloc>( std::move( Item ) ); }

    template <class... Args>
    [[nodiscard]] bool TryEmplace( Args&&... Params ) {
        return InnerEnqueue<AllocMode::CannotAlloc>( std::forward<Args>( Params )... );
    }

    bool Enqueue( const T& Item ) { return InnerEnqueue<AllocMode::CanAlloc>( Item ); }

    bool Enqueue( T&& Item ) { return InnerEnqueue<AllocMode::CanAlloc>( std::move( Item ) ); }

    template <class... Args>
    bool Emplace( Args&&... Params ) {
        return InnerEnqueue<AllocMode::CanAlloc>( std::forward<Args>( Params )... );
    }

    template <class U>
    [[nodiscard]] bool TryDequeue( U& Result ) noexcept {
#ifndef NDEBUG
        QueueGuard DequeueGuard( DequeueStatus );
#endif
        Block*      CurrentBlock = FrontBlock.Load();
        std::size_t BlockFront   = CurrentBlock->Front.Load();
        std::size_t BlockTail    = CurrentBlock->LocalTail;

        if ( CurrentBlock != TailBlock.Load() && BlockFront == ( BlockTail = CurrentBlock->LocalTail = CurrentBlock->Tail.Load() ) ) {
            // CurrentBlock is Empty and not the last block, try to dequeue from the next block
            CurrentBlock = CurrentBlock->Next.Load();
            BlockFront   = CurrentBlock->Front.Load();
            BlockTail = CurrentBlock->LocalTail = CurrentBlock->Tail.Load();
            std::atomic_thread_fence( std::memory_order_acquire );

            assert( BlockFront != BlockTail );

            // expose quickly pending changes to FrontBlock
            std::atomic_thread_fence( std::memory_order_release );
            FrontBlock.Store( CurrentBlock );

            T* Element = CurrentBlock->Data + BlockFront;
            Result     = std::move( *Element );
            Element->~T();

            BlockFront = ( BlockFront + 1 ) & CurrentBlock->SizeMask;
            CurrentBlock->Front.Store( BlockFront );
        }
        else if ( BlockFront != BlockTail || BlockFront != ( CurrentBlock->LocalTail = CurrentBlock->Tail.Load() ) ) {
            std::atomic_thread_fence( std::memory_order_acquire );

            T* Element = CurrentBlock->Data + BlockFront;
            Result     = std::move( *Element );
            Element->~T();

            BlockFront = ( BlockFront + 1 ) & CurrentBlock->SizeMask;

            std::atomic_thread_fence( std::memory_order_release );
            CurrentBlock->Front.Store( BlockFront );
        }
        else {
            return false;
        }
        return true;
    }

    [[nodiscard]] T* Peek() const noexcept {
#ifndef NDEBUG
        QueueGuard DequeueGuard( DequeueStatus );
#endif

        Block*      CurrentBlock = FrontBlock.Load();
        std::size_t BlockFront   = CurrentBlock->Front.Load();
        std::size_t BlockTail    = CurrentBlock->LocalTail;

        if ( CurrentBlock != TailBlock.Load() && BlockFront == BlockTail && BlockFront == ( CurrentBlock->LocalTail = CurrentBlock->Tail.Load() ) ) {
            // CurrentBlock is Empty and not the last block, try to dequeue from the next block
            CurrentBlock = CurrentBlock->Next.Load();
            BlockFront   = CurrentBlock->Front.Load();
            BlockTail = CurrentBlock->LocalTail = CurrentBlock->Tail.Load();
            std::atomic_thread_fence( std::memory_order_acquire );

            assert( BlockFront != BlockTail );

            return CurrentBlock->Data + BlockFront;
        }
        else if ( BlockFront != BlockTail || BlockFront != ( CurrentBlock->LocalTail = CurrentBlock->Tail.Load() ) ) {
            std::atomic_thread_fence( std::memory_order_acquire );

            return CurrentBlock->Data + BlockFront;
        }
        return nullptr;
    }

    [[nodiscard]] bool Pop() noexcept {
#ifndef NDEBUG
        QueueGuard DequeueGuard( DequeueStatus );
#endif

        Block*      CurrentBlock = FrontBlock.Load();
        std::size_t BlockFront   = CurrentBlock->Front.Load();
        std::size_t BlockTail    = CurrentBlock->LocalTail;

        if ( CurrentBlock != TailBlock.Load() && BlockFront == BlockTail && BlockFront == ( CurrentBlock->LocalTail = CurrentBlock->Tail.Load() ) ) {
            // CurrentBlock is Empty and not the last block, try to dequeue from the next block
            CurrentBlock = CurrentBlock->Next.Load();
            BlockFront   = CurrentBlock->Front.Load();
            BlockTail = CurrentBlock->LocalTail = CurrentBlock->Tail.Load();
            std::atomic_thread_fence( std::memory_order_acquire );

            assert( BlockFront != BlockTail );

            // expose quickly pending changes to FrontBlock
            std::atomic_thread_fence( std::memory_order_release );
            FrontBlock.Store( CurrentBlock );

            T* Element = CurrentBlock->Data + BlockFront;
            Element->~T();

            BlockFront = ( BlockFront + 1 ) & CurrentBlock->SizeMask;
            CurrentBlock->Front.Store( BlockFront );
        }
        else if ( BlockFront != BlockTail || BlockFront != ( CurrentBlock->LocalTail = CurrentBlock->Tail.Load() ) ) {
            std::atomic_thread_fence( std::memory_order_acquire );

            T* Element = CurrentBlock->Data + BlockFront;
            Element->~T();

            BlockFront = ( BlockFront + 1 ) & CurrentBlock->SizeMask;

            std::atomic_thread_fence( std::memory_order_release );
            CurrentBlock->Front.Store( BlockFront );
        }
        else {
            return false;
        }
        return true;
    }

    [[nodiscard]] std::size_t SizeApprox() const noexcept {
        std::size_t Size         = 0;
        Block*      FirstBlock   = FrontBlock.Load();
        Block*      CurrentBlock = FirstBlock;
        do {
            Size += ( CurrentBlock->Tail.Load() - CurrentBlock->Front.Load() ) & CurrentBlock->SizeMask;
            std::atomic_thread_fence( std::memory_order_acquire );
            CurrentBlock = CurrentBlock->Next.Load();
        } while ( CurrentBlock != FirstBlock );
        return Size;
    }

    [[nodiscard]] std::size_t MaxCapacity() const noexcept {
        std::size_t Capacity     = 0;
        Block*      FirstBlock   = FrontBlock.Load();
        Block*      CurrentBlock = FirstBlock;
        do {
            Capacity += CurrentBlock->SizeMask;
            CurrentBlock = CurrentBlock->Next.Load();
        } while ( CurrentBlock != FirstBlock );
        return Capacity;
    }

private:
    enum class AllocMode { CanAlloc, CannotAlloc };

    template <AllocMode Mode, class... Args>
    bool InnerEnqueue( Args&&... Params ) {
#ifndef NDEBUG
        QueueGuard EnqueueGuard( EnqueueStatus );
#endif

        Block*      CurrentBlock = TailBlock.Load();
        std::size_t BlockTail    = CurrentBlock->Tail.Load();
        std::size_t BlockFront   = CurrentBlock->LocalFront;

        std::size_t NextBlockTail = ( BlockTail + 1 ) & CurrentBlock->SizeMask;
        if ( NextBlockTail != BlockFront || NextBlockTail != ( CurrentBlock->LocalFront = CurrentBlock->Front.Load() ) ) {
            std::atomic_thread_fence( std::memory_order_acquire );

            T* Location = CurrentBlock->Data + BlockTail;
            ::new ( Location ) T( std::forward<Args>( Params )... );

            std::atomic_thread_fence( std::memory_order_release );
            CurrentBlock->Tail.Store( NextBlockTail );
        }
        else if ( CurrentBlock->Next.Load() != FrontBlock.Load() ) {
            std::atomic_thread_fence( std::memory_order_acquire );

            CurrentBlock  = CurrentBlock->Next.Load();
            NextBlockTail = CurrentBlock->Tail.Load();
            BlockFront = CurrentBlock->LocalFront = CurrentBlock->Front.Load();
            std::atomic_thread_fence( std::memory_order_acquire );

            assert( BlockFront == NextBlockTail );

            T* Location = CurrentBlock->Data + NextBlockTail;
            ::new ( Location ) T( std::forward<Args>( Params )... );

            CurrentBlock->Tail.Store( ( NextBlockTail + 1 ) & CurrentBlock->SizeMask );

            std::atomic_thread_fence( std::memory_order_release );
            TailBlock.Store( CurrentBlock );
        }
        else {
            std::atomic_thread_fence( std::memory_order_acquire );

            HAKLE_CONSTEXPR_IF( Mode == AllocMode::CanAlloc ) {

                if ( BlockSize < EXPECTED_BLOCK_SIZE ) {
                    BlockSize *= 2;
                    if ( BlockSize > EXPECTED_BLOCK_SIZE ) {
                        BlockSize = EXPECTED_BLOCK_SIZE;
                    }
                }

                Block* NewBlock = MakeBlock( BlockSize );
                if ( NewBlock == nullptr ) {
                    return false;
                }

                ::new ( NewBlock->Data ) T( std::forward<Args>( Params )... );

                assert( NewBlock->Front.Load() == 0 );
                NewBlock->Tail = NewBlock->LocalTail = 1;

                NewBlock->Next.Store( CurrentBlock->Next.Load() );
                CurrentBlock->Next.Store( NewBlock );

                std::atomic_thread_fence( std::memory_order_release );
                TailBlock.Store( NewBlock );
            }
            else HAKLE_CONSTEXPR_IF( Mode == AllocMode::CannotAlloc ) {
                // full and cannot alloc
                return false;
            }
            else {
                assert( false && "Should never reach here" );
            }
        }
        return true;
    }

    static std::size_t CeilToPow2( std::size_t X ) noexcept {
        --X;
        X |= X >> 1;
        X |= X >> 2;
        X |= X >> 4;
        constexpr std::size_t N = sizeof( std::size_t );
        for ( std::size_t i = 1; i < N; ++i ) {
            X |= X >> ( i << 3 );
        }
        ++X;
        return X;
    }

#if HAKLE_CPP_VERSION < 17
    template <class U>
    static U* AlignFor( char* ptr ) noexcept {
        const std::size_t Alignment = alignof( U );
        assert( ( Alignment & ( Alignment - 1 ) ) == 0 && "Alignment must be power of two" );
        return reinterpret_cast<U*>( ptr + ( ( Alignment - ( reinterpret_cast<uintptr_t>( ptr ) & ( Alignment - 1 ) ) ) & ( Alignment - 1 ) ) );
    }
#endif

#ifndef NDEBUG
    struct QueueGuard {
        explicit QueueGuard( WeakAtomic<bool>& InStatus ) noexcept : Status( InStatus ) {
            assert( !InStatus
                    && "Concurrent (or re-entrant) enqueue or dequeue operation detected (only one thread at a time may hold the producer or "
                       "consumer role)" );
            Status = true;
        }

        ~QueueGuard() { Status = false; }

        QueueGuard( const QueueGuard& Other ) = delete;

        QueueGuard( QueueGuard&& Other ) = delete;

        QueueGuard& operator=( const QueueGuard& Other ) = delete;

        QueueGuard& operator=( QueueGuard&& Other ) = delete;

    private:
        WeakAtomic<bool>& Status;
    };
#endif

    struct Block {
        alignas( HAKLE_CACHE_LINE_SIZE ) WeakAtomic<std::size_t> Front;
        std::size_t LocalTail;

        alignas( HAKLE_CACHE_LINE_SIZE ) WeakAtomic<std::size_t> Tail;
        std::size_t LocalFront;

        WeakAtomic<Block*> Next;

        T* Data;

        const std::size_t SizeMask;

#if HAKLE_CPP_VERSION >= 17
        Block( const std::size_t Size, T* InData ) noexcept
            : Front( 0 ), LocalTail( 0 ), Tail( 0 ), LocalFront( 0 ), Next( nullptr ), Data( InData ), SizeMask( Size - 1 ) {}
#else
        char* RawThis;
        char* RawData;

        Block( const std::size_t Size, char* InRawThis, char* InRawData, T* InData ) noexcept
            : Front( 0 ), LocalTail( 0 ), Tail( 0 ), LocalFront( 0 ), Next( nullptr ), Data( InData ), SizeMask( Size - 1 ), RawThis( InRawThis ),
              RawData( InRawData ) {}
#endif

        ~Block() {
#if HAKLE_CPP_VERSION >= 17
            if constexpr ( !std::is_trivially_destructible_v<T> ) {
                for ( std::size_t i = Front; i != Tail; i = ( i + 1 ) & SizeMask ) {
                    Data[ i ].~T();
                }
            }
            HAKLE_OPERATOR_DELETE( Data );
#else
            Clear();
            HAKLE_OPERATOR_DELETE( RawData );
#endif
        }

        Block( const Block& Other ) = delete;

        Block( Block&& Other ) = delete;

        Block& operator=( const Block& Other ) = delete;

        Block& operator=( Block&& Other ) = delete;

    private:
#if HAKLE_CPP_VERSION < 17
        template <bool IsTrivial = std::is_trivially_destructible<T>::value>
        typename std::enable_if<!IsTrivial>::type Clear() noexcept {
            for ( std::size_t i = Front; i != Tail; i = ( i + 1 ) & SizeMask ) {
                Data[ i ].~T();
            }
        }

        template <bool IsTrivial = std::is_trivially_destructible<T>::value>
        typename std::enable_if<IsTrivial>::type Clear() noexcept {
            // nothing to do
        }
#endif
    };

    static Block* MakeBlock( std::size_t capacity ) noexcept {
#if HAKLE_CPP_VERSION >= 17
        T*     NewData  = HAKLE_OPERATOR_NEW_ARRAY( T, capacity );
        Block* NewBlock = HAKLE_OPERATOR_NEW( Block );
        return ::new ( NewBlock ) Block( capacity, NewData );
#else
        char* NewData  = HAKLE_OPERATOR_NEW_ARRAY( char, capacity * sizeof( T ) + alignof( T ) - 1 );
        char* NewBlock = HAKLE_OPERATOR_NEW_ARRAY( char, sizeof( Block ) + alignof( Block ) - 1 );
        if ( !NewData || !NewBlock ) {
            return nullptr;
        }
        Block* NewBlockHead = AlignFor<Block>( NewBlock );
        T*     NewDataHead  = AlignFor<T>( NewData );
        return new ( NewBlockHead ) Block( capacity, NewBlock, NewData, NewDataHead );
#endif
    }

    alignas( HAKLE_CACHE_LINE_SIZE ) WeakAtomic<Block*> FrontBlock;

    alignas( HAKLE_CACHE_LINE_SIZE ) WeakAtomic<Block*> TailBlock;

    std::size_t BlockSize;

#ifndef NDEBUG
    mutable WeakAtomic<bool> DequeueStatus;
    WeakAtomic<bool>         EnqueueStatus;
#endif
};

#if HAKLE_CPP_VERSION >= 20
using Semaphore = std::binary_semaphore;

//---------------------------------------------------------
// LightweightSemaphore
//---------------------------------------------------------
class LightWeightSemaphore {
public:
    using signed_size_t = std::make_signed<std::size_t>::type;

    explicit LightWeightSemaphore( signed_size_t InitialCount = 0 ) : Count( InitialCount ), Sem( 0 ) { assert( InitialCount >= 0 ); }

    bool TryWait() noexcept {
        if ( Count.Load() > 0 ) {
            Count.FetchAddAcquire( -1 );
            return true;
        }
        return false;
    }

    bool Wait( const std::int64_t Timeout = -1 ) { return TryWait() || TryWaitWithPartialSpinning( Timeout ); }

    void Signal( signed_size_t Num = 1 ) {
        assert( Num > 0 );
        const signed_size_t OldCount = Count.FetchAddRelease( Num );
        assert( OldCount >= -1 );
        if ( OldCount < 0 ) {
            Sem.release();
        }
    }

    [[nodiscard]] std::size_t Available() const noexcept {
        const signed_size_t Num = Count.Load();
        return Num > 0 ? static_cast<std::size_t>( Num ) : 0;
    }

private:
    bool TryWaitWithPartialSpinning( const std::int64_t Timeout = -1 ) {
        // spin
        short spin = 1024;
        while ( --spin >= 0 ) {
            if ( Count.Load() > 0 ) {
                Count.FetchAddAcquire( -1 );
                return true;
            }
            // TODO: Prevent the compiler from collapsing the loop.
        }
        // sub
        signed_size_t OldCount = Count.FetchAddAcquire( -1 );
        if ( OldCount > 0 ) {
            return true;
        }
        if ( Timeout < 0 ) {
            Sem.acquire();
            return true;
        }
        if ( Timeout > 0 && Sem.try_acquire_for( std::chrono::milliseconds( Timeout ) ) ) {
            return true;
        }
        // since we sub, we need to add
        while ( true ) {
            OldCount = Count.FetchAdd( 1 );
            if ( OldCount < 0 ) {
                return false;
            }
            else if ( Sem.try_acquire() ) {
                return true;
            }
        }
    }

    WeakAtomic<signed_size_t> Count;
    Semaphore                 Sem;
};
#else
#if defined( _WIN32 )
//---------------------------------------------------------
// Semaphore (Windows)
//---------------------------------------------------------
class Semaphore {
private:
    void* m_hSema;

public:
    Semaphore( int initialCount = 0 ) : m_hSema() {
        assert( initialCount >= 0 );
        const long maxLong = 0x7fffffff;
        m_hSema            = CreateSemaphoreW( nullptr, initialCount, maxLong, nullptr );
        assert( m_hSema );
    }

    ~Semaphore() { CloseHandle( m_hSema ); }

    Semaphore( const Semaphore& other ) = delete;

    Semaphore& operator=( const Semaphore& other ) = delete;

    bool Wait() {
        const unsigned long infinite = 0xffffffff;
        return WaitForSingleObject( m_hSema, infinite ) == 0;
    }

    bool TryWait() { return WaitForSingleObject( m_hSema, 0 ) == 0; }

    bool TimedWait( std::uint64_t usecs ) { return WaitForSingleObject( m_hSema, ( unsigned long )( usecs / 1000 ) ) == 0; }

    void Signal( int count = 1 ) {
        while ( !ReleaseSemaphore( m_hSema, count, nullptr ) )
            ;
    }
};

#elif defined( __MACH__ )
//---------------------------------------------------------
// Semaphore (Apple iOS and OSX)
// Can't use POSIX semaphores due to http://lists.apple.com/archives/darwin-kernel/2009/Apr/msg00010.html
//---------------------------------------------------------
class Semaphore {
private:
    semaphore_t m_sema;

public:
    Semaphore( int initialCount = 0 ) : m_sema() {
        assert( initialCount >= 0 );
        kern_return_t rc = semaphore_create( mach_task_self(), &m_sema, SYNC_POLICY_FIFO, initialCount );
        assert( rc == KERN_SUCCESS );
    }

    Semaphore( const Semaphore& other )            = delete;
    Semaphore& operator=( const Semaphore& other ) = delete;

    ~Semaphore() { semaphore_destroy( mach_task_self(), m_sema ); }

    bool Wait() { return semaphore_wait( m_sema ) == KERN_SUCCESS; }

    bool TryWait() { return TimedWait( 0 ); }

    bool TimedWait( std::uint64_t timeout_usecs ) {
        mach_timespec_t ts;
        ts.tv_sec  = static_cast<unsigned int>( timeout_usecs / 1000000 );
        ts.tv_nsec = static_cast<int>( ( timeout_usecs % 1000000 ) * 1000 );

        // added in OSX 10.10:
        // https://developer.apple.com/library/prerelease/mac/documentation/General/Reference/APIDiffsMacOSX10_10SeedDiff/modules/Darwin.html
        kern_return_t rc = semaphore_timedwait( m_sema, ts );
        return rc == KERN_SUCCESS;
    }

    void Signal() {
        while ( semaphore_signal( m_sema ) != KERN_SUCCESS )
            ;
    }

    void Signal( int count ) {
        while ( count-- > 0 ) {
            while ( semaphore_signal( m_sema ) != KERN_SUCCESS )
                ;
        }
    }
};
#elif defined( __unix__ )
//---------------------------------------------------------
// Semaphore (POSIX, Linux)
//---------------------------------------------------------
class Semaphore {
private:
    sem_t m_sema;

public:
    Semaphore( int initialCount = 0 ) : m_sema() {
        assert( initialCount >= 0 );
        int rc = sem_init( &m_sema, 0, static_cast<unsigned int>( initialCount ) );
        assert( rc == 0 );
        AE_UNUSED( rc );
    }

    Semaphore( const Semaphore& other )            = delete;
    Semaphore& operator=( const Semaphore& other ) = delete;

    ~Semaphore() { sem_destroy( &m_sema ); }

    bool Wait() {
        // http://stackoverflow.com/questions/2013181/gdb-causes-sem-Wait-to-fail-with-eintr-error
        int rc;
        do {
            rc = sem_wait( &m_sema );
        } while ( rc == -1 && errno == EINTR );
        return rc == 0;
    }

    bool TryWait() {
        int rc;
        do {
            rc = sem_trywait( &m_sema );
        } while ( rc == -1 && errno == EINTR );
        return rc == 0;
    }

    bool TimedWait( std::uint64_t usecs ) {
        struct timespec ts;
        const int       usecs_in_1_sec = 1000000;
        const int       nsecs_in_1_sec = 1000000000;
        clock_gettime( CLOCK_REALTIME, &ts );
        ts.tv_sec += static_cast<time_t>( usecs / usecs_in_1_sec );
        ts.tv_nsec += static_cast<long>( usecs % usecs_in_1_sec ) * 1000;
        // sem_timedwait bombs if you have more than 1e9 in tv_nsec
        // so we have to clean things up before passing it in
        if ( ts.tv_nsec >= nsecs_in_1_sec ) {
            ts.tv_nsec -= nsecs_in_1_sec;
            ++ts.tv_sec;
        }

        int rc;
        do {
            rc = sem_timedwait( &m_sema, &ts );
        } while ( rc == -1 && errno == EINTR );
        return rc == 0;
    }

    void Signal() {
        while ( sem_post( &m_sema ) == -1 )
            ;
    }

    void Signal( int count ) {
        while ( count-- > 0 ) {
            while ( sem_post( &m_sema ) == -1 )
                ;
        }
    }
};
#else

#error Unsupported platform!

#endif

//---------------------------------------------------------
// LightweightSemaphore
//---------------------------------------------------------
class LightWeightSemaphore {
public:
    typedef std::make_signed<std::size_t>::type ssize_t;

private:
    WeakAtomic<ssize_t> m_count;
    Semaphore           m_sema;

    bool WaitWithPartialSpinning( std::int64_t timeout_usecs = -1 ) {
        ssize_t oldCount;
        // Is there a better way to set the initial spin count?
        // If we lower it to 1000, testBenaphore becomes 15x slower on my Core i7-5930K Windows PC,
        // as threads start hitting the kernel semaphore.
        int spin = 1024;
        while ( --spin >= 0 ) {
            if ( m_count.Load() > 0 ) {
                m_count.FetchAddAcquire( -1 );
                return true;
            }
            // TODO: Prevent the compiler from collapsing the loop.
        }
        oldCount = m_count.FetchAddAcquire( -1 );
        if ( oldCount > 0 )
            return true;
        if ( timeout_usecs < 0 ) {
            if ( m_sema.Wait() )
                return true;
        }
        if ( timeout_usecs > 0 && m_sema.TimedWait( static_cast<uint64_t>( timeout_usecs ) ) )
            return true;
        // At this point, we've timed out waiting for the semaphore, but the
        // count is still decremented indicating we may still be waiting on
        // it. So we have to re-adjust the count, but only if the semaphore
        // wasn't signaled enough times for us too since then. If it was, we
        // need to release the semaphore too.
        while ( true ) {
            oldCount = m_count.FetchAddAcquire( 1 );
            if ( oldCount < 0 )
                return false;  // successfully restored things to the way they were
            // Oh, the producer thread just signaled the semaphore after all. Try again:
            oldCount = m_count.FetchAddAcquire( -1 );
            if ( oldCount > 0 && m_sema.TryWait() )
                return true;
        }
    }

public:
    LightWeightSemaphore( ssize_t initialCount = 0 ) : m_count( initialCount ), m_sema() { assert( initialCount >= 0 ); }

    bool TryWait() {
        if ( m_count.Load() > 0 ) {
            m_count.FetchAddAcquire( -1 );
            return true;
        }
        return false;
    }

    bool Wait() { return TryWait() || WaitWithPartialSpinning(); }

    bool Wait( std::int64_t timeout_usecs ) { return TryWait() || WaitWithPartialSpinning( timeout_usecs ); }

    void Signal( ssize_t count = 1 ) {
        assert( count >= 0 );
        ssize_t oldCount = m_count.FetchAddRelease( count );
        assert( oldCount >= -1 );
        if ( oldCount < 0 ) {
            m_sema.Signal( 1 );
        }
    }

    std::size_t Available() const {
        ssize_t count = m_count.Load();
        return count > 0 ? static_cast<std::size_t>( count ) : 0;
    }
};

#endif

template <class T, std::size_t EXPECTED_BLOCK_SIZE = 512>
class BlockingReaderWriterQueue {
private:
    typedef ReaderWriterQueue<T, EXPECTED_BLOCK_SIZE> InnerQueue;

public:
    explicit BlockingReaderWriterQueue( std::size_t ReservedSize = 15 )
        : Inner( ReservedSize ), Sem(
#if HAKLE_CPP_VERSION >= 14
                                     std::make_unique<LightWeightSemaphore>()
#else
                                     // TODO: fix warning
                                     new LightWeightSemaphore()
#endif
                                 ) {
    }

    ~BlockingReaderWriterQueue() = default;

    BlockingReaderWriterQueue( BlockingReaderWriterQueue&& Other )            = default;
    BlockingReaderWriterQueue& operator=( BlockingReaderWriterQueue&& Other ) = default;

    BlockingReaderWriterQueue( const BlockingReaderWriterQueue& )            = delete;
    BlockingReaderWriterQueue& operator=( const BlockingReaderWriterQueue& ) = delete;

    bool TryEnqueue( const T& Item ) {
        if ( Inner.TryEnqueue( Item ) ) {
            Sem->Signal();
            return true;
        }
        return false;
    }

    bool TryEnqueue( T&& Item ) {
        if ( Inner.TryEnqueue( std::move( Item ) ) ) {
            Sem->Signal();
            return true;
        }
        return false;
    }

    template <class... Args>
    bool TryEmplace( Args&&... Params ) {
        if ( Inner.TryEmplace( std::forward<Args>( Params )... ) ) {
            Sem->Signal();
            return true;
        }
        return false;
    }

    bool Enqueue( const T& Item ) {
        if ( Inner.Enqueue( Item ) ) {
            Sem->Signal();
            return true;
        }
        return false;
    }

    bool Enqueue( T&& Item ) {
        if ( Inner.Enqueue( std::move( Item ) ) ) {
            Sem->Signal();
            return true;
        }
        return false;
    }

    template <class... Args>
    bool Emplace( Args&&... Params ) {
        if ( Inner.Emplace( std::forward<Args>( Params )... ) ) {
            Sem->Signal();
            return true;
        }
        return false;
    }

    bool TryDequeue( T& Result ) {
        if ( Sem->TryWait() ) {
            bool Success = Inner.TryDequeue( Result );
            assert( Success );
            return true;
        }
        return false;
    }

    bool Dequeue( T& Result ) {
        if ( Sem->Wait() ) {
            bool Success = Inner.TryDequeue( Result );
            assert( Success );
            return true;
        }
        return false;
    }

    bool DequeueWaitFor( T& Result, std::int64_t Timeout ) {
        if ( Sem->Wait( Timeout ) ) {
            bool Success = Inner.TryDequeue( Result );
            assert( Success );
            return true;
        }
        return false;
    }

    T* Peek() noexcept { return Inner.Peek(); }

    bool Pop() noexcept {
        if ( Sem->TryWait() ) {
            bool Success = Inner.Pop();
            assert( Success );
            return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t SizeApprox() const noexcept { return Sem->Available(); }

    [[nodiscard]] std::size_t MaxCapacity() const noexcept { return Inner.MaxCapacity(); }

private:
    InnerQueue                            Inner;
    std::unique_ptr<LightWeightSemaphore> Sem;
};

}  // namespace hakle

#endif  // READERWRITERQUEUE_H