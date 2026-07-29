# Known issues

当前仍确认的限制：

1. `ConcurrentQueue/Block.h` 使用 `std::has_single_bit`，但公开 include 链没有自行包含 `<bit>`。在修复
   header self-containment 前，调用方应先 `#include <bit>`。
2. MSVC 需要 `/Zc:preprocessor` 才能正确展开项目使用 `__VA_OPT__` 的宏；项目 CMake target 已配置。
