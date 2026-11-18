#pragma once
// 26.1 内置维度的世界生成器工厂：把世界元数据中的维度实例/维度类型映射到
// `NoiseGeneratorSettings` 与 `BiomeSource`，供在线区块生成与离线探针共用。

#include "redstone/dimension.hpp"
#include "redstone/worldgen/chunk_generator.hpp"

#include <memory>
#include <string_view>

namespace redstone::worldgen {

class WorldgenRegistries;

// 为 26.1 内置维度（overworld / overworld_caves / the_nether / the_end）构造噪声生成器。
// 自定义维度或无法解析的维度返回 nullptr；调用方必须显式报告并保持“缺失区块不生成”。
std::shared_ptr<ChunkGenerator>
createVanillaChunkGenerator(const WorldgenRegistries &registries,
                            std::string_view dimensionId,
                            const DimensionTypeSpec &type);

} // namespace redstone::worldgen
