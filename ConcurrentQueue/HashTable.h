//
// Created by wwjszz on 25-11-23.
//

#ifndef HashTable_H
#define HashTable_H

#include <atomic>
#include <concepts>

#include "common/CompressPair.h"
#include "common/allocator.h"
#include "common/common.h"
#include "common/utility.h"

#ifndef HAKLE_USE_CONCEPT
#include <assert.h>
#endif

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
        constexpr static uint32_t Hash( uint32_t Key ) noexcept {
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
        constexpr static uint64_t Hash( uint64_t Key ) noexcept {
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

    template <HAKLE_CONCEPT( std::integral ) T>
    struct Hash {
        constexpr T operator()( T X ) const noexcept { return HashImpl<T>::Hash( X ); }
    };

}  // namespace core

#ifdef HAKLE_USE_CONCEPT
template <class T>
concept AtomicIsLockFree = std::atomic<T>::is_always_lock_free;

template <class T, class Hash>
concept IsSupportHash = requires( T Key, Hash hash ) {
    { hash( Key ) } -> std::integral;
};
#endif

enum class HashTableStatus {
    GET_SUCCESS,
    ADD_SUCCESS,
    FAILED,
};

// TODO: more useful
template <class TKey, HAKLE_CONCEPT( AtomicIsLockFree ) TValue, std::size_t INITIAL_HASH_SIZE, class HashType = core::Hash<TKey>,
          class Allocator = HakleAllocator<Pair<std::atomic<TKey>, std::atomic<TValue>>>>
HAKLE_REQUIRES( IsSupportHash<TKey, HashType> )
class HashTable {
private:
    struct HashNode;

    using PairAllocatorType   = Allocator;
    using PairAllocatorTraits = HakeAllocatorTraits<PairAllocatorType>;

    using NodeAllocatorType   = typename PairAllocatorTraits::template RebindAlloc<HashNode>;
    using NodeAllocatorTraits = typename PairAllocatorTraits::template RebindTraits<HashNode>;

public:
    using Entry = Pair<std::atomic<TKey>, std::atomic<TValue>>;

    explicit constexpr HashTable( TKey InValidKey = TKey{}, const Allocator& InAllocator = Allocator{} ) : PairAllocatorPair( ValueInitTag{}, InAllocator ), INVALID_KEY( InValidKey ) {
#ifndef HAKLE_USE_CONCEPT
        assert( std::atomic<TValue>{}.is_lock_free() );
#endif

        HashNode* Node = CreateNewHashNode( INITIAL_HASH_SIZE );
        MainHash().store( Node, std::memory_order_relaxed );
    }

    HAKLE_CPP20_CONSTEXPR ~HashTable() { Clear(); }

    constexpr HashTable( const HashTable& Other ) = delete;
    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    constexpr HashTable( HashTable&& Other ) noexcept {
        core::SwapRelaxed( EntriesCount, Other.EntriesCount );
        core::SwapRelaxed( MainHash(), Other.MainHash() );
        using std::swap;
        swap( Hash, Other.Hash );
        swap( INVALID_KEY, Other.INVALID_KEY );
    }

    constexpr HashTable& operator=( const HashTable& Other ) = delete;
    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    constexpr HashTable& operator=( HashTable&& Other ) noexcept {
        Clear();
        MainHash().store( nullptr, std::memory_order_relaxed );
        EntriesCount.store( 0, std::memory_order_relaxed );
        swap( Other );
        return *this;
    }

    constexpr void Clear() noexcept {
        auto CurrentHash = MainHash().load( std::memory_order_relaxed );
        while ( CurrentHash != nullptr ) {
            auto Prev = CurrentHash->Prev;
            DeleteHashNode( CurrentHash );
            CurrentHash = Prev;
        }
    }

    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    constexpr void swap( HashTable& Other ) noexcept {
        // can't swap during resizing.
        if ( &Other != this ) {
            core::SwapRelaxed( EntriesCount, Other.EntriesCount );
            core::SwapRelaxed( MainHash(), Other.MainHash() );
        }
    }

    constexpr bool Get( const TKey& Key, TValue& OutValue ) const noexcept {
        HashNode* CurrentMainHash = MainHash().load( std::memory_order_acquire );
        if ( Entry* CurrentEntry = InnerGetEntry( Key, CurrentMainHash ) ) {
            OutValue = CurrentEntry->Second.load( std::memory_order_acquire );
            return true;
        }
        return false;
    }

    constexpr bool Set( const TKey& Key, const TValue& Value ) noexcept {
        HashNode* CurrentMainHash = MainHash().load( std::memory_order_acquire );

        Entry* CurrentEntry = InnerGetEntry( Key, CurrentMainHash );
        if ( CurrentEntry != nullptr ) {
            CurrentEntry->Second.store( Value, std::memory_order_release );
            return true;
        }

        bool AddResult = InnerAdd( Key, Value, CurrentMainHash );
        return AddResult;
    }

    // OutValue will be set when Get is successful
    constexpr HashTableStatus GetOrAdd( const TKey& Key, TValue& OutValue, const TValue& InValue ) {
        HashNode* CurrentMainHash = MainHash().load( std::memory_order_acquire );

        Entry* CurrentEntry = InnerGetEntry( Key, CurrentMainHash );
        if ( CurrentEntry != nullptr ) {
            OutValue = CurrentEntry->Second.load( std::memory_order_acquire );
            return HashTableStatus::GET_SUCCESS;
        }

        bool AddResult = InnerAdd( Key, InValue, CurrentMainHash );
        if ( !AddResult ) {
            return HashTableStatus::FAILED;
        }

        OutValue = InValue;
        return HashTableStatus::ADD_SUCCESS;
    }

    template <class F, class... Args>
    HAKLE_REQUIRES( std::is_pointer_v<TValue> )
    constexpr HashTableStatus GetOrAddByFunc( const TKey& Key, TValue& OutValue, F&& AllocateValueFunc, Args&&... InArgs ) {
        HashNode* CurrentMainHash = MainHash().load( std::memory_order_acquire );

        Entry* CurrentEntry = InnerGetEntry( Key, CurrentMainHash );
        if ( CurrentEntry != nullptr ) {
            OutValue = CurrentEntry->Second.load( std::memory_order_acquire );
            return HashTableStatus::GET_SUCCESS;
        }

        TValue NewValue = AllocateValueFunc( InArgs... );
        if ( NewValue == nullptr ) {
            return HashTableStatus::FAILED;
        }

        bool AddResult = InnerAdd( Key, NewValue, CurrentMainHash );
        if ( !AddResult ) {
            return HashTableStatus::FAILED;
        }

        OutValue = NewValue;
        return HashTableStatus::ADD_SUCCESS;
    }

    HAKLE_NODISCARD constexpr std::size_t GetSize() const noexcept { return EntriesCount.load( std::memory_order_relaxed ); }

private:
    HashNode* CreateNewHashNode( std::size_t InCapacity ) {
        HashNode* NewNode = NodeAllocatorTraits::Allocate( NodeAllocator() );
        NodeAllocatorTraits::Construct( NodeAllocator(), NewNode, InCapacity );
        NewNode->Entries = PairAllocatorTraits::Allocate( PairAllocator(), InCapacity );
        for ( std::size_t i = 0; i < InCapacity; ++i ) {
            PairAllocatorTraits::Construct( PairAllocator(), NewNode->Entries + i );
            NewNode->Entries[ i ].First.store( INVALID_KEY, std::memory_order_relaxed );
        }
        return NewNode;
    }

    void DeleteHashNode( HashNode* Node ) {
        std::size_t Capacity = Node->Capacity;
        for ( std::size_t i = 0; i < Capacity; ++i ) {
            PairAllocatorTraits::Destroy( PairAllocator(), Node->Entries + i );
        }
        PairAllocatorTraits::Deallocate( PairAllocator(), Node->Entries, Node->Capacity );
        NodeAllocatorTraits::Destroy( NodeAllocator(), Node );
        NodeAllocatorTraits::Deallocate( NodeAllocator(), Node, 1 );
    }

    // TODO: add controller
    struct HashNode {
        constexpr HashNode() = default;
        constexpr explicit HashNode( std::size_t InCapacity ) noexcept : Capacity( InCapacity ) {}

        HashNode*   Prev{ nullptr };
        std::size_t Capacity{ 0 };
        Entry*      Entries{ nullptr };
    };

    constexpr Entry* InnerGetEntry( const TKey& Key, HashNode* CurrentMainHash ) const {
        std::size_t HashId = Hash( Key );
        for ( HashNode* CurrentHash = CurrentMainHash; CurrentHash != nullptr; CurrentHash = CurrentHash->Prev ) {
            std::size_t Index = HashId;

            while ( true ) {
                Index &= CurrentHash->Capacity - 1;

                TKey CurrentKey = CurrentHash->Entries[ Index ].First.load( std::memory_order_relaxed );
                if ( CurrentKey == Key ) {
                    TValue CurrentValue = CurrentHash->Entries[ Index ].Second.load( std::memory_order_acquire );

                    if ( CurrentHash != CurrentMainHash ) {
                        Index                          = HashId;
                        const std::size_t MainCapacity = CurrentMainHash->Capacity;

                        while ( true ) {
                            Index &= MainCapacity - 1;
                            auto Empty = INVALID_KEY;
                            if ( CurrentMainHash->Entries[ Index ].First.compare_exchange_strong( Empty, Key, std::memory_order_acquire, std::memory_order_relaxed ) ) {
                                CurrentMainHash->Entries[ Index ].Second.store( CurrentValue, std::memory_order_release );
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

    constexpr bool InnerAdd( const TKey& Key, const TValue& InValue, HashNode* CurrentMainHash ) {
        std::size_t NewCount = EntriesCount.fetch_add( 1, std::memory_order_relaxed );

        while ( true ) {
            if ( NewCount >= ( CurrentMainHash->Capacity >> 1 ) && !HashResizeInProgressFlag().test_and_set( std::memory_order_acquire ) ) {
                CurrentMainHash = MainHash().load( std::memory_order_acquire );
                if ( NewCount < ( CurrentMainHash->Capacity >> 1 ) ) {
                    HashResizeInProgressFlag().clear( std::memory_order_relaxed );
                }
                else {
                    std::size_t NewCapacity = CurrentMainHash->Capacity << 1;
                    while ( NewCount >= NewCapacity >> 1 ) {
                        NewCount <<= 1;
                    }
                    HashNode* NewHash = CreateNewHashNode( NewCapacity );
                    if ( NewHash == nullptr ) {
                        EntriesCount.fetch_sub( 1, std::memory_order_relaxed );
                        return false;
                    }
                    NewHash->Prev = CurrentMainHash;
                    MainHash().store( NewHash, std::memory_order_release );
                    HashResizeInProgressFlag().clear( std::memory_order_release );
                    CurrentMainHash = NewHash;
                }
            }

            // if there is enough space, add the new entry
            if ( NewCount < ( CurrentMainHash->Capacity >> 1 ) + ( CurrentMainHash->Capacity >> 2 ) ) {
                std::size_t HashId = Hash( Key );
                std::size_t Index  = HashId;
                while ( true ) {
                    Index &= CurrentMainHash->Capacity - 1;

                    TKey CurrentKey = CurrentMainHash->Entries[ Index ].First.load( std::memory_order_relaxed );
                    if ( CurrentKey == INVALID_KEY ) {
                        TKey Empty = INVALID_KEY;
                        if ( CurrentMainHash->Entries[ Index ].First.compare_exchange_strong( Empty, Key, std::memory_order_acq_rel, std::memory_order_relaxed ) ) {
                            CurrentMainHash->Entries[ Index ].Second.store( InValue, std::memory_order_release );
                            break;
                        }
                    }

                    ++Index;
                }
                return true;
            }

            CurrentMainHash = MainHash().load( std::memory_order_acquire );
        }
    }

    std::atomic<std::size_t>                                EntriesCount{ 0 };
    CompressPair<std::atomic_flag, PairAllocatorType>       PairAllocatorPair{};
    CompressPair<std::atomic<HashNode*>, NodeAllocatorType> NodeAllocatorPair{};

    // TODO: use compress pair
    HashType Hash{};
    TKey     INVALID_KEY{};

    constexpr PairAllocatorType& PairAllocator() noexcept { return PairAllocatorPair.Second(); }
    constexpr NodeAllocatorType& NodeAllocator() noexcept { return NodeAllocatorPair.Second(); }

    constexpr const PairAllocatorType& PairAllocator() const noexcept { return PairAllocatorPair.Second(); }
    constexpr const NodeAllocatorType& NodeAllocator() const noexcept { return NodeAllocatorPair.Second(); }

    // producer only fields
    constexpr std::atomic_flag&       HashResizeInProgressFlag() noexcept { return PairAllocatorPair.First(); }
    constexpr std::atomic<HashNode*>& MainHash() noexcept { return NodeAllocatorPair.First(); }

    HAKLE_NODISCARD constexpr const std::atomic_flag&       HashResizeInProgressFlag() const noexcept { return PairAllocatorPair.First(); }
    HAKLE_NODISCARD constexpr const std::atomic<HashNode*>& MainHash() const noexcept { return NodeAllocatorPair.First(); }
};

}  // namespace hakle

#endif  // HashTable_H
