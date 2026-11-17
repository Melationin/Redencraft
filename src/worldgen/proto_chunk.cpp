#include "redstone/worldgen/proto_chunk.hpp"

#include "redstone/engine.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <unordered_map>

namespace redstone::worldgen {
namespace {

unsigned biomeBitsForPaletteSize(std::size_t size) noexcept {
  if (size <= 1)
    return 0;
  return static_cast<unsigned>(std::bit_width(size - 1));
}

} // namespace

BiomePaletteContainer::BiomePaletteContainer(std::string defaultBiome) {
  if (defaultBiome.empty())
    defaultBiome = "minecraft:plains";
  palette_.push_back(std::move(defaultBiome));
  indices_.fill(0);
}

std::uint8_t BiomePaletteContainer::intern(std::string_view biomeId) {
  for (std::size_t i = 0; i < palette_.size(); ++i) {
    if (palette_[i].data() == biomeId.data() &&
        palette_[i].size() == biomeId.size())
      return static_cast<std::uint8_t>(i);
    if (palette_[i] == biomeId)
      return static_cast<std::uint8_t>(i);
  }
  if (palette_.size() >= kCellCount) {
    // At most 64 distinct cells exist in a 4x4x4 container; prune stale entries first.
    std::array<bool, kCellCount> used{};
    for (std::uint8_t idx : indices_) {
      if (idx < palette_.size())
        used[idx] = true;
    }
    std::array<std::uint8_t, kCellCount> remap{};
    std::vector<std::string> compacted;
    compacted.reserve(palette_.size());
    for (std::size_t i = 0; i < palette_.size(); ++i) {
      if (used[i]) {
        remap[i] = static_cast<std::uint8_t>(compacted.size());
        compacted.push_back(std::move(palette_[i]));
      }
    }
    for (std::uint8_t &idx : indices_)
      idx = remap[idx];
    palette_ = std::move(compacted);
    for (std::size_t i = 0; i < palette_.size(); ++i) {
      if (palette_[i] == biomeId)
        return static_cast<std::uint8_t>(i);
    }
  }
  const std::uint8_t id = static_cast<std::uint8_t>(palette_.size());
  palette_.emplace_back(biomeId);
  return id;
}

const std::string &BiomePaletteContainer::get(int qx, int qy, int qz) const {
  const std::uint8_t pIdx = indices_[index(qx, qy, qz)];
  return palette_[pIdx];
}

void BiomePaletteContainer::set(int qx, int qy, int qz,
                                std::string_view biomeId) {
  const std::size_t cell = index(qx, qy, qz);
  const std::uint8_t prev = indices_[cell];
  if (palette_[prev] == biomeId)
    return;
  indices_[cell] = intern(biomeId);
}

void BiomePaletteContainer::fill(std::string_view biomeId) {
  palette_.clear();
  palette_.emplace_back(biomeId.empty() ? std::string_view{"minecraft:plains"}
                                        : biomeId);
  indices_.fill(0);
}

void BiomePaletteContainer::pack(std::vector<std::string> &outPalette,
                                 std::vector<std::uint64_t> &outData,
                                 int &outBits) const {
  outPalette.clear();
  outData.clear();

  std::array<std::int16_t, 256> remap{};
  remap.fill(-1);
  std::array<std::uint8_t, kCellCount> remappedCells{};
  for (std::size_t i = 0; i < kCellCount; ++i) {
    const std::uint8_t raw = indices_[i];
    if (remap[raw] < 0) {
      remap[raw] = static_cast<std::int16_t>(outPalette.size());
      outPalette.push_back(palette_[raw]);
    }
    remappedCells[i] = static_cast<std::uint8_t>(remap[raw]);
  }

  const unsigned bits = biomeBitsForPaletteSize(outPalette.size());
  outBits = static_cast<int>(bits);
  if (bits == 0)
    return;

  const std::size_t perLong = 64u / bits;
  const std::size_t longCount = (kCellCount + perLong - 1u) / perLong;
  outData.assign(longCount, 0ull);
  for (std::size_t i = 0; i < kCellCount; ++i) {
    const std::size_t wordIdx = i / perLong;
    const std::size_t bitOffset = (i % perLong) * bits;
    outData[wordIdx] |=
        static_cast<std::uint64_t>(remappedCells[i]) << bitOffset;
  }
}

bool BiomePaletteContainer::unpack(std::span<const std::string> palette,
                                   std::span<const std::uint64_t> data,
                                   std::string &error) {
  if (palette.empty() || palette.size() > kCellCount) {
    error = "biomes palette size must be in 1..64";
    return false;
  }
  const unsigned bits = biomeBitsForPaletteSize(palette.size());
  if (bits == 0) {
    if (!data.empty()) {
      error = "single biome palette has unexpected data";
      return false;
    }
    palette_.assign(palette.begin(), palette.end());
    indices_.fill(0);
    return true;
  }
  const std::size_t perLong = 64u / bits;
  const std::size_t expectedLongs = (kCellCount + perLong - 1u) / perLong;
  if (data.size() != expectedLongs) {
    error = "biomes palette data length is invalid";
    return false;
  }
  const std::uint64_t mask = (std::uint64_t{1} << bits) - 1ull;
  std::array<std::uint8_t, kCellCount> parsed{};
  for (std::size_t i = 0; i < kCellCount; ++i) {
    const std::size_t wordIdx = i / perLong;
    const std::size_t bitOffset = (i % perLong) * bits;
    const std::size_t val =
        static_cast<std::size_t>((data[wordIdx] >> bitOffset) & mask);
    if (val >= palette.size()) {
      error = "biomes palette index is out of range";
      return false;
    }
    parsed[i] = static_cast<std::uint8_t>(val);
  }
  palette_.assign(palette.begin(), palette.end());
  indices_ = parsed;
  return true;
}

CarvingMask::CarvingMask(int minY, int height) : minY_(minY), height_(height) {
  if (height <= 0)
    throw std::invalid_argument("CarvingMask height must be positive");
}

std::size_t CarvingMask::bitIndex(int localX, int y, int localZ) const {
  if (y < minY_ || y >= minY_ + height_)
    throw std::out_of_range("CarvingMask y coordinate out of range");
  return static_cast<std::size_t>((localX & 15) | ((localZ & 15) << 4) |
                                  ((y - minY_) << 8));
}

bool CarvingMask::get(int localX, int y, int localZ) const {
  if (words_.empty() || y < minY_ || y >= minY_ + height_)
    return false;
  const std::size_t idx = bitIndex(localX, y, localZ);
  const std::size_t wordIdx = idx >> 6u;
  if (wordIdx >= words_.size())
    return false;
  return ((words_[wordIdx] >> (idx & 63u)) & 1ull) != 0ull;
}

void CarvingMask::set(int localX, int y, int localZ) {
  const std::size_t idx = bitIndex(localX, y, localZ);
  const std::size_t wordIdx = idx >> 6u;
  if (words_.empty()) {
    const std::size_t totalBits = static_cast<std::size_t>(256) * height_;
    words_.assign((totalBits + 63u) >> 6u, 0ull);
  }
  words_[wordIdx] |= (std::uint64_t{1} << (idx & 63u));
}

std::vector<std::uint64_t> CarvingMask::toLongArray() const {
  if (words_.empty())
    return {};
  std::size_t end = words_.size();
  while (end > 0 && words_[end - 1] == 0ull)
    --end;
  return std::vector<std::uint64_t>(words_.begin(),
                                    words_.begin() + static_cast<std::ptrdiff_t>(end));
}

CarvingMask CarvingMask::fromLongArray(std::span<const std::uint64_t> data,
                                       int minY, int height) {
  CarvingMask mask(minY, height);
  if (data.empty())
    return mask;
  const std::size_t totalBits = static_cast<std::size_t>(256) * height;
  const std::size_t maxWords = (totalBits + 63u) >> 6u;
  mask.words_.assign(maxWords, 0ull);
  const std::size_t copyCount = std::min(maxWords, data.size());
  for (std::size_t i = 0; i < copyCount; ++i)
    mask.words_[i] = data[i];
  return mask;
}

ProtoChunkSection::ProtoChunkSection(std::int8_t sectionY,
                                     std::string defaultBiome)
    : sectionY_(sectionY), biomes_(std::move(defaultBiome)) {}

BlockState ProtoChunkSection::getBlockState(int localX, int localY,
                                            int localZ) const {
  if (blocks_.empty())
    return BlockState{};
  return BlockState::fromId(blocks_[cellIndex(localX, localY, localZ)]);
}

BlockState ProtoChunkSection::setBlockState(int localX, int localY, int localZ,
                                            const BlockState &state) {
  const std::size_t idx = cellIndex(localX, localY, localZ);
  if (blocks_.empty()) {
    if (state.isAir())
      return BlockState{};
    blocks_.assign(4096, BlockState{}.id());
  }
  const BlockState old = BlockState::fromId(blocks_[idx]);
  if (old.id() == state.id())
    return old;
  if (old.isAir() && !state.isAir())
    ++nonEmptyBlockCount_;
  else if (!old.isAir() && state.isAir())
    --nonEmptyBlockCount_;
  blocks_[idx] = state.id();
  return old;
}

FluidState ProtoChunkSection::getFluidState(int localX, int localY,
                                            int localZ) const {
  if (blocks_.empty())
    return FluidState{};
  return fluidStateOf(getBlockState(localX, localY, localZ));
}

ProtoChunk::ProtoChunk(ChunkTicketPos pos, int minY, int height,
                       std::string defaultBiome)
    : pos_(pos), minY_(minY), height_(height) {
  if ((minY & 15) != 0 || height <= 0 || (height & 15) != 0) {
    throw std::invalid_argument(
        "ProtoChunk minY and height must be multiples of 16");
  }
  const int count = height >> 4;
  sections_.reserve(static_cast<std::size_t>(count));
  const int minSec = minY >> 4;
  for (int i = 0; i < count; ++i) {
    sections_.emplace_back(static_cast<std::int8_t>(minSec + i), defaultBiome);
  }
  postProcessing_.resize(static_cast<std::size_t>(count));
}

void ProtoChunk::setPersistedStatus(ChunkStatusId status) {
  status_.value.store(status, std::memory_order_release);
}

BlockState ProtoChunk::getBlockState(int x, int y, int z) const {
  if (isOutsideBuildHeight(y))
    return defaultState("void_air");
  const ProtoChunkSection &sec = sections_[static_cast<std::size_t>(sectionIndex(y))];
  if (sec.hasOnlyAir())
    return BlockState{};
  return sec.getBlockState(x & 15, y & 15, z & 15);
}

FluidState ProtoChunk::getFluidState(int x, int y, int z) const {
  if (isOutsideBuildHeight(y))
    return FluidState{};
  const ProtoChunkSection &sec = sections_[static_cast<std::size_t>(sectionIndex(y))];
  if (sec.hasOnlyAir())
    return FluidState{};
  return sec.getFluidState(x & 15, y & 15, z & 15);
}

std::optional<BlockState> ProtoChunk::setBlockState(int x, int y, int z,
                                                    const BlockState &state,
                                                    int flags) {
  (void)flags;
  if (isOutsideBuildHeight(y))
    return defaultState("void_air");
  ProtoChunkSection &sec = sections_[static_cast<std::size_t>(sectionIndex(y))];
  if (sec.hasOnlyAir() && state.isAir())
    return state;
  const int lx = x & 15;
  const int ly = y & 15;
  const int lz = z & 15;
  const BlockState oldState = sec.setBlockState(lx, ly, lz, state);
  if (createdHeightmapMask_ != 0) {
    dirtyHeightmapColsMask_[static_cast<std::size_t>((lz << 4) | lx)] |=
        createdHeightmapMask_;
    dirtyHeightmapSummaryMask_ |= createdHeightmapMask_;
  }
  return oldState;
}

std::string_view ProtoChunk::getNoiseBiome(int quartX, int quartY,
                                           int quartZ) const {
  const int minQuartY = minY_ >> 2;
  const int maxQuartY = (maxY() >> 2) - 1;
  const int clampedY = std::clamp(quartY, minQuartY, maxQuartY);
  const int secIdx = (clampedY >> 2) - minSectionY();
  return sections_[static_cast<std::size_t>(secIdx)].biomes().get(
      quartX & 3, clampedY & 3, quartZ & 3);
}

void ProtoChunk::setNoiseBiome(int quartX, int quartY, int quartZ,
                               std::string_view biomeId) {
  const int minQuartY = minY_ >> 2;
  const int maxQuartY = (maxY() >> 2) - 1;
  if (quartY < minQuartY || quartY > maxQuartY)
    return;
  const int secIdx = (quartY >> 2) - minSectionY();
  sections_[static_cast<std::size_t>(secIdx)].biomes().set(
      quartX & 3, quartY & 3, quartZ & 3, biomeId);
}

void ProtoChunk::recomputeHeightmapColumn(Heightmap &hm, int lx, int lz) {
  const int topY = maxY() - 1;
  bool found = false;
  for (int y = topY; y >= minY_; --y) {
    const BlockState st = getBlockState(lx, y, lz);
    if (hm.matches(st)) {
      // Force full update by calling update from above.
      found = true;
      hm.update(lx, y, lz, st, [this, lx, lz](int, int py, int) {
        return getBlockState(lx, py, lz);
      });
      break;
    }
  }
  if (!found) {
    hm.setRawData(std::vector<std::uint64_t>{}, maxY(),
                  [this](int x, int py, int z) { return getBlockState(x, py, z); });
  }
}

void ProtoChunk::flushDirtyHeightmapSingleColumn(int lx, int lz,
                                                 std::uint8_t mask) const {
  if (mask == 0)
    return;
  std::uint8_t active = mask & createdHeightmapMask_;
  if (active == 0)
    return;
  const std::size_t col = static_cast<std::size_t>((lz << 4) | lx);
  dirtyHeightmapColsMask_[col] &= static_cast<std::uint8_t>(~active);

  int remaining = 0;
  for (int t = 0; t < 6; ++t) {
    if (active & (1u << t)) {
      heightmaps_[static_cast<std::size_t>(t)]->setHeight(lx, lz, minY_);
      ++remaining;
    }
  }
  for (int s = sectionCount() - 1; s >= 0 && remaining > 0; --s) {
    const ProtoChunkSection &sec = sections_[static_cast<std::size_t>(s)];
    if (sec.hasOnlyAir())
      continue;
    const int baseY = minY_ + (s << 4);
    for (int ly = 15; ly >= 0 && remaining > 0; --ly) {
      const BlockState st = sec.getBlockState(lx, ly, lz);
      if (st.isAir())
        continue;
      const int availY = baseY + ly + 1;
      for (int t = 0; t < 6; ++t) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << t);
        if ((active & bit) != 0 &&
            heightmaps_[static_cast<std::size_t>(t)]->matches(st)) {
          heightmaps_[static_cast<std::size_t>(t)]->setHeight(lx, lz, availY);
          active = static_cast<std::uint8_t>(active & ~bit);
          --remaining;
        }
      }
    }
  }
}

void ProtoChunk::flushDirtyHeightmapColumns(std::uint8_t mask) const {
  const std::uint8_t targetMask = mask & dirtyHeightmapSummaryMask_ & createdHeightmapMask_;
  if (targetMask == 0)
    return;
  int highestNonEmptySection = -1;
  for (int s = sectionCount() - 1; s >= 0; --s) {
    if (!sections_[static_cast<std::size_t>(s)].hasOnlyAir()) {
      highestNonEmptySection = s;
      break;
    }
  }
  for (int lz = 0; lz < 16; ++lz) {
    for (int lx = 0; lx < 16; ++lx) {
      const std::size_t col = static_cast<std::size_t>((lz << 4) | lx);
      std::uint8_t active = dirtyHeightmapColsMask_[col] & targetMask;
      if (active == 0)
        continue;
      dirtyHeightmapColsMask_[col] &= static_cast<std::uint8_t>(~active);
      int remaining = 0;
      for (int t = 0; t < 6; ++t) {
        if (active & (1u << t)) {
          heightmaps_[static_cast<std::size_t>(t)]->setHeight(lx, lz, minY_);
          ++remaining;
        }
      }
      for (int s = highestNonEmptySection; s >= 0 && remaining > 0; --s) {
        const ProtoChunkSection &sec = sections_[static_cast<std::size_t>(s)];
        if (sec.hasOnlyAir())
          continue;
        const int baseY = minY_ + (s << 4);
        for (int ly = 15; ly >= 0 && remaining > 0; --ly) {
          const BlockState st = sec.getBlockState(lx, ly, lz);
          if (st.isAir())
            continue;
          const int availY = baseY + ly + 1;
          for (int t = 0; t < 6; ++t) {
            const std::uint8_t bit = static_cast<std::uint8_t>(1u << t);
            if ((active & bit) != 0 &&
                heightmaps_[static_cast<std::size_t>(t)]->matches(st)) {
              heightmaps_[static_cast<std::size_t>(t)]->setHeight(lx, lz, availY);
              active = static_cast<std::uint8_t>(active & ~bit);
              --remaining;
            }
          }
        }
      }
    }
  }
  dirtyHeightmapSummaryMask_ &= static_cast<std::uint8_t>(~targetMask);
}

void ProtoChunk::flushDirtyHeightmaps() const {
  flushDirtyHeightmapColumns(dirtyHeightmapSummaryMask_);
}

Heightmap &ProtoChunk::getOrCreateHeightmapUnprimed(HeightmapType type) {
  const std::size_t idx = static_cast<std::size_t>(type);
  if (!heightmaps_[idx].has_value()) {
    heightmaps_[idx].emplace(type, minY_, height_);
    createdHeightmapMask_ |= static_cast<std::uint8_t>(1u << idx);
  }
  return *heightmaps_[idx];
}

const Heightmap *ProtoChunk::findHeightmap(HeightmapType type) const {
  const std::size_t idx = static_cast<std::size_t>(type);
  if (!heightmaps_[idx].has_value())
    return nullptr;
  const std::uint8_t bit = static_cast<std::uint8_t>(1u << idx);
  if ((dirtyHeightmapSummaryMask_ & bit) != 0)
    flushDirtyHeightmapColumns(bit);
  return &*heightmaps_[idx];
}

void ProtoChunk::setHeightmapData(HeightmapType type,
                                  const std::vector<std::uint64_t> &rawData) {
  Heightmap &hm = getOrCreateHeightmapUnprimed(type);
  hm.setRawData(rawData, maxY(), [this](int x, int y, int z) {
    return getBlockState(x, y, z);
  });
  const std::uint8_t bit =
      static_cast<std::uint8_t>(1u << static_cast<std::size_t>(type));
  for (std::uint8_t &colMask : dirtyHeightmapColsMask_)
    colMask &= static_cast<std::uint8_t>(~bit);
  dirtyHeightmapSummaryMask_ &= static_cast<std::uint8_t>(~bit);
}

void ProtoChunk::primeHeightmaps(std::span<const HeightmapType> types) {
  flushDirtyHeightmaps();
  std::uint8_t newlyCreatedMask = 0;
  for (HeightmapType type : types) {
    const std::size_t idx = static_cast<std::size_t>(type);
    if (!heightmaps_[idx].has_value()) {
      heightmaps_[idx].emplace(type, minY_, height_);
      const std::uint8_t bit = static_cast<std::uint8_t>(1u << idx);
      createdHeightmapMask_ |= bit;
      newlyCreatedMask |= bit;
    }
  }
  if (newlyCreatedMask == 0)
    return;
  int highestNonEmptySection = -1;
  for (int s = sectionCount() - 1; s >= 0; --s) {
    if (!sections_[static_cast<std::size_t>(s)].hasOnlyAir()) {
      highestNonEmptySection = s;
      break;
    }
  }
  if (highestNonEmptySection < 0)
    return;
  for (int lz = 0; lz < 16; ++lz) {
    for (int lx = 0; lx < 16; ++lx) {
      std::uint8_t active = newlyCreatedMask;
      int remaining = std::popcount(active);
      for (int s = highestNonEmptySection; s >= 0 && remaining > 0; --s) {
        const ProtoChunkSection &sec = sections_[static_cast<std::size_t>(s)];
        if (sec.hasOnlyAir())
          continue;
        const int baseY = minY_ + (s << 4);
        for (int ly = 15; ly >= 0 && remaining > 0; --ly) {
          const BlockState st = sec.getBlockState(lx, ly, lz);
          if (st.isAir())
            continue;
          const int availY = baseY + ly + 1;
          for (int t = 0; t < 6; ++t) {
            const std::uint8_t bit = static_cast<std::uint8_t>(1u << t);
            if ((active & bit) != 0 &&
                heightmaps_[static_cast<std::size_t>(t)]->matches(st)) {
              heightmaps_[static_cast<std::size_t>(t)]->setHeight(lx, lz, availY);
              active = static_cast<std::uint8_t>(active & ~bit);
              --remaining;
            }
          }
        }
      }
    }
  }
}

int ProtoChunk::getHeight(HeightmapType type, int x, int z) {
  const int lx = x & 15;
  const int lz = z & 15;
  const std::size_t idx = static_cast<std::size_t>(type);
  const std::uint8_t bit = static_cast<std::uint8_t>(1u << idx);
  if (!heightmaps_[idx].has_value()) {
    const std::array<HeightmapType, 1> single{type};
    primeHeightmaps(single);
  } else {
    const std::size_t col = static_cast<std::size_t>((lz << 4) | lx);
    const std::uint8_t colDirty = dirtyHeightmapColsMask_[col];
    if ((colDirty & bit) != 0)
      flushDirtyHeightmapSingleColumn(lx, lz, colDirty);
  }
  return heightmaps_[idx]->firstAvailable(lx, lz) - 1;
}

void ProtoChunk::markPosForPostprocessing(int x, int y, int z) {
  if (isOutsideBuildHeight(y))
    return;
  const int secIdx = sectionIndex(y);
  postProcessing_[static_cast<std::size_t>(secIdx)].push_back(
      packOffsetCoordinates(x, y, z));
}

void ProtoChunk::addPackedPostProcess(std::span<const std::int16_t> offsets,
                                      int secIdx) {
  if (secIdx < 0 || static_cast<std::size_t>(secIdx) >= postProcessing_.size())
    return;
  auto &dst = postProcessing_[static_cast<std::size_t>(secIdx)];
  dst.insert(dst.end(), offsets.begin(), offsets.end());
}

void ProtoChunk::clearPostProcessing() {
  for (auto &list : postProcessing_)
    list.clear();
}

CarvingMask &ProtoChunk::getOrCreateCarvingMask() {
  if (!carvingMask_)
    carvingMask_.emplace(minY_, height_);
  return *carvingMask_;
}

void ProtoChunk::setCarvingMask(CarvingMask mask) {
  carvingMask_ = std::move(mask);
}

const std::vector<std::int64_t> &
ProtoChunk::structureReferences(std::string_view structureId) const {
  static const std::vector<std::int64_t> empty;
  auto it = structureReferences_.find(structureId);
  return it == structureReferences_.end() ? empty : it->second;
}

void ProtoChunk::addStructureReference(std::string_view structureId,
                                       std::int64_t packedStartChunkPos) {
  auto &vec = structureReferences_[std::string(structureId)];
  if (std::find(vec.begin(), vec.end(), packedStartChunkPos) == vec.end())
    vec.push_back(packedStartChunkPos);
}

ImposterProtoChunk::ImposterProtoChunk(World &world, ChunkTicketPos pos,
                                       bool allowWrites)
    : world_(world), pos_(pos), minY_(world.minY()),
      height_(world.maxY() - world.minY()), allowWrites_(allowWrites) {}

BlockState ImposterProtoChunk::getBlockState(int x, int y, int z) const {
  return world_.getBlock({(pos_.x << 4) + (x & 15), y, (pos_.z << 4) + (z & 15)});
}

FluidState ImposterProtoChunk::getFluidState(int x, int y, int z) const {
  return fluidStateOf(world_.getBlock(
      {(pos_.x << 4) + (x & 15), y, (pos_.z << 4) + (z & 15)}));
}

std::optional<BlockState> ImposterProtoChunk::setBlockState(
    int x, int y, int z, const BlockState &state, int flags) {
  (void)flags;
  if (!allowWrites_)
    return std::nullopt;
  const Pos p{(pos_.x << 4) + (x & 15), y, (pos_.z << 4) + (z & 15)};
  const BlockState old = world_.getBlock(p);
  world_.setBlock(p, state, 0);
  return old;
}

std::string_view ImposterProtoChunk::getNoiseBiome(int, int, int) const {
  // TODO: 当 World::Chunk 接入 BiomeSection 存储后，从底层区段容器读取真实生物群系。
  return "minecraft:plains";
}

void ImposterProtoChunk::setNoiseBiome(int, int, int, std::string_view) {}

int ImposterProtoChunk::getHeight(HeightmapType type, int x, int z) {
  return world_.height(fixType(type), (pos_.x << 4) + (x & 15),
                       (pos_.z << 4) + (z & 15));
}

namespace {

nbt::Tag makeIntTag(std::int32_t v) {
  nbt::Tag t;
  t.type = 3;
  t.number = v;
  return t;
}

nbt::Tag makeByteTag(std::int8_t v) {
  nbt::Tag t;
  t.type = 1;
  t.number = v;
  return t;
}

nbt::Tag makeShortTag(std::int16_t v) {
  nbt::Tag t;
  t.type = 2;
  t.number = v;
  return t;
}

nbt::Tag makeLongTag(std::int64_t v) {
  nbt::Tag t;
  t.type = 4;
  t.number = v;
  return t;
}

nbt::Tag makeStringTag(std::string s) {
  nbt::Tag t;
  t.type = 8;
  t.text = std::move(s);
  return t;
}

void addCompoundField(nbt::Tag &compound, std::string key, nbt::Tag value) {
  compound.compoundOrder.push_back(key);
  compound.compound.emplace(std::move(key), std::move(value));
}

nbt::Tag makeBlockStatePaletteEntry(const BlockState &state) {
  nbt::Tag entry;
  entry.type = 10;
  addCompoundField(entry, "Name", makeStringTag(std::string(state.name())));
  auto props = state.properties();
  if (!props.empty()) {
    nbt::Tag pTag;
    pTag.type = 10;
    for (auto &[k, v] : props)
      addCompoundField(pTag, k, makeStringTag(v));
    addCompoundField(entry, "Properties", std::move(pTag));
  }
  return entry;
}

terrain::PackedBlockStates packSectionBlocks(const ProtoChunkSection &sec) {
  terrain::PackedBlockStates packed;
  if (sec.hasOnlyAir()) {
    const BlockState air{};
    terrain::PaletteEntry pe;
    pe.name = std::string(air.name());
    pe.state = air;
    pe.supported = true;
    packed.palette.push_back(std::move(pe));
    packed.bitsPerEntry = 0;
    return packed;
  }

  std::span<const BlockState::Id> raw = sec.rawBlocks();
  std::unordered_map<BlockState::Id, std::uint16_t> idToIndex;
  std::array<std::uint16_t, 4096> cellIndices{};
  for (std::size_t i = 0; i < 4096; ++i) {
    const BlockState::Id sid = raw[i];
    auto it = idToIndex.find(sid);
    if (it == idToIndex.end()) {
      const std::uint16_t idx =
          static_cast<std::uint16_t>(packed.palette.size());
      const BlockState st = BlockState::fromId(sid);
      terrain::PaletteEntry pe;
      pe.name = std::string(st.name());
      pe.properties = st.properties();
      pe.state = st;
      pe.supported = true;
      packed.palette.push_back(std::move(pe));
      it = idToIndex.emplace(sid, idx).first;
    }
    cellIndices[i] = it->second;
  }
  if (packed.palette.size() <= 1) {
    packed.bitsPerEntry = 0;
    return packed;
  }
  const unsigned bits = std::max(
      4u, static_cast<unsigned>(std::bit_width(packed.palette.size() - 1)));
  packed.bitsPerEntry = static_cast<int>(bits);
  const std::size_t perLong = 64u / bits;
  const std::size_t longCount = (4096u + perLong - 1u) / perLong;
  packed.data.assign(longCount, 0ull);
  for (std::size_t i = 0; i < 4096; ++i) {
    packed.data[i / perLong] |=
        static_cast<std::uint64_t>(cellIndices[i]) << ((i % perLong) * bits);
  }
  return packed;
}

nbt::Tag makeBlockStatesTag(const terrain::PackedBlockStates &packed) {
  nbt::Tag container;
  container.type = 10;
  nbt::Tag pal;
  pal.type = 9;
  pal.listElementType = 10;
  pal.list.reserve(packed.palette.size());
  for (const auto &entry : packed.palette)
    pal.list.push_back(makeBlockStatePaletteEntry(entry.state));
  addCompoundField(container, "palette", std::move(pal));
  if (!packed.data.empty()) {
    nbt::Tag dataTag;
    dataTag.type = 12;
    dataTag.longs.reserve(packed.data.size());
    for (std::uint64_t w : packed.data)
      dataTag.longs.push_back(static_cast<std::int64_t>(w));
    addCompoundField(container, "data", std::move(dataTag));
  }
  return container;
}

nbt::Tag makeBiomesTag(const BiomePaletteContainer &biomes) {
  std::vector<std::string> palette;
  std::vector<std::uint64_t> data;
  int bits = 0;
  biomes.pack(palette, data, bits);
  nbt::Tag container;
  container.type = 10;
  nbt::Tag pal;
  pal.type = 9;
  pal.listElementType = 8;
  pal.list.reserve(palette.size());
  for (std::string &name : palette)
    pal.list.push_back(makeStringTag(std::move(name)));
  addCompoundField(container, "palette", std::move(pal));
  if (!data.empty()) {
    nbt::Tag dataTag;
    dataTag.type = 12;
    dataTag.longs.reserve(data.size());
    for (std::uint64_t w : data)
      dataTag.longs.push_back(static_cast<std::int64_t>(w));
    addCompoundField(container, "data", std::move(dataTag));
  }
  return container;
}

nbt::Tag makeSavedTickList(const std::vector<terrain::SavedTick> &ticks) {
  nbt::Tag list;
  list.type = 9;
  list.listElementType = ticks.empty() ? 0 : 10;
  list.list.reserve(ticks.size());
  for (const terrain::SavedTick &tick : ticks) {
    nbt::Tag entry;
    entry.type = 10;
    addCompoundField(entry, "i", makeStringTag(tick.id));
    addCompoundField(entry, "x", makeIntTag(tick.x));
    addCompoundField(entry, "y", makeIntTag(tick.y));
    addCompoundField(entry, "z", makeIntTag(tick.z));
    addCompoundField(entry, "t", makeIntTag(tick.delay));
    addCompoundField(entry, "p", makeIntTag(tick.priority));
    list.list.push_back(std::move(entry));
  }
  return list;
}

} // namespace

nbt::RootTag encodeProtoChunkToNbt(const ProtoChunk &chunk,
                                   std::int64_t lastUpdateTime) {
  const ChunkStatus &status = ChunkStatus::byId(chunk.persistedStatus());
  nbt::RootTag root;
  root.value.type = 10;
  addCompoundField(root.value, "DataVersion", makeIntTag(4786));
  addCompoundField(root.value, "xPos", makeIntTag(chunk.pos().x));
  addCompoundField(root.value, "yPos", makeIntTag(chunk.minSectionY()));
  addCompoundField(root.value, "zPos", makeIntTag(chunk.pos().z));
  addCompoundField(root.value, "LastUpdate", makeLongTag(lastUpdateTime));
  addCompoundField(root.value, "InhabitedTime",
                   makeLongTag(chunk.inhabitedTime()));
  addCompoundField(root.value, "Status",
                   makeStringTag(std::string(status.name())));

  nbt::Tag sectionsTag;
  sectionsTag.type = 9;
  sectionsTag.listElementType = 10;
  sectionsTag.list.reserve(chunk.sections().size());
  for (const ProtoChunkSection &sec : chunk.sections()) {
    nbt::Tag secTag;
    secTag.type = 10;
    addCompoundField(secTag, "Y", makeByteTag(sec.sectionY()));
    addCompoundField(secTag, "block_states",
                     makeBlockStatesTag(packSectionBlocks(sec)));
    addCompoundField(secTag, "biomes", makeBiomesTag(sec.biomes()));
    if (sec.blockLight()) {
      nbt::Tag bl;
      bl.type = 7;
      bl.bytes.reserve(2048);
      for (std::uint8_t b : *sec.blockLight())
        bl.bytes.push_back(static_cast<std::int8_t>(b));
      addCompoundField(secTag, "BlockLight", std::move(bl));
    }
    if (sec.skyLight()) {
      nbt::Tag sl;
      sl.type = 7;
      sl.bytes.reserve(2048);
      for (std::uint8_t b : *sec.skyLight())
        sl.bytes.push_back(static_cast<std::int8_t>(b));
      addCompoundField(secTag, "SkyLight", std::move(sl));
    }
    sectionsTag.list.push_back(std::move(secTag));
  }
  addCompoundField(root.value, "sections", std::move(sectionsTag));

  if (chunk.isLightCorrect())
    addCompoundField(root.value, "isLightOn", makeByteTag(1));

  nbt::Tag beList;
  beList.type = 9;
  beList.listElementType = chunk.pendingBlockEntities().empty() ? 0 : 10;
  beList.list = chunk.pendingBlockEntities();
  addCompoundField(root.value, "block_entities", std::move(beList));

  if (status.chunkType() == ChunkType::ProtoChunk) {
    nbt::Tag entList;
    entList.type = 9;
    entList.listElementType = chunk.entities().empty() ? 0 : 10;
    entList.list = chunk.entities();
    addCompoundField(root.value, "entities", std::move(entList));

    if (const CarvingMask *mask = chunk.carvingMask(); mask && !mask->empty()) {
      auto words = mask->toLongArray();
      if (!words.empty()) {
        nbt::Tag maskTag;
        maskTag.type = 12;
        maskTag.longs.reserve(words.size());
        for (std::uint64_t w : words)
          maskTag.longs.push_back(static_cast<std::int64_t>(w));
        addCompoundField(root.value, "carving_mask", std::move(maskTag));
      }
    }
  }

  addCompoundField(root.value, "block_ticks",
                   makeSavedTickList(chunk.blockTicks()));
  addCompoundField(root.value, "fluid_ticks",
                   makeSavedTickList(chunk.fluidTicks()));

  nbt::Tag postProcTag;
  postProcTag.type = 9;
  postProcTag.listElementType = 9;
  postProcTag.list.reserve(chunk.postProcessing().size());
  for (const auto &secList : chunk.postProcessing()) {
    nbt::Tag inner;
    inner.type = 9;
    inner.listElementType = secList.empty() ? 0 : 2;
    inner.list.reserve(secList.size());
    for (std::int16_t packedOffset : secList)
      inner.list.push_back(makeShortTag(packedOffset));
    postProcTag.list.push_back(std::move(inner));
  }
  addCompoundField(root.value, "PostProcessing", std::move(postProcTag));

  nbt::Tag hmTag;
  hmTag.type = 10;
  for (HeightmapType hmType : status.heightmapsAfter()) {
    if (const Heightmap *hm = chunk.findHeightmap(hmType)) {
      nbt::Tag arr;
      arr.type = 12;
      arr.longs.reserve(hm->rawData().size());
      for (std::uint64_t w : hm->rawData())
        arr.longs.push_back(static_cast<std::int64_t>(w));
      addCompoundField(hmTag, std::string(heightmapSerializationKey(hmType)),
                       std::move(arr));
    }
  }
  addCompoundField(root.value, "Heightmaps", std::move(hmTag));

  nbt::Tag structuresTag;
  structuresTag.type = 10;
  nbt::Tag startsTag;
  startsTag.type = 10;
  addCompoundField(structuresTag, "starts", std::move(startsTag));
  nbt::Tag refsTag;
  refsTag.type = 10;
  for (const auto &[id, longs] : chunk.allStructureReferences()) {
    nbt::Tag arr;
    arr.type = 12;
    arr.longs.reserve(longs.size());
    for (std::int64_t packedPos : longs)
      arr.longs.push_back(packedPos);
    addCompoundField(refsTag, id, std::move(arr));
  }
  addCompoundField(structuresTag, "References", std::move(refsTag));
  addCompoundField(root.value, "structures", std::move(structuresTag));
  return root;
}

std::optional<ProtoChunk>
decodeProtoChunkFromDecoded(const terrain::DecodedChunk &decoded, int minY,
                            int height, std::string &error) {
  error.clear();
  if (decoded.status != terrain::DecodeStatus::Ok) {
    error = decoded.detail.empty() ? "decoded chunk failed" : decoded.detail;
    return std::nullopt;
  }
  const ChunkStatus *cs = ChunkStatus::byName(decoded.statusName);
  if (!cs) {
    error = "unknown chunk Status: " + decoded.statusName;
    return std::nullopt;
  }
  ProtoChunk chunk({decoded.chunkX, decoded.chunkZ}, minY, height);
  chunk.setPersistedStatus(cs->id());
  chunk.setLightCorrect(decoded.isLightOn);
  chunk.setInhabitedTime(decoded.inhabitedTime);

  const int minSec = chunk.minSectionY();
  const int secCount = chunk.sectionCount();
  for (const terrain::Section &srcSec : decoded.sections) {
    const int idx = static_cast<int>(srcSec.y) - minSec;
    if (idx < 0 || idx >= secCount)
      continue;
    ProtoChunkSection &dstSec = chunk.section(idx);
    if (srcSec.blockStates) {
      const auto &bs = *srcSec.blockStates;
      if (bs.bitsPerEntry == 0) {
        if (!bs.palette.empty() && !bs.palette[0].state.isAir()) {
          for (int ly = 0; ly < 16; ++ly) {
            for (int lz = 0; lz < 16; ++lz) {
              for (int lx = 0; lx < 16; ++lx)
                dstSec.setBlockState(lx, ly, lz, bs.palette[0].state);
            }
          }
        }
      } else {
        for (std::size_t cell = 0; cell < terrain::PackedBlockStates::CellCount;
             ++cell) {
          const std::size_t palIdx = bs.cell(cell);
          if (palIdx >= bs.palette.size()) {
            error = "block state palette index out of range";
            return std::nullopt;
          }
          const BlockState &st = bs.palette[palIdx].state;
          if (!st.isAir()) {
            const int lx = static_cast<int>(cell & 15u);
            const int lz = static_cast<int>((cell >> 4u) & 15u);
            const int ly = static_cast<int>((cell >> 8u) & 15u);
            dstSec.setBlockState(lx, ly, lz, st);
          }
        }
      }
    }
    if (srcSec.biomes) {
      std::string bioErr;
      if (!dstSec.biomes().unpack(srcSec.biomes->palette, srcSec.biomes->data,
                                  bioErr)) {
        error = bioErr;
        return std::nullopt;
      }
    }
    dstSec.blockLight() = srcSec.blockLight;
    dstSec.skyLight() = srcSec.skyLight;
  }

  if (!decoded.carvingMask.empty()) {
    chunk.setCarvingMask(
        CarvingMask::fromLongArray(decoded.carvingMask, minY, height));
  }
  for (std::size_t i = 0;
       i < decoded.postProcessing.size() && i < static_cast<std::size_t>(secCount);
       ++i) {
    chunk.addPackedPostProcess(decoded.postProcessing[i], static_cast<int>(i));
  }
  for (const auto &[hmType, data] : decoded.heightmaps) {
    chunk.setHeightmapData(hmType, data);
  }
  chunk.blockTicks() = decoded.blockTicks;
  chunk.fluidTicks() = decoded.fluidTicks;
  chunk.movingPistons() = decoded.movingPistons;
  chunk.pendingBlockEntities() = decoded.blockEntities;
  chunk.entities() = decoded.entities;

  if (const nbt::Tag *structures = decoded.root.value.get("structures")) {
    if (const nbt::Tag *refs = structures->get("References");
        refs && refs->type == 10) {
      for (const auto &[structId, arrTag] : refs->compound) {
        if (arrTag.type == 12) {
          for (std::int64_t packedPos : arrTag.longs)
            chunk.addStructureReference(structId, packedPos);
        }
      }
    }
  }
  return chunk;
}

std::optional<ProtoChunk>
decodeProtoChunkNbt(std::span<const std::uint8_t> nbtBytes, int minY, int height,
                    std::string &error) {
  terrain::DecodeOptions opts;
  opts.requireFullStatus = false;
  opts.retainRawBlockEntities = true;
  const terrain::DecodedChunk decoded = terrain::decodeChunkNbt(nbtBytes, opts);
  return decodeProtoChunkFromDecoded(decoded, minY, height, error);
}

LifecycleLoadedChunk promoteProtoChunkToLoaded(ProtoChunk &&chunk,
                                               std::uint64_t epoch) {
  LifecycleLoadedChunk out;
  out.position = chunk.pos();
  out.epoch = epoch;
  out.chunkX = chunk.pos().x;
  out.chunkZ = chunk.pos().z;
  out.statusName = "minecraft:full";
  out.isLightOn = chunk.isLightCorrect();
  out.sections.reserve(chunk.sections().size());
  for (ProtoChunkSection &sec : chunk.sections()) {
    if (sec.hasOnlyAir() && !sec.blockLight() && !sec.skyLight())
      continue;
    terrain::Section dst;
    dst.y = sec.sectionY();
    if (!sec.hasOnlyAir())
      dst.blockStates = packSectionBlocks(sec);
    terrain::PackedBiomes packedBiomes;
    sec.biomes().pack(packedBiomes.palette, packedBiomes.data,
                      packedBiomes.bitsPerEntry);
    dst.biomes = std::move(packedBiomes);
    dst.blockLight = std::move(sec.blockLight());
    dst.skyLight = std::move(sec.skyLight());
    out.sections.push_back(std::move(dst));
  }
  out.blockTicks = std::move(chunk.blockTicks());
  out.fluidTicks = std::move(chunk.fluidTicks());
  out.movingPistons = std::move(chunk.movingPistons());
  out.entities = std::move(chunk.entities());
  return out;
}

} // namespace redstone::worldgen
