#pragma once
// 统一光照输入构建：ProtoChunk 与 World::Chunk 都继承 BaseChunk，共用同一套
// 区段/群系/代次数据。光照路径不再按 chunk 类型分支，只依赖 BaseChunk。

#include "redstone/chunk_section.hpp"
#include "redstone/light/optical.hpp"
#include "redstone/light/snapshot.hpp"

#include <optional>

namespace redstone {

// 从统一区块基类构建 LightService 提交载荷：只登记元数据（绝不复制几何），
// 工作线程通过 `chunkHandle`（生成期 ProtoChunk）或 `World` 直读回调按区段取实时数据。
// `deferCompute` 为真时只注册、停在“未计算”，由 `Light` 阶段的 compute 请求首次计算。
// 调用方负责设置 lifecycleEpoch（加载生命周期与光照生命周期现在是同一个令牌）。
LightChunkInput buildLightChunkInput(
    const BaseChunk &chunk, std::optional<PersistedChunkLight> persistedLight,
    bool deferCompute, std::shared_ptr<const BaseChunk> chunkHandle = {},
    std::shared_ptr<const PrecomputedChunkLight> precomputedLight = {});

} // namespace redstone
