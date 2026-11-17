#pragma once
// 对齐 Minecraft Java Edition 26.1 的 12 阶段区块状态机（`ChunkStatus`）、
// 任务依赖半径表（`ChunkDependencies`）、生成步骤（`ChunkStep`）与生成/加载依赖金字塔（`ChunkPyramid`）。
// 之所以将其独立建模而不直接在 `WorldChunkManager` 里硬编码偏移表，是因为每个生成阶段都有不同的
// 邻域读写半径（如结构引用 r=8、特性 r=1、光照 r=1），且需要与 `ChunkLevel` 加载等级传播精确自洽。

#include "redstone/heightmap.hpp"
#include "redstone/tickets.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace redstone::worldgen {

// 区分区块处于原型生成期（ProtoChunk）还是已晋升为可进入主世界模拟的完整区块（LevelChunk）。
enum class ChunkType : std::uint8_t {
  ProtoChunk = 0,
  LevelChunk = 1,
};

// 直接复用 `tickets.hpp` 中的 12 阶段枚举，避免两套状态索引映射漂移。
using ChunkStatusId = GenerationStatus;
inline constexpr std::size_t kChunkStatusCount = 12;

// 单个区块生成/加载阶段的只读元数据（对齐 `ChunkStatus.java`）。
class ChunkStatus {
public:
  static constexpr int kMaxStructureDistance = 8;

  static const ChunkStatus &empty();
  static const ChunkStatus &structureStarts();
  static const ChunkStatus &structureReferences();
  static const ChunkStatus &biomes();
  static const ChunkStatus &noise();
  static const ChunkStatus &surface();
  static const ChunkStatus &carvers();
  static const ChunkStatus &features();
  static const ChunkStatus &initializeLight();
  static const ChunkStatus &light();
  static const ChunkStatus &spawn();
  static const ChunkStatus &full();

  static const ChunkStatus &byId(ChunkStatusId id);
  static const ChunkStatus *byName(std::string_view name);
  static const std::array<const ChunkStatus *, kChunkStatusCount> &statusList();

  ChunkStatusId id() const noexcept { return id_; }
  int index() const noexcept { return static_cast<int>(id_); }
  const ChunkStatus &parent() const noexcept { return *parent_; }
  ChunkType chunkType() const noexcept { return chunkType_; }
  std::string_view shortName() const noexcept { return shortName_; }
  std::string_view name() const noexcept { return fullName_; }

  // 返回当前阶段完成后要求已建立/更新的高度图列表（对应 `ChunkStatus.heightmapsAfter()`）。
  // `Empty..Surface` 仅维护两张 `*WG` 原型高度图，`Carvers..Full` 维护四张终态高度图。
  std::span<const HeightmapType> heightmapsAfter() const noexcept;
  std::uint32_t heightmapsAfterMask() const noexcept { return heightmapMask_; }

  bool isOrAfter(const ChunkStatus &other) const noexcept {
    return index() >= other.index();
  }
  bool isAfter(const ChunkStatus &other) const noexcept {
    return index() > other.index();
  }
  bool isOrBefore(const ChunkStatus &other) const noexcept {
    return index() <= other.index();
  }
  bool isBefore(const ChunkStatus &other) const noexcept {
    return index() < other.index();
  }

  static const ChunkStatus &max(const ChunkStatus &a,
                                const ChunkStatus &b) noexcept {
    return a.isAfter(b) ? a : b;
  }

  friend bool operator==(const ChunkStatus &a, const ChunkStatus &b) noexcept {
    return a.id_ == b.id_;
  }

private:
  struct Storage;
  friend const Storage &statusStorage();

  ChunkStatus(ChunkStatusId id, const ChunkStatus *parent,
              std::string_view shortName, std::string_view fullName,
              ChunkType chunkType, std::uint32_t heightmapMask);

  ChunkStatusId id_;
  const ChunkStatus *parent_;
  std::string_view shortName_;
  std::string_view fullName_;
  ChunkType chunkType_;
  std::uint32_t heightmapMask_;
};

// 记录某一阶段按切比雪夫距离索引的前置状态需求（对齐 `ChunkDependencies.java`）。
// 提供 `status -> radius` 与 `distance -> maxRequiredStatus` 双向 O(1) 查询，
// 供 `WorldGenRegion` 越界访问拦截与 `ChunkGenerationTask` 递归建图使用。
class ChunkDependencies {
public:
  ChunkDependencies() = default;
  explicit ChunkDependencies(std::vector<ChunkStatusId> dependencyByRadius);

  const std::vector<ChunkStatusId> &asList() const noexcept {
    return dependencyByRadius_;
  }
  std::size_t size() const noexcept { return dependencyByRadius_.size(); }
  bool empty() const noexcept { return dependencyByRadius_.empty(); }
  int getRadius() const noexcept {
    return dependencyByRadius_.empty()
               ? 0
               : static_cast<int>(dependencyByRadius_.size()) - 1;
  }
  ChunkStatusId get(int distance) const;
  int getRadiusOf(ChunkStatusId status) const;

private:
  std::vector<ChunkStatusId> dependencyByRadius_;
  std::vector<int> radiusByDependency_;
};

// 描述到达目标 `targetStatus` 所需的直接邻域依赖、传递累积依赖以及允许写入方块的最大半径。
// `blockStateWriteRadius == -1` 表示该步骤禁止写入方块（仅操作生物群系/光照等元数据）。
class ChunkStep {
public:
  class Builder {
  public:
    explicit Builder(ChunkStatusId status);
    Builder(ChunkStatusId status, const ChunkStep &parent);

    Builder &addRequirement(ChunkStatusId status, int radius);
    Builder &blockStateWriteRadius(int radius);
    ChunkStep build() const;

  private:
    std::vector<ChunkStatusId> buildAccumulatedDependencies() const;
    int getRadiusOfParent(ChunkStatusId parentStatus) const;

    ChunkStatusId status_;
    const ChunkStep *parent_ = nullptr;
    std::vector<ChunkStatusId> directDependenciesByRadius_;
    int blockStateWriteRadius_ = -1;
  };

  ChunkStatusId targetStatus() const noexcept { return targetStatus_; }
  const ChunkDependencies &directDependencies() const noexcept {
    return directDependencies_;
  }
  const ChunkDependencies &accumulatedDependencies() const noexcept {
    return accumulatedDependencies_;
  }
  int blockStateWriteRadius() const noexcept { return blockStateWriteRadius_; }
  int getAccumulatedRadiusOf(ChunkStatusId status) const;

private:
  ChunkStep(ChunkStatusId targetStatus, ChunkDependencies directDependencies,
            ChunkDependencies accumulatedDependencies, int blockStateWriteRadius)
      : targetStatus_(targetStatus),
        directDependencies_(std::move(directDependencies)),
        accumulatedDependencies_(std::move(accumulatedDependencies)),
        blockStateWriteRadius_(blockStateWriteRadius) {}

  ChunkStatusId targetStatus_;
  ChunkDependencies directDependencies_;
  ChunkDependencies accumulatedDependencies_;
  int blockStateWriteRadius_;
};

// 12 阶段依赖金字塔（对齐 `ChunkPyramid.java`）：
// 区分从零生成新区块的 `generationPyramid()` 与仅从磁盘恢复已落盘区块（跳过已固化阶段、只补齐光/晋升）的 `loadingPyramid()`。
class ChunkPyramid {
public:
  static const ChunkPyramid &generationPyramid();
  static const ChunkPyramid &loadingPyramid();

  const ChunkStep &getStepTo(ChunkStatusId status) const;
  const std::vector<ChunkStep> &steps() const noexcept { return steps_; }

private:
  explicit ChunkPyramid(std::vector<ChunkStep> steps)
      : steps_(std::move(steps)) {}

  std::vector<ChunkStep> steps_;
};

// 按 26.1 `ChunkLevel.getStatusAroundFullChunk` 直接从 `generationPyramid()` 推导某加载等级的目标状态。
// `level <= 33` 返回 `Full`，`34..44` 返回对应外围前置阶段，`> 44` 返回 `std::nullopt`。
std::optional<ChunkStatusId> generationStatusForLevel(int level);
int chunkLevelForStatus(ChunkStatusId status);

} // namespace redstone::worldgen
