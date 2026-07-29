# LockFreeStructures

`LockFreeStructures` 是一个 C++20、header-only 的并发数据结构项目。当前主入口是
`hakle::ConcurrentQueue<T>`：它参考 moodycamel::ConcurrentQueue 的分生产者队列设计，在其上重新组织了
队列封装、Block/BlockManager、HashTable 和可替换 allocator/traits。

本轮工程化重构调整了构建、测试、benchmark、文档和许可声明，并修复 move assignment 的资源释放顺序及
默认 traits 的 allocator 传播。

## 主要能力

- 多生产者、多消费者（MPMC）并发入队和出队。
- implicit producer 与显式 `ProducerToken` / `ConsumerToken` 两套接口。
- 单元素和 bulk 入队/出队。
- 支持 move-only、非平凡析构类型。
- allocator、Block 和 BlockManager 可通过模板及 traits 定制。
- header-only，不需要单独编译库文件。

队列延续 moodycamel 类设计的语义：保证单个 producer 内的 FIFO；多个 producer 之间没有一个统一的全局
FIFO 顺序。Token 只能与创建它的队列配合使用，同一个 producer token 不应被多个生产线程同时操作。

## 基本使用

```cpp
#include <bit> // 当前公开头使用 std::has_single_bit，需由调用方先包含。
#include "ConcurrentQueue/ConcurrentQueue.h"

hakle::ConcurrentQueue<int> queue;
queue.Enqueue(42);

int value = 0;
if (queue.TryDequeue(value)) {
  // value == 42
}
```

显式 token：

```cpp
#include <bit>
#include "ConcurrentQueue/ConcurrentQueue.h"

hakle::ConcurrentQueue<int> queue;
auto producer = queue.GetProducerToken();
auto consumer = queue.GetConsumerToken();

queue.EnqueueWithToken(producer, 7);

int value = 0;
queue.TryDequeue(consumer, value);
```

## 构建与测试

已在 Windows 11、Visual Studio 2022、MSVC 19.44、CMake 3.31 上实际编译和运行。CMake 会为 MSVC
启用标准预处理器模式，因为项目宏使用了 `__VA_OPT__`。

```powershell
cmake -S . -B build -A x64 `
  -DBUILD_TESTING=ON `
  -DLOCKFREESTRUCTURES_BUILD_BENCHMARKS=OFF
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

测试结构与 `co_mira` 一致：`test/test_*.cc` 中每个文件生成一个独立可执行文件，由轻量 `CHECK`
测试框架输出 `[PASS]` / `[FAIL]`，再注册到 CTest。默认套件包含：

- 基础 API、implicit/explicit token、bulk、move-only、move construction、move assignment 和 swap；
- implicit/token/bulk MPMC，逐值去重校验及逐 producer FIFO；
- 非平凡对象生命周期、构造异常恢复、默认/自定义 traits 的 allocator 分配与构造平衡。

## Benchmark

benchmark 是可选目标，比较相同 MPMC workload 下的：

- `hakle::ConcurrentQueue`：implicit 与 token；
- moodycamel::ConcurrentQueue：implicit、token 和 token bulk；
- Boost.Lockfree `queue`；
- oneTBB `concurrent_queue`；
- `std::queue + std::mutex` 基线；
- Hakle token bulk。

Google Benchmark 和 oneTBB 会优先使用 `thirdparty/` 下已有源码，缺失时由 FetchContent 下载固定版本。
Boost.Lockfree 需要本机 Boost headers；可通过 `BOOST_ROOT` 指定。仓库当前的 `thirdparty/boost_1_90_0`
也会被自动识别。

```powershell
cmake -S . -B build -A x64 `
  -DBUILD_TESTING=ON `
  -DLOCKFREESTRUCTURES_BUILD_BENCHMARKS=ON `
  -DBOOST_ROOT=C:\path\to\boost
cmake --build build --config Release --target queue_benchmark --parallel
.\build\benchmark\bin\queue_benchmark.exe
```

只跑一组 smoke benchmark：

```powershell
.\build\benchmark\bin\queue_benchmark.exe `
  '--benchmark_filter=.*producers:1/consumers:1.*'
```

计时规则：队列构造、线程创建和队列析构位于暂停计时区；所有 worker 就绪后用 latch 同时起跑；报告
wall-clock real time 和 `items_per_second`。每轮结束都会校验消费总数与 checksum，队列默认按各自增长策略
运行，不为某一个实现额外预分配完整 workload。场景为 1P/1C、2P/2C、4P/4C、8P/8C、12P/12C、
16P/16C、20P/20C 和 24P/24C；benchmark 不注册到 CTest，避免常规测试被性能任务拖慢。

### Windows 实测结果

以下数据在 Windows、MSVC 19.44、Release 配置下于 2026-07-29 重新完整实测，运行主机为 32 个逻辑处理器，Google Benchmark 的 `MinTime` 为 0.25 秒。横坐标为从 1P/1C 到 24P/24C 的生产者/消费者线程配置，纵坐标为吞吐量（百万 items/s）；每种队列实现或调用模式对应一条折线，名称统一标注在右侧图例中。由于 bulk 吞吐量比单元素操作高约两个数量级，为了把所有队列保留在同一张图中且不压扁单元素曲线，纵轴使用对数刻度。

![Queue throughput benchmark](docs/benchmark-throughput.svg)

完整原始数据保存在 `benchmark/results/windows-msvc-19.44-2026-07-29.json`，图表不在折线上标注具体数值，由 Python/matplotlib 脚本生成：

```powershell
python -m pip install matplotlib
python benchmark/plot_benchmark.py `
  benchmark/results/windows-msvc-19.44-2026-07-29.json `
  --output docs/benchmark-throughput.svg
```

## 自定义 allocator

allocator 不是 `std::allocator_traits` 接口，而是项目自己的 `HakeAllocatorTraits` 协议。allocator 需要提供
`ValueType`、指针/引用/size 类型、`Allocate`、`Deallocate`、`Construct`、`Destroy`，并支持 rebind。
完整示例见 `test/test_allocator_and_lifetime.cc`。

默认 queue traits 会把重绑定后的 allocator 继续传给 explicit/implicit block manager，因此 block pool 和
free list 也使用同一 allocator 体系。自定义 block 或 manager 时仍可提供自定义 queue traits。

## 许可

项目代码使用 Apache License 2.0。设计来源与 benchmark vendored 依赖的归属见 [NOTICE](NOTICE) 和
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。moodycamel 原始 header 保留其上游许可证头。
