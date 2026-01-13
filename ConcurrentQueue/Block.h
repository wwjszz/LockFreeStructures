//
// Created by admin on 2025/12/12.
//

#ifndef LOCKFREESTRUCTURES_BLOCK_H
#define LOCKFREESTRUCTURES_BLOCK_H

#include <array>
#include <atomic>

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
concept IsBlock = requires {
    typename T::ValueType;
    { T::HasMeaningfulSetResult } -> std::convertible_to<bool>;
    { T::BlockSize } -> std::convertible_to<std::size_t>;
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

template <class T, std::size_t BLOCK_SIZE>
using HakleFlagsBlock = HakleBlock<T, BLOCK_SIZE, FlagsCheckPolicy<BLOCK_SIZE>>;

template <class T, std::size_t BLOCK_SIZE>
using HakleCounterBlock = HakleBlock<T, BLOCK_SIZE, CounterCheckPolicy<BLOCK_SIZE>>;

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

enum class BlockMethod { Flags, Counter };

template <class T, std::size_t BLOCK_SIZE, HAKLE_CONCEPT( IsPolicy ) Policy>
struct HakleBlock : FreeListNode<HakleBlock<T, BLOCK_SIZE, Policy>>, Policy {
    using ValueType = T;
    using BlockType = HakleBlock;
    using Policy::HasMeaningfulSetResult;
    constexpr static std::size_t BlockSize = BLOCK_SIZE;

    constexpr T*       operator[]( std::size_t Index ) noexcept { return reinterpret_cast<T*>( Elements.data() ) + Index; }
    constexpr const T* operator[]( std::size_t Index ) const noexcept { return reinterpret_cast<T*>( Elements.data() ) + Index; }

    alignas( T ) std::array<HAKLE_BYTE, sizeof( T ) * BLOCK_SIZE> Elements{};

    HakleBlock* Next{ nullptr };
};

}  // namespace hakle

#endif  // LOCKFREESTRUCTURES_BLOCK_H
