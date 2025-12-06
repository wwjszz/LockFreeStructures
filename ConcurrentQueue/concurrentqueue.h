//
// Created by admin on 25-12-1.
//

#ifndef CONCURRENTQUEUE_H
#define CONCURRENTQUEUE_H

#include "BlockPool.h"
#include "common/CompressPair.h"
#include "common/common.h"
#include "common/utility.h"

#include <atomic>
#include <type_traits>

namespace hakle {

struct BlockCheckPolicy {
    virtual constexpr ~BlockCheckPolicy() = default;

    [[nodiscard]] virtual bool IsEmpty() const                                      = 0;
    virtual constexpr bool     SetEmpty( std::size_t Index )                        = 0;
    virtual constexpr bool     SetSomeEmpty( std::size_t Index, std::size_t Count ) = 0;
    virtual constexpr void     SetAllEmpty()                                        = 0;
    virtual constexpr void     Reset()                                              = 0;
};

template <std::size_t BLOCK_SIZE>
struct FlagsCheckPolicy : BlockCheckPolicy {
    constexpr ~FlagsCheckPolicy() override = default;

    [[nodiscard]] constexpr bool IsEmpty() const override {
        for ( auto& Flag : Flags ) {
            if ( !Flag.load( std::memory_order_relaxed ) ) {
                return false;
            }
        }

        std::atomic_thread_fence( std::memory_order_acquire );
        return true;
    }

    constexpr bool SetEmpty( std::size_t Index ) override {
        Flags[ Index ].store( 1, std::memory_order_release );
        return false;
    }

    constexpr bool SetSomeEmpty( std::size_t Index, std::size_t Count ) override {
        std::atomic_thread_fence( std::memory_order_release );

        for ( std::size_t i = 0; i < Count; ++i ) {
            Flags[ Index + i ].store( 1, std::memory_order_relaxed );
        }
        return false;
    }

    constexpr void SetAllEmpty() override {
        for ( std::size_t i = 0; i < BLOCK_SIZE; ++i ) {
            Flags[ i ].store( 1, std::memory_order_relaxed );
        }
    }

    constexpr void Reset() override {
        for ( auto& Flag : Flags ) {
            Flag.store( 0, std::memory_order_relaxed );
        }
    }

    std::array<std::atomic<uint8_t>, BLOCK_SIZE> Flags;
};

template <std::size_t BLOCK_SIZE>
struct CounterCheckPolicy : BlockCheckPolicy {
    constexpr ~CounterCheckPolicy() override = default;

    [[nodiscard]] constexpr bool IsEmpty() const override {
        if ( Counter.load( std::memory_order_relaxed ) == BLOCK_SIZE ) {
            std::atomic_thread_fence( std::memory_order_acquire );
            return true;
        }
        return false;
    }

    // Increments the counter and returns true if the block is now empty
    constexpr bool SetEmpty( [[maybe_unused]] std::size_t Index ) override {
        std::size_t OldCounter = Counter.fetch_add( 1, std::memory_order_release );
        return OldCounter + 1 == BLOCK_SIZE;
    }

    constexpr bool SetSomeEmpty( [[maybe_unused]] std::size_t Index, std::size_t Count ) override {
        std::size_t OldCounter = Counter.fetch_add( Count, std::memory_order_release );
        return OldCounter + Count == BLOCK_SIZE;
    }

    constexpr void SetAllEmpty() override { Counter.store( BLOCK_SIZE, std::memory_order_relaxed ); }

    constexpr void Reset() override { Counter.store( 0, std::memory_order_relaxed ); }

    std::atomic<std::size_t> Counter;
};

enum class BlockMethod { Flags, Counter };

template <class T, std::size_t BLOCK_SIZE, class Policy>
struct HakleBlock : FreeListNode<HakleBlock<T, BLOCK_SIZE, Policy>>, Policy {
    using ValueType                        = T;
    using BlockType                        = HakleBlock;
    constexpr static std::size_t BlockSize = BLOCK_SIZE;

    constexpr T*       operator[]( std::size_t Index ) noexcept { return reinterpret_cast<T*>( Elements ) + Index; }
    constexpr const T* operator[]( std::size_t Index ) const noexcept { return reinterpret_cast<T*>( Elements ) + Index; }

    alignas( T ) std::byte Elements[ sizeof( T ) * BLOCK_SIZE ]{};
    HakleBlock* Next{ nullptr };
};

template <class T, std::size_t BLOCK_SIZE>
using HakleFlagsBlock = HakleBlock<T, BLOCK_SIZE, FlagsCheckPolicy<BLOCK_SIZE>>;

template <class T, std::size_t BLOCK_SIZE>
using HakleCounterBlock = HakleBlock<T, BLOCK_SIZE, CounterCheckPolicy<BLOCK_SIZE>>;

// TODO: manager traits
template <class BLOCK_TYPE, class BLOCK_MANAGER_TYPE>
struct QueueBase {
public:
    using BlockManagerType                 = BLOCK_MANAGER_TYPE;
    using BlockType                        = BLOCK_TYPE;
    using ValueType                        = typename BlockManagerType::ValueType;
    constexpr static std::size_t BlockSize = BlockManagerType::BlockSize;

    using BlockAllocatorTraits = typename BlockManagerType::BlockAllocatorTraits;
    using BlockAllocatorType   = typename BlockManagerType::AllocatorType;

    using AllocMode = typename BlockManagerType::AllocMode;

    virtual constexpr ~QueueBase() = default;

    [[nodiscard]] constexpr std::size_t Size() const noexcept {
        std::size_t Tail = TailIndex.load( std::memory_order_relaxed );
        std::size_t Head = HeadIndex.load( std::memory_order_relaxed );
        return CircularLessThan( Head, Tail ) ? Tail - Head : 0;
    }

    [[nodiscard]] constexpr std::size_t GetTail() const noexcept { return TailIndex.load( std::memory_order_relaxed ); }

protected:
    std::atomic<std::size_t> HeadIndex{};
    std::atomic<std::size_t> TailIndex{};
    std::atomic<std::size_t> DequeueAttemptsCount{};
    std::atomic<std::size_t> DequeueFailedCount{};
    BlockType*               TailBlock{ nullptr };
};

// SPMC Queue
template <class BLOCK_TYPE, class BLOCK_MANAGER_TYPE = HakleBlockManager<BLOCK_TYPE>>
class ExplicitQueue : public QueueBase<BLOCK_TYPE, BLOCK_MANAGER_TYPE> {
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
    constexpr static std::size_t BlockSizeLog2 = BitWidth( BlockSize );

    struct IndexEntry;
    struct IndexEntryArray;
    using IndexEntryAllocatorType        = typename BlockAllocatorTraits::template RebindAlloc<IndexEntry>;
    using IndexEntryArrayAllocatorType   = typename BlockAllocatorTraits::template RebindAlloc<IndexEntryArray>;
    using ValueAllocatorType             = typename BlockAllocatorTraits::template RebindAlloc<ValueType>;
    using IndexEntryAllocatorTraits      = HakeAllocatorTraits<IndexEntryAllocatorType>;
    using IndexEntryArrayAllocatorTraits = HakeAllocatorTraits<IndexEntryArrayAllocatorType>;
    using ValueAllocatorTraits           = HakeAllocatorTraits<ValueAllocatorType>;

public:
    constexpr explicit ExplicitQueue( std::size_t InSize, BLOCK_MANAGER_TYPE& InBlockManager ) : BlockManager( InBlockManager ) {
        std::size_t InitialSize = CeilToPow2( InSize );
        if ( InitialSize < 2 ) {
            InitialSize = 2;
        }
        PO_IndexEntriesSize() = InitialSize;

        CreateNewBlockIndexArray( 0 );
    }

    constexpr ~ExplicitQueue() override {
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
    constexpr bool Enqueue( Args&&... args ) {
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
                // TODO: add a overflow check and MAX_SIZE check
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

            // TODO: we need an option to indicate whether to enable exceptions
            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<ValueType, Args&&...>::value ) {
                // we need to handle exception here
                HAKLE_TRY {
                    ValueAllocatorType::Construct( ValueAllocator(), ( *( this->TailBlock ) )[ InnerIndex ], std::forward<Args>( args )... );
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

    // Dequeue
    template <class U>
        requires std::is_assignable_v<U&, ValueType&&>
    constexpr bool Dequeue( U& Element ) {
        std::size_t FailedCount = this->DequeueFailedCount.load( std::memory_order_relaxed );
        if ( HAKLE_LIKELY( CircularLessThan( this->DequeueAttemptsCount.load( std::memory_order_relaxed ) - FailedCount,
                                             this->TailIndex.load( std::memory_order_relaxed ) ) ) ) {
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t AttemptsCount = this->DequeueAttemptsCount.fetch_add( 1, std::memory_order_relaxed );
            if ( HAKLE_LIKELY( CircularLessThan( AttemptsCount - FailedCount, this->TailIndex.load( std::memory_order_acquire ) ) ) ) {
                // NOTE: getting headIndex must be front of getting CurrentIndexEntryArray
                // if getting CurrentIndexEntryArray first, there is a situation that makes FirstBlockIndexBase larger than IndexEntryTailBase
                // TODO: may be should be acq_rel?
                std::size_t Index      = this->HeadIndex.fetch_add( 1, std::memory_order_relaxed );
                std::size_t InnerIndex = Index & ( BlockSize - 1 );

                // we can dequeue
                IndexEntryArray* LocalIndexEntryArray = this->CurrentIndexEntryArray.load( std::memory_order_acquire );
                std::size_t      LocalIndexEntryIndex = LocalIndexEntryArray->Tail.load( std::memory_order_acquire );

                std::size_t IndexEntryTailBase  = LocalIndexEntryArray->Entries[ LocalIndexEntryIndex ].Base;
                std::size_t FirstBlockIndexBase = Index & ~( BlockSize - 1 );
                std::size_t Offset              = ( FirstBlockIndexBase - IndexEntryTailBase ) >> ( BlockSizeLog2 - 1 );
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
                    } guard{ DequeueBlock, { InnerIndex, ValueAllocator() } };

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

    constexpr bool CreateNewBlockIndexArray( std::size_t FilledSlot ) noexcept {
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

    [[nodiscard]] constexpr const std::size_t& PO_IndexEntriesUsed() const noexcept { return IndexEntryAllocatorPair.First(); }
    [[nodiscard]] constexpr const std::size_t& PO_IndexEntriesSize() const noexcept { return IndexEntryArrayAllocatorPair.First(); }
    [[nodiscard]] constexpr const std::size_t& PO_NextIndexEntry() const noexcept { return ValueAllocatorPair.First(); }

    IndexEntry* PO_PrevEntries{ nullptr };
};

}  // namespace hakle

#endif  // CONCURRENTQUEUE_H
