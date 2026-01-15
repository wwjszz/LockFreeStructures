//
// Created by admin on 25-12-1.
//

#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <cstddef>
#include <type_traits>
#if defined( ENABLE_MEMORY_LEAK_DETECTION )
#include <atomic>
#include <cstdio>
#include <typeinfo>
#endif

#include "common/common.h"
#include "memory.h"

namespace hakle {

template <class...>
using VoidT = void;

template <class T, class U, class = void>
struct HasRebindOther : std::false_type {};

template <class T, class U>
struct HasRebindOther<T, U, VoidT<typename T::template rebind<U>::other>> : std::true_type {};

template <class T, class U, bool = HasRebindOther<T, U>::value>
struct AllocatorTraitsRebind {
    static_assert( HasRebindOther<T, U>::value, "This allocator has to implement rebind" );
    using Type = typename T::template rebind<U>::other;
};

template <template <class, class...> class Alloc, class T, class... Args, class U>
struct AllocatorTraitsRebind<Alloc<T, Args...>, U, true> {
    using Type = typename Alloc<T, Args...>::template rebind<U>::other;
};

template <template <class, class...> class Alloc, class T, class... Args, class U>
struct AllocatorTraitsRebind<Alloc<T, Args...>, U, false> {
    using Type = Alloc<U, Args...>;
};

#ifdef HAKLE_USE_CONCEPT
template <class Tp>
concept IsAllocator = requires( Tp alloc, typename Tp::SizeType n, typename Tp::Pointer p, typename Tp::Pointer last ) {
    typename Tp::ValueType;
    typename Tp::Pointer;
    typename Tp::ConstPointer;
    typename Tp::Reference;
    typename Tp::ConstReference;
    typename Tp::SizeType;
    typename Tp::DifferenceType;

    { alloc.Allocate() } -> std::same_as<typename Tp::Pointer>;
    { alloc.Allocate( n ) } -> std::same_as<typename Tp::Pointer>;
    alloc.Deallocate( p );
    alloc.Deallocate( p, n );
    alloc.Destroy( p );
    alloc.Destroy( p, n );
    alloc.Destroy( p, last );
    // TODO: support Construct
};
#endif

// TODO: propagate on container copy and ...
template <HAKLE_CONCEPT( IsAllocator ) HakleAllocator>
struct HakeAllocatorTraits {
    using AllocatorType  = HakleAllocator;
    using ValueType      = typename AllocatorType::ValueType;
    using Pointer        = typename AllocatorType::Pointer;
    using ConstPointer   = typename AllocatorType::ConstPointer;
    using Reference      = typename AllocatorType::Reference;
    using ConstReference = typename AllocatorType::ConstReference;
    using SizeType       = typename AllocatorType::SizeType;
    using DifferenceType = typename AllocatorType::DifferenceType;

    template <class U>
    using RebindAlloc = typename AllocatorTraitsRebind<AllocatorType, U>::Type;
    template <class U>
    using RebindTraits = HakeAllocatorTraits<RebindAlloc<U>>;

    static Pointer Allocate( AllocatorType& Allocator ) { return Allocator.Allocate(); }
    static Pointer Allocate( AllocatorType& Allocator, SizeType n ) { return Allocator.Allocate( n ); }

    static void Deallocate( AllocatorType& Allocator, Pointer ptr ) noexcept { Allocator.Deallocate( ptr ); }
    static void Deallocate( AllocatorType& Allocator, Pointer ptr, SizeType n ) noexcept { Allocator.Deallocate( ptr, n ); }

    template <class... Args>
    static void Construct( AllocatorType& Allocator, Pointer ptr, Args&&... args ) {
        Allocator.Construct( ptr, std::forward<Args>( args )... );
    }

    static void Destroy( AllocatorType& Allocator, Pointer ptr ) noexcept { Allocator.Destroy( ptr ); }
    static void Destroy( AllocatorType& Allocator, Pointer ptr, SizeType n ) noexcept { Allocator.Destroy( ptr, n ); }
    static void Destroy( AllocatorType& Allocator, Pointer first, Pointer last ) noexcept { Destroy( Allocator, first, last - first ); }
};

#if defined( ENABLE_MEMORY_LEAK_DETECTION )

template <class Tp>
class HakleAllocator {
public:
    using ValueType      = Tp;
    using Pointer        = Tp*;
    using ConstPointer   = const Tp*;
    using Reference      = Tp&;
    using ConstReference = const Tp&;
    using SizeType       = size_t;
    using DifferenceType = std::ptrdiff_t;

    ~HakleAllocator() { printf( "Allocator %s Quit when AllocateCount = %d, ConstructCount = %d\n", typeid( Tp ).name(), AllocateCount.load(), ConstructCount.load() ); }

    static std::atomic<int> AllocateCount;
    static std::atomic<int> ConstructCount;

    constexpr Pointer Allocate() {
        ++AllocateCount;
        return HAKLE_OPERATOR_NEW( Tp );
    }
    constexpr Pointer Allocate( SizeType n ) {
        AllocateCount += n;
        return HAKLE_OPERATOR_NEW_ARRAY( Tp, n );
    }

    constexpr void Deallocate( Pointer ptr ) noexcept {
        HAKLE_OPERATOR_DELETE( ptr );
        --AllocateCount;
    }
    constexpr void Deallocate( Pointer ptr, HAKLE_MAYBE_UNUSED SizeType n ) noexcept {
        Deallocate( ptr );
        AllocateCount -= n - 1;
    }

    template <class... Args>
    constexpr void Construct( Pointer ptr, Args&&... args ) {
        HAKLE_CONSTRUCT( ptr, std::forward<Args>( args )... );
        ++ConstructCount;
    }

    constexpr void Destroy( Pointer ptr ) noexcept {
        HAKLE_DESTROY( ptr );
        --ConstructCount;
    }
    constexpr void Destroy( Pointer ptr, SizeType n ) noexcept {
        HAKLE_DESTROY_ARRAY( ptr, n );
        ConstructCount -= n;
    }
    constexpr void Destroy( Pointer first, Pointer last ) noexcept { Destroy( first, last - first ); }
};

template <class Tp>
std::atomic<int> HakleAllocator<Tp>::AllocateCount{};

template <class Tp>
std::atomic<int> HakleAllocator<Tp>::ConstructCount{};

#else

// TODO: private
template <class Tp>
class HakleAllocator {
public:
    using ValueType      = Tp;
    using Pointer        = Tp*;
    using ConstPointer   = const Tp*;
    using Reference      = Tp&;
    using ConstReference = const Tp&;
    using SizeType       = size_t;
    using DifferenceType = std::ptrdiff_t;

    static constexpr Pointer Allocate() { return HAKLE_OPERATOR_NEW( Tp ); }
    static constexpr Pointer Allocate( SizeType n ) { return HAKLE_OPERATOR_NEW_ARRAY( Tp, n ); }

    static constexpr void Deallocate( Pointer ptr ) noexcept { HAKLE_OPERATOR_DELETE( ptr ); }
    static constexpr void Deallocate( Pointer ptr, HAKLE_MAYBE_UNUSED SizeType n ) noexcept { Deallocate( ptr ); }

    template <class... Args>
    static constexpr void Construct( Pointer ptr, Args&&... args ) {
        HAKLE_CONSTRUCT( ptr, std::forward<Args>( args )... );
    }

    static constexpr void Destroy( Pointer ptr ) noexcept { HAKLE_DESTROY( ptr ); }
    static constexpr void Destroy( Pointer ptr, SizeType n ) noexcept { HAKLE_DESTROY_ARRAY( ptr, n ); }
    static constexpr void Destroy( Pointer first, Pointer last ) noexcept { Destroy( first, last - first ); }
};
#endif

}  // namespace hakle

#endif  // ALLOCATOR_H
