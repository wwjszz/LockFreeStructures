//
// Created by wwjszz on 25-11-24.
//

#ifndef BLOCKMANAGER_H
#define BLOCKMANAGER_H

#include <atomic>
#include <cstddef>

#include "Block.h"
#include "common/CompressPair.h"
#include "common/allocator.h"

// BlockPool + FreeList
namespace hakle {

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
concept IsBlockManager = requires( T& t, typename T::AllocMode Mode, typename T::BlockType* p ) {
    { t.RequisitionBlock( Mode ) } -> std::same_as<typename T::BlockType*>;
    t.ReturnBlock( p );
    t.ReturnBlocks( p );
};

template <class BLOCK_TYPE, class T>
concept CheckBlockManager = IsBlock<BLOCK_TYPE> && std::same_as<BLOCK_TYPE, typename T::BlockType>;

#endif

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
    static_assert( std::is_base_of<FreeListNode<Node>, Node>::value, "Node must be derived from FreeListNode<Node>" );

    using AllocatorType   = ALLOCATOR_TYPE;
    using AllocatorTraits = HakeAllocatorTraits<AllocatorType>;

    constexpr explicit FreeList( const AllocatorType& InAllocator = AllocatorType{} ) : AllocatorPair( nullptr, InAllocator ) {}

    HAKLE_CPP20_CONSTEXPR ~FreeList() {
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

    constexpr void Add( Node* InNode ) noexcept {
        // Set AddFlag first
        if ( InNode->FreeListRefs.fetch_add( AddFlag, std::memory_order_relaxed ) == 0 ) {
            InnerAdd( InNode );
        }
    }

    constexpr Node* TryGet() noexcept {
        Node* CurrentHead = Head().load( std::memory_order_relaxed );
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
            if ( Head().compare_exchange_strong( CurrentHead, Next, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                // taken success, decrease refcount twice, for our and list's ref
                CurrentHead->FreeListRefs.fetch_add( -2, std::memory_order_relaxed );
                return CurrentHead;
            }

            // taken failed, decrease refcount
            Refs = PrevHead->FreeListRefs.fetch_add( -1, std::memory_order_relaxed );
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
    constexpr void InnerAdd( Node* InNode ) noexcept {
        Node* CurrentHead = Head().load( std::memory_order_relaxed );
        while ( true ) {
            // first update next then refs
            InNode->FreeListNext.store( CurrentHead, std::memory_order_relaxed );
            InNode->FreeListRefs.store( 1, std::memory_order_release );
            if ( !Head().compare_exchange_strong( CurrentHead, InNode, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                // check if someone already using it
                if ( InNode->FreeListRefs.fetch_add( AddFlag - 1, std::memory_order_release ) == 1 ) {
                    continue;
                }
            }
            return;
        }
    }

    static constexpr uint32_t RefsMask = 0x7fffffff;
    static constexpr uint32_t AddFlag  = 0x80000000;

    constexpr AllocatorType&            Allocator() noexcept { return AllocatorPair.Second(); }
    constexpr const AllocatorType&      Allocator() const noexcept { return AllocatorPair.Second(); }
    constexpr std::atomic<Node*>&       Head() noexcept { return AllocatorPair.First(); }
    constexpr const std::atomic<Node*>& Head() const noexcept { return AllocatorPair.First(); }

    // compressed allocator
    CompressPair<std::atomic<Node*>, AllocatorType> AllocatorPair{};
};

template <HAKLE_CONCEPT( IsBlock ) BLOCK_TYPE, HAKLE_CONCEPT( IsAllocator ) ALLOCATOR_TYPE = HakleAllocator<BLOCK_TYPE>>
class BlockPool {
public:
    using AllocatorType   = ALLOCATOR_TYPE;
    using AllocatorTraits = HakeAllocatorTraits<AllocatorType>;

    constexpr explicit BlockPool( std::size_t InSize, const AllocatorType& InAllocator = AllocatorType{} ) : AllocatorPair{ InSize, InAllocator } {
        Head = AllocatorTraits::Allocate( Allocator(), Size() );
        for ( std::size_t i = 0; i < Size(); i++ ) {
            AllocatorTraits::Construct( Allocator(), Head + i );
            Head[ i ].HasOwner = true;
        }
    }

    HAKLE_CPP20_CONSTEXPR ~BlockPool() {
        AllocatorTraits::Destroy( Allocator(), Head, Size() );
        AllocatorTraits::Deallocate( Allocator(), Head, Size() );
    }

    constexpr BLOCK_TYPE* GetBlock() noexcept {
        if ( Index.load( std::memory_order_relaxed ) >= Size() )
            return nullptr;

        std::size_t CurrentIndex = Index.fetch_add( 1, std::memory_order_relaxed );
        return CurrentIndex < Size() ? ( Head + CurrentIndex ) : nullptr;
    }

private:
    constexpr AllocatorType&                     Allocator() noexcept { return AllocatorPair.Second(); }
    constexpr const AllocatorType&               Allocator() const noexcept { return AllocatorPair.Second(); }
    constexpr std::size_t&                       Size() noexcept { return AllocatorPair.First(); }
    HAKLE_NODISCARD constexpr const std::size_t& Size() const noexcept { return AllocatorPair.First(); }

    // compressed allocator
    CompressPair<std::size_t, AllocatorType> AllocatorPair{};
    std::atomic<std::size_t>                 Index{ 0 };
    BLOCK_TYPE*                              Head{ nullptr };
};

template <HAKLE_CONCEPT( IsBlock ) BLOCK_TYPE, HAKLE_CONCEPT( IsAllocator ) ALLOCATOR_TYPE>
class BlockManagerBase : private CompressPairElem<ALLOCATOR_TYPE, 0> {
public:
    using AllocatorType        = ALLOCATOR_TYPE;
    using BlockAllocatorTraits = HakeAllocatorTraits<AllocatorType>;
    using BlockType            = BLOCK_TYPE;
#if HAKLE_CPP_VERSION >= 20
    constexpr static std::size_t BlockSize = BlockType::BlockSize;
    using ValueType                        = typename BlockType::ValueType;
#else
    using BlockTraits                      = BlockTraits<BlockType>;
    constexpr static std::size_t BlockSize = BlockTraits::BlockSize;
    using ValueType                        = typename BlockTraits::ValueType;
#endif

    constexpr BlockManagerBase() = default;
    constexpr explicit BlockManagerBase( const AllocatorType& InAllocator = AllocatorType{} ) : Base( InAllocator ) {}
    virtual ~BlockManagerBase() = default;

    enum class AllocMode { CanAlloc, CannotAlloc };

    virtual HAKLE_CPP20_CONSTEXPR BlockType* RequisitionBlock( AllocMode InMode ) = 0;
    virtual HAKLE_CPP20_CONSTEXPR void       ReturnBlocks( BlockType* InBlock )   = 0;
    virtual HAKLE_CPP20_CONSTEXPR void       ReturnBlock( BlockType* InBlock )    = 0;

    constexpr AllocatorType&       Allocator() noexcept { return Base::Get(); }
    constexpr const AllocatorType& Allocator() const noexcept { return Base::Get(); }

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

    constexpr explicit HakleBlockManager( std::size_t InSize, const AllocatorType& InAllocator = AllocatorType{} ) : BaseManager( InAllocator ), Pool( InSize, InAllocator ), FreeList( InAllocator ) {}
    HAKLE_CPP20_CONSTEXPR ~HakleBlockManager() override = default;

    constexpr BlockType* RequisitionBlock( AllocMode Mode ) override {
        BlockType* Block = Pool.GetBlock();
        if ( Block != nullptr ) {
            return Block;
        }

        Block = FreeList.TryGet();
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

    constexpr void ReturnBlock( BlockType* InBlock ) override { FreeList.Add( InBlock ); }
    constexpr void ReturnBlocks( BlockType* InBlock ) override {
        while ( InBlock != nullptr ) {
            BlockType* Next = InBlock->Next;
            FreeList.Add( InBlock );
            InBlock = Next;
        }
    }

private:
    BlockPool<BlockType> Pool;
    FreeList<BlockType>  FreeList;
};

template <class T, std::size_t BLOCK_SIZE>
using HakleFlagsBlockManager = HakleBlockManager<HakleFlagsBlock<T, BLOCK_SIZE>>;

template <class T, std::size_t BLOCK_SIZE>
using HakleCounterBlockManager = HakleBlockManager<HakleCounterBlock<T, BLOCK_SIZE>>;

}  // namespace hakle

#endif  // BLOCKMANAGER_H
