//
// Created by admin on 25-12-1.
//

#ifndef CONCURRENTQUEUE_H
#define CONCURRENTQUEUE_H

#include "BlockManager.h"
#include "common/CompressPair.h"
#include "common/common.h"
#include "common/utility.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hakle {

// TODO: manager traits
template <HAKLE_CONCEPT( IsBlock ) BLOCK_TYPE, HAKLE_CONCEPT( IsBlockManager ) BLOCK_MANAGER_TYPE>
struct QueueBase {
public:
    using BlockManagerType                 = BLOCK_MANAGER_TYPE;
    using BlockType                        = BLOCK_TYPE;
    using ValueType                        = typename BlockManagerType::ValueType;
    constexpr static std::size_t BlockSize = BlockManagerType::BlockSize;

    using BlockAllocatorTraits = typename BlockManagerType::BlockAllocatorTraits;
    using BlockAllocatorType   = typename BlockManagerType::AllocatorType;

    using AllocMode = typename BlockManagerType::AllocMode;

    virtual HAKLE_CPP20_CONSTEXPR ~QueueBase() = default;

    HAKLE_NODISCARD constexpr std::size_t Size() const noexcept {
        std::size_t Tail = TailIndex.load( std::memory_order_relaxed );
        std::size_t Head = HeadIndex.load( std::memory_order_relaxed );
        return CircularLessThan( Head, Tail ) ? Tail - Head : 0;
    }

    HAKLE_NODISCARD constexpr std::size_t GetTail() const noexcept { return TailIndex.load( std::memory_order_relaxed ); }

protected:
    std::atomic<std::size_t> HeadIndex{};
    std::atomic<std::size_t> TailIndex{};
    std::atomic<std::size_t> DequeueAttemptsCount{};
    std::atomic<std::size_t> DequeueFailedCount{};
    BlockType*               TailBlock{ nullptr };
};

// SPMC Queue
template <HAKLE_CONCEPT( IsBlock ) BLOCK_TYPE, HAKLE_CONCEPT( IsBlockManager ) BLOCK_MANAGER_TYPE = HakleBlockManager<BLOCK_TYPE>>
class FastQueue : public QueueBase<BLOCK_TYPE, BLOCK_MANAGER_TYPE> {
public:
    using Base = QueueBase<BLOCK_TYPE, BLOCK_MANAGER_TYPE>;

    using Base::BlockSize;
    using typename Base::AllocMode;
    using typename Base::BlockAllocatorTraits;
    using typename Base::BlockAllocatorType;
    using typename Base::BlockManagerType;
    using typename Base::BlockType;
    using typename Base::ValueType;

private:
    constexpr static std::size_t BlockSizeLog2 = BitWidth( BlockSize ) - 1;

    struct IndexEntry;
    struct IndexEntryArray;
    using IndexEntryAllocatorType        = typename BlockAllocatorTraits::template RebindAlloc<IndexEntry>;
    using IndexEntryArrayAllocatorType   = typename BlockAllocatorTraits::template RebindAlloc<IndexEntryArray>;
    using ValueAllocatorType             = typename BlockAllocatorTraits::template RebindAlloc<ValueType>;
    using IndexEntryAllocatorTraits      = HakeAllocatorTraits<IndexEntryAllocatorType>;
    using IndexEntryArrayAllocatorTraits = HakeAllocatorTraits<IndexEntryArrayAllocatorType>;
    using ValueAllocatorTraits           = HakeAllocatorTraits<ValueAllocatorType>;

public:
    constexpr explicit FastQueue( std::size_t InSize, BlockManagerType& InBlockManager ) : BlockManager( InBlockManager ) {
        std::size_t InitialSize = CeilToPow2( InSize ) >> 1;
        if ( InitialSize < 2 ) {
            InitialSize = 2;
        }
        PO_IndexEntriesSize() = InitialSize;

        CreateNewBlockIndexArray( 0 );
    }

    HAKLE_CPP20_CONSTEXPR ~FastQueue() override {
        if ( this->TailBlock != nullptr ) {
            // first, we find the first block that's half dequeued
            BlockType* HalfDequeuedBlock = nullptr;
            if ( ( this->HeadIndex.load( std::memory_order_relaxed ) & ( BlockSize - 1 ) ) != 0 ) {
                std::size_t i = ( PO_NextIndexEntry() - PO_IndexEntriesUsed() ) & ( PO_IndexEntriesSize() - 1 );
                while ( CircularLessThan( this->PO_PrevEntries[ i ].Base + BlockSize, this->HeadIndex.load( std::memory_order_relaxed ) ) ) {
                    i = ( i + 1 ) & ( PO_IndexEntriesSize() - 1 );
                }
                HalfDequeuedBlock = PO_PrevEntries[ i ].InnerBlock;
            }

            // then, we can return back all the blocks
            BlockType* Block = this->TailBlock;
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
                while ( i != BlockSize && ( Block != this->TailBlock || i != LastIndex ) ) {
                    ValueAllocatorTraits::Destroy( ValueAllocator(), ( *Block )[ i++ ] );
                }
            } while ( Block != this->TailBlock );
        }

        // let's return block to manager
        if ( this->TailBlock != nullptr ) {
            BlockType* Block = this->TailBlock;
            do {
                BlockType* NextBlock = Block->Next;
                BlockManager.ReturnBlock( Block );
                Block = NextBlock;
            } while ( Block != this->TailBlock );
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
        if ( InnerIndex == 0 ) {
            BlockType*  OldTailBlock        = this->TailBlock;
            std::size_t OldIndexEntriesUsed = PO_IndexEntriesUsed();
            // zero, in fact
            // we must find a new block
            if ( this->TailBlock != nullptr && this->TailBlock->Next->IsEmpty() ) {
                // we can re-use that block
                this->TailBlock = this->TailBlock->Next;
                this->TailBlock->Reset();
            }
            else {
                // we need to find a new block index and get a new block from block manager
                // TODO: add MAX_SIZE check
                if ( !CircularLessThan( this->HeadIndex.load( std::memory_order_relaxed ), CurrentTailIndex + BlockSize ) ) {
                    return false;
                }

                if ( CurrentIndexEntryArray.load( std::memory_order_relaxed ) == nullptr || PO_IndexEntriesUsed() == PO_IndexEntriesSize() ) {
                    // need to create a new index entry array
                    HAKLE_CONSTEXPR_IF( Mode == AllocMode::CannotAlloc ) { return false; }
                    else if ( !CreateNewBlockIndexArray( PO_IndexEntriesUsed() ) ) {
                        return false;
                    }
                }

                BlockType* NewBlock = BlockManager.RequisitionBlock( Mode );
                if ( NewBlock == nullptr ) {
                    return false;
                }

                NewBlock->Reset();
                if ( this->TailBlock == nullptr ) {
                    NewBlock->Next = NewBlock;
                }
                else {
                    NewBlock->Next        = this->TailBlock->Next;
                    this->TailBlock->Next = NewBlock;
                }
                this->TailBlock = NewBlock;
                // get a new block
                ++PO_IndexEntriesUsed();
            }

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<ValueType, Args&&...>::value ) {
                // we need to handle exception here
                HAKLE_TRY {
                    ValueAllocatorTraits::Construct( ValueAllocator(), ( *( this->TailBlock ) )[ InnerIndex ], std::forward<Args>( args )... );
                }
                HAKLE_CATCH( ... ) {
                    // when OldTailBlock is nullptr, we should not go back to prevent block leak
                    this->TailBlock       = OldTailBlock == nullptr ? this->TailBlock : OldTailBlock;
                    PO_IndexEntriesUsed() = OldIndexEntriesUsed;
                    HAKLE_RETHROW;
                }
            }

            IndexEntry& Entry = this->CurrentIndexEntryArray.load( std::memory_order_relaxed )->Entries[ PO_NextIndexEntry() ];
            Entry.Base        = CurrentTailIndex;
            Entry.InnerBlock  = this->TailBlock;
            this->CurrentIndexEntryArray.load( std::memory_order_relaxed )->Tail.store( PO_NextIndexEntry(), std::memory_order_release );
            PO_NextIndexEntry() = ( PO_NextIndexEntry() + 1 ) & ( PO_IndexEntriesSize() - 1 );

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<ValueType, Args&&...>::value ) {
                this->TailIndex.store( NewTailIndex, std::memory_order_release );
                return true;
            }
        }

        ValueAllocatorTraits::Construct( ValueAllocator(), ( *( this->TailBlock ) )[ InnerIndex ], std::forward<Args>( args )... );

        this->TailIndex.store( NewTailIndex, std::memory_order_release );
        return true;
    }

    template <AllocMode Mode, HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_CPP20_CONSTEXPR bool EnqueueBulk( Iterator ItemFirst, std::size_t Count ) {
        // set original state
        std::size_t OriginIndexEntriesUsed = PO_IndexEntriesUsed();
        std::size_t OriginNextIndexEntry   = PO_NextIndexEntry();
        BlockType*  StartBlock             = ( this->TailBlock );
        std::size_t StartTailIndex         = this->TailIndex.load( std::memory_order_relaxed );
        BlockType*  FirstAllocatedBlock    = nullptr;

        // roll back
        auto RollBack = [ this, &OriginIndexEntriesUsed, &OriginNextIndexEntry, &StartBlock, &FirstAllocatedBlock ]() -> bool {
            PO_IndexEntriesUsed() = OriginIndexEntriesUsed;
            PO_NextIndexEntry()   = OriginNextIndexEntry;
            this->TailBlock       = StartBlock == nullptr ? FirstAllocatedBlock : StartBlock;
            return false;
        };

        std::size_t LastTailIndex = StartTailIndex - 1;
        // std::size_t BlockCountNeed =
        //     ( ( ( Count + LastTailIndex ) & ~( BlockSize - 1 ) ) - ( ( LastTailIndex & ~( BlockSize - 1 ) ) ) ) >> BlockSizeLog2;

        std::size_t BlockCountNeed = ( ( Count + StartTailIndex - 1 ) >> BlockSizeLog2 )
                                     - ( static_cast<std::make_signed_t<std::size_t>>( StartTailIndex - 1 ) >> BlockSizeLog2 );
        std::size_t CurrentTailIndex = LastTailIndex & ~( BlockSize - 1 );

        if ( BlockCountNeed > 0 ) {
            while ( BlockCountNeed > 0 && this->TailBlock != nullptr && this->TailBlock->Next->IsEmpty() ) {
                // we can re-use that block
                --BlockCountNeed;
                CurrentTailIndex += BlockSize;
                this->TailBlock     = this->TailBlock->Next;
                FirstAllocatedBlock = FirstAllocatedBlock == nullptr ? this->TailBlock : FirstAllocatedBlock;
                this->TailBlock->Reset();

                auto& Entry         = this->CurrentIndexEntryArray.load( std::memory_order_relaxed )->Entries[ PO_NextIndexEntry() ];
                Entry.Base          = CurrentTailIndex;
                Entry.InnerBlock    = this->TailBlock;
                PO_NextIndexEntry() = ( PO_NextIndexEntry() + 1 ) & ( PO_IndexEntriesSize() - 1 );
            }

            while ( BlockCountNeed > 0 ) {
                // we must get a new block
                CurrentTailIndex += BlockSize;
                --BlockCountNeed;
                // TODO: add MAX_SIZE check
                if ( !CircularLessThan( this->HeadIndex.load( std::memory_order_relaxed ), CurrentTailIndex + BlockSize ) ) {
                    return RollBack();
                }

                if ( CurrentIndexEntryArray.load( std::memory_order_relaxed ) == nullptr || PO_IndexEntriesUsed() == PO_IndexEntriesSize() ) {
                    // need to create a new index entry array
                    HAKLE_CONSTEXPR_IF( Mode == AllocMode::CannotAlloc ) { return RollBack(); }
                    else if ( !CreateNewBlockIndexArray( OriginIndexEntriesUsed ) ) {
                        return RollBack();
                    }

                    OriginNextIndexEntry = OriginIndexEntriesUsed;
                }

                BlockType* NewBlock = BlockManager.RequisitionBlock( Mode );
                if ( NewBlock == nullptr ) {
                    return RollBack();
                }

                NewBlock->Reset();
                if ( this->TailBlock == nullptr ) {
                    NewBlock->Next = NewBlock;
                }
                else {
                    NewBlock->Next        = this->TailBlock->Next;
                    this->TailBlock->Next = NewBlock;
                }
                this->TailBlock     = NewBlock;
                FirstAllocatedBlock = FirstAllocatedBlock == nullptr ? this->TailBlock : FirstAllocatedBlock;
                // get a new block
                ++PO_IndexEntriesUsed();

                auto& Entry         = this->CurrentIndexEntryArray.load( std::memory_order_relaxed )->Entries[ PO_NextIndexEntry() ];
                Entry.Base          = CurrentTailIndex;
                Entry.InnerBlock    = this->TailBlock;
                PO_NextIndexEntry() = ( PO_NextIndexEntry() + 1 ) & ( PO_IndexEntriesSize() - 1 );
            }
        }

        // we already have enough blocks, let's fill them
        std::size_t StartInnerIndex = StartTailIndex & ( BlockSize - 1 );
        BlockType*  CurrentBlock    = ( StartInnerIndex == 0 && FirstAllocatedBlock != nullptr ) ? FirstAllocatedBlock : StartBlock;
        while ( true ) {
            std::size_t EndInnerIndex = ( CurrentBlock == this->TailBlock ) ? ( StartTailIndex + Count - 1 ) & ( BlockSize - 1 ) : ( BlockSize - 1 );
            HAKLE_CONSTEXPR_IF( std::is_nothrow_constructible<ValueType, typename std::iterator_traits<Iterator>::value_type>::value ) {
                while ( StartInnerIndex <= EndInnerIndex ) {
                    ValueAllocatorTraits::Construct( ValueAllocator(), ( *CurrentBlock )[ StartInnerIndex ], *ItemFirst++ );
                    ++StartInnerIndex;
                }
            }
            else {
                HAKLE_TRY {
                    while ( StartInnerIndex <= EndInnerIndex ) {
                        ValueAllocatorTraits::Construct( ValueAllocator(), ( *( CurrentBlock ) )[ StartInnerIndex ], *ItemFirst++ );
                        ++StartInnerIndex;
                    }
                }
                HAKLE_CATCH( ... ) {
                    // we need to set all allocated blocks to empty
                    if ( FirstAllocatedBlock != nullptr ) {
                        BlockType* AllocatedBlock = FirstAllocatedBlock;
                        while ( true ) {
                            AllocatedBlock->SetAllEmpty();
                            if ( AllocatedBlock == this->TailBlock ) {
                                break;
                            }
                            AllocatedBlock = AllocatedBlock->Next;
                        }
                    }

                    // TODO: return false
                    RollBack();

                    // destroy all values
                    HAKLE_CONSTEXPR_IF( !std::is_trivially_destructible<ValueType>::value ) {
                        std::size_t StartInnerIndex2 = StartTailIndex & ( BlockSize - 1 );
                        BlockType*  StartBlock2      = ( StartInnerIndex2 == 0 && FirstAllocatedBlock != nullptr ) ? FirstAllocatedBlock : StartBlock;
                        while ( true ) {
                            std::size_t EndInnerIndex2 = ( StartBlock2 == CurrentBlock ) ? StartInnerIndex : BlockSize;
                            while ( StartInnerIndex2 != EndInnerIndex2 ) {
                                ValueAllocatorTraits::Destroy( ValueAllocator(), ( *( StartBlock2 ) )[ StartInnerIndex2 ] );
                                ++StartInnerIndex2;
                            }
                            if ( StartBlock2 == CurrentBlock ) {
                                break;
                            }
                            StartBlock2      = StartBlock2->Next;
                            StartInnerIndex2 = 0;
                        }
                    }
                    HAKLE_RETHROW;
                }
            }
            if ( CurrentBlock == this->TailBlock ) {
                break;
            }
            StartInnerIndex = 0;
            CurrentBlock    = CurrentBlock->Next;
        }

        this->CurrentIndexEntryArray.load( std::memory_order_relaxed )
            ->Tail.store( ( PO_NextIndexEntry() - 1 ) & ( PO_IndexEntriesSize() - 1 ), std::memory_order_release );
        this->TailIndex.store( StartTailIndex + Count, std::memory_order_release );
        return true;
    }

    // TODO: EnqueueBulkMove

    // Dequeue
    template <class U>
    constexpr bool Dequeue( U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), ValueType&&> ) {
        std::size_t FailedCount = this->DequeueFailedCount.load( std::memory_order_relaxed );
        if ( HAKLE_LIKELY( CircularLessThan( this->DequeueAttemptsCount.load( std::memory_order_relaxed ) - FailedCount,
                                             this->TailIndex.load( std::memory_order_relaxed ) ) ) ) {
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
                BlockType*  DequeueBlock =
                    LocalIndexEntryArray->Entries[ ( LocalIndexEntryIndex + Offset ) & ( LocalIndexEntryArray->Size - 1 ) ].InnerBlock;

                ValueType& Value = *( *DequeueBlock )[ InnerIndex ];

                HAKLE_CONSTEXPR_IF( !std::is_nothrow_assignable<U, ValueType>::value ) {
                    struct Guard {
                        BlockType*                                    Block;
                        CompressPair<std::size_t, ValueAllocatorType> ValueAllocatorPair;

                        ~Guard() {
                            ValueAllocatorTraits::Destroy( ValueAllocatorPair.Second(), ( *Block )[ ValueAllocatorPair.First() ] );
                            Block->SetEmpty( ValueAllocatorPair.First() );
                        }
                    } guard{ .Block = DequeueBlock, .ValueAllocatorPair = { InnerIndex, ValueAllocator() } };

                    Element = std::move( Value );
                }
                else {
                    Element = std::move( Value );
                    ValueAllocatorTraits::Destroy( ValueAllocator(), &Value );
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
        std::size_t FailedCount = this->DequeueFailedCount.load( std::memory_order_relaxed );
        std::size_t DesiredCount =
            this->TailIndex.load( std::memory_order_relaxed ) - ( this->DequeueAttemptsCount.load( std::memory_order_relaxed ) - FailedCount );
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
                BlockType*  FirstDequeueBlock =
                    LocalIndexEntriesArray->Entries[ ( LocalIndexEntryIndex + Offset ) & ( LocalIndexEntriesArray->Size - 1 ) ].InnerBlock;

                BlockType*  DequeueBlock = FirstDequeueBlock;
                std::size_t StartIndex   = InnerIndex;
                std::size_t NeedCount    = ActualCount;
                while ( NeedCount != 0 ) {
                    std::size_t EndIndex     = ( NeedCount > ( BlockSize - StartIndex ) ) ? BlockSize : ( NeedCount + StartIndex );
                    std::size_t CurrentIndex = StartIndex;
                    HAKLE_CONSTEXPR_IF( std::is_nothrow_assignable<typename std::iterator_traits<Iterator>::value_type, ValueType&&>::value ) {
                        while ( CurrentIndex != EndIndex ) {
                            ValueType& Value = *( *DequeueBlock )[ CurrentIndex ];
                            *ItemFirst       = std::move( Value );
                            ++ItemFirst;
                            ValueAllocatorTraits::Destroy( ValueAllocator(), &Value );
                            ++CurrentIndex;
                            --NeedCount;
                        }
                    }
                    else {
                        HAKLE_TRY {
                            while ( CurrentIndex != EndIndex ) {
                                ValueType& Value = *( *DequeueBlock )[ CurrentIndex ];
                                *ItemFirst++     = std::move( Value );
                                ValueAllocatorTraits::Destroy( ValueAllocator(), &Value );
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
                                    ValueAllocatorTraits::Destroy( ValueAllocator(), &Value );
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
                    DequeueBlock->SetSomeEmpty( StartIndex, EndIndex - StartIndex );
                    StartIndex   = 0;
                    DequeueBlock = DequeueBlock->Next;
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
            std::size_t i = ( PO_NextIndexEntry() - PO_IndexEntriesUsed() ) & SizeMask;
            do {
                NewEntries[ j++ ] = PO_PrevEntries[ i ];
                i                 = ( i + 1 ) & SizeMask;
            } while ( i != PO_NextIndexEntry() );
        }

        NewIndexEntryArray->Size    = PO_IndexEntriesSize();
        NewIndexEntryArray->Entries = NewEntries;
        NewIndexEntryArray->Tail.store( FilledSlot - 1, std::memory_order_relaxed );
        NewIndexEntryArray->Prev = CurrentIndexEntryArray.load( std::memory_order_relaxed );

        PO_NextIndexEntry() = j;
        PO_PrevEntries      = NewEntries;
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
    CompressPair<std::size_t, ValueAllocatorType>           ValueAllocatorPair{};

    constexpr IndexEntryAllocatorType&      IndexEntryAllocator() noexcept { return IndexEntryAllocatorPair.Second(); }
    constexpr IndexEntryArrayAllocatorType& IndexEntryArrayAllocator() noexcept { return IndexEntryArrayAllocatorPair.Second(); }
    constexpr ValueAllocatorType&           ValueAllocator() noexcept { return ValueAllocatorPair.Second(); }

    constexpr const IndexEntryAllocatorType&      IndexEntryAllocator() const noexcept { return IndexEntryAllocatorPair.Second(); }
    constexpr const IndexEntryArrayAllocatorType& IndexEntryArrayAllocator() const noexcept { return IndexEntryArrayAllocatorPair.Second(); }
    constexpr const ValueAllocatorType&           ValueAllocator() const noexcept { return ValueAllocatorPair.Second(); }

    // producer only fields
    constexpr std::size_t& PO_IndexEntriesUsed() noexcept { return IndexEntryAllocatorPair.First(); }
    constexpr std::size_t& PO_IndexEntriesSize() noexcept { return IndexEntryArrayAllocatorPair.First(); }
    constexpr std::size_t& PO_NextIndexEntry() noexcept { return ValueAllocatorPair.First(); }

    HAKLE_NODISCARD constexpr const std::size_t& PO_IndexEntriesUsed() const noexcept { return IndexEntryAllocatorPair.First(); }
    HAKLE_NODISCARD constexpr const std::size_t& PO_IndexEntriesSize() const noexcept { return IndexEntryArrayAllocatorPair.First(); }
    HAKLE_NODISCARD constexpr const std::size_t& PO_NextIndexEntry() const noexcept { return ValueAllocatorPair.First(); }

    IndexEntry* PO_PrevEntries{ nullptr };
};

template <HAKLE_CONCEPT( IsBlockWithMeaningfulSetResult ) BLOCK_TYPE,
          HAKLE_CONCEPT( IsBlockManager ) BLOCK_MANAGER_TYPE = HakleBlockManager<BLOCK_TYPE>>
class SlowQueue : public QueueBase<BLOCK_TYPE, BLOCK_MANAGER_TYPE> {
public:
    using Base = QueueBase<BLOCK_TYPE, BLOCK_MANAGER_TYPE>;

    using Base::BlockSize;
    using typename Base::AllocMode;
    using typename Base::BlockAllocatorTraits;
    using typename Base::BlockAllocatorType;
    using typename Base::BlockManagerType;
    using typename Base::BlockType;
    using typename Base::ValueType;

private:
    constexpr static std::size_t BlockSizeLog2      = BitWidth( BlockSize ) - 1;
    constexpr static std::size_t INVALID_BLOCK_BASE = 1;

    struct IndexEntry;
    struct IndexEntryArray;
    using IndexEntryAllocatorType        = typename BlockAllocatorTraits::template RebindAlloc<IndexEntry>;
    using IndexEntryArrayAllocatorType   = typename BlockAllocatorTraits::template RebindAlloc<IndexEntryArray>;
    using IndexEntryPointerAllocatorType = typename BlockAllocatorTraits::template RebindAlloc<IndexEntry*>;
    using ValueAllocatorType             = typename BlockAllocatorTraits::template RebindAlloc<ValueType>;

    using IndexEntryAllocatorTraits        = HakeAllocatorTraits<IndexEntryAllocatorType>;
    using IndexEntryArrayAllocatorTraits   = HakeAllocatorTraits<IndexEntryArrayAllocatorType>;
    using IndexEntryPointerAllocatorTraits = HakeAllocatorTraits<IndexEntryPointerAllocatorType>;
    using ValueAllocatorTraits             = HakeAllocatorTraits<ValueAllocatorType>;

public:
    constexpr SlowQueue( std::size_t InSize, BlockManagerType& InBlockManager ) : IndexEntryArrayAllocatorPair( InBlockManager, ValueInitTag{} ) {
        std::size_t InitialSize = CeilToPow2( InSize ) >> 1;
        if ( InitialSize < 2 ) {
            InitialSize = 2;
        }

        IndexEntriesSize() = InitialSize;
        CreateNewBlockIndexArray();
    }

    HAKLE_CPP20_CONSTEXPR ~SlowQueue() override {
        std::size_t Index = this->HeadIndex.load( std::memory_order_relaxed );
        std::size_t Tail  = this->TailIndex.load( std::memory_order_relaxed );

        // Release all block
        BlockType* Block = nullptr;
        while ( Index != Tail ) {
            std::size_t InnerIndex = Index & ( BlockSize - 1 );
            if ( InnerIndex == 0 || Block == nullptr ) {
                Block = GetBlockIndexEntryForIndex( Index )->Value.load( std::memory_order_relaxed );
            }
            ValueAllocatorTraits::Destroy( ValueAllocator(), ( *Block )[ InnerIndex ] );
            if ( InnerIndex == BlockSize - 1 ) {
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
                IndexEntryPointerAllocatorTraits::Deallocate( IndexEntryPointerAllocator(), CurrentArray->Index );
                IndexEntryAllocatorTraits::Deallocate( IndexEntryAllocator(), CurrentArray->Entries );
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
        if ( InnerIndex == 0 ) {
            // TODO: add MAX_SIZE check
            if ( !CircularLessThan( this->HeadIndex.load( std::memory_order_relaxed ), CurrentTailIndex + BlockSize ) ) {
                return false;
            }

            IndexEntry* NewIndexEntry = nullptr;
            if ( !InsertBlockIndexEntry<Mode>( NewIndexEntry, CurrentTailIndex ) ) {
                return false;
            }

            BlockType* NewBlock = BlockManager().RequisitionBlock( Mode );
            if ( NewBlock == nullptr ) {
                RewindBlockIndexTail();
                NewIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                return false;
            }

            NewBlock->Reset();

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<ValueType, Args&&...>::value ) {
                HAKLE_TRY { ValueAllocatorTraits::Construct( ValueAllocator(), ( *NewBlock )[ InnerIndex ], std::forward<Args>( args )... ); }
                HAKLE_CATCH( ... ) {
                    RewindBlockIndexTail();
                    NewIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                    BlockManager().ReturnBlock( NewBlock );
                    HAKLE_RETHROW;
                }
            }

            NewIndexEntry->Value.store( NewBlock, std::memory_order_relaxed );

            this->TailBlock = NewBlock;

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<ValueType, Args&&...>::value ) {
                this->TailIndex.store( NewTailIndex, std::memory_order_release );
                return true;
            }
        }

        ValueAllocatorTraits::Construct( ValueAllocator(), ( *this->TailBlock )[ InnerIndex ], std::forward<Args>( args )... );
        this->TailIndex.store( NewTailIndex, std::memory_order_release );
        return true;
    }

    template <class U>
    constexpr bool Dequeue( U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), ValueType&&> ) {
        std::size_t FailedCount = this->DequeueFailedCount.load( std::memory_order_relaxed );
        if ( HAKLE_LIKELY( CircularLessThan( this->DequeueAttemptsCount.load( std::memory_order_relaxed ) - FailedCount,
                                             this->TailIndex.load( std::memory_order_relaxed ) ) ) ) {
            // TODO: understand this
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t AttemptsCount = this->DequeueAttemptsCount.fetch_add( 1, std::memory_order_relaxed );
            if ( HAKLE_LIKELY( CircularLessThan( AttemptsCount - FailedCount, this->TailIndex.load( std::memory_order_acquire ) ) ) ) {
                std::size_t Index      = this->HeadIndex.fetch_add( 1, std::memory_order_relaxed );
                std::size_t InnerIndex = Index & ( BlockSize - 1 );

                IndexEntry* Entry = GetBlockIndexEntryForIndex( Index );
                BlockType*  Block = Entry->Value.load( std::memory_order_relaxed );
                ValueType&  Value = *( *Block )[ InnerIndex ];

                HAKLE_CONSTEXPR_IF( !std::is_nothrow_assignable<U, ValueType>::value ) {
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
                    } guard{ .Entry = Entry, .Block = Block, .BlockManager = BlockManager(), .ValueAllocatorPair = { InnerIndex, ValueAllocator() } };

                    Element = std::move( Value );
                }
                else {
                    Element = std::move( Value );
                    ValueAllocatorTraits::Destroy( ValueAllocator(), &Value );
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
        if ( LocalIndexEntriesArray == nullptr ) {
            return false;
        }

        std::size_t NewTail = ( LocalIndexEntriesArray->Tail.load( std::memory_order_relaxed ) + 1 ) & ( LocalIndexEntriesArray->Size - 1 );
        IdxEntry            = LocalIndexEntriesArray->Index[ NewTail ];
        if ( IdxEntry->Key.load( std::memory_order_relaxed ) == INVALID_BLOCK_BASE || IdxEntry->Value.load( std::memory_order_relaxed ) == nullptr ) {
            IdxEntry->Key.store( BlockStartIndex, std::memory_order_relaxed );
            LocalIndexEntriesArray->Tail.store( NewTail, std::memory_order_relaxed );
            return true;
        }

        if constexpr ( Mode == AllocMode::CannotAlloc ) {
            return false;
        }
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
        LocalBlockEntryArray->Tail.store( ( LocalBlockEntryArray->Tail.load( std::memory_order_relaxed ) - 1 ) & ( LocalBlockEntryArray->Size - 1 ),
                                          std::memory_order_relaxed );
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
        std::size_t Offset     = ( (Index & ~( BlockSize - 1 )) - TailBase ) >> BlockSizeLog2;
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

        if ( Prev != nullptr ) {
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
    CompressPair<std::size_t, CompressPair<ValueAllocatorType, IndexEntryPointerAllocatorType>> ValueAndIndexEntryPointerAllocatorPair{};

    constexpr IndexEntryAllocatorType&        IndexEntryAllocator() noexcept { return IndexEntryAllocatorPair.Second(); }
    constexpr IndexEntryArrayAllocatorType&   IndexEntryArrayAllocator() noexcept { return IndexEntryArrayAllocatorPair.Second(); }
    constexpr IndexEntryPointerAllocatorType& IndexEntryPointerAllocator() noexcept {
        return ValueAndIndexEntryPointerAllocatorPair.Second().Second();
    }
    constexpr ValueAllocatorType& ValueAllocator() noexcept { return ValueAndIndexEntryPointerAllocatorPair.Second().First(); }

    constexpr const IndexEntryAllocatorType&        IndexEntryAllocator() const noexcept { return IndexEntryAllocatorPair.Second(); }
    constexpr const IndexEntryArrayAllocatorType&   IndexEntryArrayAllocator() const noexcept { return IndexEntryArrayAllocatorPair.Second(); }
    constexpr const IndexEntryPointerAllocatorType& IndexEntryPointerAllocator() const noexcept {
        return ValueAndIndexEntryPointerAllocatorPair.Second().Second();
    }
    constexpr const ValueAllocatorType& ValueAllocator() const noexcept { return ValueAndIndexEntryPointerAllocatorPair.Second().First(); }

    constexpr std::atomic<IndexEntryArray*>& CurrentIndexEntryArray() noexcept { return IndexEntryAllocatorPair.First(); }
    constexpr BlockManagerType&              BlockManager() noexcept { return IndexEntryArrayAllocatorPair.First(); }
    constexpr std::size_t&                   IndexEntriesSize() noexcept { return ValueAndIndexEntryPointerAllocatorPair.First(); }

    constexpr const std::atomic<IndexEntryArray*>& CurrentIndexEntryArray() const noexcept { return IndexEntryAllocatorPair.First(); }
    constexpr const BlockManagerType&              BlockManager() const noexcept { return IndexEntryArrayAllocatorPair.First(); }
    HAKLE_NODISCARD constexpr const std::size_t&   IndexEntriesSize() const noexcept { return ValueAndIndexEntryPointerAllocatorPair.First(); }
};

}  // namespace hakle

#endif  // CONCURRENTQUEUE_H
