//
// Created by wwjszz on 25-11-24.
//

#ifndef BLOCKPOOL_H
#define BLOCKPOOL_H
#include <atomic>
#include <cstdint>
#include <array>

#include "common/memory.h"

// BlockPool + FreeList
namespace hakle {

struct MemoryBase {
    bool HasOwner{ false };
};

template <class T>
struct FreeListNode : virtual MemoryBase {
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

template <class T, std::size_t BLOCK_SIZE>
struct BaseBlock : virtual MemoryBase {
    alignas( HAKLE_CACHE_LINE_SIZE ) std::array<T, BLOCK_SIZE> Elements;
};

template <class T, std::size_t BLOCK_SIZE, class BLOCK_TYPE = BaseBlock<T, BLOCK_SIZE>>
class BlockPool {
public:
    static_assert( std::is_base_of_v<BaseBlock<T, BLOCK_SIZE>, BLOCK_TYPE>, "BLOCK_ TYPE must be derived from BaseBlock<T, BLOCK_SIZE>" );

    explicit BlockPool( std::size_t InSize ) : Size( InSize ) {
        Head = HAKLE_OPERATOR_NEW_ARRAY( BLOCK_TYPE, Size );
        for ( std::size_t i = 0; i < Size; i++ ) {
            new ( Head + i ) BLOCK_TYPE();
            Head[ i ].HasOwner = true;
        }
    }
    ~BlockPool() { HAKLE_DELETE_ARRAY( Head, Size ); }

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



}  // namespace hakle

#endif  // BLOCKPOOL_H
