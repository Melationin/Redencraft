#include "redstone/engine.hpp"

// net/minecraft/world/level/block/Blocks: one static singleton per block type.
// Declaration order matches official 26.1 BuiltInRegistries.BLOCK, so
// minecraft:air is registry state id 0 and all 1168 vanilla blocks / 29873
// states are registered in exact vanilla order.
namespace redstone {
namespace Blocks {
#include "blocks_registry.inc"
} // namespace Blocks
} // namespace redstone
