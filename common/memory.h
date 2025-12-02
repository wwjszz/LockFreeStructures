//
// Created by admin on 25-11-24.
//

#ifndef MEMORY_H
#define MEMORY_H

#include <utility>

#include "common.h"

#if __cpp_lib_hardware_interference_size >= 201703L
#include <new>
#define HAKLE_CACHE_LINE_SIZE std::hardware_destructive_interference_size
#else
#define HAKLE_CACHE_LINE_SIZE 64
#endif

#if HAKLE_CPP_VERSION >= 17
#define HAKLE_OPERATOR_NEW( T ) HakleOperatorNew<T>()
#define HAKLE_OPERATOR_NEW_ARRAY( T, N ) HakleOperatorNewArray<T>( N )
#define HAKLE_OPERATOR_DELETE( Ptr ) HakleOperatorDelete( Ptr )

#define HAKLE_CONSTRUCT( Ptr, ... ) HakleConstruct( Ptr, __VA_ARGS__ )
#define HAKLE_DESTROY( Ptr ) HakleDestroy( Ptr )
#define HAKLE_DESTROY_ARRAY( Ptr, N ) HakleDestroyArray( Ptr, N )

#define HAKLE_NEW( T, ... ) HakleNew<T>( __VA_ARGS__ )
#define HAKLE_DELETE( Ptr ) HakleDelete( Ptr )
#define HAKLE_DELETE_ARRAY( Ptr, N ) HakleDeleteArray( Ptr, N )

#define HAKLE_CREATE_ARRAY( T, N ) HakleCreateArray<T>( N )

template <class T>
inline constexpr bool RequiresSpecialAlignment() noexcept {
    return alignof( T ) > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}

template <class T>
inline constexpr T* HakleOperatorNew() {
    HAKLE_CONSTEXPR_IF HAKLE_LIKELY( RequiresSpecialAlignment<T>() ) {
        return static_cast<T*>( ::operator new( sizeof( T ), static_cast<std::align_val_t>( alignof( T ) ) ) );
    }
    return static_cast<T*>( ::operator new( sizeof( T ) ) );
}

template <class T>
inline constexpr T* HakleOperatorNewArray( const std::size_t N ) {
    HAKLE_CONSTEXPR_IF HAKLE_LIKELY( RequiresSpecialAlignment<T>() ) {
        return static_cast<T*>( ::operator new( N * sizeof( T ), static_cast<std::align_val_t>( alignof( T ) ) ) );
    }
    return static_cast<T*>( ::operator new( N * sizeof( T ) ) );
}

template <class T>
inline constexpr void HakleOperatorDelete( T* ptr ) noexcept {
    HAKLE_CONSTEXPR_IF HAKLE_LIKELY( RequiresSpecialAlignment<T>() ) { ::operator delete( ptr, static_cast<std::align_val_t>( alignof( T ) ) ); }
    else {
        ::operator delete( ptr );
    }
}

template <class T, class... Args>
inline constexpr T* HakleConstruct( T* ptr, Args&&... InArgs ) {
    return ::new ( ptr ) T( std::forward<Args>( InArgs )... );
}

template <class T>
inline constexpr void HakleDestroy( T* ptr ) noexcept {
    if HAKLE_LIKELY ( ptr ) {
        ptr->~T();
    }
}

template <class T>
inline constexpr void HakleDestroyArray( T* ptr, const std::size_t N ) noexcept {
    if HAKLE_LIKELY ( ptr ) {
        for ( std::size_t i = 0; i < N; ++i ) {
            ptr[ i ].~T();
        }
    }
}

template <class T, class... Args>
inline constexpr T* HakleNew( Args&&... InArgs ) {
    T* Ptr = HakleOperatorNew<T>();
    if HAKLE_LIKELY ( !Ptr )
        return nullptr;
    return HAKLE_CONSTRUCT( Ptr, std::forward<Args>( InArgs )... );
}

template <class T>
inline constexpr void HakleDelete( T* ptr ) noexcept {
    if HAKLE_LIKELY ( ptr ) {
        HakleDestroy( ptr );
        HakleOperatorDelete( ptr );
    }
}

template <class T>
inline constexpr void HakleDeleteArray( T* ptr, const std::size_t N ) noexcept {
    if HAKLE_LIKELY ( ptr ) {
        HakleDestroyArray( ptr, N );
        HakleOperatorDelete( ptr );
    }
}

template <class T>
inline constexpr T* HakleCreateArray( const std::size_t N ) {
    T* ptr = HakleOperatorNewArray<T>( N );
    for ( std::size_t i = 0; i < N; ++i ) {
        HakleConstruct( ptr + i );
    }
    return ptr;
}

#else
#define HAKLE_OPERATOR_NEW( T ) HakleOperatorNew<T>()
#define HAKLE_OPERATOR_NEW_ARRAY( T, N ) HakleOperatorNewArray<T>( N )
#define HAKLE_OPERATOR_DELETE( Ptr ) HakleOperatorDelete( Ptr )

#define HAKLE_CONSTRUCT( Ptr, ... ) HakleConstruct( Ptr, __VA_ARGS__ )
#define HAKLE_DESTROY( Ptr ) HakleDestroy( Ptr )
#define HAKLE_DESTROY_ARRAY( Ptr, N ) HakleDestroyArray( Ptr, N )

#define HAKLE_NEW( T, ... ) HakleNew<T>( __VA_ARGS__ )
#define HAKLE_DELETE( Ptr ) HakleDelete( Ptr )
#define HAKLE_DELETE_ARRAY( Ptr, N ) HakleDeleteArray( Ptr, N )

#define HAKLE_CREATE_ARRAY( T, N ) HakleCreateArray<T>( N )

template <class T>
inline constexpr T* HakleOperatorNew() {
    return static_cast<T*>( ::operator new( sizeof( T ) ) );
}

template <class T>
inline constexpr T* HakleOperatorNewArray( const std::size_t N ) {
    return static_cast<T*>( ::operator new( N * sizeof( T ) ) );
}

template <class T>
inline constexpr void HakleOperatorDelete( T* ptr ) noexcept {
    ::operator delete( ptr );
}

template <class T, class... Args>
inline constexpr T* HakleConstruct( T* ptr, Args&&... InArgs ) {
    return ::new ( ptr ) T( std::forward<Args>( InArgs )... );
}

template <class T>
inline constexpr void HakleDestroy( T* ptr ) noexcept {
    if HAKLE_LIKELY ( ptr ) {
        ptr->~T();
    }
}

template <class T>
inline constexpr void HakleDestroyArray( T* ptr, const std::size_t N ) noexcept {
    if HAKLE_LIKELY ( ptr ) {
        for ( std::size_t i = 0; i < N; ++i ) {
            if HAKLE_LIKELY ( ptr[ i ] ) {
                ptr[ i ].~T();
            }
        }
    }
}

template <class T, class... Args>
inline constexpr T* HakleNew( Args&&... InArgs ) {
    T* Ptr = HakleOperatorNew<T>();
    if HAKLE_LIKELY ( !Ptr )
        return nullptr;
    return HAKLE_CONSTRUCT( Ptr, std::forward<Args>( InArgs )... );
}

template <class T>
inline constexpr void HakleDelete( T* ptr ) noexcept {
    if HAKLE_LIKELY ( ptr ) {
        HakleDestroy( ptr );
        HakleOperatorDelete( ptr );
    }
}

template <class T>
inline constexpr void HakleDeleteArray( T* ptr, const std::size_t N ) noexcept {
    if HAKLE_LIKELY ( ptr ) {
        HakleDestroyArray( ptr, N );
        HakleOperatorDelete( ptr );
    }
}

template <class T>
inline constexpr T* HakleCreateArray( const std::size_t N ) {
    T* ptr = HakleOperatorNewArray<T>( N );
    for ( std::size_t i = 0; i < N; ++i ) {
        HakleConstruct( ptr + i );
    }
    return ptr;
}
#endif

#endif  // MEMORY_H
