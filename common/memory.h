//
// Created by admin on 25-11-24.
//

#ifndef MEMORY_H
#define MEMORY_H

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

#define HAKLE_NEW(T, ...) HakleNew<T>(__VA_ARGS__)
#define HAKLE_DELETE( Ptr ) HakleDelete( Ptr )
#define HAKLE_DELETE_ARRAY( Ptr, N ) HakleDeleteArray( Ptr, N )

#define CONSTEXPR_IF constexpr

template <class T>
inline constexpr bool RequiresSpecialAlignment() noexcept {
    return alignof( T ) > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}

template <class T>
inline constexpr T* HakleOperatorNew() {
    if constexpr ( RequiresSpecialAlignment<T>() ) {
        return static_cast<T*>( ::operator new( sizeof( T ), static_cast<std::align_val_t>( alignof( T ) ) ) );
    }
    return static_cast<T*>( ::operator new( sizeof( T ) ) );
}

template <class T, class... Args>
inline constexpr T* HakleNew(Args&&... InArgs) {
    T* Ptr = HakleOperatorNew<T>();
    if ( !Ptr )
        return nullptr;
    return new ( Ptr ) T(std::forward<Args>(InArgs)...);
}

template <class T>
inline constexpr T* HakleOperatorNewArray( const std::size_t N ) {
    if constexpr ( RequiresSpecialAlignment<T>() ) {
        return static_cast<T*>( ::operator new( N * sizeof( T ), static_cast<std::align_val_t>( alignof( T ) ) ) );
    }
    return static_cast<T*>( ::operator new( N * sizeof( T ) ) );
}

template <class T>
inline constexpr void HakleOperatorDelete( T* ptr ) noexcept {
    if constexpr ( RequiresSpecialAlignment<T>() ) {
        ::operator delete( ptr, static_cast<std::align_val_t>( alignof( T ) ) );
    }
    else {
        ::operator delete( ptr );
    }
}

template<class T>
inline constexpr void HakleDelete( T* ptr ) noexcept {
    if ( ptr ) {
        ptr->~T();
        HakleOperatorDelete( ptr );
    }
}

template<class T>
inline constexpr void HakleDeleteArray( T* ptr, const std::size_t N ) noexcept {
    if ( ptr ) {
        for ( std::size_t i = 0; i < N; ++i ) {
            ptr[i].~T();
        }
        HakleOperatorDelete( ptr );
    }
}

#else
#define OPERATOR_NEW( T, M ) static_cast<T*>( ::operator new( M ) )
#define OPERATOR_NEW_ARRAY( T, M, N ) static_cast<T*>( ::operator new( N * M ) )
#define OPERATOR_DELETE( Ptr ) ::operator delete( Ptr )
#define CONSTEXPR_IF
#endif

#endif  // MEMORY_H
