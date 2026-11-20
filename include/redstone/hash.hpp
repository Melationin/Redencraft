#pragma once
// 统一单线程扁平哈希表（`phmap::flat_hash_map`/`flat_hash_set`）与 `ankerl` 字符串哈希入口。

#include "ankerl/unordered_dense.h"
#include "parallel_hashmap/phmap.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace redstone {

// 基于 `ankerl::unordered_dense::hash<std::string_view>`（wyhash）的透明字符串哈希，
// 同时支持 `std::string`、`std::string_view` 与 `const char *` 零分配异构查找。
struct AnkerlStringHash {
  using is_transparent = void;
  using is_avalanching = void;

  std::size_t operator()(std::string_view sv) const noexcept {
    return static_cast<std::size_t>(
        ankerl::unordered_dense::hash<std::string_view>{}(sv));
  }
};

using TransparentStringHash = AnkerlStringHash;

} // namespace redstone
