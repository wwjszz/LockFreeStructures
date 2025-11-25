//
// Created by wwjszz on 25-11-24.
//

#ifndef BLOCKPOOL_H
#define BLOCKPOOL_H
#include <atomic>
#include <cstdint>

// BlockPool + FreeList
namespace hakle {

template <class T>
struct FreeListNode {
    std::atomic<uint32_t> Refs{ 0 };
    std::atomic<T*>       Next{ 0 };
};

// TODO: figure out memory order
template <class Node>
class FreeList {
public:
    static_assert( std::is_base_of_v<FreeListNode<Node>, Node>, "Node must be derived from FreeListNode<Node>" );

    void Add( Node* InNode ) noexcept {
        // Set AddFlag first
        if ( InNode->Refs.fetch_add( AddFlag, std::memory_order_release ) == 0 ) {
            InnerAdd( InNode );
        }
    }

    Node* TryGet() noexcept {
        Node* CurrentHead = Head.load( std::memory_order_acquire );
        while ( CurrentHead != nullptr ) {
            Node*    PrevHead = CurrentHead;
            uint32_t Refs     = CurrentHead->Refs.load( std::memory_order_relaxed );
            if ( ( Refs & RefsMask ) == 0  // check if already taken or adding
                 || ( !CurrentHead->Refs.compare_exchange_strong( Refs, Refs + 1, std::memory_order_acquire,
                                                                  std::memory_order_relaxed ) ) )  // try add refs
            {
                CurrentHead = Head.load( std::memory_order_acquire );
                continue;
            }

            // try Taken
            Node* Next = CurrentHead->Next.load( std::memory_order_relaxed );
            if ( Head.compare_exchange_strong( CurrentHead, Next, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                // taken success, decrease refcount twice, for our and list's ref
                CurrentHead->Refs.fetch_add( -2, std::memory_order_release );
                return CurrentHead;
            }

            // taken failed, decrease refcount
            Refs = PrevHead->Refs.fetch_add( -1, std::memory_order_release );
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

}  // namespace hakle

#endif  // BLOCKPOOL_H
