//
// Created by admin on 25-12-1.
//

#ifndef CONCURRENTQUEUE_H
#define CONCURRENTQUEUE_H

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>

#include "BlockManager.h"
#include "ConcurrentQueue/Block.h"
#include "ConcurrentQueue/HashTable.h"
#include "common/CompressPair.h"
#include "common/allocator.h"
#include "common/common.h"
#include "common/utility.h"

namespace hakle {

namespace details {
    using thread_id_t = std::thread::id;
    static const thread_id_t invalid_thread_id;
    inline thread_id_t       thread_id() noexcept { return std::this_thread::get_id(); }
    using thread_hash = std::hash<std::thread::id>;
}  // namespace details

#ifdef HAKLE_USE_CONCEPT
template <class Traits, class = void>
struct HasMakeExplicitBlockManagerHelper : std::false_type {};

template <class Traits>
struct HasMakeExplicitBlockManagerHelper<Traits, std::void_t<decltype( &Traits::MakeExplicitBlockManager )>> : std::true_type {};

// 检测 Traits 里有没有 MakeImplicit
template <class Traits, class = void>
struct HasMakeImplicitBlockManagerHelper : std::false_type {};

template <class Traits>
struct HasMakeImplicitBlockManagerHelper<Traits, std::void_t<decltype( &Traits::MakeImplicitBlockManager )>> : std::true_type {};

template <class Traits>
concept IsConcurrentQueueTraits = requires( const typename Traits::ExplicitAllocatorType& ExplicitAllocator, const typename Traits::ImplicitAllocatorType& ImplicitAllocator ) {
    { Traits::BlockSize } -> std::convertible_to<std::size_t>;
    { Traits::InitialHashSize } -> std::convertible_to<std::size_t>;
    { Traits::InitialExplicitQueueSize } -> std::convertible_to<std::size_t>;
    { Traits::InitialImplicitQueueSize } -> std::convertible_to<std::size_t>;

    { Traits::MakeDefaultExplicitBlockManager( ExplicitAllocator ) } -> std::same_as<typename Traits::ExplicitBlockManagerType>;
    { Traits::MakeDefaultImplicitBlockManager( ImplicitAllocator ) } -> std::same_as<typename Traits::ImplicitBlockManagerType>;

    requires Traits::BlockSize > 0 && Traits::InitialHashSize > 0;
    requires IsAllocator<typename Traits::AllocatorType> && IsAllocator<typename Traits::ExplicitAllocatorType> && IsAllocator<typename Traits::ImplicitAllocatorType>;
    requires IsBlock<typename Traits::ExplicitBlockType> && IsBlock<typename Traits::ImplicitBlockType>;
    requires IsBlockManager<typename Traits::ExplicitBlockManagerType> && IsBlockManager<typename Traits::ImplicitBlockManagerType>;
};

template <class Traits>
concept HasMakeExplicitBlockManager = HasMakeExplicitBlockManagerHelper<Traits>::value;

template <class Traits>
concept HasMakeImplicitBlockManager = HasMakeImplicitBlockManagerHelper<Traits>::value;

#endif

struct _QueueTypelessBase {};

// TODO: manager traits
// NOTE: QueueBase is an internal non-virtual base class and must never be destroyed via a base-class pointer.
template <class T, std::size_t BLOCK_SIZE, class Allocator, HAKLE_CONCEPT( IsBlock ) BLOCK_TYPE, HAKLE_CONCEPT( IsBlockManager ) BLOCK_MANAGER_TYPE>
HAKLE_REQUIRES( CheckBlockSize<BLOCK_SIZE, BLOCK_TYPE>&& CheckBlockManager<BLOCK_TYPE, BLOCK_MANAGER_TYPE> )
struct _QueueBase : public _QueueTypelessBase {
public:
    using BlockManagerType                 = BLOCK_MANAGER_TYPE;
    using BlockType                        = BLOCK_TYPE;
    using ValueType                        = typename BlockManagerType::ValueType;
    constexpr static std::size_t BlockSize = BlockManagerType::BlockSize;

    using ValueAllocatorType   = Allocator;
    using ValueAllocatorTraits = HakeAllocatorTraits<ValueAllocatorType>;

    using AllocMode = typename BlockManagerType::AllocMode;

    constexpr explicit _QueueBase( const ValueAllocatorType& InAllocator = ValueAllocatorType{} ) noexcept : ValueAllocatorPair( nullptr, InAllocator ) {}
    HAKLE_CPP20_CONSTEXPR ~_QueueBase() = default;

    HAKLE_NODISCARD constexpr std::size_t Size() const noexcept {
        std::size_t Tail = TailIndex.load( std::memory_order_relaxed );
        std::size_t Head = HeadIndex.load( std::memory_order_relaxed );
        return CircularLessThan( Head, Tail ) ? Tail - Head : 0;
    }

    HAKLE_NODISCARD constexpr std::size_t GetTail() const noexcept { return TailIndex.load( std::memory_order_relaxed ); }

protected:
    std::atomic<std::size_t>                     HeadIndex{};
    std::atomic<std::size_t>                     TailIndex{};
    std::atomic<std::size_t>                     DequeueAttemptsCount{};
    std::atomic<std::size_t>                     DequeueFailedCount{};
    CompressPair<BlockType*, ValueAllocatorType> ValueAllocatorPair{};

    constexpr ValueAllocatorType&       ValueAllocator() noexcept { return ValueAllocatorPair.Second(); }
    constexpr const ValueAllocatorType& ValueAllocator() const noexcept { return ValueAllocatorPair.Second(); }

    constexpr BlockType*&                       TailBlock() noexcept { return ValueAllocatorPair.First(); }
    HAKLE_NODISCARD constexpr const BlockType*& TailBlock() const noexcept { return ValueAllocatorPair.First(); }
};

// SPMC Queue
template <class T, std::size_t BLOCK_SIZE, class Allocator = HakleAllocator<T>, HAKLE_CONCEPT( IsBlock ) BLOCK_TYPE = HakleFlagsBlock<T, BLOCK_SIZE>,
          HAKLE_CONCEPT( IsBlockManager ) BLOCK_MANAGER_TYPE = HakleBlockManager<BLOCK_TYPE>>
class FastQueue : public _QueueBase<T, BLOCK_SIZE, Allocator, BLOCK_TYPE, BLOCK_MANAGER_TYPE> {
public:
    using Base = _QueueBase<T, BLOCK_SIZE, Allocator, BLOCK_TYPE, BLOCK_MANAGER_TYPE>;

    using Base::BlockSize;
    using typename Base::AllocMode;
    using typename Base::BlockManagerType;
    using typename Base::BlockType;
    using typename Base::ValueAllocatorTraits;
    using typename Base::ValueAllocatorType;
    using typename Base::ValueType;

private:
    constexpr static std::size_t BlockSizeLog2 = BitWidth( BlockSize ) - 1;

    struct IndexEntry;
    struct IndexEntryArray;
    using IndexEntryAllocatorType        = typename ValueAllocatorTraits::template RebindAlloc<IndexEntry>;
    using IndexEntryArrayAllocatorType   = typename ValueAllocatorTraits::template RebindAlloc<IndexEntryArray>;
    using IndexEntryAllocatorTraits      = typename ValueAllocatorTraits::template RebindTraits<IndexEntry>;
    using IndexEntryArrayAllocatorTraits = typename ValueAllocatorTraits::template RebindTraits<IndexEntryArray>;

public:
    constexpr explicit FastQueue( std::size_t InSize, BlockManagerType& InBlockManager, const ValueAllocatorType& InAllocator = ValueAllocatorType{} ) noexcept
        : Base( InAllocator ), BlockManager( InBlockManager ), IndexEntryAllocatorPair( 0, IndexEntryAllocatorType( InAllocator ) ),
          IndexEntryArrayAllocatorPair( 0, IndexEntryArrayAllocatorType( InAllocator ) ) {
        std::size_t InitialSize = CeilToPow2( InSize ) >> 1;
        if ( InitialSize < 2 ) {
            InitialSize = 2;
        }
        PO_IndexEntriesSize() = InitialSize;

        CreateNewBlockIndexArray( 0 );
    }

    HAKLE_CPP20_CONSTEXPR ~FastQueue() {
        if ( this->TailBlock() != nullptr ) {
            // first, we find the first block that's half dequeued
            BlockType* HalfDequeuedBlock = nullptr;
            if ( ( this->HeadIndex.load( std::memory_order_relaxed ) & ( BlockSize - 1 ) ) != 0 ) {
                std::size_t i = ( PO_NextIndexEntry - PO_IndexEntriesUsed() ) & ( PO_IndexEntriesSize() - 1 );
                while ( CircularLessThan( this->PO_PrevEntries[ i ].Base + BlockSize, this->HeadIndex.load( std::memory_order_relaxed ) ) ) {
                    i = ( i + 1 ) & ( PO_IndexEntriesSize() - 1 );
                }
                HalfDequeuedBlock = PO_PrevEntries[ i ].InnerBlock;
            }

            // then, we can return back all the blocks
            BlockType* Block = this->TailBlock();
            do {
                Block = Block->Next;
                if ( Block->IsEmpty() ) {
                    continue;
                }

                std::size_t i = 0;
                if ( Block == HalfDequeuedBlock ) {
                    i = this->HeadIndex.load( std::memory_order_relaxed ) & ( BlockSize - 1 );
                }
                std::size_t Temp      = this->TailIndex.load( std::memory_order_relaxed ) & ( BlockSize - 1 );
                std::size_t LastIndex = Temp == 0 ? BlockSize : Temp;
                while ( i != BlockSize && ( Block != this->TailBlock() || i != LastIndex ) ) {
                    ValueAllocatorTraits::Destroy( this->ValueAllocator(), ( *Block )[ i++ ] );
                }
            } while ( Block != this->TailBlock() );
        }

        // let's return block to manager
        if ( this->TailBlock() != nullptr ) {
            BlockType* Block = this->TailBlock();
            do {
                BlockType* NextBlock = Block->Next;
                BlockManager.ReturnBlock( Block );
                Block = NextBlock;
            } while ( Block != this->TailBlock() );
        }

        // delete index entry arrays
        IndexEntryArray* Current = CurrentIndexEntryArray.load( std::memory_order_relaxed );
        while ( Current != nullptr ) {
            IndexEntryArray* Prev = Current->Prev;
            IndexEntryAllocatorTraits::Deallocate( IndexEntryAllocator(), Current->Entries, Current->Size );
            IndexEntryArrayAllocatorTraits::Destroy( IndexEntryArrayAllocator(), Current );
            IndexEntryArrayAllocatorTraits::Deallocate( IndexEntryArrayAllocator(), Current );
            Current = Prev;
        }
    }

    // Enqueue, SPMC queue only supports one producer
    template <AllocMode Mode, class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<ValueType, Args&&...> )
    HAKLE_CPP20_CONSTEXPR bool Enqueue( Args&&... args ) {
        std::size_t CurrentTailIndex = this->TailIndex.load( std::memory_order_relaxed );
        std::size_t NewTailIndex     = CurrentTailIndex + 1;
        std::size_t InnerIndex       = CurrentTailIndex & ( BlockSize - 1 );
        if HAKLE_UNLIKELY ( InnerIndex == 0 ) {
            BlockType* OldTailBlock = this->TailBlock();
            // zero, in fact
            // we must find a new block
            if ( this->TailBlock() != nullptr && this->TailBlock()->Next->IsEmpty() ) {
                // we can re-use that block
                this->TailBlock() = this->TailBlock()->Next;
                this->TailBlock()->Reset();
            }
            else {
                // we need to find a new block index and get a new block from block manager
                // TODO: add MAX_SIZE check
                if HAKLE_UNLIKELY ( !CircularLessThan( this->HeadIndex.load( std::memory_order_relaxed ), CurrentTailIndex + BlockSize ) ) {
                    return false;
                }

                if HAKLE_UNLIKELY ( CurrentIndexEntryArray.load( std::memory_order_relaxed ) == nullptr || PO_IndexEntriesUsed() == PO_IndexEntriesSize() ) {
                    // need to create a new index entry array
                    HAKLE_CONSTEXPR_IF( Mode == AllocMode::CannotAlloc ) { return false; }
                    else if ( !CreateNewBlockIndexArray( PO_IndexEntriesUsed() ) ) {
                        return false;
                    }
                }

                BlockType* NewBlock = BlockManager.RequisitionBlock( Mode );
                if HAKLE_UNLIKELY ( NewBlock == nullptr ) {
                    return false;
                }

                NewBlock->Reset();
                if HAKLE_UNLIKELY ( this->TailBlock() == nullptr ) {
                    NewBlock->Next = NewBlock;
                }
                else {
                    NewBlock->Next          = this->TailBlock()->Next;
                    this->TailBlock()->Next = NewBlock;
                }
                this->TailBlock() = NewBlock;
                // get a new block
                ++PO_IndexEntriesUsed();
            }

            IndexEntry& Entry = this->CurrentIndexEntryArray.load( std::memory_order_relaxed )->Entries[ PO_NextIndexEntry ];
            Entry.Base        = CurrentTailIndex;
            Entry.InnerBlock  = this->TailBlock();
            this->CurrentIndexEntryArray.load( std::memory_order_relaxed )->Tail.store( PO_NextIndexEntry, std::memory_order_release );
            PO_NextIndexEntry = ( PO_NextIndexEntry + 1 ) & ( PO_IndexEntriesSize() - 1 );

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<ValueType, Args&&...>::value ) {
                // we need to handle exception here
                HAKLE_TRY { ValueAllocatorTraits::Construct( this->ValueAllocator(), ( *( this->TailBlock() ) )[ InnerIndex ], std::forward<Args>( args )... ); }
                HAKLE_CATCH( ... ) {
                    // when OldTailBlock is nullptr, we should not go back to prevent block leak
                    // rollback
                    this->TailBlock()->SetAllEmpty();
                    this->TailBlock() = OldTailBlock == nullptr ? this->TailBlock() : OldTailBlock;
                    PO_NextIndexEntry = ( PO_NextIndexEntry - 1 ) & ( PO_IndexEntriesSize() - 1 );
                    HAKLE_RETHROW;
                }
            }

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<ValueType, Args&&...>::value ) {
                this->TailIndex.store( NewTailIndex, std::memory_order_release );
                return true;
            }
        }

        ValueAllocatorTraits::Construct( this->ValueAllocator(), ( *( this->TailBlock() ) )[ InnerIndex ], std::forward<Args>( args )... );

        this->TailIndex.store( NewTailIndex, std::memory_order_release );
        return true;
    }

    template <AllocMode Mode, HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { ValueType( *Item ); } )
    HAKLE_CPP20_CONSTEXPR bool EnqueueBulk( Iterator ItemFirst, std::size_t Count ) {
        // set original state
        std::size_t OriginIndexEntriesUsed = PO_IndexEntriesUsed();
        std::size_t OriginNextIndexEntry   = PO_NextIndexEntry;
        BlockType*  StartBlock             = this->TailBlock();
        std::size_t StartTailIndex         = this->TailIndex.load( std::memory_order_relaxed );
        BlockType*  FirstAllocatedBlock    = nullptr;

        // roll back
        auto RollBack = [ this, &OriginNextIndexEntry, &StartBlock ]() -> void {
            PO_NextIndexEntry = OriginNextIndexEntry;
            this->TailBlock() = StartBlock;
        };

        std::size_t LastTailIndex = StartTailIndex - 1;
        // std::size_t BlockCountNeed =
        //     ( ( ( Count + LastTailIndex ) & ~( BlockSize - 1 ) ) - ( ( LastTailIndex & ~( BlockSize - 1 ) ) ) ) >> BlockSizeLog2;

        // StartTailIndex - 1 must be signed before shifting
        std::size_t BlockCountNeed   = ( ( Count + StartTailIndex - 1 ) >> BlockSizeLog2 ) - ( static_cast<std::make_signed_t<std::size_t>>( StartTailIndex - 1 ) >> BlockSizeLog2 );
        std::size_t CurrentTailIndex = LastTailIndex & ~( BlockSize - 1 );

        if HAKLE_LIKELY ( BlockCountNeed > 0 ) {
            while ( BlockCountNeed > 0 && this->TailBlock() != nullptr && this->TailBlock()->Next->IsEmpty() ) {
                // we can re-use that block
                --BlockCountNeed;
                CurrentTailIndex += BlockSize;

                this->TailBlock()   = this->TailBlock()->Next;
                FirstAllocatedBlock = FirstAllocatedBlock == nullptr ? this->TailBlock() : FirstAllocatedBlock;
                this->TailBlock()->Reset();

                auto& Entry       = this->CurrentIndexEntryArray.load( std::memory_order_relaxed )->Entries[ PO_NextIndexEntry ];
                Entry.Base        = CurrentTailIndex;
                Entry.InnerBlock  = this->TailBlock();
                PO_NextIndexEntry = ( PO_NextIndexEntry + 1 ) & ( PO_IndexEntriesSize() - 1 );
            }

            while ( BlockCountNeed > 0 ) {
                // we must get a new block
                --BlockCountNeed;
                CurrentTailIndex += BlockSize;

                // TODO: add MAX_SIZE check
                if HAKLE_UNLIKELY ( !CircularLessThan( this->HeadIndex.load( std::memory_order_relaxed ), CurrentTailIndex + BlockSize ) ) {
                    RollBack();
                    return false;
                }

                if HAKLE_UNLIKELY ( CurrentIndexEntryArray.load( std::memory_order_relaxed ) == nullptr || PO_IndexEntriesUsed() == PO_IndexEntriesSize() ) {
                    // need to create a new index entry array
                    HAKLE_CONSTEXPR_IF( Mode == AllocMode::CannotAlloc ) {
                        RollBack();
                        return false;
                    }
                    else if ( !CreateNewBlockIndexArray( OriginIndexEntriesUsed ) ) {
                        RollBack();
                        return false;
                    }

                    OriginNextIndexEntry = OriginIndexEntriesUsed;
                }

                BlockType* NewBlock = BlockManager.RequisitionBlock( Mode );
                if HAKLE_UNLIKELY ( NewBlock == nullptr ) {
                    RollBack();
                    return false;
                }

                NewBlock->Reset();
                if ( this->TailBlock() == nullptr ) {
                    NewBlock->Next = NewBlock;
                }
                else {
                    NewBlock->Next          = this->TailBlock()->Next;
                    this->TailBlock()->Next = NewBlock;
                }
                this->TailBlock()   = NewBlock;
                FirstAllocatedBlock = FirstAllocatedBlock == nullptr ? this->TailBlock() : FirstAllocatedBlock;
                // get a new block
                ++PO_IndexEntriesUsed();

                auto& Entry       = this->CurrentIndexEntryArray.load( std::memory_order_relaxed )->Entries[ PO_NextIndexEntry ];
                Entry.Base        = CurrentTailIndex;
                Entry.InnerBlock  = this->TailBlock();
                PO_NextIndexEntry = ( PO_NextIndexEntry + 1 ) & ( PO_IndexEntriesSize() - 1 );
            }
        }

        // we already have enough blocks, let's fill them
        std::size_t StartInnerIndex = StartTailIndex & ( BlockSize - 1 );
        BlockType*  CurrentBlock    = ( StartInnerIndex == 0 && FirstAllocatedBlock != nullptr ) ? FirstAllocatedBlock : StartBlock;
        while ( true ) {
            std::size_t EndInnerIndex = ( CurrentBlock == this->TailBlock() ) ? ( StartTailIndex + Count - 1 ) & ( BlockSize - 1 ) : ( BlockSize - 1 );
            HAKLE_CONSTEXPR_IF( std::is_nothrow_constructible<ValueType, typename std::iterator_traits<Iterator>::value_type&>::value ) {
                while ( StartInnerIndex <= EndInnerIndex ) {
                    ValueAllocatorTraits::Construct( this->ValueAllocator(), ( *CurrentBlock )[ StartInnerIndex ], *ItemFirst++ );
                    ++StartInnerIndex;
                }
            }
            else {
                HAKLE_TRY {
                    while ( StartInnerIndex <= EndInnerIndex ) {
                        ValueAllocatorTraits::Construct( this->ValueAllocator(), ( *( CurrentBlock ) )[ StartInnerIndex ], *ItemFirst++ );
                        ++StartInnerIndex;
                    }
                }
                HAKLE_CATCH( ... ) {
                    // we need to set all allocated blocks to empty
                    if ( FirstAllocatedBlock != nullptr ) {
                        BlockType* AllocatedBlock = FirstAllocatedBlock;
                        while ( true ) {
                            AllocatedBlock->SetAllEmpty();
                            // BlockManager.ReturnBlock( AllocatedBlock  );
                            if ( AllocatedBlock == this->TailBlock() ) {
                                break;
                            }
                            AllocatedBlock = AllocatedBlock->Next;
                        }
                    }

                    RollBack();

                    // destroy all values
#if !defined( ENABLE_MEMORY_LEAK_DETECTION )
                    HAKLE_CONSTEXPR_IF( !std::is_trivially_destructible<ValueType>::value ) {
#endif
                        std::size_t StartInnerIndex2 = StartTailIndex & ( BlockSize - 1 );
                        BlockType*  StartBlock2      = ( StartInnerIndex2 == 0 && FirstAllocatedBlock != nullptr ) ? FirstAllocatedBlock : StartBlock;
                        while ( true ) {
                            std::size_t EndInnerIndex2 = ( StartBlock2 == CurrentBlock ) ? StartInnerIndex : BlockSize;
                            while ( StartInnerIndex2 != EndInnerIndex2 ) {
                                ValueAllocatorTraits::Destroy( this->ValueAllocator(), ( *( StartBlock2 ) )[ StartInnerIndex2 ] );
                                ++StartInnerIndex2;
                            }
                            if ( StartBlock2 == CurrentBlock ) {
                                break;
                            }
                            StartBlock2      = StartBlock2->Next;
                            StartInnerIndex2 = 0;
                        }
#if !defined( ENABLE_MEMORY_LEAK_DETECTION )
                    }
#endif

                    HAKLE_RETHROW;
                }
            }
            if ( CurrentBlock == this->TailBlock() ) {
                break;
            }
            StartInnerIndex = 0;
            CurrentBlock    = CurrentBlock->Next;
        }

        if ( FirstAllocatedBlock != nullptr ) {
            this->CurrentIndexEntryArray.load( std::memory_order_relaxed )->Tail.store( ( PO_NextIndexEntry - 1 ) & ( PO_IndexEntriesSize() - 1 ), std::memory_order_release );
        }
        this->TailIndex.store( StartTailIndex + Count, std::memory_order_release );
        return true;
    }

    // TODO: EnqueueBulkMove

    // Dequeue
    template <class U>
    constexpr bool Dequeue( U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), ValueType&&> ) {
        std::size_t FailedCount = this->DequeueFailedCount.load( std::memory_order_relaxed );
        if ( HAKLE_LIKELY( CircularLessThan( this->DequeueAttemptsCount.load( std::memory_order_relaxed ) - FailedCount, this->TailIndex.load( std::memory_order_relaxed ) ) ) ) {
            // TODO: understand this
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t AttemptsCount = this->DequeueAttemptsCount.fetch_add( 1, std::memory_order_relaxed );
            if ( HAKLE_LIKELY( CircularLessThan( AttemptsCount - FailedCount, this->TailIndex.load( std::memory_order_acquire ) ) ) ) {
                // NOTE: getting headIndex must be front of getting CurrentIndexEntryArray
                // if get CurrentIndexEntryArray first, there is a situation that makes FirstBlockIndexBase larger than IndexEntryTailBase
                std::size_t Index      = this->HeadIndex.fetch_add( 1, std::memory_order_relaxed );
                std::size_t InnerIndex = Index & ( BlockSize - 1 );

                // we can dequeue
                IndexEntryArray* LocalIndexEntryArray = this->CurrentIndexEntryArray.load( std::memory_order_acquire );
                std::size_t      LocalIndexEntryIndex = LocalIndexEntryArray->Tail.load( std::memory_order_acquire );

                std::size_t IndexEntryTailBase  = LocalIndexEntryArray->Entries[ LocalIndexEntryIndex ].Base;
                std::size_t FirstBlockIndexBase = Index & ~( BlockSize - 1 );
                std::size_t Offset              = ( FirstBlockIndexBase - IndexEntryTailBase ) >> BlockSizeLog2;
                BlockType*  DequeueBlock        = LocalIndexEntryArray->Entries[ ( LocalIndexEntryIndex + Offset ) & ( LocalIndexEntryArray->Size - 1 ) ].InnerBlock;
                ValueType&  Value               = *( *DequeueBlock )[ InnerIndex ];

                HAKLE_CONSTEXPR_IF( !std::is_nothrow_assignable<U&, ValueType&&>::value ) {
                    struct Guard {
                        BlockType*                                    Block;
                        CompressPair<std::size_t, ValueAllocatorType> ValueAllocatorPair;

                        ~Guard() {
                            ValueAllocatorTraits::Destroy( ValueAllocatorPair.Second(), ( *Block )[ ValueAllocatorPair.First() ] );
                            Block->SetEmpty( ValueAllocatorPair.First() );
                        }
                    } guard{ .Block = DequeueBlock, .ValueAllocatorPair = { InnerIndex, this->ValueAllocator() } };

                    Element = std::move( Value );
                }
                else {
                    Element = std::move( Value );
                    ValueAllocatorTraits::Destroy( this->ValueAllocator(), &Value );
                    DequeueBlock->SetEmpty( InnerIndex );
                }
                return true;
            }

            this->DequeueFailedCount.fetch_add( 1, std::memory_order_release );
        }
        return false;
    }

    template <HAKLE_CONCEPT( std::output_iterator<ValueType&&> ) Iterator>
    std::size_t DequeueBulk( Iterator ItemFirst, std::size_t MaxCount ) {
        std::size_t FailedCount  = this->DequeueFailedCount.load( std::memory_order_relaxed );
        std::size_t DesiredCount = this->TailIndex.load( std::memory_order_relaxed ) - ( this->DequeueAttemptsCount.load( std::memory_order_relaxed ) - FailedCount );
        if ( HAKLE_LIKELY( CircularLessThan<std::size_t>( 0, DesiredCount ) ) ) {
            DesiredCount = std::min( DesiredCount, MaxCount );
            // TODO: understand this
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t AttemptsCount = this->DequeueAttemptsCount.fetch_add( DesiredCount, std::memory_order_relaxed );
            std::size_t ActualCount   = this->TailIndex.load( std::memory_order_acquire ) - ( AttemptsCount - FailedCount );
            if ( HAKLE_LIKELY( CircularLessThan<std::size_t>( 0, ActualCount ) ) ) {
                ActualCount = std::min( ActualCount, DesiredCount );
                if ( ActualCount < DesiredCount ) {
                    this->DequeueFailedCount.fetch_add( DesiredCount - ActualCount, std::memory_order_release );
                }

                std::size_t FirstIndex = this->HeadIndex.fetch_add( ActualCount, std::memory_order_relaxed );
                std::size_t InnerIndex = FirstIndex & ( BlockSize - 1 );

                IndexEntryArray* LocalIndexEntriesArray = this->CurrentIndexEntryArray.load( std::memory_order_acquire );
                std::size_t      LocalIndexEntryIndex   = LocalIndexEntriesArray->Tail.load( std::memory_order_acquire );

                std::size_t IndexEntryTailBase  = LocalIndexEntriesArray->Entries[ LocalIndexEntryIndex ].Base;
                std::size_t FirstBlockIndexBase = FirstIndex & ~( BlockSize - 1 );
                std::size_t Offset              = ( FirstBlockIndexBase - IndexEntryTailBase ) >> BlockSizeLog2;
                BlockType*  FirstDequeueBlock   = LocalIndexEntriesArray->Entries[ ( LocalIndexEntryIndex + Offset ) & ( LocalIndexEntriesArray->Size - 1 ) ].InnerBlock;

                BlockType*  DequeueBlock = FirstDequeueBlock;
                std::size_t StartIndex   = InnerIndex;
                std::size_t NeedCount    = ActualCount;
                while ( NeedCount != 0 ) {
                    std::size_t EndIndex     = ( NeedCount > ( BlockSize - StartIndex ) ) ? BlockSize : ( NeedCount + StartIndex );
                    std::size_t CurrentIndex = StartIndex;
                    HAKLE_CONSTEXPR_IF( std::is_nothrow_assignable<typename std::iterator_traits<Iterator>::value_type&, ValueType&&>::value ) {
                        while ( CurrentIndex != EndIndex ) {
                            ValueType& Value = *( *DequeueBlock )[ CurrentIndex ];
                            *ItemFirst       = std::move( Value );
                            ++ItemFirst;
                            ValueAllocatorTraits::Destroy( this->ValueAllocator(), &Value );
                            ++CurrentIndex;
                            --NeedCount;
                        }
                    }
                    else {
                        HAKLE_TRY {
                            while ( CurrentIndex != EndIndex ) {
                                ValueType& Value = *( *DequeueBlock )[ CurrentIndex ];
                                *ItemFirst++     = std::move( Value );
                                ValueAllocatorTraits::Destroy( this->ValueAllocator(), &Value );
                                ++CurrentIndex;
                                --NeedCount;
                            }
                        }
                        HAKLE_CATCH( ... ) {
                            // we need to destroy all the remaining values
                            goto Enter;
                            while ( NeedCount != 0 ) {
                                EndIndex     = ( NeedCount > ( BlockSize - StartIndex ) ) ? BlockSize : ( NeedCount + StartIndex );
                                CurrentIndex = StartIndex;
                            Enter:
                                while ( CurrentIndex != EndIndex ) {
                                    ValueType& Value = *( *DequeueBlock )[ CurrentIndex ];
                                    ValueAllocatorTraits::Destroy( this->ValueAllocator(), &Value );
                                    --NeedCount;
                                    ++CurrentIndex;
                                }

                                DequeueBlock->SetSomeEmpty( StartIndex, EndIndex - StartIndex );
                                StartIndex   = 0;
                                DequeueBlock = DequeueBlock->Next;
                            }
                            HAKLE_RETHROW;
                        }
                    }
                    BlockType* TempBlock = DequeueBlock;
                    DequeueBlock         = DequeueBlock->Next;
                    TempBlock->SetSomeEmpty( StartIndex, EndIndex - StartIndex );
                    StartIndex = 0;
                }
                return ActualCount;
            }

            this->DequeueFailedCount.fetch_add( DesiredCount, std::memory_order_release );
        }
        return 0;
    }

private:
    struct IndexEntry {
        std::size_t Base{ 0 };
        BlockType*  InnerBlock{ nullptr };
    };

    struct IndexEntryArray {
        std::size_t              Size{};
        std::atomic<std::size_t> Tail{};
        IndexEntry*              Entries{ nullptr };
        IndexEntryArray*         Prev{ nullptr };
    };

    HAKLE_CPP20_CONSTEXPR bool CreateNewBlockIndexArray( std::size_t FilledSlot ) noexcept {
        std::size_t SizeMask = PO_IndexEntriesSize() - 1;

        PO_IndexEntriesSize() <<= 1;
        IndexEntryArray* NewIndexEntryArray = nullptr;
        IndexEntry*      NewEntries         = nullptr;

        HAKLE_TRY {
            NewIndexEntryArray = IndexEntryArrayAllocatorTraits::Allocate( IndexEntryArrayAllocator() );
            NewEntries         = IndexEntryAllocatorTraits::Allocate( IndexEntryAllocator(), PO_IndexEntriesSize() );
        }
        HAKLE_CATCH( ... ) {
            if ( NewIndexEntryArray ) {
                IndexEntryArrayAllocatorTraits::Deallocate( IndexEntryArrayAllocator(), NewIndexEntryArray );
                NewIndexEntryArray = nullptr;
            }
            PO_IndexEntriesSize() >>= 1;
            return false;
        }

        // noexcept
        IndexEntryArrayAllocatorTraits::Construct( IndexEntryArrayAllocator(), NewIndexEntryArray );

        std::size_t j = 0;
        if ( PO_IndexEntriesUsed() != 0 ) {
            std::size_t i = ( PO_NextIndexEntry - PO_IndexEntriesUsed() ) & SizeMask;
            do {
                NewEntries[ j++ ] = PO_PrevEntries[ i ];
                i                 = ( i + 1 ) & SizeMask;
            } while ( i != PO_NextIndexEntry );
        }

        NewIndexEntryArray->Size    = PO_IndexEntriesSize();
        NewIndexEntryArray->Entries = NewEntries;
        NewIndexEntryArray->Tail.store( FilledSlot - 1, std::memory_order_relaxed );
        NewIndexEntryArray->Prev = CurrentIndexEntryArray.load( std::memory_order_relaxed );

        PO_NextIndexEntry = j;
        PO_PrevEntries    = NewEntries;
        CurrentIndexEntryArray.store( NewIndexEntryArray, std::memory_order_release );
        return true;
    }

    // Tail Index Entry Array
    std::atomic<IndexEntryArray*> CurrentIndexEntryArray{ nullptr };

    // Block Manager
    BlockManagerType& BlockManager{};

    // compressed allocator
    CompressPair<std::size_t, IndexEntryAllocatorType>      IndexEntryAllocatorPair{};
    CompressPair<std::size_t, IndexEntryArrayAllocatorType> IndexEntryArrayAllocatorPair{};

    constexpr IndexEntryAllocatorType&      IndexEntryAllocator() noexcept { return IndexEntryAllocatorPair.Second(); }
    constexpr IndexEntryArrayAllocatorType& IndexEntryArrayAllocator() noexcept { return IndexEntryArrayAllocatorPair.Second(); }

    constexpr const IndexEntryAllocatorType&      IndexEntryAllocator() const noexcept { return IndexEntryAllocatorPair.Second(); }
    constexpr const IndexEntryArrayAllocatorType& IndexEntryArrayAllocator() const noexcept { return IndexEntryArrayAllocatorPair.Second(); }

    // producer only fields
    constexpr std::size_t& PO_IndexEntriesUsed() noexcept { return IndexEntryAllocatorPair.First(); }
    constexpr std::size_t& PO_IndexEntriesSize() noexcept { return IndexEntryArrayAllocatorPair.First(); }

    HAKLE_NODISCARD constexpr const std::size_t& PO_IndexEntriesUsed() const noexcept { return IndexEntryAllocatorPair.First(); }
    HAKLE_NODISCARD constexpr const std::size_t& PO_IndexEntriesSize() const noexcept { return IndexEntryArrayAllocatorPair.First(); }

    std::size_t PO_NextIndexEntry{};
    IndexEntry* PO_PrevEntries{ nullptr };
};

template <class T, std::size_t BLOCK_SIZE, class Allocator = HakleAllocator<T>, HAKLE_CONCEPT( IsBlockWithMeaningfulSetResult ) BLOCK_TYPE = HakleCounterBlock<T, BLOCK_SIZE>,
          HAKLE_CONCEPT( IsBlockManager ) BLOCK_MANAGER_TYPE = HakleBlockManager<BLOCK_TYPE>>
class SlowQueue : public _QueueBase<T, BLOCK_SIZE, Allocator, BLOCK_TYPE, BLOCK_MANAGER_TYPE> {
public:
    using Base = _QueueBase<T, BLOCK_SIZE, Allocator, BLOCK_TYPE, BLOCK_MANAGER_TYPE>;

    using Base::BlockSize;
    using typename Base::AllocMode;
    using typename Base::BlockManagerType;
    using typename Base::BlockType;
    using typename Base::ValueAllocatorTraits;
    using typename Base::ValueAllocatorType;
    using typename Base::ValueType;

private:
    constexpr static std::size_t BlockSizeLog2      = BitWidth( BlockSize ) - 1;
    constexpr static std::size_t INVALID_BLOCK_BASE = 1;

    struct IndexEntry;
    struct IndexEntryArray;
    using IndexEntryAllocatorType        = typename ValueAllocatorTraits::template RebindAlloc<IndexEntry>;
    using IndexEntryArrayAllocatorType   = typename ValueAllocatorTraits::template RebindAlloc<IndexEntryArray>;
    using IndexEntryPointerAllocatorType = typename ValueAllocatorTraits::template RebindAlloc<IndexEntry*>;

    using IndexEntryAllocatorTraits        = typename ValueAllocatorTraits::template RebindTraits<IndexEntry>;
    using IndexEntryArrayAllocatorTraits   = typename ValueAllocatorTraits::template RebindTraits<IndexEntryArray>;
    using IndexEntryPointerAllocatorTraits = typename ValueAllocatorTraits::template RebindTraits<IndexEntry*>;

public:
    constexpr SlowQueue( std::size_t InSize, BlockManagerType& InBlockManager, const ValueAllocatorType& InAllocator = ValueAllocatorType{} )
        : Base( InAllocator ), IndexEntryAllocatorPair( ValueInitTag{}, IndexEntryAllocatorType( InAllocator ) ),
          IndexEntryArrayAllocatorPair( InBlockManager, IndexEntryArrayAllocatorType( InAllocator ) ), IndexEntryPointerAllocatorPair( 0, IndexEntryPointerAllocatorType( InAllocator ) ) {
        std::size_t InitialSize = CeilToPow2( InSize ) >> 1;
        if ( InitialSize < 2 ) {
            InitialSize = 2;
        }

        IndexEntriesSize() = InitialSize;
        CreateNewBlockIndexArray();
    }

    HAKLE_CPP20_CONSTEXPR ~SlowQueue() {
        std::size_t Index = this->HeadIndex.load( std::memory_order_relaxed );
        std::size_t Tail  = this->TailIndex.load( std::memory_order_relaxed );

        // Release all block
        BlockType* Block = nullptr;
        while ( Index != Tail ) {
            std::size_t InnerIndex = Index & ( BlockSize - 1 );
            if ( InnerIndex == 0 || Block == nullptr ) {
                Block = GetBlockIndexEntryForIndex( Index )->Value.load( std::memory_order_relaxed );
            }
            ValueAllocatorTraits::Destroy( this->ValueAllocator(), ( *Block )[ InnerIndex ] );
            if ( InnerIndex == BlockSize - 1 || Index == Tail - 1 ) {
                BlockManager().ReturnBlock( Block );
            }
            ++Index;
        }

        // Delete IndexEntryArray
        IndexEntryArray* CurrentArray = CurrentIndexEntryArray().load( std::memory_order_relaxed );
        if ( CurrentArray != nullptr ) {
            for ( std::size_t i = 0; i < CurrentArray->Size; ++i ) {
                IndexEntryAllocatorTraits::Destroy( IndexEntryAllocator(), CurrentArray->Index[ i ] );
            }

            while ( CurrentArray != nullptr ) {
                IndexEntryArray* Prev = CurrentArray->Prev;
                // pass size to detect memory leaks
                IndexEntryPointerAllocatorTraits::Deallocate( IndexEntryPointerAllocator(), CurrentArray->Index, CurrentArray->Size );
                IndexEntryAllocatorTraits::Deallocate( IndexEntryAllocator(), CurrentArray->Entries, Prev == nullptr ? CurrentArray->Size : ( CurrentArray->Size >> 1 ) );
                IndexEntryArrayAllocatorTraits::Destroy( IndexEntryArrayAllocator(), CurrentArray );
                IndexEntryArrayAllocatorTraits::Deallocate( IndexEntryArrayAllocator(), CurrentArray );
                CurrentArray = Prev;
            }
        }
    }

    template <AllocMode Mode, class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<ValueType, Args&&...> )
    HAKLE_CPP20_CONSTEXPR bool Enqueue( Args&&... args ) {
        std::size_t CurrentTailIndex = this->TailIndex.load( std::memory_order_relaxed );
        std::size_t NewTailIndex     = CurrentTailIndex + 1;
        std::size_t InnerIndex       = CurrentTailIndex & ( BlockSize - 1 );
        if HAKLE_UNLIKELY ( InnerIndex == 0 ) {
            // TODO: add MAX_SIZE check
            if HAKLE_UNLIKELY ( !CircularLessThan( this->HeadIndex.load( std::memory_order_relaxed ), CurrentTailIndex + BlockSize ) ) {
                return false;
            }

            IndexEntry* NewIndexEntry = nullptr;
            if HAKLE_UNLIKELY ( !InsertBlockIndexEntry<Mode>( NewIndexEntry, CurrentTailIndex ) ) {
                return false;
            }

            BlockType* NewBlock = BlockManager().RequisitionBlock( Mode );
            if HAKLE_UNLIKELY ( NewBlock == nullptr ) {
                RewindBlockIndexTail();
                NewIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                return false;
            }

            NewBlock->Reset();

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<ValueType, Args&&...>::value ) {
                HAKLE_TRY { ValueAllocatorTraits::Construct( this->ValueAllocator(), ( *NewBlock )[ InnerIndex ], std::forward<Args>( args )... ); }
                HAKLE_CATCH( ... ) {
                    RewindBlockIndexTail();
                    NewIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                    BlockManager().ReturnBlock( NewBlock );
                    HAKLE_RETHROW;
                }
            }

            NewIndexEntry->Value.store( NewBlock, std::memory_order_relaxed );

            this->TailBlock() = NewBlock;

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<ValueType, Args&&...>::value ) {
                this->TailIndex.store( NewTailIndex, std::memory_order_release );
                return true;
            }
        }

        ValueAllocatorTraits::Construct( this->ValueAllocator(), ( *this->TailBlock() )[ InnerIndex ], std::forward<Args>( args )... );
        this->TailIndex.store( NewTailIndex, std::memory_order_release );
        return true;
    }
    ValueAllocatorType TestAllocator{};
    template <AllocMode Mode, HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { ValueType( *Item ); } )
    HAKLE_CPP20_CONSTEXPR bool EnqueueBulk( Iterator ItemFirst, std::size_t Count ) {
        std::size_t OriginTailIndex     = this->TailIndex.load( std::memory_order_relaxed );
        BlockType*  OriginTailBlock     = this->TailBlock();
        BlockType*  FirstAllocatedBlock = nullptr;

        auto&& RollBack = [ this, &FirstAllocatedBlock, OriginTailIndex, OriginTailBlock ]() {
            IndexEntry* IndexEntry       = nullptr;
            std::size_t CurrentTailIndex = ( OriginTailIndex - 1 ) & ~( BlockSize - 1 );
            for ( BlockType* Block = FirstAllocatedBlock; Block; Block = Block->Next ) {
                CurrentTailIndex += BlockSize;
                IndexEntry = GetBlockIndexEntryForIndex( CurrentTailIndex );
                IndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                RewindBlockIndexTail();
            }

            BlockManager().ReturnBlocks( FirstAllocatedBlock );
            this->TailBlock() = OriginTailBlock;
        };

        std::size_t NeedCount        = ( ( OriginTailIndex + Count - 1 ) >> BlockSizeLog2 ) - ( static_cast<std::make_signed_t<std::size_t>>( OriginTailIndex - 1 ) >> BlockSizeLog2 );
        std::size_t CurrentTailIndex = ( OriginTailIndex - 1 ) & ~( BlockSize - 1 );
        // allocate index entry and block
        if ( NeedCount > 0 ) {
            while ( NeedCount > 0 ) {
                CurrentTailIndex += BlockSize;
                --NeedCount;

                bool        IndexInserted = false;
                BlockType*  NewBlock      = nullptr;
                IndexEntry* IndexEntry    = nullptr;

                // TODO: add MAX_SIZE check
                bool full = !CircularLessThan( this->HeadIndex.load( std::memory_order_relaxed ), CurrentTailIndex + BlockSize );
                if ( full || !( IndexInserted = InsertBlockIndexEntry<Mode>( IndexEntry, CurrentTailIndex ) ) || !( NewBlock = BlockManager().RequisitionBlock( Mode ) ) ) {
                    if ( IndexInserted ) {
                        RewindBlockIndexTail();
                        IndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                    }
                    RollBack();
                    return false;
                }

                NewBlock->Reset();
                NewBlock->Next = nullptr;

                IndexEntry->Value.store( NewBlock, std::memory_order_relaxed );

                if ( ( OriginTailIndex & ( BlockSize - 1 ) ) != 0 || FirstAllocatedBlock != nullptr ) {
                    this->TailBlock()->Next = NewBlock;
                }
                this->TailBlock() = NewBlock;
                if ( FirstAllocatedBlock == nullptr ) {
                    FirstAllocatedBlock = NewBlock;
                }
            }
        }
        // std::allocator<T> alloc;
        // we already have enough blocks, let's fill them
        std::size_t StartInnerIndex = OriginTailIndex & ( BlockSize - 1 );
        BlockType*  StartBlock      = ( StartInnerIndex == 0 && FirstAllocatedBlock != nullptr ) ? FirstAllocatedBlock : OriginTailBlock;
        BlockType*  CurrentBlock    = StartBlock;
        while ( true ) {
            std::size_t EndInnerIndex = ( CurrentBlock == this->TailBlock() ) ? ( OriginTailIndex + Count - 1 ) & ( BlockSize - 1 ) : ( BlockSize - 1 );
            HAKLE_CONSTEXPR_IF( std::is_nothrow_constructible<ValueType, typename std::iterator_traits<Iterator>::value_type&>::value ) {
                while ( StartInnerIndex <= EndInnerIndex ) {
                    ValueAllocatorTraits::Construct( this->ValueAllocatorPair.Second(), ( *CurrentBlock )[ StartInnerIndex++ ], *ItemFirst++ );
                }
            }
            else {
                HAKLE_TRY {
                    while ( StartInnerIndex <= EndInnerIndex ) {
                        ValueAllocatorTraits::Construct( this->ValueAllocator(), ( *( CurrentBlock ) )[ StartInnerIndex ], *ItemFirst++ );
                        ++StartInnerIndex;
                    }
                }
                HAKLE_CATCH( ... ) {
                    // first, we need to destroy all values
#if !defined( ENABLE_MEMORY_LEAK_DETECTION )
                    HAKLE_CONSTEXPR_IF( !std::is_trivially_destructible<ValueType>::value ) {
#endif
                        std::size_t StartInnerIndex2 = OriginTailIndex & ( BlockSize - 1 );
                        BlockType*  StartBlock2      = ( StartInnerIndex2 == 0 && FirstAllocatedBlock != nullptr ) ? FirstAllocatedBlock : StartBlock;
                        while ( true ) {
                            std::size_t EndInnerIndex2 = ( StartBlock2 == CurrentBlock ) ? StartInnerIndex : BlockSize;
                            while ( StartInnerIndex2 != EndInnerIndex2 ) {
                                ValueAllocatorTraits::Destroy( this->ValueAllocator(), ( *( StartBlock2 ) )[ StartInnerIndex2 ] );
                                ++StartInnerIndex2;
                            }
                            if ( StartBlock2 == CurrentBlock ) {
                                break;
                            }
                            StartBlock2      = StartBlock2->Next;
                            StartInnerIndex2 = 0;
                        }
#if !defined( ENABLE_MEMORY_LEAK_DETECTION )
                    }
#endif

                    RollBack();

                    HAKLE_RETHROW;
                }
            }
            if ( CurrentBlock == this->TailBlock() ) {
                break;
            }
            StartInnerIndex = 0;
            CurrentBlock    = CurrentBlock->Next;
        }

        this->TailIndex.store( OriginTailIndex + Count, std::memory_order_release );
        return true;
    }

    template <class U>
    constexpr bool Dequeue( U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), ValueType&&> ) {
        std::size_t FailedCount = this->DequeueFailedCount.load( std::memory_order_relaxed );
        if ( HAKLE_LIKELY( CircularLessThan( this->DequeueAttemptsCount.load( std::memory_order_relaxed ) - FailedCount, this->TailIndex.load( std::memory_order_relaxed ) ) ) ) {
            // TODO: understand this
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t AttemptsCount = this->DequeueAttemptsCount.fetch_add( 1, std::memory_order_relaxed );
            if ( HAKLE_LIKELY( CircularLessThan( AttemptsCount - FailedCount, this->TailIndex.load( std::memory_order_acquire ) ) ) ) {
                std::size_t Index      = this->HeadIndex.fetch_add( 1, std::memory_order_relaxed );
                std::size_t InnerIndex = Index & ( BlockSize - 1 );

                IndexEntry* Entry = GetBlockIndexEntryForIndex( Index );
                BlockType*  Block = Entry->Value.load( std::memory_order_relaxed );
                ValueType&  Value = *( *Block )[ InnerIndex ];

                HAKLE_CONSTEXPR_IF( !std::is_nothrow_assignable<U&, ValueType&&>::value ) {
                    struct Guard {
                        IndexEntry*                                   Entry;
                        BlockType*                                    Block;
                        BlockManagerType&                             BlockManager;
                        CompressPair<std::size_t, ValueAllocatorType> ValueAllocatorPair;

                        ~Guard() {
                            ValueAllocatorTraits::Destroy( ValueAllocatorPair.Second(), ( *Block )[ ValueAllocatorPair.First() ] );
                            if ( Block->SetEmpty( ValueAllocatorPair.First() ) ) {
                                Entry->Value.store( nullptr, std::memory_order_relaxed );
                                BlockManager.ReturnBlock( Block );
                            }
                        }
                    } guard{ .Entry = Entry, .Block = Block, .BlockManager = BlockManager(), .ValueAllocatorPair = { InnerIndex, this->ValueAllocator() } };

                    Element = std::move( Value );
                }
                else {
                    Element = std::move( Value );
                    ValueAllocatorTraits::Destroy( this->ValueAllocator(), &Value );
                    if ( Block->SetEmpty( InnerIndex ) ) {
                        Entry->Value.store( nullptr, std::memory_order_relaxed );
                        BlockManager().ReturnBlock( Block );
                    }
                }
                return true;
            }

            this->DequeueFailedCount.fetch_add( 1, std::memory_order_release );
        }
        return false;
    }

    template <HAKLE_CONCEPT( std::output_iterator<ValueType&&> ) Iterator>
    std::size_t DequeueBulk( Iterator ItemFirst, std::size_t MaxCount ) {
        std::size_t FailedCount  = this->DequeueFailedCount.load( std::memory_order_relaxed );
        std::size_t DesiredCount = this->TailIndex.load( std::memory_order_relaxed ) - ( this->DequeueAttemptsCount.load( std::memory_order_relaxed ) - FailedCount );
        if ( HAKLE_LIKELY( CircularLessThan<std::size_t>( 0, DesiredCount ) ) ) {
            DesiredCount = std::min( DesiredCount, MaxCount );
            // TODO: understand this
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t AttemptsCount = this->DequeueAttemptsCount.fetch_add( DesiredCount, std::memory_order_relaxed );
            std::size_t ActualCount   = this->TailIndex.load( std::memory_order_acquire ) - ( AttemptsCount - FailedCount );
            if ( HAKLE_LIKELY( CircularLessThan<std::size_t>( 0, ActualCount ) ) ) {
                ActualCount = std::min( ActualCount, DesiredCount );
                if ( ActualCount < DesiredCount ) {
                    this->DequeueFailedCount.fetch_add( DesiredCount - ActualCount, std::memory_order_release );
                }

                std::size_t Index      = this->HeadIndex.fetch_add( ActualCount, std::memory_order_relaxed );
                std::size_t InnerIndex = Index & ( BlockSize - 1 );

                std::size_t StartIndex = InnerIndex;
                std::size_t NeedCount  = ActualCount;

                IndexEntryArray* LocalIndexEntryArray;
                std::size_t      IndexEntryIndex = GetBlockIndexIndexForIndex( Index, LocalIndexEntryArray );
                while ( NeedCount != 0 ) {
                    IndexEntry* DequeueIndexEntry = LocalIndexEntryArray->Index[ IndexEntryIndex ];
                    BlockType*  DequeueBlock      = DequeueIndexEntry->Value.load( std::memory_order_relaxed );
                    std::size_t EndIndex          = ( NeedCount > ( BlockSize - StartIndex ) ) ? BlockSize : ( NeedCount + StartIndex );
                    std::size_t CurrentIndex      = StartIndex;
                    HAKLE_CONSTEXPR_IF( std::is_nothrow_assignable<typename std::iterator_traits<Iterator>::value_type&, ValueType&&>::value ) {
                        while ( CurrentIndex != EndIndex ) {
                            ValueType& Value = *( *DequeueBlock )[ CurrentIndex ];
                            *ItemFirst       = std::move( Value );
                            ++ItemFirst;
                            ValueAllocatorTraits::Destroy( this->ValueAllocator(), &Value );
                            ++CurrentIndex;
                            --NeedCount;
                        }
                    }
                    else {
                        HAKLE_TRY {
                            while ( CurrentIndex != EndIndex ) {
                                ValueType& Value = *( *DequeueBlock )[ CurrentIndex ];
                                *ItemFirst++     = std::move( Value );
                                ValueAllocatorTraits::Destroy( this->ValueAllocator(), &Value );
                                ++CurrentIndex;
                                --NeedCount;
                            }
                        }
                        HAKLE_CATCH( ... ) {
                            // we need to destroy all the remaining values
                            goto Enter;
                            while ( NeedCount != 0 ) {
                                DequeueIndexEntry = LocalIndexEntryArray->Index[ IndexEntryIndex ];
                                DequeueBlock      = DequeueIndexEntry->Value.load( std::memory_order_relaxed );
                                EndIndex          = ( NeedCount > ( BlockSize - StartIndex ) ) ? BlockSize : ( NeedCount + StartIndex );
                                CurrentIndex      = StartIndex;
                            Enter:
                                while ( CurrentIndex != EndIndex ) {
                                    ValueType& Value = *( *DequeueBlock )[ CurrentIndex ];
                                    ValueAllocatorTraits::Destroy( this->ValueAllocator(), &Value );
                                    --NeedCount;
                                    ++CurrentIndex;
                                }

                                if ( DequeueBlock->SetSomeEmpty( StartIndex, EndIndex - StartIndex ) ) {
                                    DequeueIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                                    BlockManager().ReturnBlock( DequeueBlock );
                                }
                                StartIndex      = 0;
                                IndexEntryIndex = ( IndexEntryIndex + 1 ) & ( LocalIndexEntryArray->Size - 1 );
                            }
                            HAKLE_RETHROW;
                        }
                    }
                    if ( DequeueBlock->SetSomeEmpty( StartIndex, EndIndex - StartIndex ) ) {
                        DequeueIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                        BlockManager().ReturnBlock( DequeueBlock );
                    }
                    StartIndex      = 0;
                    IndexEntryIndex = ( IndexEntryIndex + 1 ) & ( LocalIndexEntryArray->Size - 1 );
                }
                return ActualCount;
            }

            this->DequeueFailedCount.fetch_add( DesiredCount, std::memory_order_release );
        }
        return 0;
    }

private:
    struct IndexEntry {
        std::atomic<std::size_t> Key;
        std::atomic<BlockType*>  Value;
    };

    struct IndexEntryArray {
        std::size_t              Size{};
        std::atomic<std::size_t> Tail{};
        IndexEntry*              Entries{ nullptr };
        IndexEntry**             Index{ nullptr };
        IndexEntryArray*         Prev{ nullptr };
    };

    template <AllocMode Mode>
    HAKLE_CPP20_CONSTEXPR bool InsertBlockIndexEntry( IndexEntry*& IdxEntry, std::size_t BlockStartIndex ) noexcept {
        IndexEntryArray* LocalIndexEntriesArray = CurrentIndexEntryArray().load( std::memory_order_relaxed );
        if HAKLE_UNLIKELY ( LocalIndexEntriesArray == nullptr ) {
            return false;
        }

        std::size_t NewTail = ( LocalIndexEntriesArray->Tail.load( std::memory_order_relaxed ) + 1 ) & ( LocalIndexEntriesArray->Size - 1 );
        IdxEntry            = LocalIndexEntriesArray->Index[ NewTail ];
        if ( IdxEntry->Key.load( std::memory_order_relaxed ) == INVALID_BLOCK_BASE || IdxEntry->Value.load( std::memory_order_relaxed ) == nullptr ) {
            IdxEntry->Key.store( BlockStartIndex, std::memory_order_relaxed );
            LocalIndexEntriesArray->Tail.store( NewTail, std::memory_order_release );
            return true;
        }

        HAKLE_CONSTEXPR_IF( Mode == AllocMode::CannotAlloc ) { return false; }
        else if ( !CreateNewBlockIndexArray() ) {
            return false;
        }
        else {
            LocalIndexEntriesArray = CurrentIndexEntryArray().load( std::memory_order_relaxed );
            NewTail                = ( LocalIndexEntriesArray->Tail.load( std::memory_order_relaxed ) + 1 ) & ( LocalIndexEntriesArray->Size - 1 );
            IdxEntry               = LocalIndexEntriesArray->Index[ NewTail ];
            IdxEntry->Key.store( BlockStartIndex, std::memory_order_relaxed );
            LocalIndexEntriesArray->Tail.store( NewTail, std::memory_order_release );
            return true;
        }
    }

    HAKLE_CPP20_CONSTEXPR void RewindBlockIndexTail() noexcept {
        IndexEntryArray* LocalBlockEntryArray = CurrentIndexEntryArray().load( std::memory_order_relaxed );
        LocalBlockEntryArray->Tail.store( ( LocalBlockEntryArray->Tail.load( std::memory_order_relaxed ) - 1 ) & ( LocalBlockEntryArray->Size - 1 ), std::memory_order_relaxed );
    }

    HAKLE_CPP20_CONSTEXPR IndexEntry* GetBlockIndexEntryForIndex( std::size_t Index ) const noexcept {
        IndexEntryArray* LocalBlockIndexArray;
        std::size_t      BlockIndex = GetBlockIndexIndexForIndex( Index, LocalBlockIndexArray );
        return LocalBlockIndexArray->Index[ BlockIndex ];
    }

    HAKLE_CPP20_CONSTEXPR std::size_t GetBlockIndexIndexForIndex( std::size_t Index, IndexEntryArray*& LocalBlockIndexArray ) const noexcept {
        LocalBlockIndexArray   = CurrentIndexEntryArray().load( std::memory_order_acquire );
        std::size_t Tail       = LocalBlockIndexArray->Tail.load( std::memory_order_acquire );
        std::size_t TailBase   = LocalBlockIndexArray->Index[ Tail ]->Key.load( std::memory_order_relaxed );
        std::size_t Offset     = ( ( Index & ~( BlockSize - 1 ) ) - TailBase ) >> BlockSizeLog2;
        std::size_t BlockIndex = ( Tail + Offset ) & ( LocalBlockIndexArray->Size - 1 );
        return BlockIndex;
    }

    HAKLE_CPP20_CONSTEXPR bool CreateNewBlockIndexArray() noexcept {
        IndexEntryArray* Prev       = CurrentIndexEntryArray().load( std::memory_order_relaxed );
        std::size_t      PrevSize   = Prev == nullptr ? 0 : Prev->Size;
        std::size_t      EntryCount = Prev == nullptr ? IndexEntriesSize() : PrevSize;

        IndexEntryArray* NewIndexEntryArray = nullptr;
        IndexEntry*      NewEntries         = nullptr;
        IndexEntry**     NewIndex           = nullptr;

        HAKLE_TRY {
            NewIndexEntryArray = IndexEntryArrayAllocatorTraits::Allocate( IndexEntryArrayAllocator() );
            NewEntries         = IndexEntryAllocatorTraits::Allocate( IndexEntryAllocator(), EntryCount );
            NewIndex           = IndexEntryPointerAllocatorTraits::Allocate( IndexEntryPointerAllocator(), IndexEntriesSize() );
        }
        HAKLE_CATCH( ... ) {
            if ( NewIndexEntryArray ) {
                IndexEntryArrayAllocatorTraits::Deallocate( IndexEntryArrayAllocator(), NewIndexEntryArray );
                NewIndexEntryArray = nullptr;
            }

            if ( NewEntries ) {
                IndexEntryAllocatorTraits::Deallocate( IndexEntryAllocator(), NewEntries, EntryCount );
                NewEntries = nullptr;
            }
            return false;
        }

        // noexcept
        IndexEntryArrayAllocatorTraits::Construct( IndexEntryArrayAllocator(), NewIndexEntryArray );

        if HAKLE_LIKELY ( Prev != nullptr ) {
            std::size_t Tail = Prev->Tail.load( std::memory_order_relaxed );
            std::size_t i = Tail, j = 0;
            do {
                i               = ( i + 1 ) & ( PrevSize - 1 );
                NewIndex[ j++ ] = Prev->Index[ i ];
            } while ( i != Tail );
        }
        for ( std::size_t i = 0; i < EntryCount; ++i ) {
            IndexEntryAllocatorTraits::Construct( IndexEntryAllocator(), NewEntries + i );
            NewEntries[ i ].Key.store( INVALID_BLOCK_BASE, std::memory_order_relaxed );
            NewIndex[ PrevSize + i ] = NewEntries + i;
        }

        NewIndexEntryArray->Entries = NewEntries;
        NewIndexEntryArray->Index   = NewIndex;
        NewIndexEntryArray->Prev    = Prev;
        NewIndexEntryArray->Tail.store( ( PrevSize - 1 ) & ( IndexEntriesSize() - 1 ), std::memory_order_relaxed );
        NewIndexEntryArray->Size = IndexEntriesSize();

        CurrentIndexEntryArray().store( NewIndexEntryArray, std::memory_order_release );

        IndexEntriesSize() <<= 1;
        return true;
    }

    // compressed allocator
    // Tail Index Entry Array
    CompressPair<std::atomic<IndexEntryArray*>, IndexEntryAllocatorType> IndexEntryAllocatorPair{};
    // Block Manager
    CompressPair<BlockManagerType&, IndexEntryArrayAllocatorType> IndexEntryArrayAllocatorPair;
    // Next block index array capacity
    CompressPair<std::size_t, IndexEntryPointerAllocatorType> IndexEntryPointerAllocatorPair{};

    constexpr IndexEntryAllocatorType&        IndexEntryAllocator() noexcept { return IndexEntryAllocatorPair.Second(); }
    constexpr IndexEntryArrayAllocatorType&   IndexEntryArrayAllocator() noexcept { return IndexEntryArrayAllocatorPair.Second(); }
    constexpr IndexEntryPointerAllocatorType& IndexEntryPointerAllocator() noexcept { return IndexEntryPointerAllocatorPair.Second(); }

    constexpr const IndexEntryAllocatorType&        IndexEntryAllocator() const noexcept { return IndexEntryAllocatorPair.Second(); }
    constexpr const IndexEntryArrayAllocatorType&   IndexEntryArrayAllocator() const noexcept { return IndexEntryArrayAllocatorPair.Second(); }
    constexpr const IndexEntryPointerAllocatorType& IndexEntryPointerAllocator() const noexcept { return IndexEntryPointerAllocatorPair.Second(); }

    constexpr std::atomic<IndexEntryArray*>& CurrentIndexEntryArray() noexcept { return IndexEntryAllocatorPair.First(); }
    constexpr BlockManagerType&              BlockManager() noexcept { return IndexEntryArrayAllocatorPair.First(); }
    constexpr std::size_t&                   IndexEntriesSize() noexcept { return IndexEntryPointerAllocatorPair.First(); }

    constexpr const std::atomic<IndexEntryArray*>& CurrentIndexEntryArray() const noexcept { return IndexEntryAllocatorPair.First(); }
    constexpr const BlockManagerType&              BlockManager() const noexcept { return IndexEntryArrayAllocatorPair.First(); }
    HAKLE_NODISCARD constexpr const std::size_t&   IndexEntriesSize() const noexcept { return IndexEntryPointerAllocatorPair.First(); }
};

template <class T, HAKLE_CONCEPT( IsAllocator ) Allocator>
struct ConcurrentQueueDefaultTraits {
    static constexpr std::size_t BlockSize                = 32;
    static constexpr std::size_t InitialBlockPoolSize     = 32 * BlockSize;
    static constexpr std::size_t InitialHashSize          = 32;
    static constexpr std::size_t InitialExplicitQueueSize = 32;
    static constexpr std::size_t InitialImplicitQueueSize = 32;

    using AllocatorType = Allocator;

    using ExplicitBlockType = HakleFlagsBlock<T, BlockSize>;
    using ImplicitBlockType = HakleCounterBlock<T, BlockSize>;

    using ExplicitAllocatorType = typename HakeAllocatorTraits<AllocatorType>::template RebindAlloc<ExplicitBlockType>;
    using ImplicitAllocatorType = typename HakeAllocatorTraits<AllocatorType>::template RebindAlloc<ImplicitBlockType>;

    using ExplicitBlockManagerType = HakleFlagsBlockManager<T, BlockSize, ExplicitAllocatorType>;
    using ImplicitBlockManagerType = HakleCounterBlockManager<T, BlockSize, ImplicitAllocatorType>;

    static ExplicitBlockManagerType MakeDefaultExplicitBlockManager( const ExplicitAllocatorType& InAllocator ) { return ExplicitBlockManagerType( InitialBlockPoolSize, InAllocator ); }
    static ImplicitBlockManagerType MakeDefaultImplicitBlockManager( const ImplicitAllocatorType& InAllocator ) { return ImplicitBlockManagerType( InitialBlockPoolSize, InAllocator ); }

    static ImplicitBlockManagerType MakeExplicitBlockManager( const ExplicitAllocatorType& InAllocator, std::size_t BlockPoolSize ) { return ImplicitBlockManagerType( BlockPoolSize, InAllocator ); }
    static ImplicitBlockManagerType MakeImplicitBlockManager( const ExplicitAllocatorType& InAllocator, std::size_t BlockPoolSize ) { return ImplicitBlockManagerType( BlockPoolSize, InAllocator ); }
};

template <class T, class Allocator = HakleAllocator<T>, HAKLE_CONCEPT( IsConcurrentQueueTraits ) Traits = ConcurrentQueueDefaultTraits<T, Allocator>>
class ConcurrentQueue : private Traits {
private:
    struct ProducerListNode;
    static constexpr std::size_t EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE = 256;

public:
    struct ProducerToken;
    struct ConsumerToken;

    using Traits::BlockSize;
    using Traits::InitialBlockPoolSize;
    using Traits::InitialExplicitQueueSize;
    using Traits::InitialHashSize;
    using Traits::InitialImplicitQueueSize;

    using typename Traits::ExplicitBlockType;
    using typename Traits::ImplicitBlockType;

    using typename Traits::AllocatorType;
    using typename Traits::ExplicitAllocatorType;
    using typename Traits::ImplicitAllocatorType;

    using typename Traits::ExplicitBlockManagerType;
    using typename Traits::ImplicitBlockManagerType;

    using Traits::MakeDefaultExplicitBlockManager;
    using Traits::MakeDefaultImplicitBlockManager;

    using BaseProducer = _QueueTypelessBase;

    using ExplicitProducer = FastQueue<T, BlockSize, Allocator, ExplicitBlockType, ExplicitBlockManagerType>;
    using ImplicitProducer = SlowQueue<T, BlockSize, Allocator, ImplicitBlockType, ImplicitBlockManagerType>;

    using ExplicitProducerAllocatorTraits = typename HakeAllocatorTraits<AllocatorType>::template RebindTraits<ExplicitProducer>;
    using ImplicitProducerAllocatorTraits = typename HakeAllocatorTraits<AllocatorType>::template RebindTraits<ImplicitProducer>;
    using ProducerListNodeAllocatorTraits = typename HakeAllocatorTraits<AllocatorType>::template RebindTraits<ProducerListNode>;

    using ExplicitProducerAllocatorType = typename HakeAllocatorTraits<AllocatorType>::template RebindAlloc<ExplicitProducer>;
    using ImplicitProducerAllocatorType = typename HakeAllocatorTraits<AllocatorType>::template RebindAlloc<ImplicitProducer>;
    using ProducerListNodeAllocatorType = typename HakeAllocatorTraits<AllocatorType>::template RebindAlloc<ProducerListNode>;

    explicit constexpr ConcurrentQueue( const AllocatorType& InAllocator = AllocatorType{} )
        : ExplicitProducerAllocatorPair( MakeDefaultExplicitBlockManager( ExplicitAllocatorType( InAllocator ) ), ExplicitProducerAllocatorType( InAllocator ) ),
          ImplicitProducerAllocatorPair( MakeDefaultImplicitBlockManager( ImplicitAllocatorType( InAllocator ) ), ImplicitProducerAllocatorType( InAllocator ) ) {}

    template <class... Args1, class... Args2>
    HAKLE_REQUIRES( HasMakeImplicitBlockManager<Traits>&& HasMakeExplicitBlockManager<Traits>&& std::invocable<decltype( Traits::MakeExplicitBlockManager ), Args1&&...>&&
                                                                                                std::invocable<decltype( Traits::MakeImplicitBlockManager ), Args2&&...> )
    explicit constexpr ConcurrentQueue( std::piecewise_construct_t, std::tuple<Args1...> FirstArgs, std::tuple<Args2...> SecondArgs, const AllocatorType& InAllocator )
        :
#if HAKLE_CPP_VERSION >= 17
          ExplicitProducerAllocatorPair(
              std::apply( [ &InAllocator ]( Args1&&... args1 ) { return Traits::MakeExplicitBlockManager( ExplicitAllocatorType( InAllocator ), std::forward<Args1>( args1 )... ); }, FirstArgs ),
              ExplicitProducerAllocatorType( InAllocator ) ),
          ImplicitProducerAllocatorPair(
              std::apply( [ &InAllocator ]( Args2&&... args2 ) { return Traits::MakeImplicitBlockManager( ImplicitAllocatorType( InAllocator ), std::forward<Args2>( args2 )... ); }, SecondArgs ),
              ImplicitProducerAllocatorType( InAllocator ) )
#else
          ExplicitProducerAllocatorPair(
              hakle::Apply( [ &InAllocator ]( Args1&&... args1 ) { return Traits::MakeExplicitBlockManager( ExplicitAllocatorType( InAllocator ), std::forward<Args1>( args1 )... ); }, FirstArgs ),
              ExplicitProducerAllocatorType( InAllocator ) ),
          ImplicitProducerAllocatorPair(
              hakle::Apply( [ &InAllocator ]( Args2&&... args2 ) { return Traits::MakeImplicitBlockManager( ImplicitAllocatorType( InAllocator ), std::forward<Args2>( args2 )... ); }, SecondArgs ),
              ImplicitProducerAllocatorType( InAllocator ) )
#endif
    {
    }

    HAKLE_CPP20_CONSTEXPR ~ConcurrentQueue() { ClearList(); }

    explicit constexpr ConcurrentQueue( ConcurrentQueue&& Other ) noexcept
        : ProducerListsHead( std::move( Other.ProducerListsHead ) ), ProducerCount( std::move( Other.ProducerCount.load( std::memory_order_relaxed ) ) ),
          ExplicitProducerAllocatorPair( std::move( Other.ExplicitManager() ), std::move( Other.ExplicitProducerAllocator() ) ),
          ImplicitProducerAllocatorPair( std::move( Other.ImplicitManager() ), std::move( Other.ImplicitProducerAllocator() ) ),
          ValueAllocatorPair( std::move( Other.NextExplicitConsumerId() ), std::move( Other.ValueAllocator() ) ),
          ProducerListNodeAllocatorPair( std::move( Other.GlobalExplicitConsumerOffset() ), std::move( Other.ProducerListNodeAllocator() ) ), ImplicitMap( std::move( Other.ImplicitMap ) ) {
        Other.ProducerListsHead.store( nullptr, std::memory_order_relaxed );
        Other.ProducerCount.store( 0, std::memory_order_relaxed );
        Other.GlobalExplicitConsumerOffset() = Other.NextExplicitConsumerId() = 0;
        ReclaimProducerLists();
    }

    constexpr ConcurrentQueue& operator=( ConcurrentQueue&& Other ) noexcept {
        ClearList();
        ProducerListsHead.store( Other.ProducerListsHead.load( std::memory_order_relaxed ), std::memory_order_relaxed );
        ProducerCount.store( Other.ProducerCount.load( std::memory_order_relaxed ), std::memory_order_relaxed );
        ExplicitManager()           = std::move( Other.ExplicitManager() );
        ImplicitManager()           = std::move( Other.ImplicitManager() );
        ExplicitProducerAllocator() = std::move( Other.ExplicitProducerAllocator() );
        ImplicitProducerAllocator() = std::move( Other.ImplicitProducerAllocator() );
        ValueAllocator()            = std::move( Other.ValueAllocator() );
        ProducerListNodeAllocator() = std::move( Other.ProducerListNodeAllocator() );
        ImplicitMap                 = std::move( Other.ImplicitMap );

        Other.ProducerListsHead.store( nullptr, std::memory_order_relaxed );
        Other.ProducerCount.store( 0, std::memory_order_relaxed );
        Other.GlobalExplicitConsumerOffset() = Other.NextExplicitConsumerId() = 0;

        ReclaimProducerLists();
        return *this;
    }

    constexpr ConcurrentQueue( const ConcurrentQueue& Other )            = delete;
    constexpr ConcurrentQueue& operator=( const ConcurrentQueue& Other ) = delete;

    constexpr void swap( ConcurrentQueue& Other ) noexcept {
        core::SwapRelaxed( ProducerListsHead, Other.ProducerListsHead );
        core::SwapRelaxed( ProducerCount, Other.ProducerCount );
        core::SwapRelaxed( NextExplicitConsumerId(), Other.NextExplicitConsumerId() );
        core::SwapRelaxed( GlobalExplicitConsumerOffset(), Other.GlobalExplicitConsumerOffset() );

        using std::swap;
        swap( ExplicitManager(), Other.ExplicitManager() );
        swap( ImplicitManager(), Other.ImplicitManager() );
        swap( ExplicitProducerAllocator(), Other.ExplicitProducerAllocator() );
        swap( ImplicitProducerAllocator(), Other.ImplicitProducerAllocator() );
        swap( ValueAllocator(), Other.ValueAllocator() );
        swap( ProducerListNodeAllocator(), Other.ProducerListNodeAllocator() );
        swap( ImplicitMap, Other.ImplicitMap );
    }

    constexpr void ClearList() noexcept {
        ForEachProducerSafe( [ this ]( ProducerListNode* Node ) { DeleteProducerListNode( Node ); } );
    }

    constexpr ProducerToken GetProducerToken() noexcept { return ProducerToken( *this ); }
    constexpr ConsumerToken GetConsumerToken() noexcept { return ConsumerToken( *this ); }

    template <class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<T, Args...> )
    constexpr bool EnqueueWithToken( const ProducerToken& Token, Args&&... args ) {
        return InnerEnqueueWithToken<AllocMode::CanAlloc>( Token, std::forward<Args>( args )... );
    }

    template <class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<T, Args...> )
    constexpr bool Enqueue( Args&&... args ) {
        HAKLE_CONSTEXPR_IF( InitialHashSize == 0 ) return false;
        return InnerEnqueue<AllocMode::CanAlloc>( std::forward<Args>( args )... );
    }

    template <HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { T( *Item ); } )
    constexpr bool EnqueueBulk( const ProducerToken& Token, Iterator ItermFirst, std::size_t Count ) {
        return InnerEnqueueBulk<AllocMode::CanAlloc>( Token, ItermFirst, Count );
    }

    template <HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { T( *Item ); } )
    constexpr bool EnqueueBulk( Iterator ItermFirst, std::size_t Count ) {
        return InnerEnqueueBulk<AllocMode::CanAlloc>( ItermFirst, Count );
    }

    template <class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<T, Args...> )
    constexpr bool TryEnqueue( Args&&... args ) {
        HAKLE_CONSTEXPR_IF( InitialHashSize == 0 )
        return false;
        return InnerEnqueue<AllocMode::CannotAlloc>( std::forward<Args>( args )... );
    }

    template <class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<T, Args...> )
    constexpr bool TryEnqueue( const ProducerToken& Token, Args&&... args ) {
        return InnerEnqueue<AllocMode::CannotAlloc>( Token, std::forward<Args>( args )... );
    }

    template <HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { T( *Item ); } )
    constexpr bool TryEnqueueBulk( const ProducerToken& Token, Iterator ItermFirst, std::size_t Count ) {
        return InnerEnqueueBulk<AllocMode::CannotAlloc>( Token, ItermFirst, Count );
    }

    template <HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { T( *Item ); } )
    constexpr bool TryEnqueueBulk( Iterator ItermFirst, std::size_t Count ) {
        return InnerEnqueueBulk<AllocMode::CannotAlloc>( ItermFirst, Count );
    }

    template <class U>
    constexpr bool TryDequeue( U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), T&&> ) {
        std::size_t       NonEmptyCount = 0;
        ProducerListNode* Best          = nullptr;
        std::size_t       BestSize      = 0;
        ForEachProducerWithBreak( [ &NonEmptyCount, &Best, &BestSize ]( ProducerListNode* Node ) -> bool {
            std::size_t Size = Node->GetProducerSize();
            if ( Size > 0 ) {
                ++NonEmptyCount;
                if ( Size > BestSize ) {
                    BestSize = Size;
                    Best     = Node;
                }
            }
            return NonEmptyCount < 3;
        } );

        if ( NonEmptyCount > 0 ) {
            if ( Best->ProducerDequeue( Element ) ) {
                return true;
            }

            return ForEachProducerWithReturn( [ &Element, Best ]( ProducerListNode* Node ) -> bool { return Node != Best && Node->ProducerDequeue( Element ); } );
        }
        return false;
    }

    template <class U>
    constexpr bool TryDequeueNonInterleaved( U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), T&&> ) {
        return ForEachProducerWithReturn( [ &Element ]( ProducerListNode* Node ) -> bool { return Node->ProducerDequeue( Element ); } );
    }

    template <class U>
    constexpr bool TryDequeue( ConsumerToken& Token, U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), T&&> ) {
        if ( Token.DesiredProducer == nullptr || Token.LastKnownGlobalOffset != GlobalExplicitConsumerOffset().load( std::memory_order_relaxed ) ) {
            if ( !UpdateProducerForConsumer( Token ) ) {
                return false;
            }
        }

        if ( Token.CurrentProducer->ProducerDequeue( Element ) ) {
            if ( ++Token.ItemsConsumed == EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE ) {
                GlobalExplicitConsumerOffset().fetch_add( 1, std::memory_order_relaxed );
            }
            return true;
        }

        ProducerListNode* Head = ProducerListsHead.load( std::memory_order_acquire );
        ProducerListNode* Node = Token.CurrentProducer->Next;
        if ( Node == nullptr ) {
            Node = Head;
        }
        while ( Node != Token.CurrentProducer ) {
            if ( Node->ProducerDequeue( Element ) ) {
                Token.CurrentProducer = Node;
                Token.ItemsConsumed   = 1;
                return true;
            }
            Node = Node->Next;
            if ( Node == nullptr ) {
                Node = Head;
            }
        }

        return false;
    }

    template <HAKLE_CONCEPT( std::output_iterator<T&&> ) Iterator>
    std::size_t TryDequeueBulk( Iterator ItemFirst, std::size_t MaxCount ) {
        std::size_t Count = 0;
        ForEachProducerWithBreak( [ &ItemFirst, &MaxCount, &Count ]( ProducerListNode* Node ) -> bool {
            Count += Node->ProducerDequeueBulk( std::next( ItemFirst, Count ), MaxCount - Count );
            return Count != MaxCount;
        } );
        return Count;
    }

    template <HAKLE_CONCEPT( std::output_iterator<T&&> ) Iterator>
    std::size_t TryDequeueBulk( ConsumerToken& Token, Iterator ItemFirst, std::size_t MaxCount ) {
        if ( Token.DesiredProducer == nullptr || Token.LastKnownGlobalOffset != GlobalExplicitConsumerOffset().load( std::memory_order_relaxed ) ) {
            if ( !UpdateProducerForConsumer( Token ) ) {
                return 0;
            }
        }

        std::size_t Count = Token.CurrentProducer->ProducerDequeueBulk( ItemFirst, MaxCount );
        Token.ItemsConsumed += Count;
        if ( Count == MaxCount ) {
            if ( Token.ItemsConsumed >= EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE ) {
                GlobalExplicitConsumerOffset().fetch_add( 1, std::memory_order_relaxed );
            }
            return Count;
        }

        ProducerListNode* Head = ProducerListsHead.load( std::memory_order_acquire );
        ProducerListNode* Node = Token.CurrentProducer->Next;
        if ( Node == nullptr ) {
            Node = Head;
        }
        while ( Node != Token.CurrentProducer ) {
            std::size_t Dequeued = Node->ProducerDequeueBulk( std::next( ItemFirst, Count ), MaxCount - Count );
            Count += Dequeued;
            if ( Dequeued != 0 ) {
                Token.CurrentProducer = Node;
                Token.ItemsConsumed   = Dequeued;
            }
            if ( Count == MaxCount ) {
                break;
            }
            Node = Node->Next;
            if ( Node == nullptr ) {
                Node = Head;
            }
        }

        return Count;
    }

    template <class U>
    constexpr bool TryDequeueFromProducer( const ProducerToken& Token, U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), T&&> ) {
        return Token.ProducerNode->ProducerDequeue( Element );
    }

    template <HAKLE_CONCEPT( std::output_iterator<T&&> ) Iterator>
    std::size_t TryDequeueBulkFromProducer( const ProducerToken& Token, Iterator ItemFirst, std::size_t MaxCount ) {
        return Token.ProducerNode->ProducerDequeueBulk( ItemFirst, MaxCount );
    }

    struct ProducerToken {
        friend class ConcurrentQueue;
        explicit ProducerToken( ConcurrentQueue& queue ) : ProducerNode( queue.GetProducerListNode( ProducerType::Explicit ) ) {}
        ProducerToken( ProducerToken&& Other ) noexcept : ProducerNode( Other.ProducerNode ) {
            Other.ProducerNode = nullptr;
            if ( ProducerNode != nullptr ) {
                ProducerNode->Token = this;
            }
        }

        ~ProducerToken() {
            if ( ProducerNode != nullptr ) {
                ProducerNode->Token = nullptr;
                ProducerNode->Inactive.store( true, std::memory_order_release );
            }
        }

        ProducerToken( const ProducerToken& )            = delete;
        ProducerToken& operator=( const ProducerToken& ) = delete;

        ProducerToken& operator=( ProducerToken&& Other ) noexcept {
            swap( Other );
            return *this;
        }

        void swap( ProducerToken& Other ) noexcept {
            using std::swap;
            swap( ProducerNode, this->ProducerNode );
            if ( ProducerNode != nullptr ) {
                ProducerNode->Token = this;
            }
            if ( Other.ProducerNode != nullptr ) {
                Other.ProducerNode->Token = &Other;
            }
        }

        [[nodiscard]] bool Valid() const noexcept { return ProducerNode != nullptr; }

    protected:
        ProducerListNode* ProducerNode;
    };

    struct ConsumerToken {
        friend class ConcurrentQueue;

        // TODO: memory_order
        explicit ConsumerToken( ConcurrentQueue& queue ) noexcept : InitialOffset( queue.NextExplicitConsumerId().fetch_add( 1, std::memory_order_relaxed ) ) {}
        ConsumerToken( ConsumerToken&& Other ) noexcept
            : InitialOffset( Other.InitialOffset ), LastKnownGlobalOffset( Other.LastKnownGlobalOffset ), ItemsConsumed( Other.ItemsConsumed ), CurrentProducer( Other.CurrentProducer ),
              DesiredProducer( Other.DesiredProducer ) {}

        ConsumerToken& operator=( ConsumerToken&& Other ) noexcept {
            swap( Other );
            return *this;
        }

        void swap( ConsumerToken& Other ) noexcept {
            using std::swap;
            swap( InitialOffset, Other.InitialOffset );
            swap( DesiredProducer, Other.DesiredProducer );
            swap( LastKnownGlobalOffset, Other.LastKnownGlobalOffset );
            swap( CurrentProducer, Other.CurrentProducer );
            swap( DesiredProducer, Other.DesiredProducer );
        }

        ConsumerToken( const ConsumerToken& )            = delete;
        ConsumerToken& operator=( const ConsumerToken& ) = delete;

    private:
        std::uint32_t     InitialOffset{};
        std::uint32_t     LastKnownGlobalOffset{ static_cast<std::uint32_t>( -1 ) };
        std::uint32_t     ItemsConsumed{};
        ProducerListNode* CurrentProducer{};
        ProducerListNode* DesiredProducer{};
    };

private:
    template <AllocMode Alloc, class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<T, Args...> )
    constexpr bool InnerEnqueueWithToken( const ProducerToken& Token, Args&&... args ) {
        return Token.ProducerNode->template ProducerEnqueue<Alloc>( std::forward<Args>( args )... );
    }

    template <AllocMode Alloc, class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<T, Args...> )
    constexpr bool InnerEnqueue( Args&&... args ) {
        ImplicitProducer* producer = GetOrAddImplicitProducer();
        return producer == nullptr ? false : producer->template Enqueue<Alloc>( std::forward<Args>( args )... );
    }

    template <AllocMode Alloc, HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { T( *Item ); } )
    constexpr bool InnerEnqueueBulk( const ProducerToken& Token, Iterator ItermFirst, std::size_t Count ) {
        return Token.ProducerNode->template ProducerEnqueueBulk<Alloc>( ItermFirst, Count );
    }

    template <AllocMode Alloc, HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { T( *Item ); } )
    constexpr bool InnerEnqueueBulk( Iterator ItermFirst, std::size_t Count ) {
        ImplicitProducer* producer = GetOrAddImplicitProducer();
        return producer == nullptr ? false : producer->template EnqueueBulk<Alloc>( ItermFirst, Count );
    }

    enum class ProducerType { Explicit, Implicit };

    struct ProducerListNode {
        ProducerListNode* Next{ nullptr };
        std::atomic<bool> Inactive{ false };
        BaseProducer*     Producer{};
        ProducerToken*    Token{ nullptr };
        ConcurrentQueue*  Parent{ nullptr };
        ProducerType      Type;

        constexpr ProducerListNode( BaseProducer* InProducer, ProducerType InType, ConcurrentQueue* InParent ) noexcept : Producer( InProducer ), Parent( InParent ), Type( InType ) {}

        constexpr ExplicitProducer* GetExplicitProducer() const noexcept { return static_cast<ExplicitProducer*>( Producer ); }
        constexpr ImplicitProducer* GetImplicitProducer() const noexcept { return static_cast<ImplicitProducer*>( Producer ); }

        template <AllocMode Alloc, class... Args>
        HAKLE_REQUIRES( std::is_constructible_v<T, Args...> )
        constexpr bool ProducerEnqueue( Args&&... args ) {
            if ( Type == ProducerType::Explicit ) {
                return GetExplicitProducer()->template Enqueue<Alloc>( std::forward<Args>( args )... );
            }
            else {
                return GetImplicitProducer()->template Enqueue<Alloc>( std::forward<Args>( args )... );
            }
        }

        template <AllocMode Alloc, HAKLE_CONCEPT( std::input_iterator ) Iterator>
        HAKLE_REQUIRES( requires( Iterator Item ) { T( *Item ); } )
        constexpr bool ProducerEnqueueBulk( Iterator ItermFirst, std::size_t Count ) {
            if ( Type == ProducerType::Explicit ) {
                return GetExplicitProducer()->template EnqueueBulk<Alloc>( ItermFirst, Count );
            }
            else {
                return GetImplicitProducer()->template EnqueueBulk<Alloc>( ItermFirst, Count );
            }
        }

        template <class U>
        constexpr bool ProducerDequeue( U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), T&&> ) {
            if ( Type == ProducerType::Explicit ) {
                return GetExplicitProducer()->Dequeue( Element );
            }
            else {
                return GetImplicitProducer()->Dequeue( Element );
            }
        }

        template <HAKLE_CONCEPT( std::output_iterator<T&&> ) Iterator>
        constexpr std::size_t ProducerDequeueBulk( Iterator ItemFirst, std::size_t MaxCount ) {
            if ( Type == ProducerType::Explicit ) {
                return GetExplicitProducer()->DequeueBulk( ItemFirst, MaxCount );
            }
            else {
                return GetImplicitProducer()->DequeueBulk( ItemFirst, MaxCount );
            }
        }

        [[nodiscard]] constexpr std::size_t GetProducerSize() const noexcept { return Type == ProducerType::Explicit ? GetExplicitProducer()->Size() : GetImplicitProducer()->Size(); }

        HAKLE_CPP20_CONSTEXPR ~ProducerListNode() = default;
    };

    constexpr ProducerListNode* GetProducerListNode( ProducerType Type ) noexcept {
        for ( ProducerListNode* Node = ProducerListsHead.load( std::memory_order_relaxed ); Node != nullptr; Node = Node->Next ) {
            if ( Node->Inactive.load( std::memory_order_relaxed ) && Node->Type == Type ) {
                bool expected = true;
                if ( Node->Inactive.compare_exchange_strong( expected, false, std::memory_order_release, std::memory_order_relaxed ) ) {
                    return Node;
                }
            }
        }

        return AddProducer( CreateProducerListNode( Type ) );
    }

    constexpr void ReclaimProducerLists() noexcept {
        ForEachProducer( [ this ]( ProducerListNode* Node ) { Node->Parent = this; } );
    }

    constexpr ProducerListNode* AddProducer( ProducerListNode* Node ) {
        if ( Node == nullptr ) {
            return nullptr;
        }

        ProducerCount.fetch_add( 1, std::memory_order_relaxed );

        ProducerListNode* Head = ProducerListsHead.load( std::memory_order_relaxed );
        do {
            Node->Next = Head;
        } while ( !ProducerListsHead.compare_exchange_weak( Head, Node, std::memory_order_release, std::memory_order_relaxed ) );

        return Node;
    }

    constexpr ProducerListNode* CreateProducerListNode( ProducerType Type ) {
        BaseProducer* producer = nullptr;

        if ( Type == ProducerType::Explicit ) {
            producer = ExplicitProducerAllocatorTraits::Allocate( ExplicitProducerAllocator() );
            ExplicitProducerAllocatorTraits::Construct( ExplicitProducerAllocator(), static_cast<ExplicitProducer*>( producer ), InitialExplicitQueueSize, ExplicitManager(), ValueAllocator() );
        }
        else {
            producer = ImplicitProducerAllocatorTraits::Allocate( ImplicitProducerAllocator() );
            ImplicitProducerAllocatorTraits::Construct( ImplicitProducerAllocator(), static_cast<ImplicitProducer*>( producer ), InitialImplicitQueueSize, ImplicitManager(), ValueAllocator() );
        }

        ProducerListNode* node = ProducerListNodeAllocatorTraits::Allocate( ProducerListNodeAllocator() );
        ProducerListNodeAllocatorTraits::Construct( ProducerListNodeAllocator(), node, producer, Type, this );

        return node;
    }

    // only used in destructor
    constexpr void DeleteProducerListNode( ProducerListNode* Node ) {
        if ( Node == nullptr ) {
            return;
        }

        ProducerCount.fetch_sub( 1, std::memory_order_relaxed );

        if ( Node->Type == ProducerType::Explicit ) {
            ExplicitProducerAllocatorTraits::Destroy( ExplicitProducerAllocator(), Node->GetExplicitProducer() );
            ExplicitProducerAllocatorTraits::Deallocate( ExplicitProducerAllocator(), Node->GetExplicitProducer() );
        }
        else {
            ImplicitProducerAllocatorTraits::Destroy( ImplicitProducerAllocator(), Node->GetImplicitProducer() );
            ImplicitProducerAllocatorTraits::Deallocate( ImplicitProducerAllocator(), Node->GetImplicitProducer() );
        }

        ProducerListNodeAllocatorTraits::Destroy( ProducerListNodeAllocator(), Node );
        ProducerListNodeAllocatorTraits::Deallocate( ProducerListNodeAllocator(), Node );
    }

    constexpr void ForEachProducer( std::function<void( ProducerListNode* )> Func ) {
        for ( ProducerListNode* Node = ProducerListsHead.load( std::memory_order_relaxed ); Node != nullptr; Node = Node->Next ) {
            Func( Node );
        }
    }

    constexpr void ForEachProducerWithBreak( std::function<bool( ProducerListNode* )> Func ) {
        for ( ProducerListNode* Node = ProducerListsHead.load( std::memory_order_relaxed ); Node != nullptr; Node = Node->Next ) {
            if ( !Func( Node ) ) {
                return;
            }
        }
    }

    constexpr bool ForEachProducerWithReturn( std::function<bool( ProducerListNode* )> Func ) {
        for ( ProducerListNode* Node = ProducerListsHead.load( std::memory_order_relaxed ); Node != nullptr; Node = Node->Next ) {
            if ( Func( Node ) ) {
                return true;
            }
        }
        return false;
    }

    constexpr void ForEachProducerSafe( std::function<void( ProducerListNode* )> Func ) {
        for ( ProducerListNode* Node = ProducerListsHead.load( std::memory_order_relaxed ); Node != nullptr; ) {
            ProducerListNode* Next = Node->Next;
            Func( Node );
            Node = Next;
        }
    }

    constexpr bool UpdateProducerForConsumer( ConsumerToken& Token ) {
        ProducerListNode* Head = ProducerListsHead.load( std::memory_order_acquire );
        if ( Token.DesiredProducer == nullptr && Head == nullptr )
            return false;
        std::uint32_t ProducerCount = this->ProducerCount.load( std::memory_order_relaxed );
        std::uint32_t GlobalOffset  = GlobalExplicitConsumerOffset().load( std::memory_order_relaxed );
        if HAKLE_UNLIKELY ( Token.DesiredProducer == nullptr ) {
            std::uint32_t Offset  = Token.InitialOffset % ProducerCount;
            Token.DesiredProducer = Head;
            for ( std::uint32_t i = 0; i < Offset; ++i ) {
                Token.DesiredProducer = Token.DesiredProducer->Next;
                if ( Token.DesiredProducer == nullptr ) {
                    Token.DesiredProducer = Head;
                }
            }
        }

        std::uint32_t Delta = GlobalOffset - Token.LastKnownGlobalOffset;
        if ( Delta >= ProducerCount ) {
            Delta = Delta & ProducerCount;
        }
        for ( std::uint32_t i = 0; i < Delta; ++i ) {
            Token.DesiredProducer = Token.DesiredProducer->Next;
            if ( Token.DesiredProducer == nullptr ) {
                Token.DesiredProducer = Head;
            }
        }

        Token.LastKnownGlobalOffset = GlobalOffset;
        Token.CurrentProducer       = Token.DesiredProducer;
        Token.ItemsConsumed         = 0;

        return true;
    }

    ImplicitProducer* GetOrAddImplicitProducer() {
        details::thread_id_t thread_id = details::thread_id();
        ImplicitProducer*    producer  = nullptr;
        HashTableStatus Result = ImplicitMap.GetOrAddByFunc( thread_id, producer, [ this, &producer ]() { return producer = GetProducerListNode( ProducerType::Implicit )->GetImplicitProducer(); } );
        if ( Result == HashTableStatus::FAILED ) {
            return nullptr;
        }
        return producer;
    }

    std::atomic<ProducerListNode*> ProducerListsHead{};
    std::atomic<uint32_t>          ProducerCount{};

    CompressPair<ExplicitBlockManagerType, ExplicitProducerAllocatorType>   ExplicitProducerAllocatorPair{};
    CompressPair<ImplicitBlockManagerType, ImplicitProducerAllocatorType>   ImplicitProducerAllocatorPair{};
    CompressPair<std::atomic<std::uint32_t>, AllocatorType>                 ValueAllocatorPair{};
    CompressPair<std::atomic<std::uint32_t>, ProducerListNodeAllocatorType> ProducerListNodeAllocatorPair{};

    constexpr ExplicitBlockManagerType&   ExplicitManager() noexcept { return ExplicitProducerAllocatorPair.First(); }
    constexpr ImplicitBlockManagerType&   ImplicitManager() noexcept { return ImplicitProducerAllocatorPair.First(); }
    constexpr std::atomic<std::uint32_t>& GlobalExplicitConsumerOffset() noexcept { return ValueAllocatorPair.First(); }
    constexpr std::atomic<std::uint32_t>& NextExplicitConsumerId() noexcept { return ProducerListNodeAllocatorPair.First(); }

    constexpr ExplicitProducerAllocatorType& ExplicitProducerAllocator() noexcept { return ExplicitProducerAllocatorPair.Second(); }
    constexpr ImplicitProducerAllocatorType& ImplicitProducerAllocator() noexcept { return ImplicitProducerAllocatorPair.Second(); }
    constexpr AllocatorType&                 ValueAllocator() noexcept { return ValueAllocatorPair.Second(); }
    constexpr ProducerListNodeAllocatorType& ProducerListNodeAllocator() noexcept { return ProducerListNodeAllocatorPair.Second(); }

    constexpr const ExplicitBlockManagerType&   ExplicitManager() const noexcept { return ExplicitProducerAllocatorPair.First(); }
    constexpr const ImplicitBlockManagerType&   ImplicitManager() const noexcept { return ImplicitProducerAllocatorPair.First(); }
    constexpr const std::atomic<std::uint32_t>& GlobalExplicitConsumerOffset() const noexcept { return ValueAllocatorPair.First(); }
    constexpr const std::atomic<std::uint32_t>& NextExplicitConsumerId() const noexcept { return ProducerListNodeAllocatorPair.First(); }

    constexpr const ExplicitProducerAllocatorType& ExplicitProducerAllocator() const noexcept { return ExplicitProducerAllocatorPair.Second(); }
    constexpr const ImplicitProducerAllocatorType& ImplicitProducerAllocator() const noexcept { return ImplicitProducerAllocatorPair.Second(); }
    constexpr const AllocatorType&                 ValueAllocator() const noexcept { return ValueAllocatorPair.Second(); }
    constexpr const ProducerListNodeAllocatorType& ProducerListNodeAllocator() const noexcept { return ProducerListNodeAllocatorPair.Second(); }

    HashTable<details::thread_id_t, ImplicitProducer*, InitialHashSize, details::thread_hash> ImplicitMap{};
};

#if HAKLE_CPP_VERSION <= 14
template <class T, class Alloc>
constexpr std::size_t ConcurrentQueueDefaultTraits<T, Alloc>::BlockSize;

template <class T, class Alloc>
constexpr std::size_t ConcurrentQueueDefaultTraits<T, Alloc>::InitialBlockPoolSize;

template <class T, class Alloc>
constexpr std::size_t ConcurrentQueueDefaultTraits<T, Alloc>::InitialHashSize;

template <class T, class Alloc>
constexpr std::size_t ConcurrentQueueDefaultTraits<T, Alloc>::InitialExplicitQueueSize;

template <class T, class Alloc>
constexpr std::size_t ConcurrentQueueDefaultTraits<T, Alloc>::InitialImplicitQueueSize;
#endif

}  // namespace hakle

#endif  // CONCURRENTQUEUE_H
