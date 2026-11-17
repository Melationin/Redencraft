#pragma once
// 红石实现模块内部共享辅助工具：集中提供 `src/redstone/*.cpp` 通用的红石粉连接常量、
// 方向表/旋转、快速位域属性读取及掉落拆除辅助函数，避免在各子模块中重复样板代码。

#include "redstone/entity.hpp"
#include "redstone/redstone.hpp"

#include <array>
#include <cstdint>

namespace redstone::redstone_detail {

// 红石粉四向连接的规范枚举取值，对应 `property.cpp` 中 `WallNorth`/`WallEast`/`WallSouth`/`WallWest`
// 与墙体共用的规范取值索引（`0=none`、`5=side`、`6=up`），以便直接对 `packed()` 做整数读写。
inline constexpr std::uint8_t kWireNone = 0;
inline constexpr std::uint8_t kWireSide = 5;
inline constexpr std::uint8_t kWireUp = 6;

// 与原版红石及形状更新遍历一致的水平方向表（北、东、南、西）与全六向表。
inline constexpr std::array<Direction, 4> kHorizontalOrder{
    Direction::North, Direction::East, Direction::South, Direction::West};
inline constexpr std::array<Direction, 6> kAllDirections{
    Direction::Down,  Direction::Up,   Direction::North,
    Direction::South, Direction::West, Direction::East};

inline bool isHorizontal(Direction d) {
  return d == Direction::North || d == Direction::South ||
         d == Direction::East || d == Direction::West;
}

// 水平方向顺时针旋转 90 度；垂直方向保持不变。
inline Direction clockWise(Direction d) {
  switch (d) {
  case Direction::North:
    return Direction::East;
  case Direction::East:
    return Direction::South;
  case Direction::South:
    return Direction::West;
  case Direction::West:
    return Direction::North;
  default:
    return d;
  }
}

// 水平方向逆时针旋转 90 度；垂直方向保持不变。
inline Direction counterClockWise(Direction d) {
  switch (d) {
  case Direction::North:
    return Direction::West;
  case Direction::West:
    return Direction::South;
  case Direction::South:
    return Direction::East;
  case Direction::East:
    return Direction::North;
  default:
    return d;
  }
}

// 返回方向所属坐标轴索引：0 表示东西轴（X）、1 表示上下轴（Y）、2 表示南北轴（Z）。
inline int directionAxis(Direction d) {
  if (d == Direction::Up || d == Direction::Down)
    return 1;
  if (d == Direction::East || d == Direction::West)
    return 0;
  return 2;
}

// 将水平 `Direction` 映射为红石粉对应的规范 `PropertyKind`，从而无需按字符串名查表。
inline PropertyKind wireSideProperty(Direction d) {
  switch (d) {
  case Direction::North:
    return PropertyKind::WallNorth;
  case Direction::East:
    return PropertyKind::WallEast;
  case Direction::South:
    return PropertyKind::WallSouth;
  case Direction::West:
    return PropertyKind::WallWest;
  default:
    return PropertyKind::WallNorth;
  }
}

// 通过位域直接读取红石粉在指定方向上的连接状态，以及信号强度。
inline std::uint8_t wireSideOf(const BlockState &state, Direction d) {
  return static_cast<std::uint8_t>(propertyValue(state, wireSideProperty(d)));
}

inline bool isWireConnected(const BlockState &state, Direction d) {
  return wireSideOf(state, d) != kWireNone;
}

inline int wirePowerOf(const BlockState &state) {
  return static_cast<int>(propertyValue(state, PropertyKind::Power));
}

// 根据附着型红石元件（拉杆、按钮）的 `Face` 与 `Facing` 解析其自身朝外方向
// （调用方取反即为所附着支撑方块的方向；0=floor、1=wall、2=ceiling）。
inline Direction faceAttachedConnectedDirection(const BlockState &state) {
  const auto face = propertyValue(state, PropertyKind::Face, 1);
  if (face == 0) // 地面
    return Direction::Up;
  if (face == 2) // 天花板
    return Direction::Down;
  return directionOf(state); // 墙面
}

// 统一读取普通压力板（二值 `powered` -> 0/15）与测重压力板（模拟 `power` 0..15）的输出信号。
inline int pressurePlateSignalOf(const BlockState &state) {
  if (state.is(Blocks::LIGHT_WEIGHTED_PRESSURE_PLATE) ||
      state.is(Blocks::HEAVY_WEIGHTED_PRESSURE_PLATE))
    return static_cast<int>(propertyValue(state, PropertyKind::Power));
  return poweredOf(state) ? 15 : 0;
}

// 当红石元件失去支撑面时，按原版掉落表生成掉落物品并移除方块。
inline void dropAndRemoveBlock(World &world, Pos pos, const BlockState &state) {
  for (const auto &drop : blockDrops(state))
    world.spawnItem(pos, drop.id, drop.count);
  world.removeBlock(pos, false);
}

} // namespace redstone::redstone_detail
