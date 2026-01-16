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
HAKLE_REQUIRES( std::is_unsigned_v<T> )
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

template <class T1, class T2>
struct Pair {
    using FirstType  = T1;
    using SecondType = T2;

#if HAKLE_CPP_VERSION >= 20
    struct CheckArgs {
        static constexpr bool EnableDefault = std::is_default_constructible<FirstType>::value && std::is_default_constructible<SecondType>::value;

        template <class U1, class U2>
        static constexpr bool IsPairConstructible = std::is_constructible<FirstType, U1>::value && std::is_constructible<SecondType, U2>::value;

        template <class U1, class U2>
        static constexpr bool IsImplicit = std::is_constructible<U1, FirstType>::value && std::is_constructible<U2, SecondType>::value;
    };
#endif

    constexpr Pair() HAKLE_REQUIRES( CheckArgs::EnableDefault ) = default;
    HAKLE_CPP20_CONSTEXPR ~Pair()                               = default;

    constexpr Pair( const Pair& Other ) = default;
    constexpr Pair( Pair&& Other )      = default;

    constexpr Pair( const T1& t1, const T2& t2 ) noexcept( std::is_nothrow_copy_constructible<FirstType>::value && std::is_nothrow_copy_constructible<FirstType>::value )
        HAKLE_REQUIRES( CheckArgs::template IsPairConstructible<const T1&, const T2&> )
        : First( t1 ), Second( t2 ) {}

    template <class U1, class U2>
    HAKLE_REQUIRES( CheckArgs::template IsPairConstructible<U1, U2> )
    constexpr Pair( U1&& u1, U2&& u2 ) noexcept( std::is_nothrow_constructible<FirstType, U1>::value && std::is_nothrow_constructible<SecondType, U2>::value )
        : First( std::forward<U1>( u1 ) ), Second( std::forward<U2>( u2 ) ) {}

    template <class U1, class U2>
    HAKLE_REQUIRES( CheckArgs::template IsPairConstructible<const U1&, const U2&> )
    constexpr HAKLE_CPP20_EXPLICIT( !CheckArgs::template IsImplicit<const U1&, const U2&> )
        Pair( const Pair<U1, U2>& p ) noexcept( std::is_nothrow_constructible<FirstType, const U1&>::value && std::is_nothrow_constructible<SecondType, const U2&>::value )
        : First( p.First ), Second( p.Second ) {}

    template <class U1, class U2>
    HAKLE_REQUIRES( CheckArgs::template IsPairConstructible<U1, U2> )
    constexpr HAKLE_CPP20_EXPLICIT( !CheckArgs::template IsImplicit<U1, U2> )
        Pair( Pair<U1, U2>&& p ) noexcept( std::is_nothrow_constructible<FirstType, U1>::value && std::is_nothrow_constructible<SecondType, U2>::value )
        : First( std::forward<U1>( p.First ) ), Second( std::forward<U2>( p.Second ) ) {}

    constexpr Pair& operator=( const Pair& p ) noexcept( std::is_nothrow_assignable<FirstType&, const T1&>::value && std::is_nothrow_assignable<SecondType&, const T2&>::value )
        HAKLE_REQUIRES( std::is_copy_assignable_v<FirstType>&& std::is_copy_assignable_v<SecondType> ) {
        First  = p.First;
        Second = p.Second;
        return *this;
    }

    constexpr Pair& operator=( Pair&& p ) noexcept HAKLE_REQUIRES( std::is_move_assignable_v<FirstType>&& std::is_move_assignable_v<SecondType> ) {
        First  = std::forward<FirstType>( p.First );
        Second = std::forward<SecondType>( p.Second );
        return *this;
    }

    template <class U1, class U2>
    HAKLE_REQUIRES( std::is_assignable_v<FirstType&, const U1&>&& std::is_assignable_v<SecondType&, const U2&> )
    constexpr Pair& operator=( const Pair<U1, U2>& p ) noexcept( std::is_nothrow_assignable<FirstType&, const U1&>::value && std::is_nothrow_assignable<SecondType&, const U2&>::value ) {
        First  = p.First;
        Second = p.Second;
        return *this;
    }

    template <class U1, class U2>
    HAKLE_REQUIRES( std::is_assignable_v<FirstType&, U1>&& std::is_assignable_v<SecondType&, U2> )
    constexpr Pair& operator=( Pair<U1, U2>&& p ) noexcept( std::is_nothrow_assignable<FirstType&, U1>::value && std::is_nothrow_assignable<SecondType&, U2>::value ) {
        First  = std::forward<U1>( p.First );
        Second = std::forward<U2>( p.Second );
        return *this;
    }

    constexpr void swap( Pair& p ) noexcept( std::is_nothrow_swappable<FirstType>::value && std::is_nothrow_swappable<SecondType>::value )
        HAKLE_REQUIRES( std::is_swappable_v<FirstType>&& std::is_swappable_v<SecondType> ) {
        using std::swap;
        swap( First, p.First );
        swap( Second, p.Second );
    }

    T1 First;
    T2 Second;
};

#if HAKLE_CPP_VERSION >= 17
template <class T1, class T2>
Pair( T1, T2 ) -> Pair<T1, T2>;
#endif

template <class T1, class T2, class U1, class U2>
inline constexpr bool operator==( const Pair<T1, T2>& x, const Pair<U1, U2>& y ) {
    return x.First == y.First && x.Second == y.Second;
}

template <class T1, class T2, class U1, class U2>
inline constexpr bool operator!=( const Pair<T1, T2>& x, const Pair<U1, U2>& y ) {
    return !( x == y );
}

template <class T1, class T2, class U1, class U2>
inline constexpr bool operator<( const Pair<T1, T2>& x, const Pair<U1, U2>& y ) {
    return x.First < y.First || ( !( y.First < x.First ) && x.Second < y.Second );
}

template <class T1, class T2, class U1, class U2>
inline constexpr bool operator>( const Pair<T1, T2>& x, const Pair<U1, U2>& y ) {
    return y < x;
}

template <class T1, class T2, class U1, class U2>
inline constexpr bool operator<=( const Pair<T1, T2>& x, const Pair<U1, U2>& y ) {
    return !( y < x );
}

template <class T1, class T2, class U1, class U2>
inline constexpr bool operator>=( const Pair<T1, T2>& x, const Pair<U1, U2>& y ) {
    return !( x < y );
}

template <class T1, class T2>
inline constexpr void swap( const Pair<T1, T2>& x, const Pair<T1, T2>& y ) noexcept( noexcept( x.swap( y ) ) ) {
    x.swap( y );
}

}  // namespace hakle

#endif  // UTILITY_H
