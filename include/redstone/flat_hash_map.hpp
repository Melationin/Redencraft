#pragma once
// 开放寻址扁平哈希表实现：基于 `phmap::flat_hash_map` 提供连续控制字节 + 槽位数组探测，
// 避免红黑树与逐节点分配；字符串键统一搭配基于 ankerl 的 `TransparentStringHash` (`AnkerlStringHash`)。
#include "redstone/hash.hpp"

#include <functional>

namespace redstone {

template <class Key, class T, class Hash = phmap::Hash<Key>,
          class KeyEqual = std::equal_to<Key>>
using FlatHashMap = phmap::flat_hash_map<Key, T, Hash, KeyEqual>;

} // namespace redstone
