#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {

inline constexpr int kDimensionMinHeight = 16;
inline constexpr int kDimensionYSize = (1 << 12) - 32; // BlockPos.PACKED_Y_LENGTH = 12
inline constexpr int kDimensionMaxY = (kDimensionYSize >> 1) - 1;
inline constexpr int kDimensionMinY = kDimensionMaxY - kDimensionYSize + 1;

// Aligned with 26.1 net.minecraft.world.level.dimension.DimensionType.
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

struct DimensionId {
  std::string ns;
  std::string path;
};

struct DimensionStem {
  std::string id;
  std::string typeId;
  DimensionTypeSpec type;

  bool operator==(const DimensionStem &) const = default;
};

// Builtin 26.1 DimensionTypes definitions.
const DimensionTypeSpec &overworldDimensionType();
const DimensionTypeSpec &overworldCavesDimensionType();
const DimensionTypeSpec &netherDimensionType();
const DimensionTypeSpec &endDimensionType();
std::optional<DimensionTypeSpec> findBuiltinDimensionType(std::string_view id);

// Vanilla 26.1 DimensionType validation (Y bounds, multiples of 16, scale).
void validateDimensionType(const DimensionTypeSpec &spec);
// Engine-bounded validation: enforces vanilla constraints plus World section capacity.
void validateEngineDimensionType(const DimensionTypeSpec &spec, int maxSections = 24);

double teleportationScale(const DimensionTypeSpec &from,
                          const DimensionTypeSpec &to) noexcept;

// Normalizes "<ns>:<path>" or bare "<path>" -> "minecraft:<path>", validating characters
// and forbidding path traversal segments ("." / "..").
std::string normalizeDimensionId(std::string_view raw);
DimensionId parseDimensionId(std::string_view raw);

// Canonical 26.1 three-dimension registry order (overworld, the_nether, the_end).
std::vector<DimensionStem> defaultDimensionStems();
// Sorts stems deterministically: overworld, the_nether, the_end, then lexicographical id.
void sortDimensionStems(std::vector<DimensionStem> &stems);

// Canonical 26.1 storage layout (<worldDir>/dimensions/<ns>/<path>).
std::filesystem::path dimensionStorageDirectory(const std::filesystem::path &worldDir,
                                                std::string_view dimensionId);
// Resolves dimension root directory, checking canonical dimensions/<ns>/<path> first
// and falling back to legacy <worldDir>, DIM-1, or DIM1 when region/ lives there.
std::filesystem::path dimensionRootDirectory(const std::filesystem::path &worldDir,
                                             std::string_view dimensionId);
std::filesystem::path dimensionRegionDirectory(const std::filesystem::path &worldDir,
                                               std::string_view dimensionId);
// Given a region directory (.../region), returns its owning dimension data root.
std::filesystem::path
resolveDimensionDataDirectory(const std::filesystem::path &regionDirectory);

} // namespace redstone
