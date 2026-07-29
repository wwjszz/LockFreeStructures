# Known issues

这些问题属于现有队列实现，本轮仅重构测试与 benchmark，因此没有修改对应源码。

1. `ConcurrentQueue::operator=(ConcurrentQueue&&)` 在 MSVC Debug 下，当目标队列已经持有 producer/元素时会
   触发访问冲突。move construction 和 `swap` 已进入默认测试；move assignment 暂不作为通过门槛。
2. `HakleFlagsBlockManager` 和 `HakleCounterBlockManager` 的 alias 当前没有把其 allocator 模板参数传给
   `HakleBlockManager`。自定义 allocator 若要覆盖 block pool/free list，需要提供自定义 queue traits；测试中
   已给出完整示例。
3. `ConcurrentQueue/Block.h` 使用 `std::has_single_bit`，但公开 include 链没有自行包含 `<bit>`。在修复
   header self-containment 前，调用方应先 `#include <bit>`。
4. MSVC 需要 `/Zc:preprocessor` 才能正确展开项目使用 `__VA_OPT__` 的宏；项目 CMake target 已配置。