#include "redstone/light/chunk_input.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace redstone {

LightChunkInput buildLightChunkInput(
    const BaseChunk &chunk, std::optional<PersistedChunkLight> persistedLight,
    bool deferCompute, std::shared_ptr<const BaseChunk> chunkHandle,
    std::shared_ptr<const PrecomputedChunkLight> precomputedLight) {
  LightChunkInput input;
  input.chunkX = chunk.pos().x;
  input.chunkZ = chunk.pos().z;
  input.lifecycleEpoch = chunk.lightEpoch;
  input.minBlockSectionY = chunk.minSectionY();
  input.blockSectionCount = chunk.sectionCount();
  if (input.blockSectionCount < 0 || input.blockSectionCount > kSectionCapacity)
    throw std::invalid_argument("chunk section count is outside engine capacity");
  if (persistedLight) {
    input.persistedLight = std::move(persistedLight);
  } else if (precomputedLight && precomputedLight->isLightOn) {
    input.precomputedLight = std::move(precomputedLight);
  } else if (deferCompute) {
    input.completion = LightChunkInput::Completion::Deferred;
  }
  // 只登记元数据（句柄或 World 直读），绝不复制几何。
  input.chunkHandle = std::move(chunkHandle);
  return input;
}

} // namespace redstone
