#pragma once
// 世界生成原型区块（`ProtoChunk`）、4x4x4 生物群系调色板容器（`BiomePaletteContainer`）与雕刻掩码（`CarvingMask`）。
// 之所以在 `WorldChunk` 之外单独设计 `ProtoChunk`，是因为世界生成期的 11 个原型阶段需要保存大量仅生成期使用的状态
// （`*WG` 高度图、`carving_mask`、`postProcessing` 坐标、生物群系 quart 网格、尚未激活的实体与方块实体 NBT 等）；
// 等到达 `FULL` 后再零拷贝或紧凑移交给运行态 `WorldChunk`，既对齐原版 26.1 架构，也不拖累主循环与红石热路径的内存布局。

#include "redstone/block.hpp"
#include "redstone/chunk_section.hpp"
#include "redstone/chunk_lifecycle.hpp"
#include "redstone/engine_types.hpp"
#include "redstone/fluid.hpp"
#include "redstone/heightmap.hpp"
#include "redstone/light/snapshot.hpp"
#include "redstone/nbt.hpp"
#include "redstone/terrain_codec.hpp"
#include "redstone/tickets.hpp"
#include "redstone/worldgen/chunk_status.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {
class World;
} // namespace redstone

namespace redstone::worldgen {

// 全区块三维雕刻位掩码（对齐 26.1 `CarvingMask.java`）。
// 仅在首次调用 `set` 时按需分配 `256 * height / 64` 个字，未进入洞穴雕刻的区块保持零堆分配。
class CarvingMask {
public:
  CarvingMask(int minY, int height);

  int minY() const noexcept { return minY_; }
  int height() const noexcept { return height_; }
  bool empty() const noexcept { return words_.empty(); }

  bool get(int localX, int y, int localZ) const;
  void set(int localX, int y, int localZ);

  std::vector<std::uint64_t> toLongArray() const;
  // 诊断用：雕刻位掩码占用的堆容量（字节）；未分配时为 0。
  std::size_t memoryBytes() const noexcept {
    return words_.capacity() * sizeof(std::uint64_t);
  }
  static CarvingMask fromLongArray(std::span<const std::uint64_t> data, int minY,
                                   int height);

private:
  std::size_t bitIndex(int localX, int y, int localZ) const;

  int minY_ = -64;
  int height_ = 384;
  std::vector<std::uint64_t> words_;
};

using ProtoChunkSection = redstone::BlockSection;

// InitializeLight 阶段预计算的区块光学摘要：供 Light 阶段 3x3 并行初始光照直接汇总，
// 避免每块重复扫描全部 98,304 格。
struct ProtoChunkLightSummary {
  struct EmissiveCell {
    std::uint16_t packedLocal = 0; // (y & 15) << 8 | (z & 15) << 4 | (x & 15)
    std::int16_t worldY = 0;
    std::uint8_t emission = 0;
  };
  std::array<int, 256> skySources{}; // z * 16 + x, matching SkyLightEngine
  int minSkySourceY = 0;
  int maxSkySourceY = 0;
  std::uint32_t nonEmptySectionMask = 0;
  std::uint32_t unsupportedCells = 0;
  std::uint64_t mutationSeq = 0;
  bool skyLightEnabled = true;
  std::vector<EmissiveCell> emissiveBlocks;
};

// 生成期 ProtoChunk 常驻堆内存的分项统计（仅诊断/基准使用）。
// 把整批区块常驻内存拆成区段数组、方块调色板、生物群系调色板、高度图、
// 结构引用与待激活实体等来源，用于定位 50x50 这类全量常驻场景的构成。
// 注意：chunkObject / heightmapObjects / lightArraysInline 属于区块对象内联容量，
// 不再单独计入 heapTotal()；NBT 载荷只统计 std::vector 外壳。
// lightLayerBuffers 统计按需分配的光照层堆内存（每层 2 KiB）。
struct ProtoChunkMemoryUsage {
  std::size_t chunkObject = 0;
  std::size_t sectionsArray = 0;
  std::size_t lightLayerBuffers = 0;
  std::size_t paletteIndexWords = 0;
  std::size_t paletteEntries = 0;
  std::size_t biomePaletteVectors = 0;
  std::size_t biomeStrings = 0;
  std::size_t heightmapObjects = 0;
  std::size_t heightmapData = 0;
  std::size_t carvingMask = 0;
  std::size_t postProcessing = 0;
  std::size_t ticks = 0;
  std::size_t entities = 0;
  std::size_t blockEntities = 0;
  std::size_t movingPistons = 0;
  std::size_t structureReferences = 0;

  void add(const ProtoChunkMemoryUsage &o) noexcept;
  std::size_t heapTotal() const noexcept;
  std::size_t total() const noexcept {
    return chunkObject + heapTotal();
  }
};

// 生成期原型区块（对齐 26.1 `ProtoChunk.java`）：保存全部生成阶段中间态与待晋升计划数据。
class ProtoChunk : public BaseChunk {
public:
  ProtoChunk(ChunkTicketPos pos, int minY = -64, int height = 384,
             std::string defaultBiome = "minecraft:plains");
  ProtoChunk(ProtoChunk &&) noexcept = default;
  ProtoChunk &operator=(ProtoChunk &&) noexcept = default;
  ProtoChunk(const ProtoChunk &) = delete;
  ProtoChunk &operator=(const ProtoChunk &) = delete;

  int maxSectionY() const noexcept { return minSectionY() + sectionCount() - 1; }

  ChunkStatusId persistedStatus() const noexcept {
    return status_.value.load(std::memory_order_acquire);
  }
  void setPersistedStatus(ChunkStatusId status);

  BlockState getBlockState(int x, int y, int z) const;
  FluidState getFluidState(int x, int y, int z) const;
  std::optional<BlockState> setBlockState(int x, int y, int z,
                                          const BlockState &state,
                                          int flags = 3);

  std::string_view getNoiseBiome(int quartX, int quartY, int quartZ) const {
    return BaseChunk::getNoiseBiome(quartX, quartY, quartZ);
  }
  void setNoiseBiome(int quartX, int quartY, int quartZ,
                     std::string_view biomeId);

  int getHeight(HeightmapType type, int x, int z);
  void markPosForPostprocessing(int x, int y, int z);
  void addPackedPostProcess(std::span<const std::int16_t> offsets,
                            int sectionIndex);

  // Per-chunk cached `NoiseChunk` shared across `BIOMES` -> `NOISE` -> `SURFACE` -> `CARVERS`.
  void *cachedNoiseChunkRaw() const noexcept { return cachedNoiseChunk_.get(); }
  void setCachedNoiseChunkRaw(OpaqueUniquePtr nc) noexcept {
    cachedNoiseChunk_ = std::move(nc);
  }

  // Extract the 4 final heightmaps (flushing dirty columns first) for zero-rescan
  // promotion into WorldChunk.
  std::optional<std::array<Heightmap, 4>> takeFinalHeightmaps();

  // InitializeLight 阶段提交给光照引擎的请求序号，供 Light 阶段按区块等待。
  void setLightSubmissionGeneration(std::uint64_t generation) noexcept {
    lightGeneration_ = generation;
  }
  std::uint64_t lightSubmissionGeneration() const noexcept {
    return lightGeneration_;
  }
  bool deferredLightSubmitted() const noexcept {
    return deferredLightSubmitted_;
  }
  void setDeferredLightSubmitted(bool submitted) noexcept {
    deferredLightSubmitted_ = submitted;
  }

  std::shared_ptr<const ProtoChunkLightSummary> lightSummary() const noexcept {
    return lightSummary_.load();
  }
  bool hasLightSummary() const noexcept { return lightSummary_.hasValue(); }
  void setLightSummary(std::shared_ptr<const ProtoChunkLightSummary> summary) const noexcept {
    lightSummary_.store(std::move(summary));
  }
  std::uint64_t blockMutationSeq() const noexcept {
    return blockMutationSeq_.value.load(std::memory_order_acquire);
  }
  void bumpBlockMutationSeq() noexcept {
    blockMutationSeq_.value.fetch_add(1, std::memory_order_acq_rel);
  }
  std::shared_ptr<const PrecomputedChunkLight> precomputedLight() const noexcept {
    return precomputedLight_.load();
  }
  void setPrecomputedLight(std::shared_ptr<const PrecomputedChunkLight> light) noexcept {
    precomputedLight_.store(std::move(light));
  }

  std::string_view defaultBiome() const noexcept { return defaultBiome_; }

  std::int64_t inhabitedTime() const noexcept { return inhabitedTime_; }
  void setInhabitedTime(std::int64_t time) noexcept { inhabitedTime_ = time; }

  // 区段数据来自共享 BaseChunk：全部 sectionCount 个区段始终内联存在，
  // 与 World::Chunk 使用同一份 BlockSection 存储与计数。
  void ensureSections() const {}
  bool hasSections() const noexcept { return true; }

  ProtoChunkSection &section(int index) { return BaseChunk::section(index); }
  const ProtoChunkSection &section(int index) const {
    return BaseChunk::section(index);
  }
  std::span<ProtoChunkSection> sections() { return sectionSpan(); }
  std::span<const ProtoChunkSection> sections() const { return sectionSpan(); }

  Heightmap &getOrCreateHeightmapUnprimed(HeightmapType type);
  const Heightmap *findHeightmap(HeightmapType type) const;
  void setHeightmapData(HeightmapType type,
                        const std::vector<std::uint64_t> &rawData);
  void primeHeightmaps(std::span<const HeightmapType> types);
  void flushDirtyHeightmaps() const;

  CarvingMask *carvingMask() noexcept {
    return carvingMask_ ? &*carvingMask_ : nullptr;
  }
  const CarvingMask *carvingMask() const noexcept {
    return carvingMask_ ? &*carvingMask_ : nullptr;
  }
  CarvingMask &getOrCreateCarvingMask();
  void setCarvingMask(CarvingMask mask);
  void clearCarvingMask() noexcept { carvingMask_.reset(); }

  const std::vector<std::vector<std::int16_t>> &postProcessing() const noexcept {
    return postProcessing_;
  }
  void clearPostProcessing();

  std::vector<terrain::SavedTick> &blockTicks() noexcept { return blockTicks_; }
  const std::vector<terrain::SavedTick> &blockTicks() const noexcept {
    return blockTicks_;
  }
  std::vector<terrain::SavedTick> &fluidTicks() noexcept { return fluidTicks_; }
  const std::vector<terrain::SavedTick> &fluidTicks() const noexcept {
    return fluidTicks_;
  }

  std::vector<nbt::Tag> &entities() noexcept { return entities_; }
  const std::vector<nbt::Tag> &entities() const noexcept { return entities_; }
  std::vector<nbt::Tag> &pendingBlockEntities() noexcept {
    return pendingBlockEntities_;
  }
  const std::vector<nbt::Tag> &pendingBlockEntities() const noexcept {
    return pendingBlockEntities_;
  }
  std::vector<terrain::MovingPistonEntity> &movingPistons() noexcept {
    return movingPistons_;
  }
  const std::vector<terrain::MovingPistonEntity> &
  movingPistons() const noexcept {
    return movingPistons_;
  }

  // 诊断用：返回本区块常驻堆内存的分项统计。
  ProtoChunkMemoryUsage memoryUsage() const;

  const std::vector<std::int64_t> &structureReferences(
      std::string_view structureId) const;
  void addStructureReference(std::string_view structureId,
                             std::int64_t packedStartChunkPos);
  const std::map<std::string, std::vector<std::int64_t>, std::less<>> &
  allStructureReferences() const noexcept {
    return structureReferences_;
  }

  // 与原版 26.1 `ProtoChunk.packOffsetCoordinates` / `unpackOffsetCoordinates` 一致的 12-bit 坐标打包：
  // 低 4 位为 localX，4..7 位为 localY，8..11 位为 localZ。
  static constexpr std::int16_t packOffsetCoordinates(int x, int y,
                                                      int z) noexcept {
    return static_cast<std::int16_t>((x & 15) | ((y & 15) << 4) |
                                     ((z & 15) << 8));
  }
  static constexpr Pos unpackOffsetCoordinates(std::int16_t packed,
                                               ChunkTicketPos chunkPos,
                                               int sectionY) noexcept {
    const int lx = packed & 15;
    const int ly = (packed >> 4) & 15;
    const int lz = (packed >> 8) & 15;
    return Pos{(chunkPos.x << 4) + lx, (sectionY << 4) + ly,
               (chunkPos.z << 4) + lz};
  }

protected:
  void recomputeHeightmapColumn(Heightmap &hm, int lx, int lz);
  void flushDirtyHeightmapColumns(std::uint8_t mask) const;
  void flushDirtyHeightmapSingleColumn(int lx, int lz, std::uint8_t mask) const;

  struct AtomicChunkStatus {
    std::atomic<ChunkStatusId> value{ChunkStatusId::Empty};
    AtomicChunkStatus() noexcept = default;
    AtomicChunkStatus(const AtomicChunkStatus &o) noexcept
        : value(o.value.load(std::memory_order_acquire)) {}
    AtomicChunkStatus &operator=(const AtomicChunkStatus &o) noexcept {
      value.store(o.value.load(std::memory_order_acquire),
                  std::memory_order_release);
      return *this;
    }
    AtomicChunkStatus(AtomicChunkStatus &&o) noexcept
        : value(o.value.load(std::memory_order_acquire)) {}
    AtomicChunkStatus &operator=(AtomicChunkStatus &&o) noexcept {
      value.store(o.value.load(std::memory_order_acquire),
                  std::memory_order_release);
      return *this;
    }
  };

  struct AtomicU64 {
    std::atomic<std::uint64_t> value{0};
    AtomicU64() noexcept = default;
    AtomicU64(const AtomicU64 &o) noexcept
        : value(o.value.load(std::memory_order_acquire)) {}
    AtomicU64 &operator=(const AtomicU64 &o) noexcept {
      value.store(o.value.load(std::memory_order_acquire),
                  std::memory_order_release);
      return *this;
    }
    AtomicU64(AtomicU64 &&o) noexcept
        : value(o.value.load(std::memory_order_acquire)) {}
    AtomicU64 &operator=(AtomicU64 &&o) noexcept {
      value.store(o.value.load(std::memory_order_acquire),
                  std::memory_order_release);
      return *this;
    }
  };

  template <class T> struct AtomicSharedSlot {
    mutable std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::shared_ptr<const T> value;
    AtomicSharedSlot() noexcept = default;
    AtomicSharedSlot(const AtomicSharedSlot &) = delete;
    AtomicSharedSlot &operator=(const AtomicSharedSlot &) = delete;
    AtomicSharedSlot(AtomicSharedSlot &&o) noexcept {
      while (o.lock.test_and_set(std::memory_order_acquire)) {
      }
      value = std::move(o.value);
      o.lock.clear(std::memory_order_release);
    }
    AtomicSharedSlot &operator=(AtomicSharedSlot &&o) noexcept {
      if (this == &o)
        return *this;
      while (lock.test_and_set(std::memory_order_acquire)) {
      }
      while (o.lock.test_and_set(std::memory_order_acquire)) {
      }
      value = std::move(o.value);
      o.lock.clear(std::memory_order_release);
      lock.clear(std::memory_order_release);
      return *this;
    }
    std::shared_ptr<const T> load() const noexcept {
      while (lock.test_and_set(std::memory_order_acquire)) {
      }
      auto copy = value;
      lock.clear(std::memory_order_release);
      return copy;
    }
    void store(std::shared_ptr<const T> next) noexcept {
      while (lock.test_and_set(std::memory_order_acquire)) {
      }
      value = std::move(next);
      lock.clear(std::memory_order_release);
    }
    bool hasValue() const noexcept {
      while (lock.test_and_set(std::memory_order_acquire)) {
      }
      const bool present = static_cast<bool>(value);
      lock.clear(std::memory_order_release);
      return present;
    }
  };

  AtomicChunkStatus status_{};
  std::int64_t inhabitedTime_ = 0;
  std::uint64_t lightGeneration_ = 0;
  bool deferredLightSubmitted_ = false;
  mutable AtomicSharedSlot<ProtoChunkLightSummary> lightSummary_;
  AtomicU64 blockMutationSeq_;
  AtomicSharedSlot<PrecomputedChunkLight> precomputedLight_;
  OpaqueUniquePtr cachedNoiseChunk_{nullptr, +[](void *) {}};
  mutable std::array<std::optional<Heightmap>, 6> heightmaps_{};
  std::uint8_t createdHeightmapMask_ = 0;
  mutable std::uint8_t dirtyHeightmapSummaryMask_ = 0;
  mutable std::array<std::uint8_t, 256> dirtyHeightmapColsMask_{};
  std::optional<CarvingMask> carvingMask_;
  std::vector<std::vector<std::int16_t>> postProcessing_;
  std::vector<terrain::SavedTick> blockTicks_;
  std::vector<terrain::SavedTick> fluidTicks_;
  std::vector<nbt::Tag> entities_;
  std::vector<nbt::Tag> pendingBlockEntities_;
  std::vector<terrain::MovingPistonEntity> movingPistons_;
  std::map<std::string, std::vector<std::int64_t>, std::less<>>
      structureReferences_;
};

// 按原版 26.1 `SerializableChunkData.write()` 格式将原型区块序列化为 Anvil 区块 NBT（DataVersion=4786）。
// 使未到达 `FULL` 即因票据撤销卸载的半成品区块能无损写盘，后续重载可从断点阶段无缝续接。
nbt::RootTag encodeProtoChunkToNbt(const ProtoChunk &chunk,
                                   std::int64_t lastUpdateTime = 0);

// 将允许非满状态的 `terrain::DecodedChunk` 还原为内存中的 `ProtoChunk` 对象。
std::optional<ProtoChunk>
decodeProtoChunkFromDecoded(const terrain::DecodedChunk &decoded, int minY,
                            int height, std::string &error);

// 直接从原始未压缩区块 NBT 字节流解码为 `ProtoChunk`（内部设 `requireFullStatus = false`）。
std::optional<ProtoChunk>
decodeProtoChunkNbt(std::span<const std::uint8_t> nbtBytes, int minY, int height,
                    std::string &error);

// 将到达 `ChunkStatusId::Full` 的 `ProtoChunk` 转换为 `ChunkLifecycleController` 使用的
// `LifecycleLoadedChunk` 载荷，供 `WorldChunkManager::installLoaded` 正式挂载进主世界。
LifecycleLoadedChunk promoteProtoChunkToLoaded(
    std::shared_ptr<ProtoChunk> chunk, std::uint64_t epoch);

} // namespace redstone::worldgen
