//
// Created by wwjszz on 25-11-23.
//

#ifndef HashTable_H
#define HashTable_H
#include <array>
#include <assert.h>
#include <atomic>

namespace hakle {

namespace core {
    template <class T>
    inline static constexpr void SwapRelaxed( std::atomic<T>& Left, std::atomic<T>& Right ) noexcept {
        T Temp = Left.load( std::memory_order_relaxed );
        Left.store( Right.load( std::memory_order_relaxed ), std::memory_order_relaxed );
        Right.store( Temp, std::memory_order_relaxed );
    }

    template <class T>
    struct HashImpl {};

    template <>
    struct HashImpl<uint32_t> {
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
    struct HashImpl<uint64_t> {
        static uint64_t Hash( uint64_t Key ) noexcept {
            Key ^= Key >> 33;
            Key *= 0xff51afd7ed558ccd;
            Key ^= Key >> 33;
            Key *= 0xc4ceb9fe1a85ec53;
            return Key ^ ( Key >> 33 );
        }
    };

}  // namespace core

template <class TKey, class TValue, std::size_t INITIAL_HASH_SIZE , TKey INVALID_KEY>
class HashTable {
public:
    HashTable() {
        MainHash = new HashImpl<INITIAL_HASH_SIZE>();
    }
    ~HashTable() = default;

    HashTable( const HashTable& Other ) = delete;
    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    HashTable( HashTable&& Other ) noexcept {
        assert( !Other.HashResizeInProgressFlag.test( std::memory_order_acquire ) );
        core::SwapRelaxed( CurrentEntriesCount, Other.CurrentEntriesCount );
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
            core::SwapRelaxed( CurrentEntriesCount, Other.CurrentEntriesCount );
            core::SwapRelaxed( MainHash, Other.MainHash );
        }
    }

    TValue* Get( const TKey& Key ) {
        std::size_t HashId = core::HashImpl<TKey>::Hash( Key );

        Hash* CurrentMainHash = MainHash.load( std::memory_order_acquire );
        assert( CurrentMainHash != nullptr );
        for ( Hash* TempHash = CurrentMainHash; TempHash != nullptr; TempHash = TempHash->Prev ) {
            std::size_t       Index    = HashId;
            const std::size_t Capacity = TempHash->GetCapacity();

            while ( true ) {
                Index &= Capacity - 1;

                TKey CurrentKey = TempHash->GetEntry( Index ).Key;
                if ( CurrentKey == Index ) {
                    TValue* CurrentValue = TempHash->GetEntry( Index ).Value;

                    if ( TempHash != CurrentMainHash ) {
                        Index                                     = HashId;
                        const std::size_t MainCapacity        = CurrentMainHash->GetCapacity();

                        while ( true ) {
                            Index &= MainCapacity - 1;

                            // TODO: figure out memory_order
                            if ( auto Empty = INVALID_KEY; CurrentMainHash->GetEntry( Index ).Key.compare_exchange_strong(
                                     Empty, Key, std::memory_order_acq_rel, std::memory_order_relaxed ) ) {
                                CurrentMainHash->GetEntry( Index ).Value = CurrentValue;
                                break;
                            }
                            ++Index;
                        }
                    }

                    return CurrentValue;
                }
                if ( CurrentKey == INVALID_KEY ) {
                    break;
                }
                ++Index;
            }
        }
        return nullptr;
    }

private:
    struct Hash {
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

        virtual ~Hash()                                            = default;
        [[nodiscard]] virtual constexpr std::size_t GetCapacity() const noexcept = 0;
        [[nodiscard]] virtual constexpr Entry& GetEntry(std::size_t Index) noexcept = 0;

        Hash* Prev{ nullptr };
    };

    template <std::size_t N>
    struct HashImpl final : Hash {
        using Entry = typename Hash::Entry;

        [[nodiscard]] constexpr std::size_t GetCapacity() const noexcept override { return N; }
        [[nodiscard]] constexpr Entry& GetEntry( std::size_t Index ) noexcept override {
            assert( Index < N );
            return Entries[ Index ];
        }

        ~HashImpl() override = default;
        std::array<Entry, N> Entries;
    };

    std::atomic<std::size_t> CurrentEntriesCount{ 0 };
    std::atomic<Hash*>       MainHash{ nullptr };
    std::atomic_flag         HashResizeInProgressFlag;
};

}  // namespace hakle

#endif  // HashTable_H
