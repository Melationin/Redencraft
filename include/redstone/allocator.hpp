#pragma once
// 内存分配抽象层：为 `src/allocator.cpp` 中重载的全局 `operator new`/`delete`
// 提供统一的底层分配开关（支持 `jemalloc` 与系统分配器切换，以及可选的分配计数插桩）。
#include <cstddef>
#include <cstdlib>
#include <new>
#ifdef REDSTONE_USE_JEMALLOC
#define JEMALLOC_NO_DEMANGLE
#include <jemalloc/jemalloc.h>
#elif defined(_WIN32)
#include <malloc.h>
#endif

// `src/allocator.cpp` 中的所有全局 `new`/`delete` 重载均通过此单一入口分派。
// 这样可确保常规分配与过对齐（over-aligned）分配始终使用配对的后端（`jemalloc`
// 或系统堆），避免混用分配器/释放器导致未定义行为。
namespace redstone::memory {
#ifdef REDSTONE_COUNT_ALLOCATIONS
// 仅在显式开启 `REDSTONE_COUNT_ALLOCATIONS` 时启用的基准测试统计插桩；
// 正常构建的可执行文件不携带任何计数开销。
struct AllocationCounts {
  std::size_t allocations, requestedBytes;
};
AllocationCounts allocationCounts() noexcept;
#endif
// 返回当前启用的内存分配后端名称，用于日志与诊断输出。
inline constexpr const char *backend() noexcept {
#ifdef REDSTONE_USE_JEMALLOC
  return "jemalloc";
#else
  return "system";
#endif
}
// 统一底层对齐分配入口：保证至少 1 字节分配与指针级对齐，并屏蔽
// `jemalloc`、Windows CRT（`_aligned_malloc`）与 POSIX（`posix_memalign`）的接口差异。
inline void *
allocate(std::size_t size,
         std::size_t alignment = alignof(std::max_align_t)) noexcept {
  if (!size)
    size = 1;
  if (alignment < sizeof(void *))
    alignment = sizeof(void *);
#ifdef REDSTONE_USE_JEMALLOC
  return je_mallocx(size, MALLOCX_ALIGN(alignment));
#elif defined(_WIN32)
  return alignment <= alignof(std::max_align_t)
             ? std::malloc(size)
             : _aligned_malloc(size, alignment);
#else
  void *p = nullptr;
  return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
#endif
}
// 与 `allocate` 配对的释放入口：按分配后端及对齐路径安全释放内存，允许传入 `nullptr`。
inline void
release(void *p, std::size_t alignment = alignof(std::max_align_t)) noexcept {
  if (!p)
    return;
#ifdef REDSTONE_USE_JEMALLOC
  (void)alignment;
  je_dallocx(p, 0);
#elif defined(_WIN32)
  if (alignment <= alignof(std::max_align_t))
    std::free(p);
  else
    _aligned_free(p);
#else
  (void)alignment;
  std::free(p);
#endif
}
} // namespace redstone::memory
