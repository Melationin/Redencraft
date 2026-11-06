#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>
#ifdef REDSTONE_USE_JEMALLOC
#define JEMALLOC_NO_DEMANGLE
#include <jemalloc/jemalloc.h>
#elif defined(_WIN32)
#include <malloc.h>
#endif

// Every global new/delete in allocator.cpp goes through this single switch.
// Keep allocation and deallocation backends paired, including over-aligned new.
namespace redstone::memory {
inline constexpr const char *backend() noexcept {
#ifdef REDSTONE_USE_JEMALLOC
  return "jemalloc";
#else
  return "system";
#endif
}
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
