#pragma once

#include <cstddef>
#include <cstdint>

#include "redstone/block.hpp"

namespace redstone {

// LightLayer matches net.minecraft.world.level.LightLayer. Block first mirrors
// LevelLightEngine.updateSectionStatus's block-then-sky calls.
enum class LightLayer : std::uint8_t { Block = 0, Sky = 1 };
enum class LightDirection : std::uint8_t { Down = 0, Up, North, South, West, East };
inline constexpr std::size_t kLightLayerCount = 2;
inline constexpr int kLightVolume = 16 * 16 * 16;
inline constexpr std::size_t kLightLayerBytes = 2048;
inline constexpr std::uint8_t kLightUniformFlag = 0x10;

inline std::size_t lightIndex(int x, int y, int z) {
  // DataLayer.getIndex: y << 8 | z << 4 | x.
  return (static_cast<std::size_t>(y & 15) << 8) |
         (static_cast<std::size_t>(z & 15) << 4) |
         static_cast<std::size_t>(x & 15);
}

// Fixed-size ordinary block-change payload. The worker owns coordinate
// validation and optical mapping; the main thread only publishes the final raw
// state ID.
struct alignas(16) LightCellUpdate {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t z = 0;
  BlockState::Id state = 0;
};
static_assert(sizeof(LightCellUpdate) == 16);

} // namespace redstone
