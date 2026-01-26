//
// Created by admin on 2026/1/22.
//

#include "ConcurrentQueue/ConcurrentQueue.h"

#include <ios>
#include <iostream>
#include <thread>
#include <vector>

constexpr std::size_t LOOP_SIZE    = 10;
constexpr std::size_t THREAD_SIZE  = 30;
constexpr std::size_t ELEMENT_SIZE = 100000;

int main() {
    hakle::ConcurrentQueue<int> queue;

    std::vector<hakle::ConcurrentQueue<int>::ProducerToken> producer_tokens;

    for ( std::size_t i = 0; i < THREAD_SIZE; ++i ) {
        producer_tokens.emplace_back( queue.GetProducerToken() );
    }

    // 0-999
    std::vector<std::thread> inputs;
    for ( int i = 0; i < THREAD_SIZE; i++ ) {
        inputs.emplace_back( [ &queue, i, &producer_tokens ]() {
            for ( int j = 0; j < ELEMENT_SIZE; j++ ) {
                // queue.Enqueue( i * ELEMENT_SIZE + j );
                queue.Enqueue( producer_tokens[ i ], i * ELEMENT_SIZE + j );
            }
        } );

        inputs.emplace_back( [ &queue, i, &producer_tokens ]() {
            for ( int j = 0; j < ELEMENT_SIZE; j++ ) {
                queue.Enqueue( i * ELEMENT_SIZE + j );
                // queue.Enqueue( producer_tokens[ i ], i * ELEMENT_SIZE + j );
            }
        } );
    }

    std::atomic<std::size_t> count       = 0;
    std::size_t              total_count = THREAD_SIZE * 2 * ELEMENT_SIZE;

    std::atomic<std::size_t> sum = 0;
    std::vector<std::thread> outputs;
    for ( int i = 0; i < THREAD_SIZE; i++ ) {
        outputs.emplace_back( [ &queue, i, &sum, &producer_tokens, &count, &total_count ]() {
            int value;
            while ( count < total_count ) {
                if ( queue.TryDequeue( value ) ) {
                    ++count;
                    sum += value;
                }
            }
        } );

        outputs.emplace_back( [ &queue, i, &sum, &producer_tokens, &count, &total_count ]() {
            int value;
            while ( count < total_count ) {
                if ( queue.TryDequeueFromProducer( producer_tokens[ i ], value ) ) {
                    ++count;
                    sum += value;
                }
            }
        } );
    }

    for ( auto& t : outputs ) {
        t.join();
    }

    for ( auto& t : inputs ) {
        t.join();
    }

    std::size_t expected = ( ELEMENT_SIZE * THREAD_SIZE - 1 ) * ELEMENT_SIZE * THREAD_SIZE;

    std::cout << "sum: " << sum << std::endl;
    std::cout << "expected: " << expected << std::endl;
    int value;
    std::cout << std::boolalpha << "result= " << ( expected == sum ) << std::endl;
    std::cout << "has_remain_element= " << queue.TryDequeue( value ) << std::endl;

    return 0;
}