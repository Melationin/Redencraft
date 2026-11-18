#pragma once
// 统一光照输入构建：ProtoChunk 与 World::Chunk 都继承 BaseChunk，共用同一套
// 区段/群系/代次数据。光照路径不再按 chunk 类型分支，只依赖 BaseChunk。

#include "redstone/chunk_section.hpp"
#include "redstone/light/optical.hpp"
#include "redstone/light/snapshot.hpp"

#include <optional>

namespace redstone {

// 从统一区块基类构建 LightService 提交载荷。
// - directGeometry = true 时只登记区块元数据，几何由工作线程按需直读（World 低内存模式）。
// - deferCompute = true 且没有持久化光照时，只注册几何并停在“未计算”。
// 调用方负责设置 lifecycleEpoch（加载生命周期与光照生命周期现在是同一个令牌）。
LightChunkInput buildLightChunkInput(const BaseChunk &chunk,
                                     const LightOpticalTable &optical,
                                     std::optional<PersistedChunkLight> persistedLight,
                                     bool directGeometry, bool deferCompute);

} // namespace redstone
