//
// Created by wwjszz on 25-11-24.
//

#ifndef BLOCKPOOL_H
#define BLOCKPOOL_H
#include "common/allocator.h"

#include <array>
#include <atomic>
#include <cstdint>

#include "common/memory.h"

// BlockPool + FreeList
namespace hakle {

template <class BLOCK_TYPE>
struct BlockTraits {
    constexpr static std::size_t BlockSize = BLOCK_TYPE::BlockSize;
    using ValueType                        = typename BLOCK_TYPE::ValueType;
    using BlockType                        = BLOCK_TYPE;
};

struct MemoryBase {
    bool HasOwner{ false };
};

template <class T>
struct FreeListNode : MemoryBase {
    std::atomic<uint32_t> Refs{ 0 };
    std::atomic<T*>       Next{ 0 };
};

// TODO: check memory order
template <class Node>
class FreeList {
public:
    static_assert( std::is_base_of_v<FreeListNode<Node>, Node>, "Node must be derived from FreeListNode<Node>" );

    FreeList() = default;

    ~FreeList() {
        Node* CurrentNode = Head.load( std::memory_order_relaxed );
        while ( CurrentNode != nullptr ) {
            Node* Next = CurrentNode->Next.load( std::memory_order_relaxed );
            if ( !Next->HasOwner ) {
                HAKLE_DELETE( CurrentNode );
            }
            CurrentNode = Next;
        }
    }

    void Add( Node* InNode ) noexcept {
        // Set AddFlag first
        if ( InNode->Refs.fetch_add( AddFlag, std::memory_order_relaxed ) == 0 ) {
            InnerAdd( InNode );
        }
    }

    Node* TryGet() noexcept {
        Node* CurrentHead = Head.load( std::memory_order_relaxed );
        while ( CurrentHead != nullptr ) {
            Node*    PrevHead = CurrentHead;
            uint32_t Refs     = CurrentHead->Refs.load( std::memory_order_relaxed );
            if ( ( Refs & RefsMask ) == 0  // check if already taken or adding
                 || ( !CurrentHead->Refs.compare_exchange_strong( Refs, Refs + 1, std::memory_order_acquire,
                                                                  std::memory_order_relaxed ) ) )  // try add refs
            {
                CurrentHead = Head.load( std::memory_order_relaxed );
                continue;
            }

            // try Taken
            Node* Next = CurrentHead->Next.load( std::memory_order_relaxed );
            if ( Head.compare_exchange_strong( CurrentHead, Next, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                // taken success, decrease refcount twice, for our and list's ref
                CurrentHead->Refs.fetch_add( -2, std::memory_order_relaxed );
                return CurrentHead;
            }

            // taken failed, decrease refcount
            Refs = PrevHead->Refs.fetch_add( -1, std::memory_order_relaxed );
            if ( Refs == AddFlag + 1 ) {
                // no one is using it, add it back
                InnerAdd( PrevHead );
            }
        }
        return nullptr;
    }

    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    // only useful when there is no contention (e.g. destruction)
    Node* GetHead() const noexcept { return Head.load( std::memory_order_relaxed ); }

private:
    // add when ref count == 0
    void InnerAdd( Node* InNode ) noexcept {
        Node* CurrentHead = Head.load( std::memory_order_relaxed );
        while ( true ) {
            // first update next then refs
            InNode->Next.store( CurrentHead, std::memory_order_relaxed );
            InNode->Refs.store( 1, std::memory_order_release );
            if ( !Head.compare_exchange_strong( CurrentHead, InNode, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                // check if someone already using it
                if ( InNode->Refs.fetch_add( AddFlag - 1, std::memory_order_release ) == 1 ) {
                    continue;
                }
            }
            return;
        }
    }

    static constexpr uint32_t RefsMask = 0x7fffffff;
    static constexpr uint32_t AddFlag  = 0x80000000;

    std::atomic<Node*> Head{ nullptr };
};

template <class BLOCK_TYPE, class ALLOCATOR_TYPE = HakleAllocator<BLOCK_TYPE>>
class BlockPool {
public:
    using Allocator = ALLOCATOR_TYPE;

    explicit BlockPool( std::size_t InSize ) : Size( InSize ) {
        Head = Allocator::allocate( Size );
        for ( std::size_t i = 0; i < Size; i++ ) {
            Allocator::construct( Head + i );
            Head[ i ].HasOwner = true;
        }
    }
    ~BlockPool() {
        Allocator::Destroy( Head, Size );
        Allocator::Deallocate( Head, Size );
    }

    BLOCK_TYPE* GetBlock() noexcept {
        if ( Index.load( std::memory_order_relaxed ) >= Size )
            return nullptr;

        std::size_t CurrentIndex = Index.fetch_add( 1, std::memory_order_relaxed );
        return Head + CurrentIndex;
    }

private:
    std::size_t              Size{ 0 };
    std::atomic<std::size_t> Index{ 0 };
    BLOCK_TYPE*              Head{ nullptr };
};

template <class ALLOCATOR_TYPE>
class BlockManagerBase {
public:
    using AllocatorType                    = ALLOCATOR_TYPE;
    using AllocatorTraits                  = HakeAllocatorTraits<AllocatorType>;
    using BlockType                        = typename AllocatorTraits::ValueType;
    using BlockTraits                      = BlockTraits<BlockType>;
    constexpr static std::size_t BlockSize = BlockTraits::BlockSize;
    using ValueType                        = typename BlockTraits::ValueType;

    virtual ~BlockManagerBase() = default;
    enum class AllocMode { CanAlloc, CannotAlloc };

    virtual BlockType* RequisitionBlock( AllocMode InMode ) = 0;
    virtual void       ReturnBlocks( BlockType* InBlock )   = 0;
    virtual void       ReturnBlock( BlockType* InBlock )    = 0;
};

// We set a block pool and a free list
template <class ALLOCATOR_TYPE>
class HakleBlockManager : public BlockManagerBase<ALLOCATOR_TYPE> {
public:
    using BaseManager = BlockManagerBase<ALLOCATOR_TYPE>;
    using BaseManager::BlockSize;
    using typename BaseManager::AllocatorTraits;
    using typename BaseManager::AllocatorType;
    using typename BaseManager::BlockTraits;
    using typename BaseManager::BlockType;
    using typename BaseManager::ValueType;

    using AllocMode = typename BaseManager::AllocMode;

    explicit HakleBlockManager( std::size_t InSize ) : Pool( InSize ) {}
    ~HakleBlockManager() override = default;

    BlockType* RequisitionBlock( AllocMode Mode ) override {
        BlockType* Block = Pool.GetBlock();
        if ( Block != nullptr ) {
            return Block;
        }

        Block = FreeList.TryGet();
        if ( Block != nullptr ) {
            return Block;
        }

        HAKLE_CONSTEXPR_IF( Mode == AllocMode::CannotAlloc ) { return nullptr; }
        else {
            // When alloc mode is CanAlloc, we allocate a new block
            // If user finishes using the block, it must be returned to the free list
            BlockType* NewBlock = AllocatorTraits::Allocate( Allocator );
            AllocatorTraits::Construct( Allocator, NewBlock );
            return NewBlock;
        }
    }

    void ReturnBlock( BlockType* InBlock ) override { FreeList.Add( InBlock ); }
    void ReturnBlocks( BlockType* InBlock ) override {
        while ( InBlock != nullptr ) {
            BlockType* Next = InBlock->Next;
            FreeList.Add( InBlock );
            InBlock = Next;
        }
    }

private:
    // TODO: compress the allocator type
    AllocatorType        Allocator;
    BlockPool<BlockType> Pool;
    FreeList<BlockType>  FreeList;
};

}  // namespace hakle

#endif  // BLOCKPOOL_H
