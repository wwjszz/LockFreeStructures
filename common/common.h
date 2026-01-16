//
// Created by admin on 25-11-24.
//

#ifndef COMMON_H
#define COMMON_H

#ifndef HAKLE_CPP_VERSION
#if defined( _MSVC_LANG )
#define HAKLE_CPLUSPLUS _MSVC_LANG
#else
#define HAKLE_CPLUSPLUS __cplusplus
#endif

#if HAKLE_CPLUSPLUS >= 202002L
#define HAKLE_CPP_VERSION 20
#elif HAKLE_CPLUSPLUS >= 201703L
#define HAKLE_CPP_VERSION 17
#elif HAKLE_CPLUSPLUS >= 201402L
#define HAKLE_CPP_VERSION 14
#elif HAKLE_CPLUSPLUS >= 201103L
#define HAKLE_CPP_VERSION 11
#else
#define HAKLE_CPP_VERSION 0
#error "This library requires C++11 or later"
#endif

#endif

#if HAKLE_CPP_VERSION >= 17
#define HAKLE_CONSTEXPR_IF if constexpr
#define HAKLE_NOEXCEPT( expr ) noexcept( expr )
#define HAKLE_NODISCARD [[nodiscard]]
#define HAKLE_MAYBE_UNUSED [[maybe_unused]]
#define HAKLE_BYTE std::byte
#else
#define HAKLE_CONSTEXPR_IF if
#define HAKLE_NOEXCEPT( expr ) true
#define HAKLE_NODISCARD
#define HAKLE_MAYBE_UNUSED
#define HAKLE_BYTE unsigned char
#endif

#if HAKLE_CPP_VERSION >= 20
#define HAKLE_CPP20_CONSTEXPR constexpr
#define HAKLE_CPP20_EXPLICIT(...) explicit(__VA_ARGS__)
#define HAKLE_USE_CONCEPT
#define HAKLE_CONCEPT( expr ) expr
#define HAKLE_REQUIRES( ... ) requires __VA_ARGS__
#else
#define HAKLE_CPP20_CONSTEXPR
#define HAKLE_CPP20_EXPLICIT(...) explicit
#define HAKLE_CONCEPT( expr ) class
#define HAKLE_REQUIRES( ... )
#endif

#if HAKLE_CPP_VERSION >= 14
#define HAKLE_CPP14_CONSTEXPR constexpr
#else
#define HAKLE_CPP14_CONSTEXPR
#endif

#define HAKLE_TRY try
#define HAKLE_CATCH( ... ) catch ( __VA_ARGS__ )
#define HAKLE_THROW( expr ) throw( expr )
#define HAKLE_RETHROW throw

#ifdef _MSC_VER
#define HAKLE_LIKELY( x ) ( x )
#define HAKLE_UNLIKELY( x ) ( x )
#else
#define HAKLE_LIKELY( x ) ( __builtin_expect( !!( x ), 1 ) )
#define HAKLE_UNLIKELY( x ) ( __builtin_expect( !!( x ), 0 ) )
#endif

#endif  // COMMON_H
