#include "redstone/allocator.hpp"

namespace {
void *checked(std::size_t n, std::size_t a = alignof(std::max_align_t)) {
  for (;;) {
    if (auto p = redstone::memory::allocate(n, a))
      return p;
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
