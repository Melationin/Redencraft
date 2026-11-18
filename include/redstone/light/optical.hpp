#pragma once

// 光照光学属性与几何抽象层：将庞大的方块状态表压缩为 8 位光学类别，并为工作线程提供标准快照和低内存直读两种区块段几何视图。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "redstone/light/types.hpp"

namespace redstone {

// 判断两个方块状态或光学属性变化后是否会影响光照（用于过滤不改变光学行为的方块状态更新，避免无效重算）。
bool hasDifferentLightProperties(const BlockState &oldState,
                                 const BlockState &newState);
bool hasDifferentLightProperties(const BlockLightProperties &oldProperties,
                                 const BlockLightProperties &newProperties);
// 判断区块段在“全空气/非全空气”状态间切换时是否影响光照存储资格。
bool hasDifferentSectionLightProperties(bool wasEmpty, bool isEmpty);
// 判断光从 `from` 方块沿 `direction` 射向 `to` 方块时，双方的面遮挡形状并集是否完全封堵该接触面。
bool lightFaceShapesOcclude(const BlockLightProperties &from,
                            const BlockLightProperties &to,
                            LightDirection direction);

// 光学等价类条目：将具有相同 `BlockLightProperties` 与空气标记的方块归为一类，便于用 1 字节类 ID 代替完整方块状态 ID。
struct LightOpticalClass {
  BlockLightProperties properties;
  bool air = false;
};

// 不可变光学类查找表，由已注册的 `BlockState` 范围预先构建。
// 之所以需要这张表，是为了将数万个方块状态去重压缩到 <=256 个光学等价类中，使区块几何副本每格只需 1 字节；该表在任何几何转换或传播工作线程启动前即已完成发布。
class LightOpticalTable {
public:
  LightOpticalTable();

  // 根据 `BlockState::Id` 查出对应的光学类别 ID；越界状态回退到 0 号保守未支持哨兵类。
  std::uint8_t classForState(BlockState::Id state) const noexcept {
    return state < stateClasses_.size() ? stateClasses_[state] : 0;
  }
  const LightOpticalClass &operator[](std::uint8_t index) const noexcept {
    return classes_[index];
  }
  std::size_t classCount() const noexcept { return classes_.size(); }

private:
  std::vector<LightOpticalClass> classes_;
  std::vector<std::uint8_t> stateClasses_;
};

// 返回全局共享的默认只读光学表实例（线程安全懒初始化）。
std::shared_ptr<const LightOpticalTable> defaultLightOpticalTable();

// 指向实时区块段（`World` 或生成期 `ProtoChunk`）的只读视图。
// 光照工作线程通过原子读取直接借用实时区块段，因此不再需要每区块一份几何镜像。
struct LightSectionView {
  bool empty = true;
  // 调色板位宽编码：`0` 表示单值 `single`，`1..7` 表示位打包 `packedWords`，`8` 表示 `cells8`，`16` 表示 `cells16`。
  std::uint8_t bits = 0;
  BlockState::Id single = 0;
  std::span<const BlockState::Id> palette;
  std::span<const std::uint8_t> cells8;
  std::span<const std::uint16_t> cells16;
  std::span<const std::uint64_t> packedWords;
  // 在直读低内存模式下借用实时区块段时，用于延长底层 `SectionStorage` 的生命周期，防止主线程扩容或卸载时悬空。
  std::shared_ptr<const void> storageOwner;

  // 从 64 位紧凑打包数组 `words` 中解包指定 `cell` 对应的调色板索引。
  static std::uint16_t unpackBits(std::uint8_t bitWidth,
                                  std::span<const std::uint64_t> words,
                                  std::size_t cell) noexcept {
    const std::size_t perWord = std::size_t{64} / bitWidth;
    const std::size_t wordIndex = cell / perWord;
    const std::size_t shift = (cell % perWord) * bitWidth;
    const std::uint64_t mask = (std::uint64_t{1} << bitWidth) - 1u;
    return static_cast<std::uint16_t>((words[wordIndex] >> shift) & mask);
  }

  // 非原子读取指定单元的调色板索引。
  std::uint16_t paletteIndexAt(std::size_t cell) const noexcept {
    if (bits == 8)
      return cells8[cell];
    if (bits == 16)
      return cells16[cell];
    if (bits >= 1 && bits <= 7)
      return unpackBits(bits, packedWords, cell);
    return 0;
  }

  // 使用 `std::atomic_ref` 以 `acquire` 语义读取指定单元的调色板索引，供直读模式下工作线程与主线程安全并发访问。
  std::uint16_t atomicPaletteIndexAt(std::size_t cell) const noexcept {
    if (bits == 8) {
      auto &slot = const_cast<std::uint8_t &>(cells8[cell]);
      return std::atomic_ref<std::uint8_t>(slot).load(
          std::memory_order_acquire);
    }
    if (bits == 16) {
      auto &slot = const_cast<std::uint16_t &>(cells16[cell]);
      return std::atomic_ref<std::uint16_t>(slot).load(
          std::memory_order_acquire);
    }
    if (bits >= 1 && bits <= 7) {
      const std::size_t perWord = std::size_t{64} / bits;
      const std::size_t wordIndex = cell / perWord;
      const std::size_t shift = (cell % perWord) * bits;
      const std::uint64_t mask = (std::uint64_t{1} << bits) - 1u;
      auto &slot = const_cast<std::uint64_t &>(packedWords[wordIndex]);
      const std::uint64_t word =
          std::atomic_ref<std::uint64_t>(slot).load(std::memory_order_acquire);
      return static_cast<std::uint16_t>((word >> shift) & mask);
    }
    return 0;
  }

  // 在直读模式下原子地读取调色板索引并解析出对应的 `BlockState::Id`。
  BlockState::Id atomicStateAt(std::size_t cell) const noexcept {
    if (bits == 0)
      return single;
    const std::uint16_t index = atomicPaletteIndexAt(cell);
    // 调色板槽位会在其索引写入位打包字或索引数组之前以 `release` 语义填充；底层存储预留容量确保无需持锁也不会在无扩容锁时扩宽。
    auto &slot = const_cast<BlockState::Id &>(palette.data()[index]);
    return std::atomic_ref<BlockState::Id>(slot).load(
        std::memory_order_acquire);
  }
};

} // namespace redstone
