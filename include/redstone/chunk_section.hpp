#pragma once
// 共享区块数据层：BlockSection 是 ProtoChunk 与 World::Chunk 共用的区段存储，
// BaseChunk 是两者共用的非虚基类（位置/高度、区段数组、生物群系、加载与光照代次）。
// 世界生成、模拟、光照与存档不再各自维护一套方块/群系容器。

#include "redstone/block.hpp"
#include "redstone/engine_types.hpp"
#include "redstone/fluid.hpp"
#include "redstone/light/types.hpp"
#include "redstone/tickets.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {

inline constexpr int kSectionCapacity = 24;
inline constexpr int kRandomTickMinisectionSize = 248;
inline constexpr int kRandomTickMinisectionCount =
    (kLightVolume + kRandomTickMinisectionSize - 1) /
    kRandomTickMinisectionSize;
inline constexpr int kRandomTickLastMinisectionSize =
    kLightVolume -
    kRandomTickMinisectionSize * (kRandomTickMinisectionCount - 1);
static_assert(kSectionCapacity <= 32,
              "randomTickingSectionMask requires <= 32 sections");

// 世界生成期/存档期区段的两个光照层。按需分配 2 KiB，World 模拟路径不使用这两层。
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

// 4x4x4 Quart 生物群系容器的非拥有视图。存储属于 BaseChunk::biomeSections；
// 空指针表示该区段均匀默认群系（minecraft:plains），保持零额外分配。
class BiomePaletteContainer {
public:
  static constexpr std::size_t kCellCount = 64;

  BiomePaletteContainer(std::shared_ptr<SectionBiomes> &storage,
                        std::string defaultBiome);

  static constexpr std::size_t index(int qx, int qy, int qz) noexcept {
    return static_cast<std::size_t>(((qy & 3) << 4) | ((qz & 3) << 2) |
                                    (qx & 3));
  }

  const std::string &get(int qx, int qy, int qz) const;
  void set(int qx, int qy, int qz, std::string_view biomeId);
  void fill(std::string_view biomeId);

  bool isUniform() const;
  const std::vector<std::string> &palette();
  const std::array<std::uint8_t, kCellCount> &indices();

  void pack(std::vector<std::string> &outPalette,
            std::vector<std::uint64_t> &outData, int &outBits) const;
  bool unpack(std::span<const std::string> palette,
              std::span<const std::uint64_t> data, std::string &error);

private:
  SectionBiomes &ensure();
  const SectionBiomes *peek() const;
  std::uint8_t intern(std::string_view biomeId);

  std::shared_ptr<SectionBiomes> &storage_;
  std::string defaultBiome_;
};

// 调色板存储基类：由堆块自身携带位宽，最多持有一块堆内存。
struct SectionStorage {
  std::uint16_t paletteSize = 0;
  std::uint8_t bits = 0; // 1..8 或 16
};

template <std::uint8_t Bits>
struct SectionStoragePacked final : SectionStorage {
  static_assert(Bits >= 1 && Bits <= 7);
  static constexpr std::size_t kPerWord = std::size_t{64} / Bits;
  static constexpr std::size_t kWords =
      (kLightVolume + kPerWord - 1) / kPerWord;
  static constexpr std::size_t kPaletteCapacity = std::size_t{1} << Bits;
  static constexpr std::uint64_t kMask = (std::uint64_t{1} << Bits) - 1u;

  SectionStoragePacked() { bits = Bits; }
  std::array<std::uint64_t, kWords> words{};
  std::array<BlockState::Id, kPaletteCapacity> palette{};
  std::array<std::uint8_t, kPaletteCapacity> paletteTicking{};

  std::uint16_t indexAt(std::size_t cell) const noexcept {
    const std::size_t word = cell / kPerWord;
    const std::size_t shift = (cell % kPerWord) * Bits;
    return static_cast<std::uint16_t>((words[word] >> shift) & kMask);
  }
  void setIndexAt(std::size_t cell, std::uint16_t value) noexcept {
    const std::size_t word = cell / kPerWord;
    const std::size_t shift = (cell % kPerWord) * Bits;
    words[word] = (words[word] & ~(kMask << shift)) |
                  ((static_cast<std::uint64_t>(value) & kMask) << shift);
  }
  void atomicSetIndexAt(std::size_t cell, std::uint16_t value) noexcept {
    const std::size_t word = cell / kPerWord;
    const std::size_t shift = (cell % kPerWord) * Bits;
    const std::uint64_t next =
        (words[word] & ~(kMask << shift)) |
        ((static_cast<std::uint64_t>(value) & kMask) << shift);
    std::atomic_ref<std::uint64_t>(words[word])
        .store(next, std::memory_order_release);
  }
};

struct SectionStorage8 final : SectionStorage {
  static constexpr std::size_t kPaletteCapacity = 256;
  SectionStorage8() { bits = 8; }
  std::array<std::uint8_t, kLightVolume> indices{};
  std::array<BlockState::Id, 256> palette{};
  std::array<std::uint8_t, 256> paletteTicking{};

  std::uint16_t indexAt(std::size_t cell) const noexcept {
    return indices[cell];
  }
  void setIndexAt(std::size_t cell, std::uint16_t value) noexcept {
    indices[cell] = static_cast<std::uint8_t>(value);
  }
  void atomicSetIndexAt(std::size_t cell, std::uint16_t value) noexcept {
    std::atomic_ref<std::uint8_t>(indices[cell])
        .store(static_cast<std::uint8_t>(value), std::memory_order_release);
  }
};

struct SectionStorage16 final : SectionStorage {
  static constexpr std::size_t kPaletteCapacity = kLightVolume;
  SectionStorage16() { bits = 16; }
  std::array<std::uint16_t, kLightVolume> indices{};
  std::array<BlockState::Id, kLightVolume> palette{};
  std::array<std::uint8_t, kLightVolume> paletteTicking{};

  std::uint16_t indexAt(std::size_t cell) const noexcept {
    return indices[cell];
  }
  void setIndexAt(std::size_t cell, std::uint16_t value) noexcept {
    indices[cell] = value;
  }
  void atomicSetIndexAt(std::size_t cell, std::uint16_t value) noexcept {
    std::atomic_ref<std::uint16_t>(indices[cell])
        .store(value, std::memory_order_release);
  }
};

inline std::uint8_t minBitsForPaletteSize(std::size_t paletteSize) noexcept {
  if (paletteSize <= 2)
    return 1;
  if (paletteSize <= 4)
    return 2;
  if (paletteSize <= 8)
    return 3;
  if (paletteSize <= 16)
    return 4;
  if (paletteSize <= 32)
    return 5;
  if (paletteSize <= 64)
    return 6;
  if (paletteSize <= 128)
    return 7;
  if (paletteSize <= 256)
    return 8;
  return 16;
}

std::shared_ptr<SectionStorage>
makeSectionStorageForBits(std::uint8_t bits);

template <typename StoragePtr, typename Fn>
decltype(auto) visitSectionStorage(StoragePtr *base, Fn &&fn) {
  if constexpr (std::is_const_v<StoragePtr>) {
    switch (base->bits) {
    case 1:
      return fn(static_cast<const SectionStoragePacked<1> *>(base));
    case 2:
      return fn(static_cast<const SectionStoragePacked<2> *>(base));
    case 3:
      return fn(static_cast<const SectionStoragePacked<3> *>(base));
    case 4:
      return fn(static_cast<const SectionStoragePacked<4> *>(base));
    case 5:
      return fn(static_cast<const SectionStoragePacked<5> *>(base));
    case 6:
      return fn(static_cast<const SectionStoragePacked<6> *>(base));
    case 7:
      return fn(static_cast<const SectionStoragePacked<7> *>(base));
    case 8:
      return fn(static_cast<const SectionStorage8 *>(base));
    case 16:
      return fn(static_cast<const SectionStorage16 *>(base));
    }
    return fn(static_cast<const SectionStorage16 *>(base));
  } else {
    switch (base->bits) {
    case 1:
      return fn(static_cast<SectionStoragePacked<1> *>(base));
    case 2:
      return fn(static_cast<SectionStoragePacked<2> *>(base));
    case 3:
      return fn(static_cast<SectionStoragePacked<3> *>(base));
    case 4:
      return fn(static_cast<SectionStoragePacked<4> *>(base));
    case 5:
      return fn(static_cast<SectionStoragePacked<5> *>(base));
    case 6:
      return fn(static_cast<SectionStoragePacked<6> *>(base));
    case 7:
      return fn(static_cast<SectionStoragePacked<7> *>(base));
    case 8:
      return fn(static_cast<SectionStorage8 *>(base));
    case 16:
      return fn(static_cast<SectionStorage16 *>(base));
    }
    return fn(static_cast<SectionStorage16 *>(base));
  }
}

// 统一区段：World 与 ProtoChunk 共用同一份存储与同一套计数。
struct BlockSection {
  std::uint16_t nonEmpty = 0;
  std::uint16_t tickingBlockCount = 0;
  BlockState::Id single = 0;

private:
  std::int8_t sectionY_ = 0;

public:
  std::int8_t sectionY() const noexcept { return sectionY_; }
  void setSectionY(std::int8_t y) noexcept { sectionY_ = y; }
  std::array<std::uint8_t, kRandomTickMinisectionCount>
      randomTickByMinisection{};
  // 最近一次成功写入的 (状态 id, 调色板索引) 记忆：地形填充连续写同一状态时
  // 跳过调色板线性查找。只在索引仍指向同一状态时使用，扩容/追加都不会移动已有索引。
  std::uint32_t lastPaletteState = 0;
  std::uint16_t lastPaletteIndex = 0;
  bool lastPaletteValid = false;
  std::shared_ptr<SectionStorage> storage;
  LightLayerStorage blockLight;
  LightLayerStorage skyLight;

  BlockSection() = default;
  BlockSection(const BlockSection &) = delete;
  BlockSection &operator=(const BlockSection &) = delete;
  BlockSection(BlockSection &&) noexcept = default;
  BlockSection &operator=(BlockSection &&) noexcept = default;

  std::uint8_t bits() const { return storage ? storage->bits : 0; }
  bool hasOnlyAir() const { return nonEmpty == 0; }
  bool isRandomlyTicking() const { return tickingBlockCount > 0; }

  // 共享底层存储的浅拷贝：复制计数、单值、随机刻分桶与区段号，`storage` 保持同一份。
  // Proto -> World 晋升用它把区段交给 World，同时让仍持有 ProtoChunk 指针的光照
  // 工作线程可以继续读到同一份方块数据（不会把源对象掏空）。
  BlockSection share() const;

  BlockState::Id get(int index) const;
  bool tryGetRandomlyTicking(int index, BlockState::Id &outId) const;
  void initSingleRandomTick(BlockState::Id state);
  BlockState::Id set(int index, BlockState::Id state, bool compact = false,
                     std::shared_mutex *resizeMutex = nullptr);

  static constexpr std::size_t cellIndex(int localX, int localY,
                                         int localZ) noexcept {
    return (static_cast<std::size_t>(localY & 15) << 8) |
           (static_cast<std::size_t>(localZ & 15) << 4) |
           static_cast<std::size_t>(localX & 15);
  }
  BlockState getBlockState(int localX, int localY, int localZ) const {
    return BlockState::fromId(
        get(static_cast<int>(cellIndex(localX, localY, localZ))));
  }
  BlockState setBlockState(int localX, int localY, int localZ,
                           const BlockState &state) {
    return BlockState::fromId(
        set(static_cast<int>(cellIndex(localX, localY, localZ)), state.id()));
  }
  FluidState getFluidState(int localX, int localY, int localZ) const {
    return fluidStateOf(getBlockState(localX, localY, localZ));
  }
};

enum class ChunkKind : std::uint8_t {
  Proto = 0,
  World = 1,
};

// ProtoChunk 与 WorldChunk 共用的非虚基类。方块区段、生物群系、结构缓存、
// 失效标志与加载/光照生命周期代次都放在这里；两种 chunk 只追加各自阶段/模拟字段。
class BaseChunk {
public:
  using OpaqueUniquePtr = std::unique_ptr<void, void (*)(void *)>;

  BaseChunk() = default;
  explicit BaseChunk(ChunkKind kind) noexcept : kind_(kind) {}
  BaseChunk(const BaseChunk &) = delete;
  BaseChunk &operator=(const BaseChunk &) = delete;
  BaseChunk(BaseChunk &&) noexcept = default;
  BaseChunk &operator=(BaseChunk &&) noexcept = default;
  ~BaseChunk() = default;

  ChunkKind kind() const noexcept { return kind_; }
  bool isProtoChunk() const noexcept { return kind_ == ChunkKind::Proto; }
  bool isWorldChunk() const noexcept { return kind_ == ChunkKind::World; }

  bool isInvalidated() const noexcept {
    return invalidated_.value.load(std::memory_order_acquire);
  }
  void markInvalidated() noexcept {
    invalidated_.value.store(true, std::memory_order_release);
  }
  void clearInvalidated() noexcept {
    invalidated_.value.store(false, std::memory_order_release);
  }

  int generationUserCount() const noexcept {
    return generationUsers_.value.load(std::memory_order_acquire);
  }
  void addGenerationUser() noexcept {
    generationUsers_.value.fetch_add(1, std::memory_order_acq_rel);
  }
  void releaseGenerationUser() noexcept {
    generationUsers_.value.fetch_sub(1, std::memory_order_acq_rel);
  }

  ChunkTicketPos pos() const noexcept { return pos_; }
  int minY() const noexcept { return minY_; }
  int height() const noexcept { return height_; }
  int maxY() const noexcept { return minY_ + height_; }
  int minSectionY() const noexcept { return minY_ >> 4; }
  int sectionCount() const noexcept { return height_ >> 4; }
  int sectionIndex(int y) const noexcept { return (y - minY_) >> 4; }
  bool isOutsideBuildHeight(int y) const noexcept {
    return y < minY_ || y >= maxY();
  }

  // 设置区块坐标/高度并初始化每个区段的 sectionY。World 在创建区块时调用，
  // ProtoChunk 在构造函数里调用。
  void initialize(ChunkTicketPos position, int minY, int height) {
    pos_ = position;
    minY_ = minY;
    height_ = height;
    for (int i = 0; i < sectionCount(); ++i)
      sections[static_cast<std::size_t>(i)].setSectionY(
          static_cast<std::int8_t>((minY_ >> 4) + i));
  }

  BlockSection &section(int index) {
    return sections[static_cast<std::size_t>(index)];
  }
  const BlockSection &section(int index) const {
    return sections[static_cast<std::size_t>(index)];
  }
  std::span<BlockSection> sectionSpan() {
    return std::span<BlockSection>(sections.data(),
                                   static_cast<std::size_t>(sectionCount()));
  }
  std::span<const BlockSection> sectionSpan() const {
    return std::span<const BlockSection>(
        sections.data(), static_cast<std::size_t>(sectionCount()));
  }

  std::string_view defaultBiome() const noexcept { return defaultBiome_; }
  void setDefaultBiome(std::string biome) {
    defaultBiome_ = biome.empty() ? std::string("minecraft:plains")
                                  : std::move(biome);
  }

  BiomePaletteContainer biomes(int sectionIndex,
                               std::string_view defaultBiome = {});
  BiomePaletteContainer biomes(int sectionIndex,
                               std::string_view defaultBiome = {}) const {
    const int index =
        sectionIndex >= 0 && sectionIndex < sectionCount() ? sectionIndex : 0;
    return BiomePaletteContainer(
        const_cast<std::shared_ptr<SectionBiomes> &>(
            biomeSections[static_cast<std::size_t>(index)]),
        defaultBiome.empty() ? defaultBiome_ : std::string(defaultBiome));
  }

  std::string_view getNoiseBiome(int quartX, int quartY, int quartZ) const;

  bool isLightCorrect() const noexcept { return lightCorrect_; }
  void setLightCorrect(bool correct) noexcept { lightCorrect_ = correct; }

  BlockState getBlockState(int x, int y, int z) const;
  FluidState getFluidState(int x, int y, int z) const;

  // 独占结构缓存（替代原本每格原子拷贝的 shared_ptr<void>），Proto 与 WorldChunk 共用。
  void *structureStartsRaw() const noexcept { return structureData_.get(); }
  void setStructureStartsRaw(OpaqueUniquePtr data) noexcept {
    structureData_ = std::move(data);
  }
  OpaqueUniquePtr takeStructureStartsRaw() noexcept {
    return std::move(structureData_);
  }

protected:
  template <typename T> struct MovableAtomic {
    std::atomic<T> value{};
    MovableAtomic() noexcept = default;
    explicit MovableAtomic(T init) noexcept : value(init) {}
    MovableAtomic(const MovableAtomic &o) noexcept
        : value(o.value.load(std::memory_order_acquire)) {}
    MovableAtomic &operator=(const MovableAtomic &o) noexcept {
      value.store(o.value.load(std::memory_order_acquire),
                  std::memory_order_release);
      return *this;
    }
    MovableAtomic(MovableAtomic &&o) noexcept
        : value(o.value.load(std::memory_order_acquire)) {}
    MovableAtomic &operator=(MovableAtomic &&o) noexcept {
      value.store(o.value.load(std::memory_order_acquire),
                  std::memory_order_release);
      return *this;
    }
  };

  ChunkKind kind_ = ChunkKind::World;
  MovableAtomic<bool> invalidated_{false};
  MovableAtomic<int> generationUsers_{0};
  ChunkTicketPos pos_{};
  int minY_ = -64;
  int height_ = 384;
  bool lightCorrect_ = false;
  std::string defaultBiome_ = "minecraft:plains";
  OpaqueUniquePtr structureData_{nullptr, +[](void *) {}};

public:
  std::array<BlockSection, kSectionCapacity> sections;
  std::array<std::shared_ptr<SectionBiomes>, kSectionCapacity> biomeSections;
  // 由 ChunkLifecycleController 分配的加载生命周期；Proto -> World 晋升时保持不变。
  std::uint64_t loadEpoch = 0;
  std::uint64_t lightEpoch = 0;
  // 指向所属 ChunkHolder 的数据护栏锁（非空时区段扩容加写锁、并发直读加共享锁）。
  std::shared_mutex *resizeMutex = nullptr;
};

} // namespace redstone
