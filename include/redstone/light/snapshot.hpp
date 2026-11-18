#pragma once

// 光照快照与持久化数据结构：定义无锁共享的 4-bit 半字节光照页、轻量层描述符、区块输入载荷与可发布的区块光照快照。

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "redstone/light/optical.hpp"
#include "redstone/light/types.hpp"

namespace redstone {

// 生成期零拷贝句柄指向的区块基类；这里只前向声明，避免
// chunk_section.hpp -> engine_types.hpp -> light.hpp -> service.hpp 的循环包含。
class BaseChunk;

static_assert(sizeof(std::atomic<std::uint8_t>) == 1,
              "published light state and page bytes must stay one byte");
static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
              "published light nibbles require lock-free atomic bytes");

// 独立持有的单层光照描述符。
// 之所以采用 `state_` + `PackedPagePtr` 的双态布局，是为了让全缺失（`Missing`）或全均一（`Uniform`）的区块段完全不产生堆分配，仅用 `state_` 高半字节标记均一标志、低半字节存均一亮度；只有混合（`Mixed`）区块段才持有一个共享的 `atomic<uint8_t>[2048]` 半字节页。唯一写入线程与所有读取线程均通过 `relaxed` 原子字节加载/存储并发访问该页；拓扑切换（如均一与混合互转）会直接替换新区块段头中的描述符，绝不原地篡改 `packed_` 指针。
class LightLayerSnapshot {
public:
  enum class Storage : std::uint8_t { Missing, Uniform, Mixed };
  struct PackedPage {
    std::atomic<std::uint8_t> bytes[kLightLayerBytes];
  };
  static_assert(sizeof(PackedPage) == kLightLayerBytes);
  using PackedPagePtr = std::shared_ptr<PackedPage>;

  LightLayerSnapshot();
  LightLayerSnapshot(const LightLayerSnapshot &other);
  LightLayerSnapshot &operator=(const LightLayerSnapshot &other);
  LightLayerSnapshot(LightLayerSnapshot &&other) noexcept;
  LightLayerSnapshot &operator=(LightLayerSnapshot &&other) noexcept;
  ~LightLayerSnapshot();

  static LightLayerSnapshot missing();
  static LightLayerSnapshot uniform(std::uint8_t value);
  static LightLayerSnapshot mixed(
      const std::array<std::uint8_t, kLightVolume> &values);
  static LightLayerSnapshot mixedPacked(
      const std::array<std::uint8_t, kLightLayerBytes> &packed);
  // 为工作线程自有的混合层分配唯一一份 2048 字节清零光照页。
  static LightLayerSnapshot mixedEmpty();

  Storage storage() const {
    const std::uint8_t state =
        state_.load(std::memory_order_acquire);
    if ((state & kLightUniformFlag) != 0)
      return Storage::Uniform;
    return packed_ ? Storage::Mixed : Storage::Missing;
  }
  bool isMissing() const { return storage() == Storage::Missing; }
  bool isUniform() const { return storage() == Storage::Uniform; }
  bool isMixed() const { return storage() == Storage::Mixed; }
  std::uint8_t stateByte() const {
    return state_.load(std::memory_order_relaxed);
  }
  const PackedPagePtr &packedPtr() const { return packed_; }
  PackedPagePtr takePackedPtr() && { return std::move(packed_); }
  std::uint8_t uniformValue() const {
    return static_cast<std::uint8_t>(
        stateByte() & static_cast<std::uint8_t>(~kLightUniformFlag & 15));
  }
  std::uint8_t get(int x, int y, int z) const;
  std::size_t packedByteCount() const {
    return isMixed() ? kLightLayerBytes : 0;
  }
  std::uint8_t packedByte(std::size_t index) const {
    if (index >= kLightLayerBytes || !packed_)
      return 0;
    return packed_->bytes[index].load(std::memory_order_relaxed);
  }
  // 仅允许该光照层的唯一工作线程调用以下写入接口，因此无需使用 CAS。
  void writeCell(std::size_t index, std::uint8_t value);
  void writePackedByte(std::size_t byte, std::uint8_t value);
  void readPacked(std::array<std::uint8_t, kLightLayerBytes> &out) const;
  bool operator==(const LightLayerSnapshot &other) const;

private:
  void makeMixed();

  std::atomic<std::uint8_t> state_{0};
  PackedPagePtr packed_;
};

// 扁平值语义的光照层引用描述符。
// 之所以在 `LightSectionSnapshot` 中使用它而不是直接内嵌 `LightLayerSnapshot`，是因为 `LightLayerRef` 是普通值类型：`Missing` 和 `Uniform` 零堆分配，`Mixed` 直接共享同一个 `PackedPage`（2048 个原子字节），从而避免额外的中间堆节点与原子 `state_` 开销。
class LightLayerRef {
public:
  LightLayerRef() = default;
  LightLayerRef(const LightLayerSnapshot &value)
      : state_(value.stateByte()), packed_(value.packedPtr()) {}
  LightLayerRef(LightLayerSnapshot &&value) noexcept
      : state_(value.stateByte()),
        packed_(std::move(value).takePackedPtr()) {}
  explicit LightLayerRef(LightLayerSnapshot::PackedPagePtr page) noexcept
      : state_(0), packed_(std::move(page)) {}

  LightLayerRef &operator=(const LightLayerSnapshot &value) {
    state_ = value.stateByte();
    packed_ = value.packedPtr();
    return *this;
  }
  LightLayerRef &operator=(LightLayerSnapshot &&value) noexcept {
    state_ = value.stateByte();
    packed_ = std::move(value).takePackedPtr();
    return *this;
  }
  LightLayerRef &operator=(LightLayerSnapshot::PackedPagePtr page) noexcept {
    state_ = 0;
    packed_ = std::move(page);
    return *this;
  }

  LightLayerSnapshot::Storage storage() const {
    if ((state_ & kLightUniformFlag) != 0)
      return LightLayerSnapshot::Storage::Uniform;
    return packed_ ? LightLayerSnapshot::Storage::Mixed
                   : LightLayerSnapshot::Storage::Missing;
  }
  bool isMissing() const {
    return (state_ & kLightUniformFlag) == 0 && !packed_;
  }
  bool isUniform() const { return (state_ & kLightUniformFlag) != 0; }
  bool isMixed() const {
    return (state_ & kLightUniformFlag) == 0 && packed_ != nullptr;
  }
  std::uint8_t stateByte() const { return state_; }
  std::uint8_t uniformValue() const {
    return isUniform()
               ? static_cast<std::uint8_t>(
                     state_ & static_cast<std::uint8_t>(~kLightUniformFlag & 15))
               : 0;
  }
  std::uint8_t get(int x, int y, int z) const {
    if ((state_ & kLightUniformFlag) != 0)
      return static_cast<std::uint8_t>(
          state_ & static_cast<std::uint8_t>(~kLightUniformFlag & 15));
    if (!packed_)
      return 0;
    const std::size_t index = lightIndex(x, y, z);
    const std::uint8_t packed =
        packed_->bytes[index >> 1].load(std::memory_order_relaxed);
    return static_cast<std::uint8_t>((packed >> ((index & 1) * 4)) & 15);
  }
  std::uint8_t packedByte(std::size_t index) const {
    if (index >= kLightLayerBytes || !packed_)
      return 0;
    return packed_->bytes[index].load(std::memory_order_relaxed);
  }
  void writeCell(std::size_t index, std::uint8_t value) const;
  void writePackedByte(std::size_t byte, std::uint8_t value) const;
  void readPacked(std::array<std::uint8_t, kLightLayerBytes> &out) const;
  const LightLayerSnapshot::PackedPagePtr &shared() const { return packed_; }
  // 通过比较状态字节与共享页指针快速判断底层载荷是否完全同一份，避免在提交阶段无谓拷贝区块段头。
  bool samePayload(const LightLayerRef &other) const {
    return state_ == other.state_ && packed_ == other.packed_;
  }
  std::size_t packedByteCount() const {
    return isMixed() ? kLightLayerBytes : 0;
  }
  bool operator==(const LightLayerRef &other) const;
  bool operator!=(const LightLayerRef &other) const {
    return !(*this == other);
  }

private:
  std::uint8_t state_{0};
  LightLayerSnapshot::PackedPagePtr packed_;
};

// 单个区块段的光照快照头部，持有方块光与天空光两个独立槽位。
// 存在原因：在双层独立发布的架构下，生产视图只需填充自身对应层的槽位并把另一层留作 `Missing`；光学方向遮挡掩码仅由协调器内部的几何结构持有，不会冗余复制到发布给主线程/网络层的光照快照中。
struct LightSectionSnapshot {
  explicit LightSectionSnapshot(
      int sectionY = 0, bool empty = true,
      std::array<LightLayerRef, kLightLayerCount> layers = {},
      std::uint16_t unsupportedCells = 0);

  int sectionY = 0;
  bool empty = true;
  std::array<LightLayerRef, kLightLayerCount> layers{};
  std::uint16_t unsupportedCells = 0;

  std::uint8_t light(LightLayer layer, int x, int y, int z) const {
    return layers[static_cast<std::size_t>(layer)].get(x, y, z);
  }
};

// 从存档读取的单个区块段持久化光照数据（对应 Anvil `sections[].BlockLight` / `SkyLight`）。
struct PersistedLightSection {
  int sectionY = 0;
  std::optional<std::array<std::uint8_t, 2048>> blockLight;
  std::optional<std::array<std::uint8_t, 2048>> skyLight;
};

// 整区块的持久化光照载荷；当 `isLightOn` 为 true 时，服务端可直接复原已有半字节数据而无需整区块重算。
struct PersistedChunkLight {
  bool isLightOn = false;
  std::vector<PersistedLightSection> sections;
};

// 主线程向 `LightService::submitChunk` 提交区块加载/重载时使用的输入载荷。
// 之所以同时支持 `geometrySections` 与 `directGeometry`，是为了兼顾独立测试/标准快照隔离模式与 `World` 直读低内存模式。
struct LightChunkInput {
  int chunkX = 0;
  int chunkZ = 0;
  // 可选的 `World` 侧生命周期令牌。独立使用 `LightService` 时保持为 0；由 `World` 传入时会在每次卸载/重载后递增，确保旧生命周期内滞留队列的方块变更不会被误应用到新加载的同名区块上。
  std::uint64_t lifecycleEpoch = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  // 当 `directGeometry` 为 false 时，须包含恰好 `blockSectionCount` 个由 `World::Chunk::Section` 或测试用 `LightSectionView` 转换而来的区块段几何；若启用 `directGeometry`，则省略 `geometrySections` 并由工作线程按需直读。
  bool directGeometry = false;
  // 生成期零拷贝句柄：非空时工作线程直接读该区块的区段，`geometrySections` 为空。
  // 句柄同时保证对象存活；安装后由 `adoptDirectGeometry` 换成 `World` 直读。
  std::shared_ptr<const BaseChunk> chunkHandle;
  std::vector<GeometrySection> geometrySections;
  std::optional<PersistedChunkLight> persistedLight;
  // 本次提交如何推进到“完整区块”：
  //   Immediate —— 立即在工作线程计算（默认）。
  //   Deferred  —— 只注册几何与空壳，区块停在“未计算”，由
  //                `LightService::requestChunkCompute()` 触发首次计算。
  // 若 `persistedLight.isLightOn` 为真，则总是走“直接完整”（装填存档半字节、
  // 不跑 BFS），与本字段无关。
  enum class Completion { Immediate, Deferred };
  Completion completion = Completion::Immediate;

  // 光照区块段比方块区块段在上下各多 1 个缓冲区段（对应原版 `minSection - 1 .. maxSection + 1`）。
  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
};

// 异步光照工作线程向主线程/协议编码发布的单层区块光照快照。
// 存在原因：封装完整的区块段光照视图、生命周期代次、完成度标志以及面向网络协议的 `ready-to-ready` 增量脏区列表。
struct LightChunkSnapshot {
  struct LayerChange {
    int sectionY = 0;
    bool sky = false;
    bool block = false;
  };
  int chunkX = 0;
  int chunkZ = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  // 触发本次快照的最近一次输入请求序号。
  std::uint64_t generation = 0;
  // `lifecycleEpoch` 用于剔除已卸载/重载区块的过期结果；`revision` 由单一工作线程按单调顺序分配，用于区分同一加载生命周期内的不同完成版本，不受无序批处理中输入序号交错的影响。
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t revision = 0;
  bool skyLightEnabled = true;
  std::optional<LightLayer> layer;
  bool blockLightComplete = false;
  bool skyLightComplete = false;
  bool propagationComplete = false;
  // 仅当所有已加载区块拥有完整光学元数据且该层传播完全收敛时置为 true；协议编码阶段绝不会向客户端暴露尚未收敛的结果。
  bool protocolReady = false;
  std::uint32_t unsupportedCells = 0;
  std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;
  // `changedSections` 表示两次 `protocolReady` 快照之间的增量脏段集合。`deltaBaseRevision` 指明该增量所基于的上一个 `protocolReady` 快照版本号；尚未收敛的中间排空版本不会推进该基准。
  std::uint64_t deltaBaseRevision = 0;
  std::vector<LayerChange> changedSections;

  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
  // 按区块段 Y 坐标查找对应光照段快照（优先用标准偏移直算，兼容截断/自定义起始段回退）。
  const LightSectionSnapshot *section(int sectionY) const {
    const int index = sectionY - (minBlockSectionY - 1);
    if (index >= 0 && static_cast<std::size_t>(index) < sections.size()) {
      const auto &entry = sections[static_cast<std::size_t>(index)];
      if (entry && entry->sectionY == sectionY)
        return entry.get();
    }
    if (sections.empty() || !sections.front())
      return nullptr;
    const int fallback = sectionY - sections.front()->sectionY;
    if (fallback < 0 || static_cast<std::size_t>(fallback) >= sections.size())
      return nullptr;
    const auto &entry = sections[static_cast<std::size_t>(fallback)];
    return (entry && entry->sectionY == sectionY) ? entry.get() : nullptr;
  }
  // 按原版 `SkyLightSectionStorage` 语义查询指定世界坐标的光照值：若天空光所在段为 `Missing`，则向上寻找最近一个非缺失已存储段并取其底面（`y = 0`）的值（若上方全缺失则返回 15）。通过 `section()` 仍可直接读取原始层状态，因此“缺失天空光段（向下继承）”与“显式存储全 0 层”语义严格区分。
  int lightValue(LightLayer layer, int x, int y, int z) const;
};

} // namespace redstone
