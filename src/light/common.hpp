#pragma once

// 光照引擎内部公共基础结构：提供高性能扁平哈希容器、坐标与区块键工具、批处理上下文、可写光照层封装及双引擎共享的只读宿主上下文。

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "redstone/hash.hpp"
#include "redstone/light/service.hpp"
#include "redstone/light/snapshot.hpp"
#include "redstone/light/types.hpp"

namespace redstone {
const BlockLightProperties &unsupportedLightProperties() noexcept;
}

namespace redstone::light_internal {

// 直读低内存模式下记录在工作线程中的区块元数据（无需保存几何单元，仅保留坐标与生命周期代次）。
// 把实时 `BaseChunk` 的单个区段包装成直读视图：与 `World::sectionToLightView`
// 相同的原子读取契约（索引原子读、paletteSize 原子读），并用 `storageOwner`
// 延长底层存储的生命周期。
LightSectionView sectionViewOfLiveChunk(const BaseChunk &chunk, int sectionY,
                                        int minBlockSectionY,
                                        int blockSectionCount);

struct DirectChunkMeta {
  int chunkX = 0;
  int chunkZ = 0;
  std::uint64_t lifecycleEpoch = 0;
  // 非空时直接读这个区块对象（生成期的 `ProtoChunk`，安装后换成 `World` 直读）。
  // 句柄同时负责保活：登记期间对象不会被销毁，安装或卸载时由控制命令替换。
  std::shared_ptr<const BaseChunk> chunk;
};

// 线程 CPU 亲和性设置结果，记录实际生效的掩码与平台错误码。
struct ThreadAffinityResult {
  std::uint64_t mask = 0;
  std::uint32_t error = 0;
};

// 返回当前线程累计消耗的 CPU 时间（毫秒），用于统计工作线程开销。
double currentThreadCpuMs();
#if defined(_WIN32) || defined(__linux__)
ThreadAffinityResult applyThreadAffinity(std::thread::native_handle_type handle,
                                         std::uintptr_t mask);
#endif

// 将 `(chunkX, chunkZ)` 打包为 64 位无符号区块键，便于哈希查找与排序发布。
inline std::uint64_t chunkKey(int x, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
         static_cast<std::uint32_t>(z);
}

// 计算区块坐标在 64 槽（8x8）直接映射 L1 缓存中的槽位索引。
inline std::size_t chunkL1Slot(int chunkX, int chunkZ) {
  return static_cast<std::size_t>(((chunkX & 7) << 3) | (chunkZ & 7));
}

inline std::size_t chunkL1Slot(std::uint64_t key) {
  const int chunkX = static_cast<int>(static_cast<std::int32_t>(key >> 32));
  const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
  return chunkL1Slot(chunkX, chunkZ);
}

// SplitMix64 混合哈希器，避免连续整数区块键在开放寻址表中发生聚簇退化。
struct UInt64Hash {
  std::size_t operator()(std::uint64_t x) const noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<std::size_t>(x);
  }
};

// 光照引擎内部扁平哈希容器：基于 `phmap::flat_hash_set`/`phmap::flat_hash_map`，
// 并为 `FlatMap` 维护 `rehashEpoch_` 供 `BlockLightPropagator` 的 L1 段指针缓存 O(1) 失效。
template <class Key, class Hash = std::hash<Key>>
using FlatSet = phmap::flat_hash_set<Key, Hash>;

template <class Key, class Value, class Hash = std::hash<Key>>
class FlatMap : public phmap::flat_hash_map<Key, Value, Hash> {
  using Base = phmap::flat_hash_map<Key, Value, Hash>;

public:
  using Base::Base;

  std::uint64_t rehashEpoch() const noexcept { return rehashEpoch_; }

  void reserve(std::size_t n) {
    const std::size_t before = this->bucket_count();
    Base::reserve(n);
    if (this->bucket_count() != before || erasedSinceRehash_) {
      ++rehashEpoch_;
      erasedSinceRehash_ = false;
    }
  }

  void clear() {
    Base::clear();
    ++rehashEpoch_;
    erasedSinceRehash_ = false;
  }

  std::size_t erase(const Key &key) {
    const std::size_t n = Base::erase(key);
    if (n > 0)
      erasedSinceRehash_ = true;
    return n;
  }

  typename Base::iterator erase(typename Base::iterator pos) {
    erasedSinceRehash_ = true;
    return Base::erase(pos);
  }

  typename Base::iterator erase(typename Base::const_iterator pos) {
    erasedSinceRehash_ = true;
    return Base::erase(pos);
  }

  template <class... Args>
  std::pair<typename Base::iterator, bool> try_emplace(const Key &key,
                                                       Args &&...args) {
    const std::size_t before = this->bucket_count();
    auto res = Base::try_emplace(key, std::forward<Args>(args)...);
    if (this->bucket_count() != before || (erasedSinceRehash_ && res.second)) {
      ++rehashEpoch_;
      erasedSinceRehash_ = false;
    }
    return res;
  }

  template <class... Args>
  std::pair<typename Base::iterator, bool> emplace(Args &&...args) {
    const std::size_t before = this->bucket_count();
    auto res = Base::emplace(std::forward<Args>(args)...);
    if (this->bucket_count() != before || (erasedSinceRehash_ && res.second)) {
      ++rehashEpoch_;
      erasedSinceRehash_ = false;
    }
    return res;
  }

  Value &operator[](const Key &key) {
    const std::size_t before = this->bucket_count();
    const std::size_t oldSize = this->size();
    Value &ref = Base::operator[](key);
    if (this->bucket_count() != before ||
        (erasedSinceRehash_ && this->size() != oldSize)) {
      ++rehashEpoch_;
      erasedSinceRehash_ = false;
    }
    return ref;
  }

private:
  std::uint64_t rehashEpoch_ = 1;
  bool erasedSinceRehash_ = false;
};

inline bool validLightValue(std::uint8_t value) { return value <= 15; }

// 判断方块在原版光照传播中是否可视为“无遮挡形状”（空气、非遮挡方块或未启用基于形状的面遮挡）。
inline bool emptyShape(const BlockLightProperties &properties) {
  return !properties.supported || !properties.canOcclude ||
         !properties.useShapeForLightOcclusion;
}

// 计算光进入该方块后的衰减量：受支持方块至少衰减 1 级，未受支持方块保守视为完全阻挡（16）。
inline int lightOpacity(const BlockLightProperties &properties) {
  return properties.supported ? std::max(1, int(properties.dampening)) : 16;
}

// 返回空气/虚空等无阻挡方块的默认光学属性常量。
const BlockLightProperties &emptyLightProperties();
inline bool hasLightFaceRect(const BlockLightProperties &properties,
                             int direction) noexcept {
  const std::uint8_t id =
      properties.occlusionShapeId < detail::kLightOcclusionShapeTableSize
          ? properties.occlusionShapeId
          : 0;
  return (detail::kLightFaceRectMask[id] & (1u << direction)) != 0;
}
inline bool faceShapesOcclude(const BlockLightProperties &from,
                              const BlockLightProperties &to,
                              int direction) noexcept {
  return lightFaceShapesOcclude(from, to,
                                static_cast<LightDirection>(direction));
}

// 世界空间方块坐标。
struct LightPos {
  int x = 0;
  int y = 0;
  int z = 0;
  bool operator==(const LightPos &) const = default;
};

struct LightPosHash {
  std::size_t operator()(const LightPos &position) const noexcept {
    std::uint64_t value = static_cast<std::uint32_t>(position.x);
    value = value * 0x9E3779B185EBCA87ull ^
            static_cast<std::uint32_t>(position.y);
    value = value * 0xC2B2AE3D27D4EB4Full ^
            static_cast<std::uint32_t>(position.z);
    return static_cast<std::size_t>(value ^ (value >> 32));
  }
};

// 三维区块段坐标 `(sectionX, sectionY, sectionZ)`。
struct SectionPos3 {
  int x = 0;
  int y = 0;
  int z = 0;
  bool operator==(const SectionPos3 &) const = default;
};

struct SectionPos3Hash {
  std::size_t operator()(const SectionPos3 &position) const noexcept {
    std::uint64_t value = static_cast<std::uint32_t>(position.x);
    value = value * 0x9E3779B185EBCA87ull ^
            static_cast<std::uint32_t>(position.y);
    value = value * 0xC2B2AE3D27D4EB4Full ^
            static_cast<std::uint32_t>(position.z);
    return static_cast<std::size_t>(value ^ (value >> 32));
  }
};

// 与 `LightDirection` 一一对应的 6 个正交邻居偏移表（下、上、北、南、西、东）。
inline constexpr std::array<LightPos, 6> kDirectionOffsets{{
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};

inline LightPos offset(LightPos position, int direction) {
  const LightPos delta = kDirectionOffsets[static_cast<std::size_t>(direction)];
  return {position.x + delta.x, position.y + delta.y, position.z + delta.z};
}

// 单个区块段在“未计算”阶段收到的入向光照。
// 存在原因：原版会把邻居推来的光直接写进已分配的光照段；我们的“未计算”区块还没有
// 光照壳，写不进去。跨区块的光只可能从 4 个水平侧面进入（同区块内部的段间传播在
// 同一个批次里完成），所以每段只需保存 4 x 16 x 16 格、4 bit/格 = 512 字节。
// 用 max 合并，天然去重，也不需要容量上限。
struct DeferredSectionInbox {
  static constexpr int kFaceCount = 4;
  static constexpr int kFaceNibbles = 256;
  static constexpr int kFaceBytes = kFaceNibbles / 2;   // 128
  // 面顺序对应“进入方向的反向”：(north,south,west,east) 边缘 = 方向 3,2,5,4。
  std::array<std::array<std::uint8_t, kFaceBytes>, kFaceCount> faces{};

  std::uint8_t get(int face, int u, int v) const {
    const int index = (v << 4) | u;
    const std::uint8_t packed = faces[face][index >> 1];
    return static_cast<std::uint8_t>((packed >> ((index & 1) * 4)) & 15u);
  }
  void raise(int face, int u, int v, std::uint8_t level) {
    const int index = (v << 4) | u;
    std::uint8_t &packed = faces[face][index >> 1];
    const int shift = (index & 1) * 4;
    if (level <= static_cast<std::uint8_t>((packed >> shift) & 15u))
      return;
    packed = static_cast<std::uint8_t>((packed & ~(0x0Fu << shift)) |
                                       ((level & 15u) << shift));
  }
  bool empty() const {
    for (const auto &face : faces)
      for (std::uint8_t packed : face)
        if (packed != 0)
          return false;
    return true;
  }
};
// 单区块的收件箱：只保存有入向光的段（通常 1~3 个）。
using DeferredChunkInbox = std::vector<std::pair<int, DeferredSectionInbox>>;

// 按区块段首元素 `sectionY` 计算连续段数组中的下标，越界时返回 `values.size()`。
template <class T>
std::size_t vectorIndex(const std::vector<T> &values, int sectionY) {
  if (values.empty())
    return values.size();
  const int minY = values.front()->sectionY;
  const int index = sectionY - minY;
  return index >= 0 && static_cast<std::size_t>(index) < values.size()
             ? static_cast<std::size_t>(index)
             : values.size();
}

// 传入无锁环形队列 `inputQueue` 的单格几何变更事件，附带单调序号用于与控制队列定序合并。
struct GeometryInput {
  std::uint64_t sequence = 0;
  LightCellUpdate update;
};
static_assert(sizeof(GeometryInput) == 32);

// 在合批与合并阶段保留的“单格最新方块状态”记录，仅保留序号更高的写覆盖。
struct OwnedCell {
  std::uint64_t sequence = 0;
  BlockState::Id state = 0;
};

// 归属于同一区块的待处理合批控制与方块变更集合。
// 存在原因：将同一合批窗口内对同一区块的加载、卸载与多次离散方块修改折叠到一条记录中，避免重复构建/传播同一区块。
struct PendingChunk {
  std::uint64_t key = 0;
  std::uint64_t generation = 0;
  std::uint64_t inputSequence = 0;
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t revision = 0;
  std::unique_ptr<LightChunkInput> load;
  bool unload = false;
  // 首次计算请求（`LightService::requestChunkCompute`）：几何已注册、空壳已存在，
  // 本批对其实施一次完整播种（seedChunk + seedIncoming）并置为需要发布。
  bool compute = false;
  // 几何直读源重定向（LightService::rebindChunkGeometry）：保留已算好的光照层，
  // 只把 worker 侧的直读句柄切到 World/ChunkMap 直读，不重算、不发布。
  bool rebind = false;
  int chunkX = 0;
  int chunkZ = 0;
  phmap::flat_hash_map<std::uint32_t, OwnedCell> ownedCells;
};

// 写时复制（COW）辅助：若快照已被赋予非 0 发布版本号（可能已入队或被主线程持有），则克隆一份新头部供当前批次修改并清零 `revision`。
inline void mutableLayerSnapshot(std::shared_ptr<LightChunkSnapshot> &slot) {
  if (slot && slot->revision != 0) {
    slot = std::make_shared<LightChunkSnapshot>(*slot);
    slot->revision = 0;
  }
}

// 传播过程中单个区块的可变工作副本。
// 存在原因：在区块发生修改前与已发布快照共享 `shared_ptr`；一旦发生修改，通过 `sectionForEdit` 按段执行写时复制，从而只克隆真正变脏的段头。
struct WorkingChunk {
  std::uint64_t key = 0;
  std::uint64_t lifecycleEpoch = 0;
  int chunkX = 0;
  int chunkZ = 0;
  std::shared_ptr<LightChunkSnapshot> snapshot;
  std::vector<int> dirtySections;
  bool dirty = false;
  bool propagated = false;
  bool inPlace = false;

  // 按需写时复制指定 `sectionY` 的快照头部，返回可安全写入的段指针。
  LightSectionSnapshot *sectionForEdit(int sectionY);
};

// 一轮合批经 `prepareBatch` 折叠并差分后的输入摘要，供方块光与天空光两个传播阶段共用。
struct PreparedBatch {
  FlatSet<std::uint64_t, UInt64Hash> touched;
  FlatSet<std::uint64_t, UInt64Hash> dirty;
  FlatSet<std::uint64_t, UInt64Hash> removed;
  FlatSet<std::uint64_t, UInt64Hash> replaced;
  FlatSet<std::uint64_t, UInt64Hash> loaded;
  FlatSet<std::uint64_t, UInt64Hash> persistedLoaded;
  FlatSet<std::uint64_t, UInt64Hash> precomputedBlockLoaded;
  FlatSet<std::uint64_t, UInt64Hash> precomputedSkyLoaded;
  // 只注册了几何、停在“未计算”的区块（`Completion::Deferred`）。
  FlatSet<std::uint64_t, UInt64Hash> registered;
  // 本批首次计算的区块（“未计算” → “完整”）。
  FlatSet<std::uint64_t, UInt64Hash> computed;
  FlatSet<std::uint64_t, UInt64Hash> skyOnlyCompute;
  FlatSet<std::uint64_t, UInt64Hash> affected;
  std::vector<LightPos> changedPositions;
  std::uint64_t maxGeneration = 0;
};

// 单光照层单批次执行期间的所有工作区块与已移除区块集合。
struct LayerWorkspace {
  FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> chunks;
  FlatSet<std::uint64_t, UInt64Hash> removed;
};

// 单光照层执行结束后的输出（包含工作区及本轮是否完全收敛无缺元数据）。
struct LayerOutcome {
  LayerWorkspace layers;
  bool complete = false;
};

// 追踪网络协议可见的“已收敛基线版本”及跨未收敛批次累积的脏段列表。
// 存在原因：若某次传播因邻居缺失/未支持方块而未收敛（`protocolReady == false`），增量脏段不能推进协议基准版本号，而必须暂存到 `pendingDirty` 中，直到下次完全收敛时一并合并发布。
struct ProtocolBaseMeta {
  bool hasBase = false;
  std::uint64_t epoch = 0;
  std::uint64_t revision = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  std::uint64_t pendingEpoch = 0;
  std::vector<int> pendingDirty;
};

// 工作线程经 `resultQueue` 投递给主线程的单层发布项。
struct LayerResult {
  LightLayer layer = LightLayer::Block;
  std::uint64_t key = 0;
  std::shared_ptr<LightChunkSnapshot> snapshot;
};

// 增亮 BFS 任务，记录目标坐标、源亮度、允许传播方向掩码及是否来自空段/自发光（直射天空光）。
struct IncreaseTask {
  LightPos position;
  std::uint8_t level = 0;
  std::uint8_t directions = 0;
  bool fromEmpty = false;
  bool fromEmission = false;
};

// 减暗 BFS 任务，记录坐标、原传播亮度及允许扩散的邻居方向掩码。
struct DecreaseTask {
  LightPos position;
  std::uint8_t level = 0;
  std::uint8_t directions = 0;
};

// 传播器内部使用的可懒扩容单个区块段光照值容器（包装 `LightLayerRef`）。
// 存在原因：在只读或整段均一填充（如 `fill(15)`）时保持零页分配的 `Uniform` 形态；仅当局部单元被修改为异质值时才通过 `ensureMixed()` 懒分配/展开为 2048 字节半字节页，并通过代理 `CellRef` 提供数组式下标读写。
class LightValues {
public:
  class CellRef {
  public:
    CellRef(LightValues &owner, std::size_t index)
        : owner_(owner), index_(index) {}
    operator std::uint8_t() const { return owner_.value(index_); }
    CellRef &operator=(std::uint8_t value) {
      owner_.write(index_, value);
      return *this;
    }

  private:
    LightValues &owner_;
    std::size_t index_;
  };

  LightValues();
  LightValues(const LightValues &) = default;
  LightValues &operator=(const LightValues &) = default;
  LightValues(LightValues &&) noexcept = default;
  LightValues &operator=(LightValues &&) noexcept = default;

  static LightValues fromLayer(const LightLayerRef &layer);

  CellRef operator[](std::size_t index) { return {*this, index}; }
  std::uint8_t operator[](std::size_t index) const { return value(index); }
  void fill(std::uint8_t value);
  // 用从上方继承来的 256 列底面光照一次性平铺填充整个 16x16x16 混合区块段，避免逐格位操作开销。
  void fillInheritedMixed(const std::array<std::uint8_t, 256> &cols);
  // 检测当前层是否全段相同（若是混合页则扫描确认是否已退化为均一值，以便提交时折叠回 `Uniform` 省内存）。
  std::optional<std::uint8_t> uniformValue() const;
  const LightLayerRef &ref() const { return layer_; }
  const LightLayerSnapshot::PackedPagePtr &shared() const {
    return layer_.shared();
  }

private:
  std::uint8_t value(std::size_t index) const {
    if (layer_.isUniform())
      return layer_.uniformValue();
    const auto &page = layer_.shared();
    if (!page)
      return 0;
    const std::uint8_t packed =
        page->bytes[index >> 1].load(std::memory_order_relaxed);
    return static_cast<std::uint8_t>((packed >> ((index & 1) * 4)) & 15);
  }
  void write(std::size_t index, std::uint8_t value) {
    if (!layer_.isMixed())
      ensureMixed();
    layer_.writeCell(index, value);
  }
  void ensureMixed();

  LightLayerRef layer_;
};

// 工作线程自持的只读上下文宿主，暴露给独立的方块光与天空光传播器使用（除队列计数遥测与本地 L1 几何缓存外均为只读）。
// 存在原因：统一屏蔽“生成期 `BaseChunk` 句柄”与“`DirectWorldGeometrySource` 实时区块”两种数据源差异，并用 64 槽 L1 缓存加速高频坐标的光学属性查询。
struct LightEngineHost {
  LightEngineHost(
      int minSection, int sectionCount, const LightOpticalTable &table,
      const FlatMap<std::uint64_t, DirectChunkMeta, UInt64Hash> &directMeta,
      const LightWorkerOptions::DirectWorldGeometrySource &directSource,
      const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                    UInt64Hash> &readyBlock,
      const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                    UInt64Hash> &readySky,
      std::uint64_t &queueEntries, std::uint64_t &unloadedSkips)
      : minBlockSectionY(minSection), blockSectionCount(sectionCount),
        opticalTable(table), directChunks(directMeta),
        directWorld(directSource), lastReadyBlock(readyBlock),
        lastReadySky(readySky), lightQueueEntries(queueEntries),
        unloadedNeighborSkips(unloadedSkips) {}

  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  const LightOpticalTable &opticalTable;
  const FlatMap<std::uint64_t, DirectChunkMeta, UInt64Hash> &directChunks;
  LightWorkerOptions::DirectWorldGeometrySource directWorld{};
  const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>, UInt64Hash>
      &lastReadyBlock;
  const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>, UInt64Hash>
      &lastReadySky;
  std::uint64_t &lightQueueEntries;
  std::uint64_t &unloadedNeighborSkips;
  // 每个光照层各一个“未计算区块收件箱”。
  FlatMap<std::uint64_t, DeferredChunkInbox, UInt64Hash> *deferredBlock = nullptr;
  FlatMap<std::uint64_t, DeferredChunkInbox, UInt64Hash> *deferredSky = nullptr;

  // 目标区块已注册几何但还没算过（“未计算”）时，把 \`level\` 以 max 方式记进它对应
  // 段的那一侧 16x16 收件箱并返回 true；返回 false 表示目标真的不存在（世界边界外
  // 的区块）或已经有光照壳，调用方按“跳过”处理。
  bool deferIncoming(bool sky, LightPos to, int direction, std::uint8_t level) const {
    auto *inbox = sky ? deferredSky : deferredBlock;
    if (inbox == nullptr)
      return false;
    const std::uint64_t key = chunkKey(to.x >> 4, to.z >> 4);
    if (!hasChunkGeometry(key))
      return false;
    const auto &ready = sky ? lastReadySky : lastReadyBlock;
    if (ready.find(key) != ready.end())
      return false;
    DeferredChunkInbox &chunkInbox = (*inbox)[key];
    const int sectionY = to.y >> 4;
    DeferredSectionInbox *section = nullptr;
    for (auto &entry : chunkInbox) {
      if (entry.first == sectionY) {
        section = &entry.second;
        break;
      }
    }
    if (section == nullptr) {
      chunkInbox.emplace_back(sectionY, DeferredSectionInbox{});
      section = &chunkInbox.back().second;
    }
    const int face = (direction ^ 1) - 2;
    const int u = (direction == 2 || direction == 3) ? (to.x & 15) : (to.z & 15);
    section->raise(face, u, to.y & 15, level);
    return true;
  }

  const LightOpticalClass &opticalClass(std::uint16_t id) const {
    return opticalTable[static_cast<std::uint8_t>(id)];
  }
  const BlockLightProperties &propertiesForState(BlockState::Id state) const {
    return opticalClass(opticalTable.classForState(state)).properties;
  }
  struct DirectSectionCacheEntry {
    std::uint64_t key = 0;
    int sectionY = 0;
    bool present = false;
    bool valid = false;
    LightSectionView view{};
  };
  const DirectSectionCacheEntry *
  lookupDirectSectionCache(std::uint64_t key, int sectionY) const;
  bool hasChunkGeometry(std::uint64_t key) const;
  std::optional<LightSectionView> directSectionView(std::uint64_t key,
                                                    int sectionY) const;
  std::optional<bool> sectionNonEmpty(std::uint64_t key, int sectionY) const;
  // 查询世界坐标处的方块光学属性；若遇到未受支持方块，将 `incomplete` 置为 true 以阻止本轮标记为 `protocolReady`。
  const BlockLightProperties &blockPropertiesAt(int x, int y, int z,
                                                bool &incomplete) const;
  const BlockLightProperties *sectionPropertiesAt(std::uint64_t key, int x,
                                                   int y, int z) const;
  const std::shared_ptr<LightChunkSnapshot> &storedLayerSnapshot(
      LightLayer layer, std::uint64_t key) const;

private:
  mutable std::array<DirectSectionCacheEntry, 512> directSectionCache_{};
};

} // namespace redstone::light_internal
