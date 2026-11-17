#pragma once
// 维度类型规格、维度标识解析与原版 26.1 维度目录布局工具。
// 用于统一描述 Overworld / Nether / End 及自定义维度的高度范围、环境规则与磁盘存储路径。
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {

inline constexpr int kDimensionMinHeight = 16;
inline constexpr int kDimensionYSize = (1 << 12) - 32; // 对应 BlockPos.PACKED_Y_LENGTH = 12 位的最大 Y 跨度
inline constexpr int kDimensionMaxY = (kDimensionYSize >> 1) - 1;
inline constexpr int kDimensionMinY = kDimensionMaxY - kDimensionYSize + 1;

// 对齐原版 26.1 `net.minecraft.world.level.dimension.DimensionType`。
// 集中保存维度的纵向边界、光照与环境规则，使核心模拟、存档读写与传送逻辑无需硬编码维度分支。
struct DimensionTypeSpec {
  std::string id = "minecraft:overworld";
  bool hasFixedTime = false;
  bool hasSkyLight = true;
  bool hasCeiling = false;
  bool hasEnderDragonFight = false;
  double coordinateScale = 1.0;
  int minY = -64;
  int height = 384;
  int logicalHeight = 384;
  float ambientLight = 0.0f;
  bool waterEvaporates = false;
  bool fastLava = false;
  std::string defaultClock = "minecraft:overworld";

  constexpr int maxY() const noexcept { return minY + height; }
  constexpr int sectionCount() const noexcept { return height >> 4; }
  bool operator==(const DimensionTypeSpec &) const = default;
};

// 拆分为命名空间与路径后的维度标识符（如 `minecraft` / `overworld`）。
struct DimensionId {
  std::string ns;
  std::string path;
};

// 维度注册项：将维度实例 ID 与其引用的维度类型 ID 及解析后的规格绑定在一起。
struct DimensionStem {
  std::string id;
  std::string typeId;
  DimensionTypeSpec type;

  bool operator==(const DimensionStem &) const = default;
};

// 内置原版 26.1 `DimensionTypes` 定义。
const DimensionTypeSpec &overworldDimensionType();
const DimensionTypeSpec &overworldCavesDimensionType();
const DimensionTypeSpec &netherDimensionType();
const DimensionTypeSpec &endDimensionType();
std::optional<DimensionTypeSpec> findBuiltinDimensionType(std::string_view id);

// 原版 26.1 `DimensionType` 合法性校验（Y 边界、16 的倍数约束以及坐标缩放比例）。
void validateDimensionType(const DimensionTypeSpec &spec);
// 引擎容量约束校验：在原版规则基础上额外校验区段数不超过 `World` 固定区段容量上限，防止内联存储越界。
void validateEngineDimensionType(const DimensionTypeSpec &spec, int maxSections = 24);

// 计算跨维度传送时的水平坐标缩放比例（`from.coordinateScale / to.coordinateScale`）。
double teleportationScale(const DimensionTypeSpec &from,
                          const DimensionTypeSpec &to) noexcept;

// 将 `"<ns>:<path>"` 或裸 `"<path>"` 规范化为 `"<ns>:<path>"`（默认补全 `minecraft:`），
// 校验字符合法性并禁止 `"."` / `".."` 路径穿越片段。
std::string normalizeDimensionId(std::string_view raw);
DimensionId parseDimensionId(std::string_view raw);

// 原版 26.1 三大默认维度的标准注册表顺序（`overworld`、`the_nether`、`the_end`）。
std::vector<DimensionStem> defaultDimensionStems();
// 按确定性顺序排列维度注册项：依次为 `overworld`、`the_nether`、`the_end`，其余按字典序排列。
void sortDimensionStems(std::vector<DimensionStem> &stems);

// 原版 26.1 标准维度存储目录（`<worldDir>/dimensions/<ns>/<path>`）。
std::filesystem::path dimensionStorageDirectory(const std::filesystem::path &worldDir,
                                                std::string_view dimensionId);
// 解析维度根目录：优先检查标准的 `dimensions/<ns>/<path>`，若旧版位置（`<worldDir>`、`DIM-1` 或 `DIM1`）存在 `region/` 则回退兼容。
std::filesystem::path dimensionRootDirectory(const std::filesystem::path &worldDir,
                                             std::string_view dimensionId);
std::filesystem::path dimensionRegionDirectory(const std::filesystem::path &worldDir,
                                               std::string_view dimensionId);
// 给定区域目录（`.../region`），反推其所属维度的数据根目录。
std::filesystem::path
resolveDimensionDataDirectory(const std::filesystem::path &regionDirectory);

} // namespace redstone
