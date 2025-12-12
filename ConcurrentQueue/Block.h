//
// Created by admin on 2025/12/12.
//

#ifndef LOCKFREESTRUCTURES_BLOCK_H
#define LOCKFREESTRUCTURES_BLOCK_H
#include "BlockManager.h"

#include <array>
#include <atomic>

namespace hakle {

template <class T, std::size_t BLOCK_SIZE, class Policy>
struct HakleBlock;

template <std::size_t BLOCK_SIZE>
struct FlagsCheckPolicy;

template <std::size_t BLOCK_SIZE>
struct CounterCheckPolicy;

template <class T, std::size_t BLOCK_SIZE>
using HakleFlagsBlock = HakleBlock<T, BLOCK_SIZE, FlagsCheckPolicy<BLOCK_SIZE>>;

template <class T, std::size_t BLOCK_SIZE>
using HakleCounterBlock = HakleBlock<T, BLOCK_SIZE, CounterCheckPolicy<BLOCK_SIZE>>;

struct BlockCheckPolicy {
    virtual constexpr ~BlockCheckPolicy() = default;

    [[nodiscard]] virtual bool IsEmpty() const                                      = 0;
    virtual constexpr bool     SetEmpty( std::size_t Index )                        = 0;
    virtual constexpr bool     SetSomeEmpty( std::size_t Index, std::size_t Count ) = 0;
    virtual constexpr void     SetAllEmpty()                                        = 0;
    virtual constexpr void     Reset()                                              = 0;
};

template <std::size_t BLOCK_SIZE>
struct FlagsCheckPolicy : BlockCheckPolicy {
    constexpr ~FlagsCheckPolicy() override = default;

    [[nodiscard]] constexpr bool IsEmpty() const override {
        for ( auto& Flag : Flags ) {
            if ( !Flag.load( std::memory_order_relaxed ) ) {
                return false;
            }
        }

        std::atomic_thread_fence( std::memory_order_acquire );
        return true;
    }

    constexpr bool SetEmpty( std::size_t Index ) override {
        Flags[ Index ].store( 1, std::memory_order_release );
        return false;
    }

    constexpr bool SetSomeEmpty( std::size_t Index, std::size_t Count ) override {
        std::atomic_thread_fence( std::memory_order_release );

        for ( std::size_t i = 0; i < Count; ++i ) {
            Flags[ Index + i ].store( 1, std::memory_order_relaxed );
        }
        return false;
    }

    constexpr void SetAllEmpty() override {
        for ( std::size_t i = 0; i < BLOCK_SIZE; ++i ) {
            Flags[ i ].store( 1, std::memory_order_relaxed );
        }
    }

    constexpr void Reset() override {
        for ( auto& Flag : Flags ) {
            Flag.store( 0, std::memory_order_relaxed );
        }
    }

    std::array<std::atomic<uint8_t>, BLOCK_SIZE> Flags;
};

template <std::size_t BLOCK_SIZE>
struct CounterCheckPolicy : BlockCheckPolicy {
    constexpr ~CounterCheckPolicy() override = default;

    [[nodiscard]] constexpr bool IsEmpty() const override {
        if ( Counter.load( std::memory_order_relaxed ) == BLOCK_SIZE ) {
            std::atomic_thread_fence( std::memory_order_acquire );
            return true;
        }
        return false;
    }

    // Increments the counter and returns true if the block is now empty
    constexpr bool SetEmpty( [[maybe_unused]] std::size_t Index ) override {
        std::size_t OldCounter = Counter.fetch_add( 1, std::memory_order_release );
        return OldCounter + 1 == BLOCK_SIZE;
    }

    constexpr bool SetSomeEmpty( [[maybe_unused]] std::size_t Index, std::size_t Count ) override {
        std::size_t OldCounter = Counter.fetch_add( Count, std::memory_order_release );
        return OldCounter + Count == BLOCK_SIZE;
    }

    constexpr void SetAllEmpty() override { Counter.store( BLOCK_SIZE, std::memory_order_relaxed ); }

    constexpr void Reset() override { Counter.store( 0, std::memory_order_relaxed ); }

    std::atomic<std::size_t> Counter;
};

enum class BlockMethod { Flags, Counter };

template <class T, std::size_t BLOCK_SIZE, class Policy>
struct HakleBlock : FreeListNode<HakleBlock<T, BLOCK_SIZE, Policy>>, Policy {
    using ValueType                        = T;
    using BlockType                        = HakleBlock;
    constexpr static std::size_t BlockSize = BLOCK_SIZE;

    constexpr T*       operator[]( std::size_t Index ) noexcept { return reinterpret_cast<T*>( Elements.data() ) + Index; }
    constexpr const T* operator[]( std::size_t Index ) const noexcept { return reinterpret_cast<T*>( Elements.data() ) + Index; }

    alignas( T ) std::array<std::byte, sizeof( T ) * BLOCK_SIZE> Elements{};
    HakleBlock* Next{ nullptr };
};

}  // namespace hakle

#endif  // LOCKFREESTRUCTURES_BLOCK_H
