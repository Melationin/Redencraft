#include "redstone/light/chunk_input.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace redstone {
namespace {

LightSectionView sectionViewForLight(const BlockSection &section,
                                     std::vector<std::uint8_t> &cells8,
                                     std::vector<std::uint16_t> &cells16,
                                     std::vector<BlockState::Id> &palette) {
  LightSectionView view;
  view.single = section.single;
  view.bits = section.storage ? section.storage->bits : 0;
  if (!section.storage)
    return view;
  visitSectionStorage(section.storage.get(), [&](const auto *block) {
    const std::size_t paletteCount = block->paletteSize;
    if (block->bits == 16) {
      view.bits = 16;
      cells16.resize(kLightVolume);
      for (std::size_t i = 0; i < kLightVolume; ++i)
        cells16[i] = block->indexAt(i);
      view.cells16 = cells16;
    } else {
      // 把 1..8 位统一的索引展开成 8 位视图，复用共享几何转换；
      // 4096 个状态的区段仍由 16 位分支处理。
      view.bits = 8;
      cells8.resize(kLightVolume);
      for (std::size_t i = 0; i < kLightVolume; ++i)
        cells8[i] = static_cast<std::uint8_t>(block->indexAt(i));
      view.cells8 = cells8;
    }
    palette.clear();
    palette.reserve(paletteCount);
    for (std::size_t i = 0; i < paletteCount; ++i)
      palette.push_back(block->palette[i]);
    view.palette = palette;
  });
  return view;
}

} // namespace

LightChunkInput buildLightChunkInput(const BaseChunk &chunk,
                                     const LightOpticalTable &optical,
                                     std::optional<PersistedChunkLight> persistedLight,
                                     bool directGeometry, bool deferCompute,
                                     std::shared_ptr<const BaseChunk> chunkHandle) {
  LightChunkInput input;
  input.chunkX = chunk.pos().x;
  input.chunkZ = chunk.pos().z;
  input.lifecycleEpoch = chunk.lightEpoch;
  input.minBlockSectionY = chunk.minSectionY();
  input.blockSectionCount = chunk.sectionCount();
  if (persistedLight) {
    input.persistedLight = std::move(persistedLight);
  } else if (deferCompute) {
    input.completion = LightChunkInput::Completion::Deferred;
  }
  if (chunkHandle) {
    // 零拷贝：登记句柄，工作线程按区段直接读这个区块对象。
    input.chunkHandle = std::move(chunkHandle);
    input.directGeometry = true;
    return input;
  }
  if (directGeometry) {
    input.directGeometry = true;
    return input;
  }
  if (input.blockSectionCount < 0 || input.blockSectionCount > kSectionCapacity)
    throw std::invalid_argument("chunk section count is outside engine capacity");
  input.geometrySections.reserve(static_cast<std::size_t>(input.blockSectionCount));
  std::vector<std::uint8_t> cells8;
  std::vector<std::uint16_t> cells16;
  std::vector<BlockState::Id> palette;
  for (const BlockSection &section : chunk.sectionSpan()) {
    cells8.clear();
    cells16.clear();
    const LightSectionView view =
        sectionViewForLight(section, cells8, cells16, palette);
    input.geometrySections.push_back(makeLightGeometrySection(optical, view));
  }
  return input;
}

} // namespace redstone
