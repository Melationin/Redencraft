#pragma once

// 光照子系统公共基础类型与常量定义：提供跨线程共享的光照层枚举、方向、区间索引公式及轻量化方块变更载荷。

#include <cstddef>
#include <cstdint>

#include "redstone/block.hpp"

namespace redstone {

// 光照层类型，对应原版 `net.minecraft.world.level.LightLayer`。
// `Block` 排在首位，用于镜像原版 `LevelLightEngine.updateSectionStatus` 先方块光、后天空光的调用顺序。
enum class LightLayer : std::uint8_t { Block = 0, Sky = 1 };
// 光照传播与面遮挡判定的六个正交方向（成对排列，便于通过 `dir ^ 1` 求反方向）。
enum class LightDirection : std::uint8_t { Down = 0, Up, North, South, West, East };
// 每个区块段包含的光照层数量（方块光与天空光共 2 层）。
inline constexpr std::size_t kLightLayerCount = 2;
// 单个 16x16x16 区块段内的方块单元总数（4096）。
inline constexpr int kLightVolume = 16 * 16 * 16;
// 单个 4-bit 半字节压缩光照页的字节数（4096 / 2 = 2048 字节）。
inline constexpr std::size_t kLightLayerBytes = 2048;
// 存储在状态字节高半字节中的“均一值”标志位，用于无堆分配表示全段同亮度的快照层。
inline constexpr std::uint8_t kLightUniformFlag = 0x10;

// 计算区块段局部坐标 `(x, y, z)` 在 4096 单元数组中的线性索引。
// 采用与原版 `DataLayer.getIndex` 一致的 Y-Z-X 布局（`y << 8 | z << 4 | x`），确保可直接对齐原版磁盘与协议半字节顺序。
inline std::size_t lightIndex(int x, int y, int z) {
  return (static_cast<std::size_t>(y & 15) << 8) |
         (static_cast<std::size_t>(z & 15) << 4) |
         static_cast<std::size_t>(x & 15);
}

// 固定 16 字节的常规方块变更消息载荷，用于热路径从主线程向光照工作线程投递变更。
// 之所以只携带坐标与最终原始 `BlockState::Id` 并对齐到 16 字节，是为了把坐标校验和光学属性映射留在工作线程完成，避免主线程在每次方块更新时做查表开销。
struct alignas(16) LightCellUpdate {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t z = 0;
  BlockState::Id state = 0;
};
static_assert(sizeof(LightCellUpdate) == 16);

} // namespace redstone
