#include "redstone/light/initial_chunk_lighter.hpp"

#include "light/common.hpp"
#include "light/spsc_ring.hpp"
#include "redstone/worldgen/proto_chunk.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace redstone {
namespace {

using light_internal::emptyLightProperties;
using light_internal::emptyShape;
using light_internal::hasLightFaceRect;
using light_internal::lightOpacity;
using light_internal::sectionViewOfLiveChunk;
using light_internal::TaskQueue;
using worldgen::ProtoChunk;
using worldgen::ProtoChunkLightSummary;

constexpr std::array<int, 6> kDirDx{{0, 0, 0, 0, -1, 1}};
constexpr std::array<int, 6> kDirDy{{-1, 1, 0, 0, 0, 0}};
constexpr std::array<int, 6> kDirDz{{0, 0, -1, 1, 0, 0}};

bool isSourceEdgeOccluded(const BlockLightProperties &top,
                          const BlockLightProperties &bottom) noexcept {
  if (bottom.supported && bottom.dampening != 0)
    return true;
  return lightFaceShapesOcclude(top, bottom, LightDirection::Down);
}

void captureChunkViews(const BaseChunk &chunk, LightSectionView *outViews,
                       int minSecY, int secCount) {
  std::shared_lock<std::shared_mutex> lock;
  if (chunk.resizeMutex != nullptr)
    lock = std::shared_lock<std::shared_mutex>(*chunk.resizeMutex);
  for (int i = 0; i < secCount; ++i)
    outViews[i] = sectionViewOfLiveChunk(chunk, minSecY + i, minSecY, secCount);
}

std::shared_ptr<const ProtoChunkLightSummary>
buildSummaryFromViews(const LightSectionView *views, int minSecY, int secCount,
                      const LightOpticalTable &optical, bool skyLightEnabled,
                      std::uint64_t mutationSeq = 0) {
  auto summary = std::make_shared<ProtoChunkLightSummary>();
  summary->skyLightEnabled = skyLightEnabled;
  summary->mutationSeq = mutationSeq;
  const int minY = minSecY * 16;
  const int sentinel = minY - 1;

  for (int s = 0; s < secCount; ++s) {
    const LightSectionView &v = views[s];
    if (!v.empty && s < 32)
      summary->nonEmptySectionMask |= (std::uint32_t{1} << s);
    const int baseY = (minSecY + s) * 16;
    if (v.bits == 0) {
      const auto &props = optical[optical.classForState(v.single)].properties;
      if (!props.supported) {
        summary->unsupportedCells += static_cast<std::uint32_t>(kLightVolume);
      } else if (props.emission > 0) {
        summary->emissiveBlocks.reserve(summary->emissiveBlocks.size() +
                                        kLightVolume);
        for (int idx = 0; idx < kLightVolume; ++idx) {
          summary->emissiveBlocks.push_back(
              {static_cast<std::uint16_t>(idx),
               static_cast<std::int16_t>(baseY + (idx >> 8)), props.emission});
        }
      }
      continue;
    }

    bool anyEmission = false;
    bool anyUnsupported = false;
    for (BlockState::Id sid : v.palette) {
      const auto &props = optical[optical.classForState(sid)].properties;
      if (!props.supported)
        anyUnsupported = true;
      else if (props.emission > 0)
        anyEmission = true;
      if (anyEmission && anyUnsupported)
        break;
    }
    if (!anyEmission && !anyUnsupported)
      continue;

    for (int idx = 0; idx < kLightVolume; ++idx) {
      const auto &props = optical[optical.classForState(
          v.atomicStateAt(static_cast<std::size_t>(idx)))].properties;
      if (!props.supported) {
        ++summary->unsupportedCells;
      } else if (props.emission > 0) {
        summary->emissiveBlocks.push_back(
            {static_cast<std::uint16_t>(idx),
             static_cast<std::int16_t>(baseY + (idx >> 8)), props.emission});
      }
    }
  }

  if (!skyLightEnabled) {
    summary->skySources.fill(sentinel);
    summary->minSkySourceY = sentinel;
    summary->maxSkySourceY = sentinel;
    return summary;
  }

  const auto &empty = emptyLightProperties();
  summary->skySources.fill(sentinel);
  std::array<const BlockLightProperties *, 256> tops;
  tops.fill(&empty);
  std::array<bool, 256> done{};
  int remainingOpen = 256;
  int nonEmptyTopCount = 0;

  for (int s = secCount - 1; s >= 0 && remainingOpen > 0; --s) {
    const LightSectionView &v = views[s];
    const int baseY = (minSecY + s) * 16;
    if (v.bits == 0) {
      const auto &props = optical[optical.classForState(v.single)].properties;
      const bool uniformEmpty =
          v.empty && props.supported && props.dampening == 0 &&
          !hasLightFaceRect(props, 0) && !hasLightFaceRect(props, 1);
      if (uniformEmpty) {
        if (nonEmptyTopCount == 0)
          continue;
        for (int col = 0; col < 256; ++col) {
          if (done[col])
            continue;
          if (tops[col] != &empty) {
            if (isSourceEdgeOccluded(*tops[col], empty)) {
              summary->skySources[col] = baseY + 16;
              done[col] = true;
              --remainingOpen;
            } else {
              tops[col] = &empty;
            }
            --nonEmptyTopCount;
          }
        }
        continue;
      }
      const bool selfOccludes = isSourceEdgeOccluded(props, props);
      for (int col = 0; col < 256; ++col) {
        if (done[col])
          continue;
        if (isSourceEdgeOccluded(*tops[col], props)) {
          summary->skySources[col] = baseY + 16;
          done[col] = true;
          --remainingOpen;
          if (tops[col] != &empty)
            --nonEmptyTopCount;
        } else if (selfOccludes) {
          summary->skySources[col] = baseY + 15;
          done[col] = true;
          --remainingOpen;
          if (tops[col] != &empty)
            --nonEmptyTopCount;
        } else {
          if (tops[col] == &empty)
            ++nonEmptyTopCount;
          tops[col] = &props;
        }
      }
      continue;
    }

    if (v.empty) {
      bool allUniformEmpty = true;
      for (BlockState::Id sid : v.palette) {
        const auto &props = optical[optical.classForState(sid)].properties;
        if (!props.supported || props.dampening != 0 ||
            hasLightFaceRect(props, 0) || hasLightFaceRect(props, 1)) {
          allUniformEmpty = false;
          break;
        }
      }
      if (allUniformEmpty) {
        if (nonEmptyTopCount == 0)
          continue;
        for (int col = 0; col < 256; ++col) {
          if (done[col])
            continue;
          if (tops[col] != &empty) {
            if (isSourceEdgeOccluded(*tops[col], empty)) {
              summary->skySources[col] = baseY + 16;
              done[col] = true;
              --remainingOpen;
            } else {
              tops[col] = &empty;
            }
            --nonEmptyTopCount;
          }
        }
        continue;
      }
    }

    for (int localY = 15; localY >= 0 && remainingOpen > 0; --localY) {
      const std::size_t planeBase = static_cast<std::size_t>(localY) << 8;
      const int edgeResultY = baseY + localY + 1;
      for (int col = 0; col < 256; ++col) {
        if (done[col])
          continue;
        const auto &bottom =
            optical[optical.classForState(
                        v.atomicStateAt(planeBase + static_cast<std::size_t>(col)))]
                .properties;
        if (isSourceEdgeOccluded(*tops[col], bottom)) {
          summary->skySources[col] = edgeResultY;
          done[col] = true;
          --remainingOpen;
          if (tops[col] != &empty)
            --nonEmptyTopCount;
        } else {
          if (tops[col] == &empty)
            ++nonEmptyTopCount;
          tops[col] = &bottom;
        }
      }
    }
  }

  int minSource = summary->skySources[0];
  int maxSource = summary->skySources[0];
  for (int col = 1; col < 256; ++col) {
    minSource = std::min(minSource, summary->skySources[col]);
    maxSource = std::max(maxSource, summary->skySources[col]);
  }
  summary->minSkySourceY = minSource;
  summary->maxSkySourceY = maxSource;
  return summary;
}

// 64-bit packed BFS task:
//   [0..5]   gx (0..47)
//   [6..11]  gz (0..47)
//   [12..23] gy (0..lightHeight-1)
//   [24..27] level (0..15)
//   [28..33] directions (6 bits)
//   [34]     fromEmpty
//   [35]     fromEmission
inline std::uint64_t packTask(int gx, int gz, int gy, std::uint8_t level,
                              std::uint8_t dirs, bool fromEmpty,
                              bool fromEmission) noexcept {
  return static_cast<std::uint64_t>(gx & 63) |
         (static_cast<std::uint64_t>(gz & 63) << 6) |
         (static_cast<std::uint64_t>(gy & 4095) << 12) |
         (static_cast<std::uint64_t>(level & 15) << 24) |
         (static_cast<std::uint64_t>(dirs & 63) << 28) |
         (static_cast<std::uint64_t>(fromEmpty ? 1 : 0) << 34) |
         (static_cast<std::uint64_t>(fromEmission ? 1 : 0) << 35);
}

inline int taskGx(std::uint64_t t) noexcept { return static_cast<int>(t & 63); }
inline int taskGz(std::uint64_t t) noexcept {
  return static_cast<int>((t >> 6) & 63);
}
inline int taskGy(std::uint64_t t) noexcept {
  return static_cast<int>((t >> 12) & 4095);
}
inline int taskLevel(std::uint64_t t) noexcept {
  return static_cast<int>((t >> 24) & 15);
}
inline std::uint8_t taskDirs(std::uint64_t t) noexcept {
  return static_cast<std::uint8_t>((t >> 28) & 63);
}
inline bool taskFromEmpty(std::uint64_t t) noexcept {
  return ((t >> 34) & 1) != 0;
}
inline bool taskFromEmission(std::uint64_t t) noexcept {
  return ((t >> 35) & 1) != 0;
}
inline std::uint32_t taskSectionKey(std::uint64_t t) noexcept {
  return static_cast<std::uint32_t>(((taskGx(t) >> 4) << 16) |
                                    ((taskGz(t) >> 4) << 12) |
                                    (taskGy(t) >> 4));
}

struct InitialLightScratch {
  static constexpr int kMaxLightSections = kSectionCapacity + 2;

  struct SectionSlot {
    std::int8_t state = -2; // -2: Missing, -1: Mixed, 0..15: Uniform
    std::uint8_t *data = nullptr;
  };

  std::array<std::array<SectionSlot, kMaxLightSections>, 9> grid{};
  std::array<std::array<int, kMaxLightSections>, 9> inherited{};
  std::array<std::uint64_t, 9> eligibleMask{};
  std::vector<std::unique_ptr<std::array<std::uint8_t, kLightVolume>>> slabs;
  std::size_t nextFreeSlab = 0;

  TaskQueue<std::uint64_t> mainQueue;
  TaskQueue<std::uint64_t> currentQueue;

  std::uint8_t *acquireSlab(std::uint8_t fillByte) {
    if (nextFreeSlab >= slabs.size())
      slabs.push_back(std::make_unique<std::array<std::uint8_t, kLightVolume>>());
    std::uint8_t *ptr = slabs[nextFreeSlab++]->data();
    std::memset(ptr, fillByte, kLightVolume);
    return ptr;
  }

  void resetGrid(int lightSecCount) {
    nextFreeSlab = 0;
    for (int ci = 0; ci < 9; ++ci) {
      for (int sy = 0; sy < lightSecCount; ++sy)
        grid[ci][sy] = {-2, nullptr};
    }
    mainQueue.clear();
    currentQueue.clear();
  }
};

LightLayerSnapshot foldSectionSlot(const InitialLightScratch::SectionSlot &slot) {
  if (slot.state == -2)
    return LightLayerSnapshot::missing();
  if (slot.state >= 0)
    return LightLayerSnapshot::uniform(static_cast<std::uint8_t>(slot.state));
  const std::uint8_t *data = slot.data;
  const std::uint8_t first = data[0];
  bool allEqual = true;
  for (int i = 1; i < kLightVolume; ++i) {
    if (data[i] != first) {
      allEqual = false;
      break;
    }
  }
  if (allEqual)
    return LightLayerSnapshot::uniform(first);
  std::array<std::uint8_t, kLightVolume> arr;
  std::memcpy(arr.data(), data, kLightVolume);
  return LightLayerSnapshot::mixed(arr);
}

bool crossedSkySectionEdge(int direction, int localX, int localZ) noexcept {
  if (direction == 2)
    return localZ == 15;
  if (direction == 3)
    return localZ == 0;
  if (direction == 4)
    return localX == 15;
  if (direction == 5)
    return localX == 0;
  return false;
}

} // namespace

std::shared_ptr<const ProtoChunkLightSummary>
computeProtoChunkLightSummary(const BaseChunk &chunk,
                              const LightOpticalTable &optical,
                              bool skyLightEnabled) {
  const int secCount = chunk.sectionCount();
  std::array<LightSectionView, kSectionCapacity> views{};
  std::uint64_t seq = 0;
  if (chunk.isProtoChunk()) {
    const auto *proto = static_cast<const ProtoChunk *>(&chunk);
    for (int attempt = 0; attempt < 4; ++attempt) {
      seq = proto->blockMutationSeq();
      captureChunkViews(chunk, views.data(), chunk.minSectionY(), secCount);
      if (proto->blockMutationSeq() == seq)
        break;
    }
  } else {
    captureChunkViews(chunk, views.data(), chunk.minSectionY(), secCount);
  }
  return buildSummaryFromViews(views.data(), chunk.minSectionY(), secCount,
                               optical, skyLightEnabled, seq);
}

std::shared_ptr<PrecomputedChunkLight> computeInitialChunkLight(
    const std::array<const BaseChunk *, 9> &chunks3x3,
    const LightOpticalTable &optical, bool skyLightEnabled) {
  auto result = std::make_shared<PrecomputedChunkLight>();
  const BaseChunk *center = chunks3x3[4];
  if (!center)
    return result;

  const int minSecY = center->minSectionY();
  const int secCount = center->sectionCount();
  const int lightSecCount = secCount + 2;
  if (secCount <= 0 || lightSecCount > InitialLightScratch::kMaxLightSections)
    return result;

  const int minY = minSecY * 16;
  const int sentinel = minY - 1;
  const int lightBottom = (minSecY - 1) * 16;
  const int lightTop = (minSecY + secCount + 1) * 16;
  const int lightHeight = lightSecCount * 16;

  std::array<std::array<LightSectionView, kSectionCapacity>, 9> blockGrid{};
  std::array<std::shared_ptr<const ProtoChunkLightSummary>, 9> summaries{};

  for (int ci = 0; ci < 9; ++ci) {
    const BaseChunk *c = chunks3x3[ci];
    if (!c)
      continue;
    if (c->isProtoChunk()) {
      const auto *proto = static_cast<const ProtoChunk *>(c);
      std::uint64_t seqBefore = 0;
      for (int attempt = 0; attempt < 4; ++attempt) {
        seqBefore = proto->blockMutationSeq();
        captureChunkViews(*c, blockGrid[ci].data(), minSecY, secCount);
        if (proto->blockMutationSeq() == seqBefore)
          break;
      }
      auto summary = proto->lightSummary();
      if (!summary || summary->mutationSeq != seqBefore ||
          summary->skyLightEnabled != skyLightEnabled) {
        summary = buildSummaryFromViews(blockGrid[ci].data(), minSecY, secCount,
                                        optical, skyLightEnabled, seqBefore);
        if (proto->blockMutationSeq() == seqBefore) {
          proto->setLightSummary(summary);
        }
      }
      summaries[ci] = std::move(summary);
    } else {
      captureChunkViews(*c, blockGrid[ci].data(), minSecY, secCount);
      summaries[ci] = buildSummaryFromViews(blockGrid[ci].data(), minSecY,
                                            secCount, optical, skyLightEnabled);
    }
  }

  auto propsAt = [&](int gx, int gy, int gz) -> const BlockLightProperties & {
    const int worldY = lightBottom + gy;
    if (worldY < minY || worldY >= minY + secCount * 16)
      return emptyLightProperties();
    const int ci = (gz >> 4) * 3 + (gx >> 4);
    if (!chunks3x3[ci])
      return unsupportedLightProperties();
    const int secIdx = (worldY >> 4) - minSecY;
    const LightSectionView &v = blockGrid[ci][static_cast<std::size_t>(secIdx)];
    if (v.bits == 0)
      return optical[optical.classForState(v.single)].properties;
    return optical[optical.classForState(
                         v.atomicStateAt(lightIndex(gx, worldY, gz)))]
        .properties;
  };

  thread_local InitialLightScratch scratch;
  scratch.resetGrid(lightSecCount);

  auto enqueueTask = [&](std::uint64_t t, bool hasActive,
                         std::uint32_t activeSec) {
    if (hasActive && taskSectionKey(t) == activeSec)
      scratch.currentQueue.push(t);
    else
      scratch.mainQueue.push(t);
  };

  // --- 1. BlockLight ---
  {
    auto getBlockLevel = [&](int gx, int gz, int gy) -> int {
      const int ci = (gz >> 4) * 3 + (gx >> 4);
      const int sy = gy >> 4;
      const auto &slot = scratch.grid[ci][sy];
      if (slot.state == -2)
        return 0;
      if (slot.state >= 0)
        return slot.state;
      return slot.data[lightIndex(gx, gy, gz)];
    };

    auto setBlockLevel = [&](int gx, int gz, int gy, int level) {
      const int ci = (gz >> 4) * 3 + (gx >> 4);
      const int sy = gy >> 4;
      auto &slot = scratch.grid[ci][sy];
      if (slot.state == -2) {
        if (level == 0)
          return false;
        slot.data = scratch.acquireSlab(0);
        slot.state = -1;
      } else if (slot.state >= 0) {
        if (slot.state == level)
          return false;
        slot.data = scratch.acquireSlab(static_cast<std::uint8_t>(slot.state));
        slot.state = -1;
      }
      const std::size_t idx = lightIndex(gx, gy, gz);
      if (slot.data[idx] == level)
        return false;
      slot.data[idx] = static_cast<std::uint8_t>(level);
      return true;
    };

    for (int ci = 0; ci < 9; ++ci) {
      if (!summaries[ci])
        continue;
      const int dx = (ci % 3) - 1;
      const int dz = (ci / 3) - 1;
      for (const auto &cell : summaries[ci]->emissiveBlocks) {
        const int lx = dx * 16 + static_cast<int>(cell.packedLocal & 15u);
        const int lz =
            dz * 16 + static_cast<int>((cell.packedLocal >> 4) & 15u);
        const int e = static_cast<int>(cell.emission);
        if (lx <= -e || lx >= 15 + e || lz <= -e || lz >= 15 + e)
          continue;
        const int gx = lx + 16;
        const int gz = lz + 16;
        const int gy = static_cast<int>(cell.worldY) - lightBottom;
        const auto &props = propsAt(gx, gy, gz);
        scratch.mainQueue.push(packTask(gx, gz, gy, cell.emission, 63,
                                        emptyShape(props), true));
      }
    }

    if (!scratch.mainQueue.empty()) {
      bool hasActive = false;
      std::uint32_t activeSec = 0;
      while (!scratch.currentQueue.empty() || !scratch.mainQueue.empty()) {
        if (scratch.currentQueue.empty()) {
          const std::uint64_t first = scratch.mainQueue.pop();
          activeSec = taskSectionKey(first);
          hasActive = true;
          scratch.currentQueue.push(first);
        }
        const std::uint64_t t = scratch.currentQueue.pop();
        const int gx = taskGx(t);
        const int gz = taskGz(t);
        const int gy = taskGy(t);
        const int targetLevel = taskLevel(t);
        int fromLevel = getBlockLevel(gx, gz, gy);
        if (taskFromEmission(t) && fromLevel < targetLevel) {
          setBlockLevel(gx, gz, gy, targetLevel);
          fromLevel = targetLevel;
        }
        if (fromLevel == targetLevel) {
          const auto &fromQueued = taskFromEmpty(t) ? emptyLightProperties()
                                                    : propsAt(gx, gy, gz);
          const std::uint8_t dirs = taskDirs(t);
          for (int dir = 0; dir < 6; ++dir) {
            if ((dirs & (1u << dir)) == 0)
              continue;
            const int nx = gx + kDirDx[dir];
            const int ny = gy + kDirDy[dir];
            const int nz = gz + kDirDz[dir];
            if (static_cast<unsigned>(nx) >= 48u ||
                static_cast<unsigned>(nz) >= 48u ||
                static_cast<unsigned>(ny) >=
                    static_cast<unsigned>(lightHeight))
              continue;
            const int nci = (nz >> 4) * 3 + (nx >> 4);
            if (!chunks3x3[nci])
              continue;
            const int toLevel = getBlockLevel(nx, nz, ny);
            if (fromLevel - 1 <= toLevel)
              continue;
            const auto &toState = propsAt(nx, ny, nz);
            const int newLevel = fromLevel - lightOpacity(toState);
            if (newLevel <= toLevel)
              continue;
            if (lightFaceShapesOcclude(fromQueued, toState,
                                       static_cast<LightDirection>(dir)))
              continue;
            setBlockLevel(nx, nz, ny, newLevel);
            if (newLevel > 1) {
              enqueueTask(
                  packTask(nx, nz, ny, static_cast<std::uint8_t>(newLevel),
                           static_cast<std::uint8_t>(63u & ~(1u << (dir ^ 1))),
                           emptyShape(toState), false),
                  hasActive, activeSec);
            }
          }
        }
        if (scratch.currentQueue.empty())
          hasActive = false;
      }
    }

    result->blockLayers.resize(static_cast<std::size_t>(lightSecCount));
    for (int sy = 0; sy < lightSecCount; ++sy)
      result->blockLayers[static_cast<std::size_t>(sy)] =
          foldSectionSlot(scratch.grid[4][sy]);
  }

  // --- 2. SkyLight ---
  if (skyLightEnabled) {
    scratch.resetGrid(lightSecCount);
    scratch.eligibleMask.fill(0);
    for (int ci = 0; ci < 9; ++ci) {
      if (!chunks3x3[ci])
        continue;
      const int cx = ci % 3;
      const int cz = ci / 3;
      for (int sy = 0; sy < lightSecCount; ++sy) {
        bool eligible = false;
        for (int ncz = std::max(0, cz - 1); ncz <= std::min(2, cz + 1) && !eligible;
             ++ncz) {
          for (int ncx = std::max(0, cx - 1);
               ncx <= std::min(2, cx + 1) && !eligible; ++ncx) {
            const int nci = ncz * 3 + ncx;
            if (!summaries[nci])
              continue;
            const std::uint32_t mask = summaries[nci]->nonEmptySectionMask;
            for (int dy = -1; dy <= 1; ++dy) {
              const int blockIdx = (sy - 1) + dy;
              if (blockIdx >= 0 && blockIdx < secCount && blockIdx < 32 &&
                  ((mask >> blockIdx) & 1u) != 0) {
                eligible = true;
                break;
              }
            }
          }
        }
        if (eligible)
          scratch.eligibleMask[ci] |= (std::uint64_t{1} << sy);
      }
    }

    auto isEligible = [&](int ci, int sy) -> bool {
      return ((scratch.eligibleMask[ci] >> sy) & 1u) != 0;
    };

    for (int ci = 0; ci < 9; ++ci) {
      int above = -1;
      for (int sy = lightSecCount - 1; sy >= 0; --sy) {
        scratch.inherited[ci][sy] = above;
        if (isEligible(ci, sy))
          above = sy;
      }
      if (!summaries[ci])
        continue;
      const auto &sources = summaries[ci]->skySources;
      std::array<int, 256> litFrom{};
      int minLit = std::numeric_limits<int>::max();
      int maxLit = std::numeric_limits<int>::min();
      for (int col = 0; col < 256; ++col) {
        const int v = sources[col] == sentinel ? lightBottom : sources[col];
        litFrom[col] = v;
        minLit = std::min(minLit, v);
        maxLit = std::max(maxLit, v);
      }
      for (int sy = 0; sy < lightSecCount; ++sy) {
        if (!isEligible(ci, sy)) {
          scratch.grid[ci][sy] = {-2, nullptr};
          continue;
        }
        const int baseY = (minSecY - 1 + sy) * 16;
        if (maxLit <= baseY) {
          scratch.grid[ci][sy] = {15, nullptr};
        } else if (minLit >= baseY + 16) {
          scratch.grid[ci][sy] = {0, nullptr};
        } else {
          std::uint8_t *slab = scratch.acquireSlab(0);
          for (int localY = 0; localY < 16; ++localY) {
            const int wy = baseY + localY;
            const std::size_t rowOff = static_cast<std::size_t>(localY) << 8;
            for (int col = 0; col < 256; ++col)
              slab[rowOff + static_cast<std::size_t>(col)] =
                  wy >= litFrom[col] ? 15 : 0;
          }
          scratch.grid[ci][sy] = {-1, slab};
        }
      }
    }

    auto getSkyLevel = [&](int gx, int gz, int gy) -> int {
      const int ci = (gz >> 4) * 3 + (gx >> 4);
      const int sy = gy >> 4;
      const auto &slot = scratch.grid[ci][sy];
      if (slot.state >= 0)
        return slot.state;
      if (slot.state == -1)
        return slot.data[lightIndex(gx, gy, gz)];
      const int baseSy = scratch.inherited[ci][sy];
      if (baseSy < 0)
        return 15;
      const auto &aboveSlot = scratch.grid[ci][baseSy];
      if (aboveSlot.state >= 0)
        return aboveSlot.state;
      if (aboveSlot.state == -1)
        return aboveSlot.data[lightIndex(gx, 0, gz)];
      return 15;
    };

    auto setSkyLevel = [&](int gx, int gz, int gy, int level) -> bool {
      const int ci = (gz >> 4) * 3 + (gx >> 4);
      const int sy = gy >> 4;
      if (!isEligible(ci, sy))
        return false;
      auto &slot = scratch.grid[ci][sy];
      if (slot.state >= 0) {
        if (slot.state == level)
          return false;
        slot.data = scratch.acquireSlab(static_cast<std::uint8_t>(slot.state));
        slot.state = -1;
      } else if (slot.state == -2) {
        return false;
      }
      const std::size_t idx = lightIndex(gx, gy, gz);
      if (slot.data[idx] == level)
        return false;
      slot.data[idx] = static_cast<std::uint8_t>(level);
      return true;
    };

    auto countEmptySectionsBelowIfAtBorder = [&](int gx, int gz,
                                                 int gy) -> int {
      if ((gy & 15) != 0)
        return 0;
      const int lx = gx & 15;
      const int lz = gz & 15;
      if (lx != 0 && lx != 15 && lz != 0 && lz != 15)
        return 0;
      const int ci = (gz >> 4) * 3 + (gx >> 4);
      const int sy = gy >> 4;
      int count = 0;
      for (int below = sy - 1; below >= 0; --below) {
        if (isEligible(ci, below))
          break;
        ++count;
      }
      return count;
    };

    auto sourceAt = [&](int gx, int gz) -> int {
      if (static_cast<unsigned>(gx) >= 48u ||
          static_cast<unsigned>(gz) >= 48u)
        return sentinel;
      const int ci = (gz >> 4) * 3 + (gx >> 4);
      if (!summaries[ci])
        return sentinel;
      return summaries[ci]->skySources[(gz & 15) * 16 + (gx & 15)];
    };

    for (int ci = 0; ci < 9; ++ci) {
      if (!summaries[ci])
        continue;
      const int baseGx = (ci % 3) * 16;
      const int baseGz = (ci / 3) * 16;
      for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
          const int gx = baseGx + x;
          const int gz = baseGz + z;
          const int source = summaries[ci]->skySources[z * 16 + x];
          const int north = sourceAt(gx, gz - 1);
          const int south = sourceAt(gx, gz + 1);
          const int west = sourceAt(gx - 1, gz);
          const int east = sourceAt(gx + 1, gz);
          const int neighborHighest = std::max({north, south, west, east});
          const int first = source == sentinel ? minY : source;
          for (int y = lightTop - 1; y >= first; --y) {
            if (y != source && y >= neighborHighest)
              continue;
            std::uint8_t directions = 0;
            if (y == source)
              directions |= 1u << 0;
            if (y < north)
              directions |= 1u << 2;
            if (y < south)
              directions |= 1u << 3;
            if (y < west)
              directions |= 1u << 4;
            if (y < east)
              directions |= 1u << 5;
            if (directions)
              scratch.mainQueue.push(packTask(gx, gz, y - lightBottom, 15,
                                              directions, false, true));
          }
        }
      }
    }

    if (!scratch.mainQueue.empty()) {
      bool hasActive = false;
      std::uint32_t activeSec = 0;
      while (!scratch.currentQueue.empty() || !scratch.mainQueue.empty()) {
        if (scratch.currentQueue.empty()) {
          const std::uint64_t first = scratch.mainQueue.pop();
          activeSec = taskSectionKey(first);
          hasActive = true;
          scratch.currentQueue.push(first);
        }
        const std::uint64_t t = scratch.currentQueue.pop();
        const int gx = taskGx(t);
        const int gz = taskGz(t);
        const int gy = taskGy(t);
        const int targetLevel = taskLevel(t);
        int fromLevel = getSkyLevel(gx, gz, gy);
        if (taskFromEmission(t) && fromLevel < targetLevel) {
          setSkyLevel(gx, gz, gy, targetLevel);
          fromLevel = targetLevel;
        }
        if (fromLevel == targetLevel) {
          const int emptySectionsBelow =
              countEmptySectionsBelowIfAtBorder(gx, gz, gy);
          const auto &fromQueued = taskFromEmpty(t) ? emptyLightProperties()
                                                    : propsAt(gx, gy, gz);
          const std::uint8_t dirs = taskDirs(t);
          for (int dir = 0; dir < 6; ++dir) {
            if ((dirs & (1u << dir)) == 0)
              continue;
            const int nx = gx + kDirDx[dir];
            const int ny = gy + kDirDy[dir];
            const int nz = gz + kDirDz[dir];
            if (static_cast<unsigned>(nx) >= 48u ||
                static_cast<unsigned>(nz) >= 48u ||
                static_cast<unsigned>(ny) >=
                    static_cast<unsigned>(lightHeight))
              continue;
            const int nci = (nz >> 4) * 3 + (nx >> 4);
            if (!chunks3x3[nci])
              continue;
            const int toLevel = getSkyLevel(nx, nz, ny);
            if (fromLevel - 1 <= toLevel)
              continue;
            const auto &toState = propsAt(nx, ny, nz);
            const int newLevel = fromLevel - lightOpacity(toState);
            if (newLevel <= toLevel ||
                lightFaceShapesOcclude(fromQueued, toState,
                                       static_cast<LightDirection>(dir)))
              continue;
            if (!setSkyLevel(nx, nz, ny, newLevel))
              continue;
            const std::uint8_t outDirs =
                static_cast<std::uint8_t>(63u & ~(1u << (dir ^ 1)));
            if (newLevel > 1) {
              enqueueTask(packTask(nx, nz, ny, static_cast<std::uint8_t>(newLevel),
                                   outDirs, emptyShape(toState), false),
                          hasActive, activeSec);
            }
            if (emptySectionsBelow > 0 &&
                crossedSkySectionEdge(dir, nx & 15, nz & 15)) {
              const int startSy = (ny >> 4) - 1;
              const int endSy = startSy - emptySectionsBelow + 1;
              for (int sy = startSy; sy >= endSy; --sy) {
                if (!isEligible(nci, sy))
                  continue;
                for (int ly = 15; ly >= 0; --ly) {
                  const int py = (sy << 4) | ly;
                  if (setSkyLevel(nx, nz, py, newLevel) && newLevel > 1) {
                    enqueueTask(
                        packTask(nx, nz, py, static_cast<std::uint8_t>(newLevel),
                                 outDirs, true, false),
                        hasActive, activeSec);
                  }
                }
              }
            }
          }
        }
        if (scratch.currentQueue.empty())
          hasActive = false;
      }
    }

    result->skyLayers.resize(static_cast<std::size_t>(lightSecCount));
    for (int sy = 0; sy < lightSecCount; ++sy)
      result->skyLayers[static_cast<std::size_t>(sy)] =
          foldSectionSlot(scratch.grid[4][sy]);
    result->skySources = summaries[4]->skySources;
    result->hasSkySources = true;
  }

  result->isLightOn = true;
  result->skyLightEnabled = skyLightEnabled;
  result->unsupportedCells = summaries[4] ? summaries[4]->unsupportedCells : 0;
  result->nonEmptySectionMask =
      summaries[4] ? summaries[4]->nonEmptySectionMask : 0;
  return result;
}

} // namespace redstone
