#pragma once
// 世界生成原型区块（`ProtoChunk`）、只读/可写适配包装器（`ImposterProtoChunk`）、
// 区块公共访问基类（`ChunkAccess`）、4x4x4 生物群系调色板容器（`BiomePaletteContainer`）与雕刻掩码（`CarvingMask`）。
// 之所以在 `World::Chunk` 之外单独设计 `ProtoChunk`，是因为世界生成期的 11 个原型阶段需要保存大量仅生成期使用的状态
// （`*WG` 高度图、`carving_mask`、`postProcessing` 坐标、生物群系 quart 网格、尚未激活的实体与方块实体 NBT 等）；
// 等到达 `FULL` 后再零拷贝或紧凑移交给运行态 `World`，既对齐原版 26.1 架构，也不拖累主循环与红石热路径的内存布局。

#include "redstone/block.hpp"
#include "redstone/chunk_lifecycle.hpp"
#include "redstone/engine_types.hpp"
#include "redstone/fluid.hpp"
#include "redstone/heightmap.hpp"
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

// 4x4x4 Quart 生物群系调色板容器（对齐 26.1 `Strategy.createForBiomes`）。
// 每个 16x16x16 区段包含 64 个生物群系采样单元，按 `(qy << 4) | (qz << 2) | qx` 索引；
// 单独封装为紧凑本地调色板以便在无全局静态表时仍能直接与 Anvil NBT 双向无损编解码。
class BiomePaletteContainer {
public:
  static constexpr std::size_t kCellCount = 64;

  explicit BiomePaletteContainer(std::string defaultBiome = "minecraft:plains");

  static constexpr std::size_t index(int qx, int qy, int qz) noexcept {
    return static_cast<std::size_t>(((qy & 3) << 4) | ((qz & 3) << 2) |
                                    (qx & 3));
  }

  const std::string &get(int qx, int qy, int qz) const;
  void set(int qx, int qy, int qz, std::string_view biomeId);
  void fill(std::string_view biomeId);

  bool isUniform() const noexcept { return palette_.size() <= 1; }
  const std::vector<std::string> &palette() const noexcept { return palette_; }
  const std::array<std::uint8_t, kCellCount> &indices() const noexcept {
    return indices_;
  }

  // 打包为 Anvil `sections[].biomes` 的调色板与 64 位位打包字数组，位宽与 26.1 `PalettedContainer.pack` 一致。
  void pack(std::vector<std::string> &outPalette,
            std::vector<std::uint64_t> &outData, int &outBits) const;
  bool unpack(std::span<const std::string> palette,
              std::span<const std::uint64_t> data, std::string &error);

private:
  std::uint8_t intern(std::string_view biomeId);

  std::vector<std::string> palette_;
  std::array<std::uint8_t, kCellCount> indices_{};
};

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

// 生成期区段的调色板容器：空区段零分配；首次写入方块时使用 4 位调色板
// （16 项、2 KiB 位打包索引），状态数超过容量再按 5/6/7/8/16 位扩展。
class ProtoChunkPalette {
public:
  std::uint8_t bits() const noexcept { return bits_; }
  bool empty() const noexcept { return bits_ == 0; }
  std::size_t paletteSize() const noexcept { return palette_.size(); }
  // 诊断用：位打包索引数组与调色板项数组各自占用的堆容量（字节）。
  std::size_t indexBytes() const noexcept {
    return words_.capacity() * sizeof(std::uint64_t);
  }
  std::size_t entryBytes() const noexcept {
    return palette_.capacity() * sizeof(BlockState::Id);
  }
  BlockState::Id paletteEntry(std::size_t index) const noexcept {
    return index < palette_.size() ? palette_[index] : BlockState::Id{0};
  }
  BlockState::Id idAt(std::size_t cell) const noexcept;
  // 直接取位打包里的调色板下标，避免序列化时再走一次 idAt 的解引用。
  std::uint16_t rawIndexAt(std::size_t cell) const noexcept {
    return indexAt(cell);
  }
  const std::vector<std::uint64_t> &words() const noexcept { return words_; }
  // 序列化快路径：调色板没有空洞（每项都被使用），且首次使用顺序与格子顺序
  // 一致时，可以直接复用位打包 words_，无需逐格重建。
  bool canSerializeDirectly() const noexcept;
  BlockState::Id set(std::size_t cell, BlockState::Id value);
  void clear() noexcept;

private:
  static constexpr std::size_t kCells = 4096;
  static std::size_t wordsForBits(std::uint8_t bits) noexcept {
    const std::size_t perWord = std::size_t{64} / bits;
    return (kCells + perWord - 1) / perWord;
  }
  std::uint16_t indexAt(std::size_t cell) const noexcept;
  void setIndexAt(std::size_t cell, std::uint16_t index) noexcept;
  void expandTo(std::uint8_t bits);

  std::vector<BlockState::Id> palette_;
  std::vector<std::uint64_t> words_;
  std::vector<std::uint16_t> refCount_;
  std::vector<std::uint32_t> firstCell_;
  std::uint8_t bits_ = 0;
};

// 世界生成期区段的两个光照层（BlockLight / SkyLight）。
// 世界生成阶段只有挂了 LightService 的区块才会真正填充这两层，因此这里改为
// 按需在堆上分配 2 KiB，而不是让每个区段都内联预留 2x2 KiB；对外接口保持
// 与 std::optional<std::array<std::uint8_t, 2048>> 一致，避免改动调用方。
class LightLayerStorage {
public:
  using Array = std::array<std::uint8_t, 2048>;

  LightLayerStorage() noexcept = default;
  LightLayerStorage(const LightLayerStorage &other) {
    if (other.data_)
      data_ = std::make_unique<Array>(*other.data_);
  }
  LightLayerStorage(LightLayerStorage &&other) noexcept = default;
  LightLayerStorage &operator=(const LightLayerStorage &other) {
    if (this != &other) {
      if (other.data_)
        data_ = std::make_unique<Array>(*other.data_);
      else
        data_.reset();
    }
    return *this;
  }
  LightLayerStorage &operator=(LightLayerStorage &&other) noexcept = default;
  ~LightLayerStorage() = default;

  bool has_value() const noexcept { return data_ != nullptr; }
  explicit operator bool() const noexcept { return data_ != nullptr; }
  void reset() noexcept { data_.reset(); }
  Array &emplace() {
    if (!data_)
      data_ = std::make_unique<Array>();
    return *data_;
  }
  Array &operator*() noexcept { return *data_; }
  const Array &operator*() const noexcept { return *data_; }
  Array *operator->() noexcept { return data_.get(); }
  const Array *operator->() const noexcept { return data_.get(); }

  // 晋升为运行态区块 / 持久化光照快照时的载荷转换。
  std::optional<Array> copy() const {
    return data_ ? std::optional<Array>(*data_) : std::nullopt;
  }
  std::optional<Array> take() {
    std::optional<Array> out = copy();
    data_.reset();
    return out;
  }
  std::size_t heapBytes() const noexcept {
    return data_ ? sizeof(Array) : std::size_t{0};
  }

private:
  std::unique_ptr<Array> data_;
};

// 原型区块的单个 16x16x16 区段。方块缓冲按需惰性分配，纯空气区段不分配
// 16 KiB 状态数组；非空区段从 4 位调色板开始，按不同状态数向上扩展。
class ProtoChunkSection {
public:
  explicit ProtoChunkSection(
      std::int8_t sectionY,
      std::string defaultBiome = "minecraft:plains");

  std::int8_t sectionY() const noexcept { return sectionY_; }
  bool hasOnlyAir() const noexcept { return nonEmptyBlockCount_ == 0; }
  std::uint16_t nonEmptyBlockCount() const noexcept {
    return nonEmptyBlockCount_;
  }

  BlockState getBlockState(int localX, int localY, int localZ) const;
  BlockState setBlockState(int localX, int localY, int localZ,
                           const BlockState &state);
  FluidState getFluidState(int localX, int localY, int localZ) const;

  BiomePaletteContainer &biomes() noexcept { return biomes_; }
  const BiomePaletteContainer &biomes() const noexcept { return biomes_; }

  LightLayerStorage &blockLight() noexcept { return blockLight_; }
  const LightLayerStorage &blockLight() const noexcept { return blockLight_; }
  LightLayerStorage &skyLight() noexcept { return skyLight_; }
  const LightLayerStorage &skyLight() const noexcept { return skyLight_; }

  // 直接暴露 4096 格只读视图，便于批量编码 Anvil 调色板与晋升 `World` 区段。
  // 按格读取状态 id，供 Anvil 编码、光照几何与晋升 World 区段使用。
  BlockState::Id blockIdAt(std::size_t cell) const noexcept {
    return palette_.idAt(cell);
  }
  const ProtoChunkPalette &palette() const noexcept { return palette_; }

private:
  static constexpr std::size_t cellIndex(int lx, int ly, int lz) noexcept {
    return static_cast<std::size_t>(((ly & 15) << 8) | ((lz & 15) << 4) |
                                    (lx & 15));
  }

  std::int8_t sectionY_ = 0;
  std::uint16_t nonEmptyBlockCount_ = 0;
  ProtoChunkPalette palette_;
  BiomePaletteContainer biomes_;
  LightLayerStorage blockLight_;
  LightLayerStorage skyLight_;
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

// `ProtoChunk` 与 `ImposterProtoChunk` 共享的区块读写访问契约（对齐 26.1 `ChunkAccess`）。
// 使 `WorldGenRegion` 无需区分邻域格里放的是正在生成的半成品区块还是已落盘完整的 `World` 区块。
class ChunkAccess {
public:
  virtual ~ChunkAccess() = default;

  virtual ChunkTicketPos pos() const noexcept = 0;
  virtual int minY() const noexcept = 0;
  virtual int height() const noexcept = 0;
  int maxY() const noexcept { return minY() + height(); }
  int minSectionY() const noexcept { return minY() >> 4; }
  int sectionCount() const noexcept { return height() >> 4; }
  int maxSectionY() const noexcept { return minSectionY() + sectionCount() - 1; }
  bool isOutsideBuildHeight(int y) const noexcept {
    return y < minY() || y >= maxY();
  }
  int sectionIndex(int y) const noexcept { return (y >> 4) - minSectionY(); }

  virtual ChunkStatusId persistedStatus() const noexcept = 0;
  virtual void setPersistedStatus(ChunkStatusId status) = 0;

  virtual BlockState getBlockState(int x, int y, int z) const = 0;
  virtual FluidState getFluidState(int x, int y, int z) const = 0;
  virtual std::optional<BlockState> setBlockState(int x, int y, int z,
                                                  const BlockState &state,
                                                  int flags = 3) = 0;

  virtual std::string_view getNoiseBiome(int quartX, int quartY,
                                         int quartZ) const = 0;
  virtual void setNoiseBiome(int quartX, int quartY, int quartZ,
                             std::string_view biomeId) = 0;

  virtual int getHeight(HeightmapType type, int x, int z) = 0;
  virtual void markPosForPostprocessing(int x, int y, int z) = 0;

  virtual bool isLightCorrect() const noexcept = 0;
  virtual void setLightCorrect(bool correct) = 0;

  // Per-chunk cached `NoiseChunk` shared across `BIOMES` -> `NOISE` -> `SURFACE`
  // (matching `ChunkAccess.getOrCreateNoiseChunk` in 26.1).
  std::shared_ptr<void> cachedNoiseChunkOpaque() const noexcept {
    return cachedNoiseChunk_;
  }
  void setCachedNoiseChunkOpaque(std::shared_ptr<void> nc) noexcept {
    cachedNoiseChunk_ = std::move(nc);
  }

  // Per-chunk cached `StructureStart` map populated in `STRUCTURE_STARTS`
  // and read in `STRUCTURE_REFERENCES` / `NOISE` (Beardifier) / `FEATURES`.
  std::shared_ptr<void> structureStartsOpaque() const noexcept {
    return structureStartsOpaque_;
  }
  void setStructureStartsOpaque(std::shared_ptr<void> starts) noexcept {
    structureStartsOpaque_ = std::move(starts);
  }

private:
  std::shared_ptr<void> cachedNoiseChunk_;
  std::shared_ptr<void> structureStartsOpaque_;
};

// 生成期原型区块（对齐 26.1 `ProtoChunk.java`）：保存全部生成阶段中间态与待晋升计划数据。
class ProtoChunk : public ChunkAccess {
public:
  ProtoChunk(ChunkTicketPos pos, int minY = -64, int height = 384,
             std::string defaultBiome = "minecraft:plains");

  ChunkTicketPos pos() const noexcept override { return pos_; }
  int minY() const noexcept override { return minY_; }
  int height() const noexcept override { return height_; }

  ChunkStatusId persistedStatus() const noexcept override {
    return status_.value.load(std::memory_order_acquire);
  }
  void setPersistedStatus(ChunkStatusId status) override;

  BlockState getBlockState(int x, int y, int z) const override;
  FluidState getFluidState(int x, int y, int z) const override;
  std::optional<BlockState> setBlockState(int x, int y, int z,
                                          const BlockState &state,
                                          int flags = 3) override;

  std::string_view getNoiseBiome(int quartX, int quartY,
                                 int quartZ) const override;
  void setNoiseBiome(int quartX, int quartY, int quartZ,
                     std::string_view biomeId) override;

  int getHeight(HeightmapType type, int x, int z) override;
  void markPosForPostprocessing(int x, int y, int z) override;
  void addPackedPostProcess(std::span<const std::int16_t> offsets,
                            int sectionIndex);

  bool isLightCorrect() const noexcept override { return lightCorrect_; }
  void setLightCorrect(bool correct) override { lightCorrect_ = correct; }

  // InitializeLight 阶段提交给光照引擎的请求序号，供 Light 阶段按区块等待。
  void setLightSubmissionGeneration(std::uint64_t generation) noexcept {
    lightGeneration_ = generation;
  }
  std::uint64_t lightSubmissionGeneration() const noexcept {
    return lightGeneration_;
  }

  std::int64_t inhabitedTime() const noexcept { return inhabitedTime_; }
  void setInhabitedTime(std::int64_t time) noexcept { inhabitedTime_ = time; }

  // 区段数组按需分配：只有真正写入方块或生物群系的区块（BIOMES 阶段起）
  // 才会创建全部区段；只停在 STRUCTURE_STARTS 的依赖区块不再为每个区段
  // 预留调色板容器与光照槽。
  void ensureSections() const;
  bool hasSections() const noexcept { return !sections_.empty(); }

  ProtoChunkSection &section(int index) {
    ensureSections();
    return sections_.at(static_cast<std::size_t>(index));
  }
  const ProtoChunkSection &section(int index) const {
    ensureSections();
    return sections_.at(static_cast<std::size_t>(index));
  }
  const std::vector<ProtoChunkSection> &sections() const {
    ensureSections();
    return sections_;
  }
  std::vector<ProtoChunkSection> &sections() {
    ensureSections();
    return sections_;
  }

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

  ChunkTicketPos pos_;
  int minY_;
  int height_;
  AtomicChunkStatus status_{};
  bool lightCorrect_ = false;
  std::int64_t inhabitedTime_ = 0;
  std::uint64_t lightGeneration_ = 0;
  std::string defaultBiome_ = "minecraft:plains";
  mutable std::vector<ProtoChunkSection> sections_;
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

// 已加载 `World` 完整区块的生成器视图包装器（对齐 26.1 `ImposterProtoChunk.java`）。
// 当周围正在生成的区块经由 `WorldGenRegion` 读取邻域中一个已达 `FULL` 状态的 `World` 区块时，
// 通过该适配器统一提供 `ChunkAccess` 接口，并把 `*WG` 高度图请求自动重定向到对应的终态高度图。
class ImposterProtoChunk final : public ChunkAccess {
public:
  ImposterProtoChunk(World &world, ChunkTicketPos pos, bool allowWrites = false);

  World &wrappedWorld() noexcept { return world_; }
  const World &wrappedWorld() const noexcept { return world_; }
  bool allowWrites() const noexcept { return allowWrites_; }

  ChunkTicketPos pos() const noexcept override { return pos_; }
  int minY() const noexcept override { return minY_; }
  int height() const noexcept override { return height_; }

  ChunkStatusId persistedStatus() const noexcept override {
    return ChunkStatusId::Full;
  }
  void setPersistedStatus(ChunkStatusId) override {}

  BlockState getBlockState(int x, int y, int z) const override;
  FluidState getFluidState(int x, int y, int z) const override;
  std::optional<BlockState> setBlockState(int x, int y, int z,
                                          const BlockState &state,
                                          int flags = 3) override;

  std::string_view getNoiseBiome(int quartX, int quartY,
                                 int quartZ) const override;
  void setNoiseBiome(int quartX, int quartY, int quartZ,
                     std::string_view biomeId) override;

  int getHeight(HeightmapType type, int x, int z) override;
  void markPosForPostprocessing(int, int, int) override {}

  bool isLightCorrect() const noexcept override { return lightCorrect_; }
  void setLightCorrect(bool correct) override { lightCorrect_ = correct; }

private:
  static constexpr HeightmapType fixType(HeightmapType type) noexcept {
    if (type == HeightmapType::WorldSurfaceWG)
      return HeightmapType::WorldSurface;
    if (type == HeightmapType::OceanFloorWG)
      return HeightmapType::OceanFloor;
    return type;
  }

  World &world_;
  ChunkTicketPos pos_;
  int minY_;
  int height_;
  bool allowWrites_;
  bool lightCorrect_ = true;
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
LifecycleLoadedChunk promoteProtoChunkToLoaded(ProtoChunk &&chunk,
                                               std::uint64_t epoch);

} // namespace redstone::worldgen
