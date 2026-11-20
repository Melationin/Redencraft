#pragma once
// 并行 3x3 生成期初始光照计算（完全对齐 26.1 BlockLightEngine / SkyLightEngine 语义）。

#include <array>
#include <memory>

#include "redstone/chunk_section.hpp"
#include "redstone/light/optical.hpp"
#include "redstone/light/snapshot.hpp"

namespace redstone::worldgen {
struct ProtoChunkLightSummary;
}

namespace redstone {

std::shared_ptr<const worldgen::ProtoChunkLightSummary>
computeProtoChunkLightSummary(const BaseChunk &chunk,
                              const LightOpticalTable &optical,
                              bool skyLightEnabled);

// `chunks3x3` 按 `(dz + 1) * 3 + (dx + 1)` 排列（`dx, dz in {-1, 0, 1}`，索引 4 为中心区块）。
std::shared_ptr<PrecomputedChunkLight> computeInitialChunkLight(
    const std::array<const BaseChunk *, 9> &chunks3x3,
    const LightOpticalTable &optical, bool skyLightEnabled);

} // namespace redstone
