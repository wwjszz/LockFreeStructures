//
// Created by admin on 25-11-20.
//

#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <array>

#include "../ReaderWriterQueue/readerwriterqueue.h"

namespace hakle {

namespace samples {
    template <std::size_t N>
    class HashTable {
        static_assert( N != 0 && ( N & N - 1 ) == 0, "N must be a power of 2" );

    public:
        void SetItem( uint32_t InKey, uint32_t InValue ) noexcept {
            assert( InKey != 0 );
            assert( InValue != 0 );

            for ( uint32_t idx = IntegerHash( InKey );; ++idx ) {
                idx &= ( N - 1 );
                HashTableEntry& Entry = Data[ idx ];

                if ( uint32_t CurrentKey = Entry.Key.Load(); CurrentKey != InKey ) {
                    if ( CurrentKey != 0 )
                        continue;

                    if ( !Entry.Key.CompareExchangeStrong( CurrentKey, InKey ) && CurrentKey != 0 && CurrentKey != InKey )
                        continue;
                }
                Entry.Value.Store( InValue );
                return;
            }
        }

        [[nodiscard]] int GetItem( uint32_t InKey ) const noexcept {
            assert( InKey != 0 );

            for ( uint32_t idx = IntegerHash( InKey );; ++idx ) {
                idx &= ( N - 1 );
                const HashTableEntry& Entry = Data[ idx ];

                uint32_t CurrentKey = Entry.Key.Load();
                if ( CurrentKey == InKey )
                    return Entry.Value.Load();
                if ( CurrentKey == 0 )
                    break;
            }
            return 0;
        }

    private:
        static uint32_t IntegerHash( uint32_t h ) noexcept {
            h ^= h >> 16;
            h *= 0x85ebca6b;
            h ^= h >> 13;
            h *= 0xc2b2ae35;
            h ^= h >> 16;
            return h;
        }

        struct HashTableEntry {
            WeakAtomic<uint32_t> Key;
            WeakAtomic<uint32_t> Value;
        };

        std::array<HashTableEntry, N> Data;
    };
}  // namespace samples

}  // namespace hakle

#endif  // HASHTABLE_H