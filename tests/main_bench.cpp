#include "ConcurrentQueue/ConcurrentQueue.h"
#include "concurrentqueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#define USE_MY 1

// 基本配置：可以改成 benchmark 参数
constexpr std::size_t kProdThreads  = 20;
constexpr std::size_t kConsThreads  = 20;
constexpr std::size_t kItemsPerProd = 100000;
constexpr std::size_t kBulkSize     = 256;

// 一个小工具：把一次 run 中处理的总元素数写入 counters，方便看吞吐

#if USE_MY

// 1. 普通 Enqueue / TryDequeue
static void BM_CQ_NormalEnqDeq(benchmark::State& state)
{
    for (auto _ : state) {
        hakle::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                for (std::size_t i = 0; i < kItemsPerProd; ++i) {
                    int v = static_cast<int>(p * kItemsPerProd + i);
                    queue.Enqueue(v);
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                int value;
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    if (queue.TryDequeue(value)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

// 只跑一次，避免重复构造巨大 workload
        
    }
}
BENCHMARK(BM_CQ_NormalEnqDeq)->MeasureProcessCPUTime();

// 2. 普通 Bulk Enqueue / TryDequeueBulk
static void BM_CQ_BulkEnqDeq(benchmark::State& state)
{
    for (auto _ : state) {
        hakle::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                std::vector<int> buf(kBulkSize);
                std::size_t sent = 0;
                while (sent < kItemsPerProd) {
                    std::size_t n = std::min(kBulkSize, kItemsPerProd - sent);
                    for (std::size_t i = 0; i < n; ++i) {
                        buf[i] = static_cast<int>(p * kItemsPerProd + sent + i);
                    }
                    queue.EnqueueBulk(buf.data(), n);
                    produced.fetch_add(n, std::memory_order_relaxed);
                    sent += n;
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                std::vector<int> buf(kBulkSize);
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t got = queue.TryDequeueBulk(buf.data(), kBulkSize);
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_CQ_BulkEnqDeq)->MeasureProcessCPUTime();

// 3. ProducerToken + 单元素 Enq / TryDequeueFromProducer
static void BM_CQ_ProdToken_EnqDeq(benchmark::State& state)
{
    for (auto _ : state) {
        hakle::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
        prodTokens.reserve(kProdThreads);
        for (std::size_t i = 0; i < kProdThreads; ++i) {
            prodTokens.emplace_back(queue.GetProducerToken());
        }

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};
        std::atomic<std::size_t> nextProducerForConsumer{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                auto& token = prodTokens[p];
                for (std::size_t i = 0; i < kItemsPerProd; ++i) {
                    int v = static_cast<int>(p * kItemsPerProd + i);
                    queue.EnqueueWithToken(token, v);
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                int value;
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t idx = nextProducerForConsumer.fetch_add(
                        1, std::memory_order_relaxed) % kProdThreads;
                    if (queue.TryDequeueFromProducer(prodTokens[idx], value)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_CQ_ProdToken_EnqDeq)->MeasureProcessCPUTime();

// 4. ProducerToken + Bulk Enq / TryDequeueBulkFromProducer
static void BM_CQ_ProdToken_BulkEnqDeq(benchmark::State& state)
{
    for (auto _ : state) {
        hakle::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
        prodTokens.reserve(kProdThreads);
        for (std::size_t i = 0; i < kProdThreads; ++i) {
            prodTokens.emplace_back(queue.GetProducerToken());
        }

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};
        std::atomic<std::size_t> nextProducerForConsumer{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                auto&            token = prodTokens[p];
                std::vector<int> buf(kBulkSize);
                std::size_t      sent = 0;
                while (sent < kItemsPerProd) {
                    std::size_t n = std::min(kBulkSize, kItemsPerProd - sent);
                    for (std::size_t i = 0; i < n; ++i) {
                        buf[i] = static_cast<int>(p * kItemsPerProd + sent + i);
                    }
                    queue.EnqueueBulk(token, buf.data(), n);
                    produced.fetch_add(n, std::memory_order_relaxed);
                    sent += n;
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                std::vector<int> buf(kBulkSize);
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t idx = nextProducerForConsumer.fetch_add(
                        1, std::memory_order_relaxed) % kProdThreads;
                    std::size_t got =
                        queue.TryDequeueBulkFromProducer(
                            prodTokens[idx], buf.data(), kBulkSize);
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_CQ_ProdToken_BulkEnqDeq)->MeasureProcessCPUTime();

// 5. ProducerToken Enq / ConsumerToken Deq（单元素）
static void BM_CQ_ProdTokenEnq_ConsTokenDeq(benchmark::State& state)
{
    for (auto _ : state) {
        hakle::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
        prodTokens.reserve(kProdThreads);
        for (std::size_t i = 0; i < kProdThreads; ++i) {
            prodTokens.emplace_back(queue.GetProducerToken());
        }

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                auto& token = prodTokens[p];
                for (std::size_t i = 0; i < kItemsPerProd; ++i) {
                    int v = static_cast<int>(p * kItemsPerProd + i);
                    queue.EnqueueWithToken(token, v);
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                typename hakle::ConcurrentQueue<int>::ConsumerToken token(queue);
                int value;
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    if (queue.TryDequeue(token, value)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_CQ_ProdTokenEnq_ConsTokenDeq)->MeasureProcessCPUTime();

// 6. 普通 Enq / ConsumerToken Deq（单元素）
static void BM_CQ_NormalEnq_ConsTokenDeq(benchmark::State& state)
{
    for (auto _ : state) {
        hakle::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                for (std::size_t i = 0; i < kItemsPerProd; ++i) {
                    int v = static_cast<int>(p * kItemsPerProd + i);
                    queue.Enqueue(v);
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                typename hakle::ConcurrentQueue<int>::ConsumerToken token(queue);
                int value;
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    if (queue.TryDequeue(token, value)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_CQ_NormalEnq_ConsTokenDeq)->MeasureProcessCPUTime();

// 7. ProducerToken BulkEnq / ConsumerToken BulkDeq
static void BM_CQ_ProdTokenBulkEnq_ConsTokenBulkDeq(benchmark::State& state)
{
    for (auto _ : state) {
        hakle::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
        prodTokens.reserve(kProdThreads);
        for (std::size_t i = 0; i < kProdThreads; ++i) {
            prodTokens.emplace_back(queue.GetProducerToken());
        }

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                auto&            token = prodTokens[p];
                std::vector<int> buf(kBulkSize);
                std::size_t      sent = 0;
                while (sent < kItemsPerProd) {
                    std::size_t n = std::min(kBulkSize, kItemsPerProd - sent);
                    for (std::size_t i = 0; i < n; ++i) {
                        buf[i] = static_cast<int>(p * kItemsPerProd + sent + i);
                    }
                    queue.EnqueueBulk(token, buf.data(), n);
                    produced.fetch_add(n, std::memory_order_relaxed);
                    sent += n;
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                typename hakle::ConcurrentQueue<int>::ConsumerToken token(queue);
                std::vector<int>                                    buf(kBulkSize);
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t got = queue.TryDequeueBulk(token, buf.data(), kBulkSize);
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_CQ_ProdTokenBulkEnq_ConsTokenBulkDeq)->MeasureProcessCPUTime();

// 8. 普通 BulkEnq / ConsumerToken BulkDeq
static void BM_CQ_NormalBulkEnq_ConsTokenBulkDeq(benchmark::State& state)
{
    for (auto _ : state) {
        hakle::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                std::vector<int> buf(kBulkSize);
                std::size_t      sent = 0;
                while (sent < kItemsPerProd) {
                    std::size_t n = std::min(kBulkSize, kItemsPerProd - sent);
                    for (std::size_t i = 0; i < n; ++i) {
                        buf[i] = static_cast<int>(p * kItemsPerProd + sent + i);
                    }
                    queue.EnqueueBulk(buf.data(), n);
                    produced.fetch_add(n, std::memory_order_relaxed);
                    sent += n;
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                typename hakle::ConcurrentQueue<int>::ConsumerToken token(queue);
                std::vector<int>                                    buf(kBulkSize);
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t got = queue.TryDequeueBulk(token, buf.data(), kBulkSize);
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_CQ_NormalBulkEnq_ConsTokenBulkDeq)->MeasureProcessCPUTime();

#endif // USE_MY

// ---------------- moodycamel 版本，同样模式 ----------------

// 1. 普通 enqueue / try_dequeue
static void BM_MOODY_NormalEnqDeq(benchmark::State& state)
{
    for (auto _ : state) {
        moodycamel::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                for (std::size_t i = 0; i < kItemsPerProd; ++i) {
                    int v = static_cast<int>(p * kItemsPerProd + i);
                    queue.enqueue(v);
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                int value;
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    if (queue.try_dequeue(value)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_MOODY_NormalEnqDeq)->MeasureProcessCPUTime();

// 2. 普通 Bulk enqueue / try_dequeue_bulk
static void BM_MOODY_BulkEnqDeq(benchmark::State& state)
{
    for (auto _ : state) {
        moodycamel::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                std::vector<int> buf(kBulkSize);
                std::size_t      sent = 0;
                while (sent < kItemsPerProd) {
                    std::size_t n = std::min(kBulkSize, kItemsPerProd - sent);
                    for (std::size_t i = 0; i < n; ++i) {
                        buf[i] = static_cast<int>(p * kItemsPerProd + sent + i);
                    }
                    queue.enqueue_bulk(buf.data(), n);
                    produced.fetch_add(n, std::memory_order_relaxed);
                    sent += n;
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                std::vector<int> buf(kBulkSize);
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t got = queue.try_dequeue_bulk(buf.data(), kBulkSize);
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_MOODY_BulkEnqDeq)->MeasureProcessCPUTime();

// 3. ProducerToken + 单元素 Enq / try_dequeue_from_producer
static void BM_MOODY_ProdToken_EnqDeq(benchmark::State& state)
{
    for (auto _ : state) {
        moodycamel::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::vector<moodycamel::ProducerToken> prodTokens;
        prodTokens.reserve(kProdThreads);
        for (std::size_t i = 0; i < kProdThreads; ++i) {
            prodTokens.emplace_back(queue);
        }

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};
        std::atomic<std::size_t> nextProducerForConsumer{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                auto& token = prodTokens[p];
                for (std::size_t i = 0; i < kItemsPerProd; ++i) {
                    int v = static_cast<int>(p * kItemsPerProd + i);
                    queue.enqueue(token, v);
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                int value;
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t idx = nextProducerForConsumer.fetch_add(
                        1, std::memory_order_relaxed) % kProdThreads;
                    if (queue.try_dequeue_from_producer(prodTokens[idx], value)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_MOODY_ProdToken_EnqDeq)->MeasureProcessCPUTime();

// 4. ProducerToken + Bulk Enq / try_dequeue_bulk_from_producer
static void BM_MOODY_ProdToken_BulkEnqDeq(benchmark::State& state)
{
    for (auto _ : state) {
        moodycamel::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::vector<moodycamel::ProducerToken> prodTokens;
        prodTokens.reserve(kProdThreads);
        for (std::size_t i = 0; i < kProdThreads; ++i) {
            prodTokens.emplace_back(queue);
        }

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};
        std::atomic<std::size_t> nextProducerForConsumer{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                auto&            token = prodTokens[p];
                std::vector<int> buf(kBulkSize);
                std::size_t      sent = 0;
                while (sent < kItemsPerProd) {
                    std::size_t n = std::min(kBulkSize, kItemsPerProd - sent);
                    for (std::size_t i = 0; i < n; ++i) {
                        buf[i] = static_cast<int>(p * kItemsPerProd + sent + i);
                    }
                    queue.enqueue_bulk(token, buf.data(), n);
                    produced.fetch_add(n, std::memory_order_relaxed);
                    sent += n;
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                std::vector<int> buf(kBulkSize);
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t idx = nextProducerForConsumer.fetch_add(
                        1, std::memory_order_relaxed) % kProdThreads;
                    std::size_t got =
                        queue.try_dequeue_bulk_from_producer(
                            prodTokens[idx], buf.data(), kBulkSize);
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_MOODY_ProdToken_BulkEnqDeq)->MeasureProcessCPUTime();

// 5. ProducerToken Enq / ConsumerToken Deq（单元素）
static void BM_MOODY_ProdTokenEnq_ConsTokenDeq(benchmark::State& state)
{
    for (auto _ : state) {
        moodycamel::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::vector<moodycamel::ProducerToken> prodTokens;
        prodTokens.reserve(kProdThreads);
        for (std::size_t i = 0; i < kProdThreads; ++i) {
            prodTokens.emplace_back(queue);
        }

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                auto& token = prodTokens[p];
                for (std::size_t i = 0; i < kItemsPerProd; ++i) {
                    int v = static_cast<int>(p * kItemsPerProd + i);
                    queue.enqueue(token, v);
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                moodycamel::ConsumerToken token(queue);
                int value;
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    if (queue.try_dequeue(token, value)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_MOODY_ProdTokenEnq_ConsTokenDeq)->MeasureProcessCPUTime();

// 6. 普通 Enq / ConsumerToken Deq（单元素）
static void BM_MOODY_NormalEnq_ConsTokenDeq(benchmark::State& state)
{
    for (auto _ : state) {
        moodycamel::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                for (std::size_t i = 0; i < kItemsPerProd; ++i) {
                    int v = static_cast<int>(p * kItemsPerProd + i);
                    queue.enqueue(v);
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                moodycamel::ConsumerToken token(queue);
                int value;
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    if (queue.try_dequeue(token, value)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_MOODY_NormalEnq_ConsTokenDeq)->MeasureProcessCPUTime();

// 7. ProducerToken BulkEnq / ConsumerToken BulkDeq
static void BM_MOODY_ProdTokenBulkEnq_ConsTokenBulkDeq(benchmark::State& state)
{
    for (auto _ : state) {
        moodycamel::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::vector<moodycamel::ProducerToken> prodTokens;
        prodTokens.reserve(kProdThreads);
        for (std::size_t i = 0; i < kProdThreads; ++i) {
            prodTokens.emplace_back(queue);
        }

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                auto&            token = prodTokens[p];
                std::vector<int> buf(kBulkSize);
                std::size_t      sent = 0;
                while (sent < kItemsPerProd) {
                    std::size_t n = std::min(kBulkSize, kItemsPerProd - sent);
                    for (std::size_t i = 0; i < n; ++i) {
                        buf[i] = static_cast<int>(p * kItemsPerProd + sent + i);
                    }
                    queue.enqueue_bulk(token, buf.data(), n);
                    produced.fetch_add(n, std::memory_order_relaxed);
                    sent += n;
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                moodycamel::ConsumerToken token(queue);
                std::vector<int>          buf(kBulkSize);
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t got = queue.try_dequeue_bulk(token, buf.data(), kBulkSize);
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_MOODY_ProdTokenBulkEnq_ConsTokenBulkDeq)->MeasureProcessCPUTime();

// 8. 普通 BulkEnq / ConsumerToken BulkDeq
static void BM_MOODY_NormalBulkEnq_ConsTokenBulkDeq(benchmark::State& state)
{
    for (auto _ : state) {
        moodycamel::ConcurrentQueue<int> queue;
        const std::size_t totalItems = kProdThreads * kItemsPerProd;

        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers, consumers;

        // producers
        for (std::size_t p = 0; p < kProdThreads; ++p) {
            producers.emplace_back([&, p] {
                std::vector<int> buf(kBulkSize);
                std::size_t      sent = 0;
                while (sent < kItemsPerProd) {
                    std::size_t n = std::min(kBulkSize, kItemsPerProd - sent);
                    for (std::size_t i = 0; i < n; ++i) {
                        buf[i] = static_cast<int>(p * kItemsPerProd + sent + i);
                    }
                    queue.enqueue_bulk(buf.data(), n);
                    produced.fetch_add(n, std::memory_order_relaxed);
                    sent += n;
                }
            });
        }

        // consumers
        for (std::size_t c = 0; c < kConsThreads; ++c) {
            consumers.emplace_back([&] {
                moodycamel::ConsumerToken token(queue);
                std::vector<int>          buf(kBulkSize);
                while (consumed.load(std::memory_order_relaxed) < totalItems) {
                    std::size_t got = queue.try_dequeue_bulk(token, buf.data(), kBulkSize);
                    if (got > 0) {
                        consumed.fetch_add(got, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

    }
}
BENCHMARK(BM_MOODY_NormalBulkEnq_ConsTokenBulkDeq)->MeasureProcessCPUTime();

// 使用 Google Benchmark 自带的 main
BENCHMARK_MAIN();