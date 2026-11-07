#include "redstone/allocator.hpp"
#ifdef REDSTONE_COUNT_ALLOCATIONS
#include <atomic>
namespace {
std::atomic<std::size_t> allocations{0}, requestedBytes{0};
}
redstone::memory::AllocationCounts redstone::memory::allocationCounts() noexcept {
  return {allocations.load(std::memory_order_relaxed),
          requestedBytes.load(std::memory_order_relaxed)};
}
#endif

namespace {
void *checked(std::size_t n, std::size_t a = alignof(std::max_align_t)) {
  for (;;) {
    if (auto p = redstone::memory::allocate(n, a)) {
#ifdef REDSTONE_COUNT_ALLOCATIONS
      allocations.fetch_add(1, std::memory_order_relaxed);
      requestedBytes.fetch_add(n, std::memory_order_relaxed);
#endif
      return p;
    }
    auto handler = std::get_new_handler();
    if (!handler)
      throw std::bad_alloc();
    handler();
  }
}
} // namespace
void *operator new(std::size_t n) { return checked(n); }
void *operator new[](std::size_t n) { return checked(n); }
void operator delete(void *p) noexcept { redstone::memory::release(p); }
void operator delete[](void *p) noexcept { redstone::memory::release(p); }
void operator delete(void *p, std::size_t) noexcept {
  redstone::memory::release(p);
}
void operator delete[](void *p, std::size_t) noexcept {
  redstone::memory::release(p);
}
void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
  try {
    return checked(n);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept {
  try {
    return checked(n);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *p, const std::nothrow_t &) noexcept {
  redstone::memory::release(p);
}
void operator delete[](void *p, const std::nothrow_t &) noexcept {
  redstone::memory::release(p);
}
void *operator new(std::size_t n, std::align_val_t a) {
  return checked(n, static_cast<std::size_t>(a));
}
void *operator new[](std::size_t n, std::align_val_t a) {
  return checked(n, static_cast<std::size_t>(a));
}
void operator delete(void *p, std::align_val_t a) noexcept {
  redstone::memory::release(p, static_cast<std::size_t>(a));
}
void operator delete[](void *p, std::align_val_t a) noexcept {
  redstone::memory::release(p, static_cast<std::size_t>(a));
}
void operator delete(void *p, std::size_t, std::align_val_t a) noexcept {
  redstone::memory::release(p, static_cast<std::size_t>(a));
}
void operator delete[](void *p, std::size_t, std::align_val_t a) noexcept {
  redstone::memory::release(p, static_cast<std::size_t>(a));
}
void *operator new(std::size_t n, std::align_val_t a,
                   const std::nothrow_t &) noexcept {
  try {
    return checked(n, static_cast<std::size_t>(a));
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t n, std::align_val_t a,
                     const std::nothrow_t &) noexcept {
  try {
    return checked(n, static_cast<std::size_t>(a));
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *p, std::align_val_t a,
                     const std::nothrow_t &) noexcept {
  redstone::memory::release(p, static_cast<std::size_t>(a));
}
void operator delete[](void *p, std::align_val_t a,
                       const std::nothrow_t &) noexcept {
  redstone::memory::release(p, static_cast<std::size_t>(a));
}
