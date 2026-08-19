//
// Created by admin on 25-12-1.
//

#ifndef UTILITY_H
#define UTILITY_H

#include <atomic>
#include <climits>
#include <concepts>
#include <type_traits>

#include "common/common.h"

namespace hakle {

#define HAKLE_SEM ;

// Support up to 10 arguments with fixed-arity macro expansion.
#define HAKLE_EXPAND_10( macro, concat, first, ... ) macro( first ) concat HAKLE_EXPAND_9( macro, concat, __VA_ARGS__ )
#define HAKLE_EXPAND_9( macro, concat, first, ... ) macro( first ) concat HAKLE_EXPAND_8( macro, concat, __VA_ARGS__ )
#define HAKLE_EXPAND_8( macro, concat, first, ... ) macro( first ) concat HAKLE_EXPAND_7( macro, concat, __VA_ARGS__ )
#define HAKLE_EXPAND_7( macro, concat, first, ... ) macro( first ) concat HAKLE_EXPAND_6( macro, concat, __VA_ARGS__ )
#define HAKLE_EXPAND_6( macro, concat, first, ... ) macro( first ) concat HAKLE_EXPAND_5( macro, concat, __VA_ARGS__ )
#define HAKLE_EXPAND_5( macro, concat, first, ... ) macro( first ) concat HAKLE_EXPAND_4( macro, concat, __VA_ARGS__ )
#define HAKLE_EXPAND_4( macro, concat, first, ... ) macro( first ) concat HAKLE_EXPAND_3( macro, concat, __VA_ARGS__ )
#define HAKLE_EXPAND_3( macro, concat, first, ... ) macro( first ) concat HAKLE_EXPAND_2( macro, concat, __VA_ARGS__ )
#define HAKLE_EXPAND_2( macro, concat, first, ... ) macro( first ) concat HAKLE_EXPAND_1( macro, concat, __VA_ARGS__ )
#define HAKLE_EXPAND_1( macro, concat, first ) macro( first )
#define HAKLE_EXPAND_0( macro, concat )

#define HAKLE_EXPAND_COMMA_10( macro, first, ... ) macro( first ), HAKLE_EXPAND_COMMA_9( macro, __VA_ARGS__ )
#define HAKLE_EXPAND_COMMA_9( macro, first, ... ) macro( first ), HAKLE_EXPAND_COMMA_8( macro, __VA_ARGS__ )
#define HAKLE_EXPAND_COMMA_8( macro, first, ... ) macro( first ), HAKLE_EXPAND_COMMA_7( macro, __VA_ARGS__ )
#define HAKLE_EXPAND_COMMA_7( macro, first, ... ) macro( first ), HAKLE_EXPAND_COMMA_6( macro, __VA_ARGS__ )
#define HAKLE_EXPAND_COMMA_6( macro, first, ... ) macro( first ), HAKLE_EXPAND_COMMA_5( macro, __VA_ARGS__ )
#define HAKLE_EXPAND_COMMA_5( macro, first, ... ) macro( first ), HAKLE_EXPAND_COMMA_4( macro, __VA_ARGS__ )
#define HAKLE_EXPAND_COMMA_4( macro, first, ... ) macro( first ), HAKLE_EXPAND_COMMA_3( macro, __VA_ARGS__ )
#define HAKLE_EXPAND_COMMA_3( macro, first, ... ) macro( first ), HAKLE_EXPAND_COMMA_2( macro, __VA_ARGS__ )
#define HAKLE_EXPAND_COMMA_2( macro, first, ... ) macro( first ), HAKLE_EXPAND_COMMA_1( macro, __VA_ARGS__ )
#define HAKLE_EXPAND_COMMA_1( macro, first ) macro( first )
#define HAKLE_EXPAND_COMMA_0( macro )

#define HAKLE_ARGS_COUNT( ... ) HAKLE_ARGS_COUNT_IMPL( __VA_ARGS__, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 )
#define HAKLE_ARGS_COUNT_IMPL( _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, N, ... ) N

#define HAKLE_CONCAT_( a, b ) a##b
#define HAKLE_CONCAT( a, b ) HAKLE_CONCAT_( a, b )

#define HAKLE_FOR_EACH( macro, concat, ... ) HAKLE_CONCAT( HAKLE_EXPAND_, HAKLE_ARGS_COUNT( __VA_ARGS__ ) )( macro, concat, __VA_ARGS__ )
#define HAKLE_FOR_EACH_COMMA( macro, ... ) HAKLE_CONCAT( HAKLE_EXPAND_COMMA_, HAKLE_ARGS_COUNT( __VA_ARGS__ ) )( macro, __VA_ARGS__ )

#if HAKLE_CPP_VERSION >= 20
#define HAKLE_SWAP_REQUIES     \
    HAKLE_REQUIRES( requires { \
        { lhs.swap( rhs ) };   \
    } )
#endif

#define HAKLE_MOVE_ATOMIC( X ) X( std::move( Other.X.load( std::memory_order_relaxed ) ) )
#define HAKLE_MOVE( X ) X( std::move( Other.X ) )
#define HAKLE_MOVE_PAIR_ATOMIC1( X ) X( std::move( Other.X.First().load( std::memory_order_relaxed ) ), std::move( Other.X.Second() ) )
#define HAKLE_OP_MOVE( X ) X = std::move( Other.X )
#define HAKLE_OP_MOVE_ATOMIC( X ) X.store( std::move( Other.X.load( std::memory_order_relaxed ) ), std::memory_order_relaxed )
#define HAKLE_OP_MOVE_ELEM( X ) X() = std::move( Other.X() )
#define HAKLE_OP_MOVE_ATOMIC_ELEM( X ) X().store( std::move( Other.X().load( std::memory_order_relaxed ) ), std::memory_order_relaxed )
#define HAKLE_SWAP( X ) swap( X, Other.X )
#define HAKLE_SWAP_ATOMIC( X ) core::SwapRelaxed( X, Other.X )

#if HAKLE_CPP_VERSION < 17

#if HAKLE_CPP_VERSION < 14
template <class T>
struct Identity {
    using Type = T;
};

template <class T, T... Ts>
struct IntegerSequence {
    using Type                        = T;
    static constexpr std::size_t Size = sizeof...( Ts );
};

template <std::size_t... Is>
using IndexSequence = IntegerSequence<std::size_t, Is...>;

template <std::size_t N>
struct MakeIndexSequenceImpl;

template <std::size_t N>
using MakeIndexSequence = typename MakeIndexSequenceImpl<N>::Type;

template <class, class>
struct IntegerSequenceConcat;

template <std::size_t... Lhs, std::size_t... Rhs>
struct IntegerSequenceConcat<IndexSequence<Lhs...>, IndexSequence<Rhs...>> : Identity<IndexSequence<Lhs..., ( sizeof...( Lhs ) + Rhs )...>> {};

template <std::size_t N>
struct MakeIndexSequenceImpl : IntegerSequenceConcat<MakeIndexSequence<N / 2>, MakeIndexSequence<N - N / 2>> {};

template <>
struct MakeIndexSequenceImpl<0> : Identity<IndexSequence<>> {};

template <>
struct MakeIndexSequenceImpl<1> : Identity<IndexSequence<0>> {};
#else
template <std::size_t... Is>
using IndexSequence = std::index_sequence<Is...>;

template <std::size_t N>
using MakeIndexSequence = std::make_index_sequence<N>;
#endif

template <class Fn, class Tuple, std::size_t... Idx>
constexpr auto ApplyImpl( Fn&& fn, Tuple&& tuple, IndexSequence<Idx...> ) -> decltype( fn( std::get<Idx>( std::forward<Tuple>( tuple ) )... ) ) {
    return fn( std::get<Idx>( std::forward<Tuple>( tuple ) )... );
}

template <class Fn, class Tuple>
constexpr auto Apply( Fn&& fn, Tuple&& tuple )
    -> decltype( ApplyImpl( std::forward<Fn>( fn ), std::forward<Tuple>( tuple ), MakeIndexSequence<std::tuple_size<typename std::remove_reference<Tuple>::type>::value>{} ) ) {
    using Indices = MakeIndexSequence<std::tuple_size<typename std::remove_reference<Tuple>::type>::value>;
    return ApplyImpl( std::forward<Fn>( fn ), std::forward<Tuple>( tuple ), Indices{} );
}
#endif

namespace core {
    template <class T>
    HAKLE_REQUIRES( std::move_constructible<T>&& std::is_move_assignable_v<T> )
    inline static constexpr void SwapRelaxed( std::atomic<T>& Left, std::atomic<T>& Right ) noexcept {
        T Temp = Left.load( std::memory_order_relaxed );
        Left.store( Right.load( std::memory_order_relaxed ), std::memory_order_relaxed );
        Right.store( Temp, std::memory_order_relaxed );
    }
}  // namespace core

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
inline HAKLE_CPP14_CONSTEXPR uint8_t BitWidth( std::size_t X ) noexcept {
    uint8_t Count = 0;
    HAKLE_CONSTEXPR_IF( sizeof( std::size_t ) > 4 ) {
#if defined( _MSC_VER )
        // try to avoid C4293
        // according to https://github.com/MicrosoftDocs/cpp-docs/blob/main/docs/error-messages/compiler-warnings/compiler-warning-level-1-c4293.md
        if ( static_cast<unsigned __int64>( X ) >> 32 )
#else
        if ( X >> 32 )
#endif
        {
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

    return Count + static_cast<uint8_t>( X );
}

template <class T1, class T2>
struct Pair {
    using FirstType  = T1;
    using SecondType = T2;

#ifdef HAKLE_USE_CONCEPT
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

    constexpr void swap( Pair& p ) noexcept(
#if HAKLE_CPP_VERSION >= 17
        std::is_nothrow_swappable_v<FirstType> && std::is_nothrow_swappable_v<SecondType>
#else
        true
#endif
        ) HAKLE_REQUIRES( std::is_swappable_v<FirstType>&& std::is_swappable_v<SecondType> ) {
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
