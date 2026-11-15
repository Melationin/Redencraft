#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "redstone/light/types.hpp"

namespace redstone {

bool hasDifferentLightProperties(const BlockState &oldState,
                                 const BlockState &newState);
bool hasDifferentLightProperties(const BlockLightProperties &oldProperties,
                                 const BlockLightProperties &newProperties);
bool hasDifferentSectionLightProperties(bool wasEmpty, bool isEmpty);
bool lightFaceShapesOcclude(const BlockLightProperties &from,
                            const BlockLightProperties &to,
                            LightDirection direction);

// Immutable optical-class table built from the registered BlockState range.
// It is published before any conversion or propagation worker starts.
struct LightOpticalClass {
  BlockLightProperties properties;
  bool air = false;
};

class LightOpticalTable {
public:
  LightOpticalTable();

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

std::shared_ptr<const LightOpticalTable> defaultLightOpticalTable();

// Compact optical geometry for one 16x16x16 block section. Uniform sections do
// not allocate a cell array. Mixed sections own exactly one 4096-byte array of
// light optical-class ids.
struct GeometrySection {
  using Cells = std::array<std::uint8_t, kLightVolume>;

  bool empty = true;
  bool uniform = true;
  std::uint8_t uniformClass = 0;
  std::unique_ptr<Cells> cells;
  std::uint16_t nonAir = 0;
  std::uint16_t unknown = 0;

  std::uint16_t at(std::size_t index) const {
    if (uniform)
      return uniformClass;
    return (*cells)[index];
  }
};

// Read-only view into a live World section. In standard mode conversion runs
// synchronously on the world thread; in direct low-memory mode the worker
// borrows live World sections under the resize lock and reads atomically.
struct LightSectionView {
  bool empty = true;
  std::uint8_t bits = 0; // 0 = single, 1..7 = packedWords, 8 = cells8, 16 = cells16
  BlockState::Id single = 0;
  std::span<const BlockState::Id> palette;
  std::span<const std::uint8_t> cells8;
  std::span<const std::uint16_t> cells16;
  std::span<const std::uint64_t> packedWords;
  // Keeps SectionStorage alive when borrowed in direct low-memory mode.
  std::shared_ptr<const void> storageOwner;

  static std::uint16_t unpackBits(std::uint8_t bitWidth,
                                  std::span<const std::uint64_t> words,
                                  std::size_t cell) noexcept {
    const std::size_t perWord = std::size_t{64} / bitWidth;
    const std::size_t wordIndex = cell / perWord;
    const std::size_t shift = (cell % perWord) * bitWidth;
    const std::uint64_t mask = (std::uint64_t{1} << bitWidth) - 1u;
    return static_cast<std::uint16_t>((words[wordIndex] >> shift) & mask);
  }

  std::uint16_t paletteIndexAt(std::size_t cell) const noexcept {
    if (bits == 8)
      return cells8[cell];
    if (bits == 16)
      return cells16[cell];
    if (bits >= 1 && bits <= 7)
      return unpackBits(bits, packedWords, cell);
    return 0;
  }

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

  BlockState::Id atomicStateAt(std::size_t cell) const noexcept {
    if (bits == 0)
      return single;
    const std::uint16_t index = atomicPaletteIndexAt(cell);
    // Palette slots are populated with release semantics before their index
    // appears in words/cells; storage capacity prevents widening without lock.
    auto &slot = const_cast<BlockState::Id &>(palette.data()[index]);
    return std::atomic_ref<BlockState::Id>(slot).load(
        std::memory_order_acquire);
  }
};

GeometrySection makeLightGeometrySection(const LightOpticalTable &optical,
                                         const LightSectionView &section);

} // namespace redstone
