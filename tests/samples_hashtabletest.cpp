//
// Created by wwjszz on 25-11-19.
//

#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "../Hash/HashTable.h"
#include "gtest/gtest.h"

namespace hakle {

class HashTableTest : public ::testing::Test {
protected:
    static constexpr std::size_t MAP_SIZE = 8192;
    samples::HashTable<MAP_SIZE> map;
};

// Basic functionality tests
TEST_F( HashTableTest, BasicSetAndGet ) {
    map.SetItem( 1, 100 );
    EXPECT_EQ( map.GetItem( 1 ), 100 );

    map.SetItem( 2, 200 );
    EXPECT_EQ( map.GetItem( 2 ), 200 );

    // First key-value pair should still exist
    EXPECT_EQ( map.GetItem( 1 ), 100 );
}

TEST_F( HashTableTest, GetNonExistentKey ) { EXPECT_EQ( map.GetItem( 999 ), 0 ); }

TEST_F( HashTableTest, UpdateExistingKey ) {
    map.SetItem( 1, 100 );
    EXPECT_EQ( map.GetItem( 1 ), 100 );

    map.SetItem( 1, 200 );
    EXPECT_EQ( map.GetItem( 1 ), 200 );
}

TEST_F( HashTableTest, MultipleItems ) {
    for ( int i = 1; i <= 10; ++i ) {
        map.SetItem( i, i * 100 );
    }

    for ( int i = 1; i <= 10; ++i ) {
        EXPECT_EQ( map.GetItem( i ), i * 100 );
    }
}

// Concurrent tests
TEST_F( HashTableTest, ConcurrentSetDifferentKeys ) {
    constexpr int NUM_THREADS      = 4;
    constexpr int ITEMS_PER_THREAD = 20;

    std::vector<std::thread> threads;

    for ( int t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( [ this, t ]() {
            int base = t * ITEMS_PER_THREAD + 1;
            for ( int i = 0; i < ITEMS_PER_THREAD; ++i ) {
                int key   = base + i;
                int value = key * 100;
                map.SetItem( key, value );
            }
        } );
    }

    for ( auto& thread : threads ) {
        thread.join();
    }

    // Verify all items
    for ( int t = 0; t < NUM_THREADS; ++t ) {
        int base = t * ITEMS_PER_THREAD + 1;
        for ( int i = 0; i < ITEMS_PER_THREAD; ++i ) {
            int key   = base + i;
            int value = key * 100;
            EXPECT_EQ( map.GetItem( key ), value );
        }
    }
}

TEST_F( HashTableTest, ConcurrentSetSameKey ) {
    constexpr int    NUM_THREADS = 8;
    constexpr int    KEY         = 42;
    std::atomic<int> successCount{ 0 };

    std::vector<std::thread> threads;

    for ( int t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( [ this, t, &successCount ]() {
            int value = ( t + 1 ) * 100;
            map.SetItem( KEY, value );
            successCount++;
        } );
    }

    for ( auto& thread : threads ) {
        thread.join();
    }

    EXPECT_EQ( successCount, NUM_THREADS );

    // Key should exist, value should be set by one of the threads
    int result = map.GetItem( KEY );
    EXPECT_NE( result, 0 );

    // Verify the value is valid (one of the values set by threads)
    bool validValue = false;
    for ( int t = 0; t < NUM_THREADS; ++t ) {
        if ( result == ( t + 1 ) * 100 ) {
            validValue = true;
            break;
        }
    }
    EXPECT_TRUE( validValue );
}

TEST_F( HashTableTest, ConcurrentSetAndGet ) {
    constexpr int NUM_WRITER_THREADS = 4;
    constexpr int NUM_READER_THREADS = 4;
    constexpr int ITEMS_PER_WRITER   = 10;

    std::atomic<bool>        startFlag{ false };
    std::vector<std::thread> threads;

    // Writer threads
    for ( int t = 0; t < NUM_WRITER_THREADS; ++t ) {
        threads.emplace_back( [ this, t, &startFlag ]() {
            while ( !startFlag.load() ) {
                std::this_thread::yield();
            }

            int base = t * ITEMS_PER_WRITER + 1;
            for ( int i = 0; i < ITEMS_PER_WRITER; ++i ) {
                int key   = base + i;
                int value = key * 100;
                map.SetItem( key, value );
            }
        } );
    }

    // Reader threads
    for ( int t = 0; t < NUM_READER_THREADS; ++t ) {
        threads.emplace_back( [ this, &startFlag ]() {
            while ( !startFlag.load() ) {
                std::this_thread::yield();
            }

            for ( int i = 0; i < 1000; ++i ) {
                int key   = ( i % ( NUM_WRITER_THREADS * ITEMS_PER_WRITER ) ) + 1;
                int value = map.GetItem( key );

                // If value is found, verify it's correct
                if ( value != 0 ) {
                    EXPECT_EQ( value, key * 100 );
                }
            }
        } );
    }

    startFlag.store( true );

    for ( auto& thread : threads ) {
        thread.join();
    }

    // Final verification of all written data
    for ( int t = 0; t < NUM_WRITER_THREADS; ++t ) {
        int base = t * ITEMS_PER_WRITER + 1;
        for ( int i = 0; i < ITEMS_PER_WRITER; ++i ) {
            int key = base + i;
            EXPECT_EQ( map.GetItem( key ), key * 100 );
        }
    }
}

TEST_F( HashTableTest, StressTest ) {
    constexpr int NUM_THREADS           = 8;
    constexpr int OPERATIONS_PER_THREAD = 100;

    std::vector<std::thread> threads;
    std::atomic<int>         errorCount{ 0 };

    for ( int t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( [ this, t, &errorCount ]() {
            std::mt19937                       rng( t );
            std::uniform_int_distribution<int> keyDist( ( t + 1 ) * 10, ( t + 1 ) * 10 + 10 );

            for ( int i = 0; i < OPERATIONS_PER_THREAD; ++i ) {
                int key   = keyDist( rng );
                int value = key;

                map.SetItem( key, value );

                int retrieved = map.GetItem( key );
                // Value should be 0 or a valid value
                if ( retrieved != 0 && retrieved != key ) {
                    errorCount++;
                }
            }
        } );
    }

    for ( auto& thread : threads ) {
        thread.join();
    }

    EXPECT_EQ( errorCount, 0 );
}

// Boundary tests
TEST_F( HashTableTest, FillToCapacity ) {
    for ( std::size_t i = 1; i <= MAP_SIZE; ++i ) {
        map.SetItem( static_cast<int>( i ), static_cast<int>( i * 10 ) );
    }

    for ( std::size_t i = 1; i <= MAP_SIZE; ++i ) {
        EXPECT_EQ( map.GetItem( static_cast<int>( i ) ), static_cast<int>( i * 10 ) );
    }
}

// Performance benchmark test (optional)
TEST_F( HashTableTest, PerformanceBenchmark ) {
    constexpr int NUM_THREADS      = 4;
    constexpr int ITEMS_PER_THREAD = 2000;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for ( int t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( [ this, t ]() {
            int base = t * ITEMS_PER_THREAD + 1;

            // Write
            for ( int i = 0; i < ITEMS_PER_THREAD; ++i ) {
                int key = base + i;
                map.SetItem( key, key * 100 );
            }

            // Read and verify
            for ( int i = 0; i < ITEMS_PER_THREAD; ++i ) {
                int key   = base + i;
                int value = map.GetItem( key );
                if ( value != key * 100 ) {
                    std::cerr << value << " " << key * 100 << " " << "Verification failed!" << std::endl;
                }
            }
        } );
    }

    for ( auto& thread : threads ) {
        thread.join();
    }

    auto end      = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( end - start );

    std::cout << "Performance test completed in " << duration.count() << "ms" << std::endl;
}

}  // namespace hakle