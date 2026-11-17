#pragma once
// 世界生成系统（World / Chunk Generation）顶层聚合头文件（阶段 G-0 起步）。
// 提供 26.1 12 阶段 `ChunkStatus` 状态机与 `ChunkPyramid` 依赖金字塔、
// `ProtoChunk` / `ImposterProtoChunk` 原型区块数据模型、以及 `WorldGenRegion` / `ChunkGenerator` 管线抽象。

#include "redstone/worldgen/aquifer.hpp"
#include "redstone/worldgen/biome_source.hpp"
#include "redstone/worldgen/carver.hpp"
#include "redstone/worldgen/chunk_generator.hpp"
#include "redstone/worldgen/chunk_status.hpp"
#include "redstone/worldgen/climate.hpp"
#include "redstone/worldgen/cubic_spline.hpp"
#include "redstone/worldgen/density_function.hpp"
#include "redstone/worldgen/feature.hpp"
#include "redstone/worldgen/jigsaw.hpp"
#include "redstone/worldgen/noise.hpp"
#include "redstone/worldgen/noise_chunk.hpp"
#include "redstone/worldgen/noise_chunk_generator.hpp"
#include "redstone/worldgen/noise_router.hpp"
#include "redstone/worldgen/proto_chunk.hpp"
#include "redstone/worldgen/random_source.hpp"
#include "redstone/worldgen/structure.hpp"
#include "redstone/worldgen/surface_rules.hpp"
#include "redstone/worldgen/value_providers.hpp"
#include "redstone/worldgen/worldgen_scheduler.hpp"
