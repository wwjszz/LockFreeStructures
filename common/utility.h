//
// Created by admin on 25-12-1.
//

#ifndef UTILITY_H
#define UTILITY_H

#include <climits>
#include <type_traits>

#include "common/common.h"

namespace hakle {

template <class T>
#if HAKLE_CPP_VERSION >= 20
    requires std::is_unsigned_v<T>
#endif
inline constexpr bool CircularLessThan( T a, T b ) noexcept {
    return static_cast<T>( a - b ) > static_cast<T>( static_cast<T>( 1 ) << ( static_cast<T>( sizeof( T ) * CHAR_BIT - 1 ) ) );
}

inline std::size_t CeilToPow2( std::size_t X ) noexcept {
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

// TODO: not noly for 64 bit or 32 bit and optimize it
// used to calculate log2
inline constexpr uint8_t BitWidth( std::size_t X ) noexcept {
    uint8_t Count = 0;
    HAKLE_CONSTEXPR_IF( sizeof( std::size_t ) > 4 ) {
        if ( X >> 32 ) {
            Count += 32;
            X >>= 32;
        }
    }

    if ( X >> 16 ) {
        Count += 16;
        X >>= 16;
    }

    if ( X >> 8 ) {
        Count += 8;
        X >>= 8;
    }

    if ( X >> 4 ) {
        Count += 4;
        X >>= 4;
    }

    if ( X >> 2 ) {
        Count += 2;
        X >>= 2;
    }

    if ( X >> 1 ) {
        Count += 1;
        X >>= 1;
    }

    return Count + X;
}

}  // namespace hakle

#endif  // UTILITY_H
