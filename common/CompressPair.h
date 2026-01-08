//
// Created by admin on 25-12-5.
//

#ifndef COMPRESSPAIR_H
#define COMPRESSPAIR_H
#include <type_traits>
#include <utility>

namespace hakle {

struct DefaultInitTag {};
struct ValueInitTag {};

template <class T, bool>
struct DependentType : T {};

template <class T, int Index, bool CanBeEmptyBase = std::is_empty<T>::value && !std::is_final<T>::value>
struct CompressPairElem {
    using Reference      = T&;
    using ConstReference = const T&;

    CompressPairElem() = default;
    constexpr explicit CompressPairElem( DefaultInitTag ) {}
    constexpr explicit CompressPairElem( ValueInitTag ) : value() {}

    template <class U, std::enable_if_t<!std::is_same<CompressPairElem, std::decay_t<U>>::value, int> = 0>
    constexpr explicit CompressPairElem( U&& u ) : value( std::forward<U>( u ) ) {}

    Reference      Get() { return value; }
    ConstReference Get() const { return value; }

private:
    T value;
};

template <class T, int Index>
struct CompressPairElem<T, Index, true> : private T {
    using Reference      = T&;
    using ConstReference = const T&;
    using ValueType      = T;

    CompressPairElem() = default;
    constexpr explicit CompressPairElem( DefaultInitTag ) {}
    constexpr explicit CompressPairElem( ValueInitTag ) : ValueType() {}

    template <class U, std::enable_if_t<!std::is_same<CompressPairElem, std::decay_t<U>>::value, int> = 0>
    constexpr explicit CompressPairElem( U&& u ) : ValueType( std::forward<U>( u ) ) {}

    Reference      Get() { return *this; }
    ConstReference Get() const { return *this; }
};

// TODO: use [[no_unique_address] to replace
template <class T1, class T2>
HAKLE_REQUIRES( ( !std::same_as<T1, T2> ))
class CompressPair : private CompressPairElem<T1, 0>, private CompressPairElem<T2, 1> {
    static_assert( !std::is_same<T1, T2>::value, "T1 and T2 are the same" );

public:
    using Base1 = CompressPairElem<T1, 0>;
    using Base2 = CompressPairElem<T2, 1>;

    template <bool Dummy = true, std::enable_if_t<DependentType<std::is_default_constructible<T1>, Dummy>::value
                                                      && DependentType<std::is_default_constructible<T2>, Dummy>::value,
                                                  int> = 0>
    constexpr CompressPair() : Base1( ValueInitTag{} ), Base2( ValueInitTag{} ) {}

    template <class U1, class U2>
    constexpr CompressPair( U1&& X, U2&& Y ) : Base1( std::forward<U1>( X ) ), Base2( std::forward<U2>( Y ) ) {}

    constexpr typename Base1::Reference      First() { return Base1::Get(); }
    constexpr typename Base1::ConstReference First() const { return Base1::Get(); }
    constexpr typename Base2::Reference      Second() { return Base2::Get(); }
    constexpr typename Base2::ConstReference Second() const { return Base2::Get(); }

    constexpr static Base1* GetFirstBase( CompressPair* Pair ) noexcept { return static_cast<Base1*>( Pair ); }

    constexpr static Base2* GetSecondBase( CompressPair* Pair ) noexcept { return static_cast<Base2*>( Pair ); }

    constexpr void swap( CompressPair& Other ) noexcept {
        using std::swap;
        swap( First(), Other.First() );
        swap( Second(), Other.Second() );
    }
};

template <class T1, class T2>
inline constexpr void swap( CompressPair<T1, T2>& X, CompressPair<T1, T2>& Y ) noexcept {
    X.swap( Y );
}

}  // namespace hakle

#endif  // COMPRESSPAIR_H
