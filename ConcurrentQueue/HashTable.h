//
// Created by wwjszz on 25-11-23.
//

#ifndef HashTable_H
#define HashTable_H

#include <array>
#include <assert.h>
#include <atomic>
#include <functional>

#include "common/memory.h"

namespace hakle {

namespace core {
    template <class T>
    inline static constexpr void SwapRelaxed( std::atomic<T>& Left, std::atomic<T>& Right ) noexcept {
        T Temp = Left.load( std::memory_order_relaxed );
        Left.store( Right.load( std::memory_order_relaxed ), std::memory_order_relaxed );
        Right.store( Temp, std::memory_order_relaxed );
    }

    template <short N>
    struct HashDispatch {
        static_assert( N == 4 || N == 8, "hakle::core::Hash only supports 32 and 64-bit types" );
    };

    template <>
    struct HashDispatch<4> {
        static uint32_t Hash( uint32_t Key ) noexcept {
            // MurmurHash3 finalizer -- see https://code.google.com/p/smhasher/source/browse/trunk/MurmurHash3.cpp
            // Since the thread ID is already unique, all we really want to do is propagate that
            // uniqueness evenly across all the bits, so that we can use a subset of the bits while
            // reducing collisions significantly
            Key ^= Key >> 16;
            Key *= 0x85ebca6b;
            Key ^= Key >> 13;
            Key *= 0xc2b2ae35;
            return Key ^ ( Key >> 16 );
        }
    };

    template <>
    struct HashDispatch<8> {
        static uint64_t Hash( uint64_t Key ) noexcept {
            Key ^= Key >> 33;
            Key *= 0xff51afd7ed558ccd;
            Key ^= Key >> 33;
            Key *= 0xc4ceb9fe1a85ec53;
            return Key ^ ( Key >> 33 );
        }
    };

    template <class T>
    struct HashImpl : HashDispatch<sizeof( T )> {
        static_assert( std::is_integral<T>::value, "HashImpl<T> only supports integral types" );
    };

    template <class T>
        requires std::is_integral_v<T>
    T Hash( T Key ) noexcept {
        return HashImpl<T>::Hash( Key );
    }

}  // namespace core

// NOTE: value should be controlled by user
// TODO: more useful
template <class TKey, class TValue, TKey INVALID_KEY, std::size_t INITIAL_HASH_SIZE>
class HashTable {
public:
    HashTable() {
        HashNode* Temp = HAKLE_NEW( HashNode, INITIAL_HASH_SIZE );
        if ( !Temp ) {
            throw std::bad_alloc();
        }
        MainHash.store( Temp, std::memory_order_relaxed );
    }

    ~HashTable() {
        auto CurrentHash = MainHash.load( std::memory_order_relaxed );
        while ( CurrentHash != nullptr ) {
            auto Prev = CurrentHash->Prev;
            HAKLE_DELETE( CurrentHash );
            CurrentHash = Prev;
        }
    }

    HashTable( const HashTable& Other ) = delete;
    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    HashTable( HashTable&& Other ) noexcept {
        assert( !Other.HashResizeInProgressFlag.test( std::memory_order_acquire ) );
        core::SwapRelaxed( EntriesCount, Other.EntriesCount );
        core::SwapRelaxed( MainHash, Other.MainHash );
    }

    HashTable& operator=( const HashTable& Other ) = delete;
    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    HashTable& operator=( HashTable&& Other ) noexcept {
        swap( Other );
        return *this;
    }

    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    void swap( HashTable& Other ) noexcept {
        // can't swap during resizing.
        assert( !HashResizeInProgressFlag.test( std::memory_order_acquire ) && !Other.HashResizeInProgressFlag.test( std::memory_order_acquire ) );
        if ( &Other != this ) {
            core::SwapRelaxed( EntriesCount, Other.EntriesCount );
            core::SwapRelaxed( MainHash, Other.MainHash );
        }
    }

    TValue* Get( const TKey& Key ) {
        HashNode* CurrentMainHash = MainHash.load( std::memory_order_acquire );
        Entry* CurrentEntry = InnerGetEntry( Key, CurrentMainHash );
        return CurrentEntry ? CurrentEntry->Value : nullptr;
    }

    // TODO: support delete or update
    template <class F, class... Args>
    TValue* GetOrAddByFunc( const TKey& Key, F&& CreateValueFunc, Args&&... InArgs ) {
        // TODO: figure out memory_order
        std::size_t NewCount        = EntriesCount.fetch_add( 1, std::memory_order_relaxed );
        HashNode*   CurrentMainHash = MainHash.load( std::memory_order_acquire );

        Entry* CurrentEntry = InnerGetEntry( Key, CurrentMainHash );
        if ( CurrentEntry != nullptr ) {
            return CurrentEntry->Value;
        }

        while ( true ) {
            if ( NewCount >= ( CurrentMainHash->Capacity >> 1 ) && !HashResizeInProgressFlag.test_and_set( std::memory_order_acquire ) ) {
                CurrentMainHash = MainHash.load( std::memory_order_acquire );
                if ( NewCount < ( CurrentMainHash->Capacity >> 1 ) ) {
                    HashResizeInProgressFlag.clear( std::memory_order_relaxed );
                }
                else {
                    std::size_t NewCapacity = CurrentMainHash->Capacity << 1;
                    while ( NewCount >= NewCapacity >> 1 ) {
                        NewCount <<= 1;
                    }
                    HashNode* NewHash = HAKLE_NEW( HashNode, NewCapacity );
                    NewHash->Prev     = CurrentMainHash;
                    MainHash.store( NewHash, std::memory_order_release );
                    HashResizeInProgressFlag.clear( std::memory_order_release );
                    CurrentMainHash = NewHash;
                }
            }

            // if there is enough space, add the new entry
            if ( NewCount < ( CurrentMainHash->Capacity >> 1 ) + ( CurrentMainHash->Capacity >> 2 ) ) {
                TValue* NewValue = CreateValueFunc( std::forward<Args>( InArgs )... );
                if ( NewValue == nullptr ) {
                    EntriesCount.fetch_sub( 1, std::memory_order_relaxed );
                    return nullptr;
                }
                std::size_t HashId = core::HashImpl<TKey>::Hash( Key );
                std::size_t Index  = HashId;
                while ( true ) {
                    Index &= CurrentMainHash->Capacity - 1;

                    TKey CurrentKey = CurrentMainHash->Entries[ Index ].Key.load( std::memory_order_relaxed );
                    if ( CurrentKey == INVALID_KEY ) {
                        if ( TKey Empty = INVALID_KEY; CurrentMainHash->Entries[ Index ].Key.compare_exchange_strong(
                                 Empty, Key, std::memory_order_acq_rel, std::memory_order_relaxed ) ) {
                            // TODO: error in here
                            CurrentMainHash->Entries[ Index ].Value = NewValue;
                            break;
                        }
                    }

                    ++Index;
                }
                return NewValue;
            }

            CurrentMainHash = MainHash.load( std::memory_order_acquire );
        }
    }

private:
    struct HashNode {
        HashNode() = default;
        explicit HashNode( std::size_t InCapacity ) : Capacity( InCapacity ) {
            Entries = HAKLE_OPERATOR_NEW_ARRAY( Entry, InCapacity );
            for ( std::size_t i = 0; i < InCapacity; ++i ) {
                new ( Entries + i ) Entry();
                Entries[ i ].Key.store( INVALID_KEY, std::memory_order_relaxed );
            }
        }

        ~HashNode() { HAKLE_DELETE_ARRAY( Entries, Capacity ); }

        struct Entry {
            Entry() = default;
            Entry( Entry&& Other ) noexcept {
                Key.store( Other.Key.load( std::memory_order_relaxed ), std::memory_order_relaxed );
                Value = Other.Value;
            }

            Entry& operator=( Entry&& Other ) noexcept {
                swap( Other );
                return *this;
            }

            void swap( Entry& Other ) noexcept {
                if ( &Other != this ) {
                    core::SwapRelaxed( Key, Other.Key );
                    std::swap( Value, Other.Value );
                }
            }

            std::atomic<TKey> Key{ INVALID_KEY };
            TValue*           Value{ nullptr };
        };

        HashNode*   Prev{ nullptr };
        std::size_t Capacity{ 0 };
        Entry*      Entries{ nullptr };
    };

    using Entry = typename HashNode::Entry;

    Entry* InnerGetEntry( const TKey& Key, HashNode* CurrentMainHash ) {
        assert( CurrentMainHash != nullptr );

        std::size_t HashId = core::HashImpl<TKey>::Hash( Key );
        for ( HashNode* CurrentHash = CurrentMainHash; CurrentHash != nullptr; CurrentHash = CurrentHash->Prev ) {
            std::size_t Index = HashId;

            while ( true ) {
                Index &= CurrentHash->Capacity - 1;

                TKey CurrentKey = CurrentHash->Entries[ Index ].Key.load( std::memory_order_relaxed );
                if ( CurrentKey == Key ) {
                    TValue* CurrentValue = CurrentHash->Entries[ Index ].Value;

                    if ( CurrentHash != CurrentMainHash ) {
                        Index                          = HashId;
                        const std::size_t MainCapacity = CurrentMainHash->Capacity;

                        while ( true ) {
                            Index &= MainCapacity - 1;

                            // TODO: figure out memory_order
                            if ( auto Empty = INVALID_KEY; CurrentMainHash->Entries[ Index ].Key.compare_exchange_strong(
                                     Empty, Key, std::memory_order_acquire, std::memory_order_relaxed ) ) {
                                CurrentMainHash->Entries[ Index ].Value = CurrentValue;
                                break;
                            }
                            ++Index;
                        }
                    }

                    return &CurrentMainHash->Entries[ Index ];
                }
                if ( CurrentKey == INVALID_KEY ) {
                    break;
                }
                ++Index;
            }
        }
        return nullptr;
    }

    std::atomic<std::size_t> EntriesCount{ 0 };
    std::atomic<HashNode*>   MainHash{ nullptr };
    std::atomic_flag         HashResizeInProgressFlag{};
};

}  // namespace hakle

#endif  // HashTable_H
