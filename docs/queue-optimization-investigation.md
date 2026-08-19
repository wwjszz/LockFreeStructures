# ConcurrentQueue 性能优化调查与实施计划

> 用途：将本项目带到公司环境后，交给 AI 或开发人员继续实施、测试和复核。
>
> 调查日期：2026-08-19

## 1. 当前结论

Hakle `ConcurrentQueue` 当前吞吐已经接近 `moodycamel::ConcurrentQueue`，但现有 benchmark 会掩盖一部分真实差异。代码审查和临时原型显示，最值得优先验证的优化不是提前多分配 Block，而是：

1. 将 producer 遍历辅助函数的 `std::function` 参数改成模板回调。
2. 将 producer 写入的 `TailIndex` 与消费者频繁修改的 dequeue 计数器隔离到不同缓存线。
3. 修正默认 BlockPool 参数的单位；现在很可能把“元素数量”当成了“Block 数量”。
4. 让显式 `ProducerToken` 直接调用 `FastQueue`，跳过已知结果的 producer 类型判断。
5. 修正 benchmark，使队列本身而不是 benchmark 的共享计数器成为主要测量对象。

建议每项优化独立提交、独立测试，不要一次合并全部修改，否则无法确定收益来源。

## 2. 已有基线

项目现有测试：

```text
test_allocator_and_lifetime  passed
test_queue_api               passed
test_queue_concurrent        passed
```

当前构建测试命令：

```bash
cmake --build build --config Release --parallel 8
ctest --test-dir build --output-on-failure
```

现有 Windows benchmark 结果位于：

```text
benchmark/results/windows-msvc-19.44-2026-07-29.json
```

这些结果可以保留为历史基线，但不能单独用于判断小幅优化是否有效，原因见第 3 节。

## 3. 首先修正 benchmark

### 3.1 移除每元素共享 `consumed.fetch_add`

当前 `benchmark/queue_benchmark.cc` 的普通 MPMC 测试中，每成功 dequeue 一个元素都会执行：

```cpp
consumed.fetch_add(1, std::memory_order_relaxed);
```

所有消费者都会争用同一个原子变量。线程数较高时，这个 RMW 会成为 benchmark 自身的瓶颈，使不同队列的结果趋同。

建议：

- 每个消费者只更新自己的本地计数和 checksum。
- 使用 `producers_remaining` 判断生产者是否全部完成。
- 生产者全部完成后，每个消费者继续 drain，直到观察到队列为空。
- 线程 join 后再汇总本地计数，检查总数与 checksum。
- 不要在每次成功 dequeue 后修改全局共享计数。

需要谨慎设计退出条件，确保消费者不会在其他生产者仍可能发布元素时提前退出。

### 3.2 使用相同的预分配规模

benchmark wrapper 当前收到：

```cpp
Queue queue(total, producer_count, consumer_count);
```

但 Hakle 和 moodycamel 的 wrapper 基本忽略了这些参数，最终使用各自默认容量。两者默认容量的语义并不相同，因此比较不完全公平。

必须明确使用以下两种模式分别测试：

1. 相同 Block 数量：用于比较稳定状态下的算法成本。
2. 初始池为 0：用于比较分配、回收和 FreeList 慢路径。

### 3.3 增加测试场景

不要只测试 `N producer + N consumer`。至少增加：

```text
1P 1C
1P 2C / 4C / 8C       测试同一个 producer queue 上的消费者竞争
2P / 4P / 8P + 1C     测试 producer list 扫描
2P 2C / 4P 4C / 8P 8C
producer-only          测试 enqueue 与 implicit producer 查找
prefilled consumer-only
稳定小队列深度         测试 Block 重用和缓存局部性
突发大队列深度         测试索引扩容与动态分配
```

再分别测试：

```text
无 token
ProducerToken
ProducerToken + ConsumerToken
bulk size = 1 / 8 / 32 / 64 / 256
```

建议元素类型至少包含：

```text
uint32_t
64-byte trivial object
非平凡移动/析构对象
```

### 3.4 重复次数与结果输出

建议每项至少运行 7 次，输出 median、标准差和原始 JSON。Google Benchmark 可使用：

```text
--benchmark_repetitions=7
--benchmark_report_aggregates_only=true
--benchmark_out=benchmark-result.json
--benchmark_out_format=json
```

如果机器支持绑核，固定 producer 和 consumer 所在 CPU，并记录：

- CPU 型号和缓存线大小；
- 编译器版本；
- Release 编译选项；
- 是否启用 LTO；
- 系统电源模式；
- 是否有其他高负载任务。

## 4. 优化一：用模板回调替换 `std::function`

### 4.1 当前问题

`ConcurrentQueue/ConcurrentQueue.h` 中以下辅助函数接收 `std::function`：

```cpp
ForEachProducer
ForEachProducerWithBreak
ForEachProducerWithReturn
ForEachProducerSafe
```

无 `ConsumerToken` 的 `TryDequeue()` 会频繁调用这些函数。即使 lambda 能放入 `std::function` 的小对象缓冲区而不分配内存，类型擦除和间接调用仍会阻止关键代码内联。

### 4.2 建议修改

改成函数模板，例如：

```cpp
template <class Func>
HAKLE_CPP14_CONSTEXPR void ForEachProducerWithBreak(Func&& FuncValue)
    HAKLE_NOEXCEPT(noexcept(FuncValue(nullptr))) {
    for (ProducerListNode* Node =
             ProducerListsHead.load(std::memory_order_acquire);
         Node != nullptr;
         Node = Node->Next) {
        if (!FuncValue(Node)) {
            return;
        }
    }
}
```

其他三个函数使用同样方式修改。之后如果没有其他用途，可以移除公开头中的 `<functional>`。

### 4.3 内存序要求

并发可调用的 producer list 遍历必须用：

```cpp
ProducerListsHead.load(std::memory_order_acquire)
```

因为新节点通过 release CAS 发布。不能为了保留旧代码的 `relaxed` 而破坏节点初始化的可见性。

析构、move 等明确没有并发访问的内部遍历可以继续使用 relaxed，但最好将“并发遍历”和“无并发遍历”分成语义明确的函数。

### 4.4 临时原型结果

在 arm64 macOS 上，用预填充队列测试无 ConsumerToken dequeue：

```text
当前版本：约 18–41 ns/item
模板版本：约  3–12 ns/item
```

结果会随 producer 数量和系统调度波动，但收益方向很稳定。模板原型使用 acquire 读取 producer list 后仍有明显收益。

### 4.5 验收条件

- 所有测试通过。
- ASan/UBSan 通过。
- 并发压力测试无丢失、重复或损坏元素。
- 无 token 的 prefilled dequeue 明显改善。
- token 路径不能出现显著回退。

## 5. 优化二：隔离 producer/consumer 热点缓存线

### 5.1 当前问题

`_QueueBase` 中以下原子变量连续存放：

```cpp
HeadIndex
TailIndex
DequeueAttemptsCount
DequeueFailedCount
```

生产者每次 enqueue 都写 `TailIndex`。消费者会读取 `TailIndex`，同时对 `HeadIndex` 和 `DequeueAttemptsCount` 做 RMW，并偶尔更新 `DequeueFailedCount`。

它们位于同一缓存线时，生产者写入和消费者 RMW 会反复争夺缓存线所有权。此外，不同 producer 对象由 allocator 分别分配，也可能彼此发生 false sharing。

### 5.2 建议原型

先增加可配置的缓存线大小，例如：

```cpp
#ifndef HAKLE_CACHE_LINE_SIZE
#define HAKLE_CACHE_LINE_SIZE 64
#endif
```

Apple ARM 环境应额外验证 128 字节。然后至少隔离 `TailIndex`：

```cpp
alignas(HAKLE_CACHE_LINE_SIZE)
std::atomic<std::size_t> TailIndex{};

alignas(HAKLE_CACHE_LINE_SIZE)
std::atomic<std::size_t> HeadIndex{};

std::atomic<std::size_t> DequeueAttemptsCount{};
std::atomic<std::size_t> DequeueFailedCount{};
```

更完整的实现可以将字段整理为两个结构：

```text
ProducerHotFields：TailIndex、TailBlock 等 producer-only 数据
ConsumerHotFields：HeadIndex、Attempts、Failed 等 consumer 数据
```

### 5.3 临时原型结果与代价

arm64 临时原型使用 128 字节隔离后：

```text
4P 4C、8P 8C token 场景通常提升约 30%–80%
```

对象大小变化：

```text
FastQueue：88 bytes  -> 256 bytes
SlowQueue：72 bytes  -> 256 bytes
```

这个优化不能仅凭 macOS 结果直接合入。必须在公司 x86/Windows 或 Linux 机器上分别测试 64 和 128 字节，确认吞吐收益足以覆盖 producer 对象变大的代价。

### 5.4 验收条件

- 分别报告 1P1C、1P8C、8P1C、8P8C。
- 报告 FastQueue/SlowQueue/ConcurrentQueue/Token 的 `sizeof`。
- 检查 producer 数量很大、队列大多为空时，producer list 扫描是否因对象变大而退化。
- 最终缓存线大小必须可配置，不能只针对单台机器硬编码。

## 6. 优化三：修正默认 BlockPool 数量

### 6.1 当前问题

默认 traits：

```cpp
static constexpr std::size_t BlockSize            = 32;
static constexpr std::size_t InitialBlockPoolSize = 32 * BlockSize;
```

`InitialBlockPoolSize` 的值是 1024，但 `HakleBlockManager` 将它解释为 Block 数量，而不是元素数量。结果是 explicit manager 和 implicit manager 各自预分配 1024 个 Block。

以 `ConcurrentQueue<int>` 当前布局计算：

```text
ExplicitBlock：184 bytes × 1024
ImplicitBlock：160 bytes × 1024
合计约 344 KiB
```

而 1024 个元素只需要约 32 个默认 Block。

### 6.2 建议

先确定 API 语义，并使用不会混淆的名字：

```text
InitialBlockPoolElementCapacity
或
InitialBlockPoolBlockCount
```

如果目标是 1024 个元素，则传给每个 manager 的默认 Block 数量应约为：

```cpp
(ElementCapacity + BlockSize - 1) / BlockSize
```

还应考虑 explicit 和 implicit 是两个独立 pool。通常用户只使用其中一种 producer，另一个 pool 会完全闲置。长期可考虑：

- 延迟创建对应 manager 的 pool；
- 或允许 traits 分别配置 explicit/implicit 初始 Block 数量；
- 或默认使用较小 pool，让用户按工作负载显式扩大。

### 6.3 临时构造测试

arm64 临时测试结果：

```text
Hakle 默认 1024 blocks/manager：约 7–11 us/次构造
Hakle 32 blocks/manager：       约 0.24–0.33 us/次构造
```

该结果主要说明默认池过大会显著增加构造和内存清零成本；正式数据应在公司环境重新测量。

### 6.4 注意事项

减小 pool 可能使动态分配更早出现，因此必须分别报告：

- 构造时间；
- 稳定吞吐；
- 峰值队列深度；
- 动态分配次数；
- 峰值和常驻内存。

不能只看某一个吞吐数字。

## 7. 优化四：ProducerToken 直接调用 ExplicitProducer

### 7.1 当前问题

`ProducerToken` 按设计只会关联 explicit producer，但当前 token enqueue 经过：

```cpp
Token.ProducerNode->ProducerEnqueue(...)
```

随后又根据 `ProducerListNode::Type` 判断 explicit/implicit。

### 7.2 建议修改

改为直接调用：

```cpp
return Token.ProducerNode
    ->GetExplicitProducer()
    ->template Enqueue<Alloc>(std::forward<Args>(args)...);
```

同样处理：

```text
InnerEnqueueBulk(ProducerToken, ...)
TryDequeueFromProducer
TryDequeueBulkFromProducer
```

ConsumerToken 可以指向 explicit 或 implicit producer，因此普通 consumer token 扫描仍然需要类型判断，不能一并删除。

### 7.3 预期

这是低风险、小收益优化。临时结果显示高并发 producer-only 场景可能有约 5%–10% 收益，但数据噪声较大，应独立复测。

## 8. 后续实验：SlowQueue 本地备用 Block

`SlowQueue` 的 Block 被最后一个消费者清空后，会立即返回全局 `BlockManager/FreeList`。下次 producer 需要 Block 时又从全局结构获取。

可以给每个 `SlowQueue` 增加一个单槽本地缓存：

```cpp
std::atomic<BlockType*> LocalFreeBlock{};
```

思路：

```text
消费者完成最后一次 dequeue：
    如果 LocalFreeBlock 为空，把 Block 放进去
    否则返回全局 BlockManager

producer 需要新 Block：
    先 exchange 取走 LocalFreeBlock
    没有时再访问 BlockManager
```

潜在收益：

- 减少全局 FreeList CAS；
- 降低多 implicit producer 间的回收竞争；
- 更可能重用最近访问过的 Block。

代价与风险：

- 每个 implicit producer 最多多保留一个 Block；
- 析构、move、swap 必须正确归还本地 Block；
- 多消费者同时完成不同 Block 时需要正确回退到全局 FreeList；
- 必须证明 Block 已不再被 producer 或其他消费者访问；
- 需要完整的异常和 allocator 生命周期测试。

这一项暂未做代码原型，应排在前三项之后。

## 9. 次级优化候选

### 9.1 去除 BlockManager 虚调用

`BlockManagerType` 已经是模板参数，但 `BlockManagerBase` 的接口仍是 virtual。默认 manager 可以考虑：

- 将默认 `HakleBlockManager` 标记为 `final`；
- 将 override 方法标记为 `final`；
- 或完全使用静态接口，不通过虚函数分派。

调用频率约为每个 Block 一次，不是逐元素最高优先级。需要查看编译器是否已经成功去虚化和内联。

### 9.2 bulk iterator 传递

总队列 bulk dequeue 会在多个 producer 间反复使用：

```cpp
std::next(ItemFirst, Count)
```

对于非随机访问 iterator，这可能产生额外遍历。可以参考以下方向：

- 内部函数接收 iterator 引用并直接推进；
- 对 contiguous iterator 增加专门路径；
- 确保自定义 allocator 的 `Construct/Destroy` 语义不会被 `memcpy` 绕过。

### 9.3 Block Reset 的内存序

`FlagsCheckPolicy::Reset/SetAllEmpty` 和 `CounterCheckPolicy::Reset/SetAllEmpty` 当前使用 release store。Block 在重新发布前通常由单个 producer 独占，并最终通过 `TailIndex` 的 release store 发布，因此部分 reset store 可能可以使用 relaxed。

这项收益通常只发生在 Block 边界，临时 benchmark 没有显示稳定的大幅改善。只有在完成 C++ 内存模型证明和并发测试后才能修改。

## 10. 正确性检查项

性能修改过程中必须额外检查以下问题：

1. Producer list 通过 release CAS 发布，所有并发遍历必须使用 acquire 读取入口。
2. `FreeList_DAS` 如果投入使用，不能让 Head 的发布和读取全部保持 relaxed；`FreeListNext` 的初始化必须通过 release/acquire 建立可见性。
3. 不要仅因 x86 测试通过就放松内存序；需要考虑 ARM。
4. Block、IndexEntryArray 和旧索引数组的生命周期必须覆盖所有可能的并发读者。
5. 自定义 allocator 的 allocate/deallocate 数量和原始地址必须匹配。
6. 不能将 `Allocate(2)` 得到的两个连续 Block 当成两个独立 allocation 分别释放。
7. 每项修改后运行异常构造、异常赋值、move、swap、Clear 和 allocator 计数测试。

## 11. 推荐提交顺序

建议按以下顺序制作独立 commit：

### Commit A：benchmark-only

- 移除每元素共享 `consumed` RMW。
- 增加不对称并发场景。
- 统一预分配配置。
- 增加 repetitions 和 JSON 输出。
- 不修改队列实现。

### Commit B：template producer traversal

- `std::function` 改模板回调。
- 并发入口 load 改 acquire。
- 删除不再需要的 `<functional>`。
- 更新测试和 benchmark。

### Commit C：cache-line isolation

- 增加可配置缓存线大小。
- 隔离 producer/consumer 热点。
- 报告对象大小变化。
- 同时跑 64/128 字节实验。

### Commit D：pool semantics

- 明确 pool 参数单位。
- 修正默认数量。
- 增加构造时间、内存和分配次数 benchmark。

### Commit E：direct explicit-token dispatch

- token 路径直接调用 ExplicitProducer。
- 保持 consumer 的动态 producer 分派。

之后再决定是否实现 SlowQueue 本地备用 Block、BlockManager 静态分派和 bulk iterator 优化。

## 12. 最终验收报告格式

公司环境完成测试后，至少输出下表：

| 场景 | 原版本 | 修改后 | moodycamel | 变化 | 备注 |
|---|---:|---:|---:|---:|---|
| 1P1C token | | | | | |
| 1P8C token | | | | | |
| 8P1C token | | | | | |
| 8P8C token | | | | | |
| 1P1C implicit | | | | | |
| 8P8C implicit | | | | | |
| prefilled dequeue | | | | | |
| bulk 64 | | | | | |
| pool=0 allocation stress | | | | | |

还应同时报告：

```text
构造时间
峰值内存
动态 allocation 次数
FastQueue/SlowQueue sizeof
测试与 sanitizer 结果
编译器、CPU、缓存线和编译参数
```

判断标准不应只是“某个场景超过 moodycamel”，而应是：常见场景稳定改善、没有明显回退、内存代价可解释，并且所有正确性测试通过。
