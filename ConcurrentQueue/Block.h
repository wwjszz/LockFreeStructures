//
// Created by admin on 2025/12/12.
//

#ifndef LOCKFREESTRUCTURES_BLOCK_H
#define LOCKFREESTRUCTURES_BLOCK_H

#include <array>
#include <atomic>
#include <bit>
#include <limits>
#if defined( ENABLE_MEMORY_LEAK_DETECTION )
#include <cstdio>
#endif

#include "common/common.h"

namespace hakle {

#ifdef HAKLE_USE_CONCEPT
template <class T>
concept IsPolicy = requires( T& t, const T& ct, std::size_t Index, std::size_t Count ) {
    { T::HasMeaningfulSetResult } -> std::convertible_to<bool>;
    { ct.IsEmpty() } -> std::same_as<bool>;
    { t.SetEmpty( Index ) } -> std::same_as<bool>;
    { t.SetSomeEmpty( Index, Count ) } -> std::same_as<bool>;
    t.SetAllEmpty();
    t.Reset();
};

template <class T>
concept IsBlock = requires( T& t, std::size_t Index ) {
    typename T::ValueType;
    { T::HasMeaningfulSetResult } -> std::convertible_to<bool>;
    { T::BlockSize } -> std::convertible_to<std::size_t>;
    requires std::convertible_to<T*, std::remove_reference_t<decltype( t.Next )>>;
    { t[ Index ] } -> std::convertible_to<typename T::ValueType*>;
} && T::BlockSize > 1 && std::has_single_bit( static_cast<std::size_t>( T::BlockSize ) ) && IsPolicy<T>;

template <std::size_t BLOCK_SIZE, class BLOCK_TYPE>
concept CheckBlockSize = IsBlock<BLOCK_TYPE> && BLOCK_SIZE == BLOCK_TYPE::BlockSize;

template <class T>
concept IsBlockWithMeaningfulSetResult = IsBlock<T> && ( T::HasMeaningfulSetResult == true );
#endif

template <class T>
struct FreeListNode;

template <class T, std::size_t BLOCK_SIZE, HAKLE_CONCEPT( IsPolicy ) Policy>
struct HakleBlock;

template <std::size_t BLOCK_SIZE>
struct FlagsCheckPolicy;

template <std::size_t BLOCK_SIZE>
struct CounterCheckPolicy;

template <std::size_t BLOCK_SIZE>
struct WordFlagsCheckPolicy;

template <class T, std::size_t BLOCK_SIZE>
using HakleFlagsBlock = HakleBlock<T, BLOCK_SIZE, FlagsCheckPolicy<BLOCK_SIZE>>;

template <class T, std::size_t BLOCK_SIZE>
using HakleCounterBlock = HakleBlock<T, BLOCK_SIZE, CounterCheckPolicy<BLOCK_SIZE>>;

template <class T, std::size_t BLOCK_SIZE>
using HakleWordFlagsBlock = HakleBlock<T, BLOCK_SIZE, WordFlagsCheckPolicy<BLOCK_SIZE>>;

// TODO: memory_order!!!
template <std::size_t BLOCK_SIZE>
struct FlagsCheckPolicy {
    constexpr static bool HasMeaningfulSetResult = false;

    HAKLE_CPP20_CONSTEXPR ~FlagsCheckPolicy() = default;

    HAKLE_NODISCARD HAKLE_CPP20_CONSTEXPR bool IsEmpty() const {
        for ( auto& Flag : Flags ) {
            if ( !Flag.load( std::memory_order_relaxed ) ) {
                return false;
            }
        }

        std::atomic_thread_fence( std::memory_order_acquire );
        return true;
    }

    HAKLE_CPP20_CONSTEXPR bool SetEmpty( std::size_t Index ) {
        Flags[ Index ].store( 1, std::memory_order_release );
        return false;
    }

    HAKLE_CPP20_CONSTEXPR bool SetSomeEmpty( std::size_t Index, std::size_t Count ) {
        std::atomic_thread_fence( std::memory_order_release );

        for ( std::size_t i = 0; i < Count; ++i ) {
            Flags[ Index + i ].store( 1, std::memory_order_relaxed );
        }
        return false;
    }

    HAKLE_CPP20_CONSTEXPR void SetAllEmpty() {
        for ( std::size_t i = 0; i < BLOCK_SIZE; ++i ) {
            Flags[ i ].store( 1, std::memory_order_release );
        }
    }

    HAKLE_CPP20_CONSTEXPR void Reset() {
        for ( auto& Flag : Flags ) {
            Flag.store( 0, std::memory_order_release );
        }
    }

#if defined( ENABLE_MEMORY_LEAK_DETECTION )
    void PrintPolicy() {
        printf( "===PrintPolicy BLOCK_SIZE: %llu===\n", BLOCK_SIZE );
        for ( int i = 0; i < BLOCK_SIZE; ++i ) {
            printf( "Flag[%d]=%hhu\n", i, Flags[ i ].load() );
        }
    }
#endif

    std::array<std::atomic<uint8_t>, BLOCK_SIZE> Flags;
};

template <std::size_t BLOCK_SIZE>
struct CounterCheckPolicy {
    constexpr static bool HasMeaningfulSetResult = true;

    HAKLE_CPP20_CONSTEXPR ~CounterCheckPolicy() = default;

    HAKLE_NODISCARD HAKLE_CPP20_CONSTEXPR bool IsEmpty() const {
        if ( Counter.load( std::memory_order_relaxed ) == BLOCK_SIZE ) {
            std::atomic_thread_fence( std::memory_order_acquire );
            return true;
        }
        return false;
    }

    // Increments the counter and returns true if the block is now empty
    HAKLE_CPP20_CONSTEXPR bool SetEmpty( HAKLE_MAYBE_UNUSED std::size_t Index ) {
        std::size_t OldCounter = Counter.fetch_add( 1, std::memory_order_release );
        return OldCounter + 1 == BLOCK_SIZE;
    }

    HAKLE_CPP20_CONSTEXPR bool SetSomeEmpty( HAKLE_MAYBE_UNUSED std::size_t Index, std::size_t Count ) {
        std::size_t OldCounter = Counter.fetch_add( Count, std::memory_order_release );
        return OldCounter + Count == BLOCK_SIZE;
    }

    HAKLE_CPP20_CONSTEXPR void SetAllEmpty() { Counter.store( BLOCK_SIZE, std::memory_order_release ); }

    HAKLE_CPP20_CONSTEXPR void Reset() { Counter.store( 0, std::memory_order_release ); }

#if defined( ENABLE_MEMORY_LEAK_DETECTION )
    void PrintPolicy() {
        printf( "===PrintPolicy BLOCK_SIZE: %llu===\n", BLOCK_SIZE );
        printf( "Counter: %llu\n", Counter.load() );
    }
#endif
    std::atomic<std::size_t> Counter;
};

// Packed per-slot empty flags in a single machine word.  Compared with
// FlagsCheckPolicy this removes the flag-array loop from IsEmpty() and shrinks
// the block metadata to one atomic word.  SetEmpty becomes a read-modify-write
// instead of a byte store, so the two policies favour different contention
// patterns; both are kept and can be selected through the block type.
template <std::size_t BLOCK_SIZE>
struct WordFlagsCheckPolicy {
    static_assert( BLOCK_SIZE > 1 && std::has_single_bit( BLOCK_SIZE ), "BLOCK_SIZE must be a power of two and greater than one" );
    static_assert( BLOCK_SIZE <= std::numeric_limits<std::size_t>::digits, "BLOCK_SIZE does not fit in one word" );

    constexpr static bool HasMeaningfulSetResult = false;

    HAKLE_CPP20_CONSTEXPR ~WordFlagsCheckPolicy() = default;

    HAKLE_NODISCARD HAKLE_CPP20_CONSTEXPR bool IsEmpty() const {
        if ( Flags.load( std::memory_order_relaxed ) == FullMask ) {
            std::atomic_thread_fence( std::memory_order_acquire );
            return true;
        }
        return false;
    }

    HAKLE_CPP20_CONSTEXPR bool SetEmpty( std::size_t Index ) {
        Flags.fetch_or( Bit( Index ), std::memory_order_release );
        return false;
    }

    HAKLE_CPP20_CONSTEXPR bool SetSomeEmpty( std::size_t Index, std::size_t Count ) {
        if ( Count != 0 ) {
            const std::size_t Mask = Count == BLOCK_SIZE ? FullMask : ( ( std::size_t{ 1 } << Count ) - 1 ) << Index;
            Flags.fetch_or( Mask, std::memory_order_release );
        }
        return false;
    }

    HAKLE_CPP20_CONSTEXPR void SetAllEmpty() { Flags.store( FullMask, std::memory_order_release ); }

    HAKLE_CPP20_CONSTEXPR void Reset() { Flags.store( 0, std::memory_order_release ); }

#if defined( ENABLE_MEMORY_LEAK_DETECTION )
    void PrintPolicy() {
        printf( "===PrintPolicy BLOCK_SIZE: %llu===\n", BLOCK_SIZE );
        printf( "WordFlags: %llx\n", static_cast<unsigned long long>( Flags.load() ) );
    }
#endif

private:
    constexpr static std::size_t FullMask = BLOCK_SIZE == std::numeric_limits<std::size_t>::digits ? ~std::size_t{ 0 } : ( ( std::size_t{ 1 } << BLOCK_SIZE ) - 1 );

    constexpr static std::size_t Bit( std::size_t Index ) noexcept { return std::size_t{ 1 } << Index; }

public:
    std::atomic<std::size_t> Flags{ 0 };
};

enum class BlockMethod { Flags, Counter, WordFlags };

template <class T, std::size_t BLOCK_SIZE, HAKLE_CONCEPT( IsPolicy ) Policy>
struct HakleBlock : FreeListNode<HakleBlock<T, BLOCK_SIZE, Policy>>, Policy {
    using ValueType = T;
    using BlockType = HakleBlock;
    using Policy::HasMeaningfulSetResult;
    constexpr static std::size_t BlockSize = BLOCK_SIZE;

    HAKLE_CPP14_CONSTEXPR T* operator[]( std::size_t Index ) noexcept { return reinterpret_cast<T*>( Elements.data() ) + Index; }
    constexpr const T*       operator[]( std::size_t Index ) const noexcept { return reinterpret_cast<T*>( Elements.data() ) + Index; }

    alignas( T ) std::array<HAKLE_BYTE, sizeof( T ) * BLOCK_SIZE> Elements{};

    HakleBlock* Next{ nullptr };
};

}  // namespace hakle

#endif  // LOCKFREESTRUCTURES_BLOCK_H
