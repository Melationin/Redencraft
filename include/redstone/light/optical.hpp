#pragma once

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

// Read-only view into a live World section. Conversion happens synchronously
// on the world thread; the view is never stored or read by a worker.
struct LightSectionView {
  bool empty = true;
  std::uint8_t bits = 0; // 0 = single, 8 = cells8, 16 = cells16
  BlockState::Id single = 0;
  std::span<const BlockState::Id> palette;
  std::span<const std::uint8_t> cells8;
  std::span<const std::uint16_t> cells16;
};

GeometrySection makeLightGeometrySection(const LightOpticalTable &optical,
                                         const LightSectionView &section);

} // namespace redstone
