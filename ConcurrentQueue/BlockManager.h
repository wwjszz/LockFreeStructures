//
// Created by wwjszz on 25-11-24.
//

#ifndef BLOCKMANAGER_H
#define BLOCKMANAGER_H

#include <atomic>
#include <concepts>
#include <cstddef>
#include <type_traits>

#include "Block.h"
#include "common/CompressPair.h"
#include "common/allocator.h"
#include "common/utility.h"

#include <cassert>

// BlockPool + FreeList
namespace hakle {

enum class AllocMode;

#ifndef HAKLE_USE_CONCEPT
template <class BLOCK_TYPE>
struct BlockTraits {
    static_assert( BLOCK_TYPE::BlockSize > 1 && ( BLOCK_TYPE::BlockSize & ( BLOCK_TYPE::BlockSize - 1 ) ) == 0, "BlockSize must be power of 2 and greater than 1" );

    constexpr static std::size_t BlockSize = BLOCK_TYPE::BlockSize;
    using ValueType                        = typename BLOCK_TYPE::ValueType;
    using BlockType                        = BLOCK_TYPE;
};
#else
template <class T, template <class> class Constraint>
concept IsAtomicWith = requires {
    typename T::value_type;
    requires Constraint<typename T::value_type>::value;
    requires std::same_as<std::remove_cvref_t<T>, std::atomic<typename T::value_type>>;
};

template <class T>
struct IsInteger : std::bool_constant<std::integral<T>> {};

template <class T>
struct IsPointer : std::bool_constant<std::is_pointer<T>::value> {};

template <class T>
concept IsFreeListNode = requires( T& t ) {
    requires IsAtomicWith<std::remove_reference_t<decltype( t.FreeListRefs )>, IsInteger>;
    requires IsAtomicWith<std::remove_reference_t<decltype( t.FreeListNext )>, IsPointer>;
};

template <class T>
concept IsBlockManager = requires( T& t, AllocMode Mode, typename T::BlockType* p ) {
    { t.RequisitionBlock( Mode ) } -> std::same_as<typename T::BlockType*>;
    t.ReturnBlock( p );
    t.ReturnBlocks( p );
};

template <class BLOCK_TYPE, class T>
concept CheckBlockManager = IsBlock<BLOCK_TYPE> && std::same_as<BLOCK_TYPE, typename T::BlockType>;

#endif

// TODO: position?
enum class AllocMode { CanAlloc, CannotAlloc };

struct MemoryBase {
    bool HasOwner{ false };
};

template <class T>
struct FreeListNode : MemoryBase {
    std::atomic<uint32_t> FreeListRefs{ 0 };
    std::atomic<T*>       FreeListNext{ 0 };
};

template <HAKLE_CONCEPT( IsFreeListNode ) Node, HAKLE_CONCEPT( IsAllocator ) ALLOCATOR_TYPE = HakleAllocator<Node>>
class FreeList {
public:
#ifndef HAKLE_USE_CONCEPT
    static_assert( std::is_base_of<FreeListNode<Node>, Node>::value, "Node must be derived from FreeListNode<Node>" );
#endif

    using AllocatorType   = ALLOCATOR_TYPE;
    using AllocatorTraits = HakeAllocatorTraits<AllocatorType>;

    constexpr explicit FreeList( const AllocatorType& InAllocator = AllocatorType{} ) : AllocatorPair( nullptr, InAllocator ) {}

    HAKLE_CPP20_CONSTEXPR ~FreeList() { Clear(); }

    HAKLE_CPP14_CONSTEXPR FreeList( FreeList&& Other ) noexcept : HAKLE_MOVE_PAIR_ATOMIC1( AllocatorPair ) { Other.Reset(); }

    constexpr FreeList& operator=( FreeList&& Other ) noexcept {
        if ( this != &Other ) {
            Clear();
            Head().store( Other.Head().load( std::memory_order_relaxed ), std::memory_order_relaxed );
            Allocator() = std::move( Other.Allocator() );
            Other.Reset();
        }
        return *this;
    }

    constexpr FreeList( const FreeList& Other )            = delete;
    constexpr FreeList& operator=( const FreeList& Other ) = delete;

    HAKLE_CPP14_CONSTEXPR void Clear() noexcept {
        Node* CurrentNode = Head().load( std::memory_order_relaxed );
        while ( CurrentNode != nullptr ) {
            Node* Next = CurrentNode->FreeListNext.load( std::memory_order_relaxed );
            if ( !CurrentNode->HasOwner ) {
                AllocatorTraits::Destroy( Allocator(), CurrentNode );
                AllocatorTraits::Deallocate( Allocator(), CurrentNode );
            }
            CurrentNode = Next;
        }
    }

    HAKLE_CPP14_CONSTEXPR void Reset() noexcept { Head().store( nullptr, std::memory_order_relaxed ); }

#if HAKLE_CPP_VERSION >= 20
    HAKLE_CPP14_CONSTEXPR void swap( FreeList& Other ) noexcept HAKLE_REQUIRES( std::swappable<AllocatorType> ) {
        HAKLE_SWAP_ATOMIC( Head() );
        using std::swap;
        HAKLE_SWAP( Allocator() );
    }
#endif

    HAKLE_CPP14_CONSTEXPR void Add( Node* InNode ) noexcept {
        // Set AddFlag first
        if ( InNode->FreeListRefs.fetch_add( AddFlag, std::memory_order_acq_rel ) == 0 ) {
            InnerAdd( InNode );
        }
    }

    HAKLE_CPP14_CONSTEXPR Node* TryGet() noexcept {
        Node* CurrentHead = Head().load( std::memory_order_acquire );
        while ( CurrentHead != nullptr ) {
            Node*    PrevHead = CurrentHead;
            uint32_t Refs     = CurrentHead->FreeListRefs.load( std::memory_order_relaxed );
            if ( ( Refs & RefsMask ) == 0  // check if already taken or adding
                 || ( !CurrentHead->FreeListRefs.compare_exchange_strong( Refs, Refs + 1, std::memory_order_acquire,
                                                                          std::memory_order_relaxed ) ) )  // try add refs
            {
                CurrentHead = Head().load( std::memory_order_relaxed );
                continue;
            }

            // try Taken
            Node* Next = CurrentHead->FreeListNext.load( std::memory_order_relaxed );
            if ( Head().compare_exchange_strong( CurrentHead, Next, std::memory_order_acquire, std::memory_order_relaxed ) ) {
                // taken success, decrease refcount twice, for our and list's ref
                CurrentHead->FreeListRefs.fetch_sub( 2, std::memory_order_release );
                return CurrentHead;
            }

            // taken failed, decrease refcount
            Refs = PrevHead->FreeListRefs.fetch_sub( 1, std::memory_order_acq_rel );
            if ( Refs == AddFlag + 1 ) {
                // no one is using it, add it back
                InnerAdd( PrevHead );
            }
        }
        return nullptr;
    }

    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    // only useful when there is no contention (e.g. destruction)
    constexpr Node* GetHead() const noexcept { return Head().load( std::memory_order_relaxed ); }

private:
    // add when ref count == 0
    HAKLE_CPP14_CONSTEXPR void InnerAdd( Node* InNode ) noexcept {
        Node* CurrentHead = Head().load( std::memory_order_relaxed );
        while ( true ) {
            // first update next then refs
            InNode->FreeListNext.store( CurrentHead, std::memory_order_relaxed );
            InNode->FreeListRefs.store( 1, std::memory_order_release );
            if ( !Head().compare_exchange_strong( CurrentHead, InNode, std::memory_order_release, std::memory_order_relaxed ) ) {
                // check if someone already using it
                if ( InNode->FreeListRefs.fetch_add( AddFlag - 1, std::memory_order_acq_rel ) == 1 ) {
                    continue;
                }
            }
            return;
        }
    }

    static constexpr uint32_t RefsMask = 0x7fffffff;
    static constexpr uint32_t AddFlag  = 0x80000000;

    HAKLE_CPP14_CONSTEXPR AllocatorType& Allocator() noexcept { return AllocatorPair.Second(); }
    constexpr const AllocatorType&       Allocator() const noexcept { return AllocatorPair.Second(); }
    HAKLE_CPP14_CONSTEXPR std::atomic<Node*>& Head() noexcept { return AllocatorPair.First(); }
    constexpr const std::atomic<Node*>&       Head() const noexcept { return AllocatorPair.First(); }

    // compressed allocator
    CompressPair<std::atomic<Node*>, AllocatorType> AllocatorPair{};
};

template <HAKLE_CONCEPT( IsFreeListNode ) Node, HAKLE_CONCEPT( IsAllocator ) ALLOCATOR_TYPE = HakleAllocator<Node>>
class FreeList_DAS {
public:
#ifndef HAKLE_USE_CONCEPT
    static_assert( std::is_base_of<FreeListNode<Node>, Node>::value, "Node must be derived from FreeListNode<Node>" );
#endif

    using AllocatorType   = ALLOCATOR_TYPE;
    using AllocatorTraits = HakeAllocatorTraits<AllocatorType>;

    constexpr explicit FreeList_DAS( const AllocatorType& InAllocator = AllocatorType{} ) : AllocatorPair( HeadPtr{}, InAllocator ) { assert(Head().is_lock_free() && "Your platform must support DCAS for this to be lock-free"); }

    HAKLE_CPP20_CONSTEXPR ~FreeList_DAS() { Clear(); }

    HAKLE_CPP14_CONSTEXPR FreeList_DAS( FreeList_DAS&& Other ) noexcept : HAKLE_MOVE_PAIR_ATOMIC1( AllocatorPair ) { Other.Reset(); }

    constexpr FreeList_DAS& operator=( FreeList_DAS&& Other ) noexcept {
        if ( this != &Other ) {
            Clear();
            Head().store( Other.Head().load( std::memory_order_relaxed ), std::memory_order_relaxed );
            Allocator() = std::move( Other.Allocator() );
            Other.Reset();
        }
        return *this;
    }

    constexpr FreeList_DAS( const FreeList_DAS& Other )            = delete;
    constexpr FreeList_DAS& operator=( const FreeList_DAS& Other ) = delete;

    HAKLE_CPP14_CONSTEXPR void Clear() noexcept {
        Node* CurrentNode = Head().load( std::memory_order_relaxed ).Ptr;
        while ( CurrentNode != nullptr ) {
            Node* Next = CurrentNode->FreeListNext.load( std::memory_order_relaxed );
            if ( !CurrentNode->HasOwner ) {
                AllocatorTraits::Destroy( Allocator(), CurrentNode );
                AllocatorTraits::Deallocate( Allocator(), CurrentNode );
            }
            CurrentNode = Next;
        }
    }

    HAKLE_CPP14_CONSTEXPR void Reset() noexcept { Head().store( HeadPtr{}, std::memory_order_relaxed ); }

    HAKLE_CPP14_CONSTEXPR void Add( Node* InNode ) noexcept {
        HeadPtr CurrentHead = Head().load( std::memory_order_relaxed );
        HeadPtr NewHead{ InNode, 0 };

        do {
            NewHead.Tag = CurrentHead.Tag + 1;
            InNode->FreeListNext.store( CurrentHead.Ptr, std::memory_order_relaxed );
        } while ( !Head().compare_exchange_strong( CurrentHead, NewHead, std::memory_order_relaxed, std::memory_order_relaxed ) );
    }

    HAKLE_CPP14_CONSTEXPR Node* TryGet() noexcept {
        HeadPtr CurrentHead = Head().load( std::memory_order_relaxed );
        HeadPtr NewHead;
        while ( CurrentHead.Ptr != nullptr ) {
            NewHead.Ptr = CurrentHead.Ptr->FreeListNext.load( std::memory_order_relaxed );
            NewHead.Tag = CurrentHead.Tag + 1;
            if ( Head().compare_exchange_strong( CurrentHead, NewHead, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                break;
            }
        }
        return CurrentHead.Ptr;
    }

    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    // only useful when there is no contention (e.g. destruction)
    constexpr Node* GetHead() const noexcept { return Head().load( std::memory_order_relaxed ).Ptr; }

private:
    struct HeadPtr {
        Node*          Ptr{};
        std::uint16_t Tag{};
    };

    HAKLE_CPP14_CONSTEXPR AllocatorType& Allocator() noexcept { return AllocatorPair.Second(); }
    constexpr const AllocatorType&       Allocator() const noexcept { return AllocatorPair.Second(); }
    HAKLE_CPP14_CONSTEXPR std::atomic<HeadPtr>& Head() noexcept { return AllocatorPair.First(); }
    constexpr const std::atomic<HeadPtr>&       Head() const noexcept { return AllocatorPair.First(); }

    // compressed allocator
    CompressPair<std::atomic<HeadPtr>, AllocatorType> AllocatorPair{};
};

template <HAKLE_CONCEPT( IsBlock ) BLOCK_TYPE, HAKLE_CONCEPT( IsAllocator ) ALLOCATOR_TYPE = HakleAllocator<BLOCK_TYPE>>
class BlockPool {
public:
    using AllocatorType   = ALLOCATOR_TYPE;
    using AllocatorTraits = HakeAllocatorTraits<AllocatorType>;

    HAKLE_CPP14_CONSTEXPR explicit BlockPool( std::size_t InSize, const AllocatorType& InAllocator = AllocatorType{} ) : AllocatorPair{ InSize, InAllocator } {
        if ( Size() > 0 ) {
            Head = AllocatorTraits::Allocate( Allocator(), Size() );
            for ( std::size_t i = 0; i < Size(); i++ ) {
                AllocatorTraits::Construct( Allocator(), Head + i );
                Head[ i ].HasOwner = true;
            }
        }
    }

    HAKLE_CPP20_CONSTEXPR ~BlockPool() { Clear(); }

    HAKLE_CPP14_CONSTEXPR BlockPool( BlockPool&& Other ) noexcept : HAKLE_FOR_EACH_COMMA( HAKLE_MOVE, AllocatorPair, Head ), HAKLE_MOVE_ATOMIC( Index ) { Other.Reset(); }

    constexpr BlockPool& operator=( BlockPool&& Other ) noexcept {
        if ( this != &Other ) {
            Clear();
            HAKLE_FOR_EACH( HAKLE_OP_MOVE, HAKLE_SEM, AllocatorPair, Head );
            HAKLE_OP_MOVE_ATOMIC( Index );
            Other.Reset();
        }
        return *this;
    }

    constexpr BlockPool( const BlockPool& Other )            = delete;
    constexpr BlockPool& operator=( const BlockPool& Other ) = delete;

    HAKLE_CPP14_CONSTEXPR void Clear() noexcept {
        AllocatorTraits::Destroy( Allocator(), Head, Size() );
        AllocatorTraits::Deallocate( Allocator(), Head, Size() );
    }

    HAKLE_CPP14_CONSTEXPR void Reset() noexcept {
        Size() = 0;
        Head   = nullptr;
        Index.store( 0, std::memory_order_relaxed );
    }

#if HAKLE_CPP_VERSION >= 20
    HAKLE_CPP14_CONSTEXPR void swap( BlockPool& Other ) noexcept HAKLE_REQUIRES( std::swappable<AllocatorType> ) {
        HAKLE_SWAP_ATOMIC( Index );
        using std::swap;
        HAKLE_FOR_EACH( HAKLE_SWAP, HAKLE_SEM, AllocatorPair, Head );
    }
#endif

    HAKLE_NODISCARD HAKLE_CPP14_CONSTEXPR std::size_t GetSize() const noexcept { return Size(); }

    HAKLE_CPP14_CONSTEXPR BLOCK_TYPE* GetBlock() noexcept {
        if ( Index.load( std::memory_order_relaxed ) >= Size() )
            return nullptr;

        std::size_t CurrentIndex = Index.fetch_add( 1, std::memory_order_relaxed );
        return CurrentIndex < Size() ? ( Head + CurrentIndex ) : nullptr;
    }

private:
    HAKLE_CPP14_CONSTEXPR AllocatorType& Allocator() noexcept { return AllocatorPair.Second(); }
    constexpr const AllocatorType&       Allocator() const noexcept { return AllocatorPair.Second(); }
    HAKLE_CPP14_CONSTEXPR std::size_t&           Size() noexcept { return AllocatorPair.First(); }
    HAKLE_NODISCARD constexpr const std::size_t& Size() const noexcept { return AllocatorPair.First(); }

    // compressed allocator
    CompressPair<std::size_t, AllocatorType> AllocatorPair{};
    BLOCK_TYPE*                              Head{ nullptr };
    std::atomic<std::size_t>                 Index{ 0 };
};

template <HAKLE_CONCEPT( IsBlock ) BLOCK_TYPE, HAKLE_CONCEPT( IsAllocator ) ALLOCATOR_TYPE>
class BlockManagerBase : private CompressPairElem<ALLOCATOR_TYPE, 0> {
public:
    using AllocatorType        = ALLOCATOR_TYPE;
    using BlockAllocatorTraits = HakeAllocatorTraits<AllocatorType>;
    using BlockType            = BLOCK_TYPE;
#ifdef HAKLE_USE_CONCEPT
    constexpr static std::size_t BlockSize = BlockType::BlockSize;
    using ValueType                        = typename BlockType::ValueType;
#else
    using BlockTraits                      = BlockTraits<BlockType>;
    constexpr static std::size_t BlockSize = BlockTraits::BlockSize;
    using ValueType                        = typename BlockTraits::ValueType;
#endif

    constexpr BlockManagerBase() = default;
    constexpr explicit BlockManagerBase( const AllocatorType& InAllocator = AllocatorType{} ) : Base( InAllocator ) {}
    HAKLE_CPP14_CONSTEXPR ~BlockManagerBase() = default;

    HAKLE_CPP14_CONSTEXPR                   BlockManagerBase( BlockManagerBase&& Other ) noexcept = default;
    HAKLE_CPP14_CONSTEXPR BlockManagerBase& operator=( BlockManagerBase&& Other ) noexcept        = default;

    HAKLE_CPP14_CONSTEXPR                   BlockManagerBase( const BlockManagerBase& Other ) = delete;
    HAKLE_CPP14_CONSTEXPR BlockManagerBase& operator=( const BlockManagerBase& Other )        = delete;

#if HAKLE_CPP_VERSION >= 20
    HAKLE_CPP14_CONSTEXPR void swap( BlockManagerBase& Other ) noexcept HAKLE_REQUIRES( std::swappable<AllocatorType> ) {
        // TODO: swap allocator_type traits
        // TODO: checking user-defined allocator, blockmanager, block,
        using std::swap;
        swap( static_cast<Base&>( *this ), static_cast<Base&>( Other ) );
    }
#endif

    using AllocMode = hakle::AllocMode;

    virtual HAKLE_CPP20_CONSTEXPR BlockType* RequisitionBlock( AllocMode InMode ) = 0;
    virtual HAKLE_CPP20_CONSTEXPR void       ReturnBlocks( BlockType* InBlock )   = 0;
    virtual HAKLE_CPP20_CONSTEXPR void       ReturnBlock( BlockType* InBlock )    = 0;

    HAKLE_CPP14_CONSTEXPR AllocatorType& Allocator() noexcept { return Base::Get(); }
    constexpr const AllocatorType&       Allocator() const noexcept { return Base::Get(); }

private:
    using Base = CompressPairElem<ALLOCATOR_TYPE, 0>;
};

// We set a block pool and a free list
template <HAKLE_CONCEPT( IsBlock ) BLOCK_TYPE, HAKLE_CONCEPT( IsAllocator ) ALLOCATOR_TYPE = HakleAllocator<BLOCK_TYPE>>
class HakleBlockManager : public BlockManagerBase<BLOCK_TYPE, ALLOCATOR_TYPE> {
public:
    using BaseManager = BlockManagerBase<BLOCK_TYPE, ALLOCATOR_TYPE>;
    // TODO:
    // using typename BaseManager::AllocatorType;
    using AllocatorType = typename BaseManager::AllocatorType;

    using typename BaseManager::BlockAllocatorTraits;
    using typename BaseManager::BlockType;
    using typename BaseManager::ValueType;

    using AllocMode = typename BaseManager::AllocMode;

    constexpr explicit HakleBlockManager( std::size_t InSize, const AllocatorType& InAllocator = AllocatorType{} ) : BaseManager( InAllocator ), Pool( InSize, InAllocator ), List( InAllocator ) {}
    HAKLE_CPP20_CONSTEXPR ~HakleBlockManager() = default;

    HAKLE_CPP14_CONSTEXPR HakleBlockManager( HakleBlockManager&& Other ) noexcept = default;

    HAKLE_CPP14_CONSTEXPR HakleBlockManager& operator=( HakleBlockManager&& Other ) noexcept {
        if ( this != &Other ) {
            // Producers return pool-owned blocks to the free list. Clear/move the
            // list before the pool so it never traverses blocks after the pool
            // storage has been released.
            List = std::move( Other.List );
            Pool = std::move( Other.Pool );
            BaseManager::operator=( std::move( Other ) );
        }
        return *this;
    }

    HAKLE_CPP14_CONSTEXPR                    HakleBlockManager( const HakleBlockManager& Other ) = delete;
    HAKLE_CPP14_CONSTEXPR HakleBlockManager& operator=( const HakleBlockManager& Other )         = delete;

#if HAKLE_CPP_VERSION >= 20
    HAKLE_CPP14_CONSTEXPR void swap( HakleBlockManager& Other ) noexcept HAKLE_REQUIRES( std::swappable<BlockPool<BlockType, AllocatorType>>&& std::swappable<FreeList<BlockType, AllocatorType>> ) {
        BaseManager::swap( Other );
        Pool.swap( Other.Pool );
        List.swap( Other.List );
    }
#endif

    HAKLE_NODISCARD HAKLE_CPP14_CONSTEXPR std::size_t GetBlockPoolSize() const noexcept { return Pool.GetSize(); }

    HAKLE_CPP14_CONSTEXPR BlockType* RequisitionBlock( AllocMode Mode ) override {
        BlockType* Block = Pool.GetBlock();
        if ( Block != nullptr ) {
            return Block;
        }

        Block = List.TryGet();
        if ( Block != nullptr ) {
            return Block;
        }

        // TODO: constexpr
        // HAKLE_CONSTEXPR_IF( Mode == AllocMode::CannotAlloc ) { return nullptr; }
        if ( Mode == AllocMode::CannotAlloc ) {
            return nullptr;
        }
        else {
            // When alloc mode is CanAlloc, we allocate a new block
            // If user finishes using the block, it must be returned to the free list
            BlockType* NewBlock = BlockAllocatorTraits::Allocate( this->Allocator() );
            BlockAllocatorTraits::Construct( this->Allocator(), NewBlock );
            return NewBlock;
        }
    }

    HAKLE_CPP14_CONSTEXPR void ReturnBlock( BlockType* InBlock ) override { List.Add( InBlock ); }
    HAKLE_CPP14_CONSTEXPR void ReturnBlocks( BlockType* InBlock ) override {
        while ( InBlock != nullptr ) {
            BlockType* Next = InBlock->Next;
            List.Add( InBlock );
            InBlock = Next;
        }
    }

private:
    BlockPool<BlockType, AllocatorType> Pool;
    FreeList<BlockType, AllocatorType>  List;
};

#if HAKLE_CPP_VERSION >= 20

template <class Node, HAKLE_CONCEPT( std::swappable ) ALLOCATOR_TYPE>
inline HAKLE_CPP14_CONSTEXPR void swap( FreeList<Node, ALLOCATOR_TYPE>& lhs, FreeList<Node, ALLOCATOR_TYPE>& rhs ) noexcept HAKLE_SWAP_REQUIES {
    lhs.swap( rhs );
}

template <class BLOCK_TYPE, HAKLE_CONCEPT( std::swappable ) ALLOCATOR_TYPE>
inline HAKLE_CPP14_CONSTEXPR void swap( BlockPool<BLOCK_TYPE, ALLOCATOR_TYPE>& lhs, BlockPool<BLOCK_TYPE, ALLOCATOR_TYPE>& rhs ) noexcept HAKLE_SWAP_REQUIES {
    lhs.swap( rhs );
}

template <class BLOCK_TYPE, HAKLE_CONCEPT( std::swappable ) ALLOCATOR_TYPE>
inline HAKLE_CPP14_CONSTEXPR void swap( HakleBlockManager<BLOCK_TYPE, ALLOCATOR_TYPE>& lhs, HakleBlockManager<BLOCK_TYPE, ALLOCATOR_TYPE>& rhs ) noexcept HAKLE_SWAP_REQUIES {
    lhs.swap( rhs );
}

#endif

template <class T, std::size_t BLOCK_SIZE, HAKLE_CONCEPT( IsAllocator ) ALLOCATOR_TYPE = HakleAllocator<HakleFlagsBlock<T, BLOCK_SIZE>>>
using HakleFlagsBlockManager = HakleBlockManager<HakleFlagsBlock<T, BLOCK_SIZE>, ALLOCATOR_TYPE>;

template <class T, std::size_t BLOCK_SIZE, HAKLE_CONCEPT( IsAllocator ) ALLOCATOR_TYPE = HakleAllocator<HakleCounterBlock<T, BLOCK_SIZE>>>
using HakleCounterBlockManager = HakleBlockManager<HakleCounterBlock<T, BLOCK_SIZE>, ALLOCATOR_TYPE>;

inline constexpr std::size_t HAKLE_DEFAULT_POOL_SIZE = 1024;

#ifdef HAKLE_USE_CONCEPT
template <class T>
concept IsHakleBlockManagerInstance = std::is_same_v<T, HakleBlockManager<typename T::BlockType, typename T::AllocatorType>>;
#endif

template <HAKLE_CONCEPT( IsHakleBlockManagerInstance ) BLOCK_MANAGER_TYPE>
inline BLOCK_MANAGER_TYPE& GetBlockManager() {
#ifndef HAKLE_USE_CONCEPT
    static_assert( std::is_same<BlockManagerBase<typename BLOCK_MANAGER_TYPE::BlockType, typename BLOCK_MANAGER_TYPE::AllocatorType>, BLOCK_MANAGER_TYPE>::value, "BLOCK_MANAGER_TYPE must be HakleBlockManager" );
#endif
    static BLOCK_MANAGER_TYPE BlockManager( HAKLE_DEFAULT_POOL_SIZE );
    return BlockManager;
}

}  // namespace hakle

#endif  // BLOCKMANAGER_H
