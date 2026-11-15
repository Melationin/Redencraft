#include "redstone/dimension.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace redstone {
namespace {

bool isValidNamespaceChar(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.';
}

bool isValidPathChar(unsigned char c) {
  return isValidNamespaceChar(c) || c == '/';
}

int stemOrderRank(std::string_view id) {
  if (id == "minecraft:overworld")
    return 0;
  if (id == "minecraft:the_nether")
    return 1;
  if (id == "minecraft:the_end")
    return 2;
  return 3;
}

std::optional<std::filesystem::path>
legacyDimensionRoot(const std::filesystem::path &worldDir,
                    std::string_view normalizedId) {
  if (normalizedId == "minecraft:overworld")
    return worldDir;
  if (normalizedId == "minecraft:the_nether")
    return worldDir / "DIM-1";
  if (normalizedId == "minecraft:the_end")
    return worldDir / "DIM1";
  return std::nullopt;
}

} // namespace

const DimensionTypeSpec &overworldDimensionType() {
  static const DimensionTypeSpec spec{
      .id = "minecraft:overworld",
      .hasFixedTime = false,
      .hasSkyLight = true,
      .hasCeiling = false,
      .hasEnderDragonFight = false,
      .coordinateScale = 1.0,
      .minY = -64,
      .height = 384,
      .logicalHeight = 384,
      .ambientLight = 0.0f,
      .waterEvaporates = false,
      .fastLava = false,
      .defaultClock = "minecraft:overworld",
  };
  return spec;
}

const DimensionTypeSpec &overworldCavesDimensionType() {
  static const DimensionTypeSpec spec{
      .id = "minecraft:overworld_caves",
      .hasFixedTime = false,
      .hasSkyLight = true,
      .hasCeiling = true,
      .hasEnderDragonFight = false,
      .coordinateScale = 1.0,
      .minY = -64,
      .height = 384,
      .logicalHeight = 384,
      .ambientLight = 0.0f,
      .waterEvaporates = false,
      .fastLava = false,
      .defaultClock = "minecraft:overworld",
  };
  return spec;
}

const DimensionTypeSpec &netherDimensionType() {
  static const DimensionTypeSpec spec{
      .id = "minecraft:the_nether",
      .hasFixedTime = true,
      .hasSkyLight = false,
      .hasCeiling = true,
      .hasEnderDragonFight = false,
      .coordinateScale = 8.0,
      .minY = 0,
      .height = 256,
      .logicalHeight = 128,
      .ambientLight = 0.1f,
      .waterEvaporates = true,
      .fastLava = true,
      .defaultClock = "",
  };
  return spec;
}

const DimensionTypeSpec &endDimensionType() {
  static const DimensionTypeSpec spec{
      .id = "minecraft:the_end",
      .hasFixedTime = true,
      .hasSkyLight = true,
      .hasCeiling = false,
      .hasEnderDragonFight = true,
      .coordinateScale = 1.0,
      .minY = 0,
      .height = 256,
      .logicalHeight = 256,
      .ambientLight = 0.25f,
      .waterEvaporates = false,
      .fastLava = false,
      .defaultClock = "minecraft:the_end",
  };
  return spec;
}

std::optional<DimensionTypeSpec> findBuiltinDimensionType(std::string_view id) {
  std::string normalized;
  try {
    normalized = normalizeDimensionId(id);
  } catch (const std::invalid_argument &) {
    return std::nullopt;
  }
  if (normalized == "minecraft:overworld")
    return overworldDimensionType();
  if (normalized == "minecraft:overworld_caves")
    return overworldCavesDimensionType();
  if (normalized == "minecraft:the_nether")
    return netherDimensionType();
  if (normalized == "minecraft:the_end")
    return endDimensionType();
  return std::nullopt;
}

void validateDimensionType(const DimensionTypeSpec &spec) {
  if (spec.height < kDimensionMinHeight)
    throw std::invalid_argument("dimension height has to be at least 16");
  if (spec.height > kDimensionYSize)
    throw std::invalid_argument("dimension height exceeds max Y span");
  if (spec.minY < kDimensionMinY || spec.minY > kDimensionMaxY)
    throw std::invalid_argument("dimension min_y is outside packed Y bounds");
  if (spec.minY + spec.height > kDimensionMaxY + 1)
    throw std::invalid_argument("min_y + height cannot exceed max_y + 1");
  if (spec.logicalHeight < 0 || spec.logicalHeight > spec.height)
    throw std::invalid_argument("logical_height must be within [0, height]");
  if ((spec.height & 15) != 0)
    throw std::invalid_argument("dimension height must be a multiple of 16");
  if ((spec.minY & 15) != 0)
    throw std::invalid_argument("dimension min_y must be a multiple of 16");
  if (!std::isfinite(spec.coordinateScale) || spec.coordinateScale < 1.0e-5 ||
      spec.coordinateScale > 3.0e7) {
    throw std::invalid_argument("dimension coordinate_scale out of range");
  }
}

void validateEngineDimensionType(const DimensionTypeSpec &spec, int maxSections) {
  validateDimensionType(spec);
  if (spec.sectionCount() > maxSections) {
    throw std::invalid_argument(
        "dimension height exceeds engine kSectionCapacity (" +
        std::to_string(maxSections * 16) + " blocks)");
  }
}

double teleportationScale(const DimensionTypeSpec &from,
                          const DimensionTypeSpec &to) noexcept {
  return from.coordinateScale / to.coordinateScale;
}

DimensionId parseDimensionId(std::string_view raw) {
  if (raw.empty())
    throw std::invalid_argument("dimension id cannot be empty");
  const std::size_t colon = raw.find(':');
  std::string_view ns = "minecraft";
  std::string_view path = raw;
  if (colon != std::string_view::npos) {
    ns = raw.substr(0, colon);
    path = raw.substr(colon + 1);
  }
  if (ns.empty() || path.empty() || path.find(':') != std::string_view::npos)
    throw std::invalid_argument("invalid dimension id: " + std::string(raw));
  for (unsigned char c : ns) {
    if (!isValidNamespaceChar(c))
      throw std::invalid_argument("invalid namespace in dimension id: " +
                                  std::string(raw));
  }
  if (path.front() == '/' || path.back() == '/')
    throw std::invalid_argument("invalid path in dimension id: " +
                                std::string(raw));
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
    const auto segment = path.substr(start, end - start);
    if (segment.empty() || segment == "." || segment == "..") {
      throw std::invalid_argument("invalid path segment in dimension id: " +
                                  std::string(raw));
    }
    for (unsigned char c : segment) {
      if (!isValidPathChar(c) || c == '/')
        throw std::invalid_argument("invalid character in dimension id: " +
                                    std::string(raw));
    }
    if (slash == std::string_view::npos)
      break;
    start = slash + 1;
  }
  return DimensionId{std::string(ns), std::string(path)};
}

std::string normalizeDimensionId(std::string_view raw) {
  const DimensionId parsed = parseDimensionId(raw);
  return parsed.ns + ":" + parsed.path;
}

std::vector<DimensionStem> defaultDimensionStems() {
  return {
      DimensionStem{"minecraft:overworld", "minecraft:overworld",
                    overworldDimensionType()},
      DimensionStem{"minecraft:the_nether", "minecraft:the_nether",
                    netherDimensionType()},
      DimensionStem{"minecraft:the_end", "minecraft:the_end",
                    endDimensionType()},
  };
}

void sortDimensionStems(std::vector<DimensionStem> &stems) {
  std::sort(stems.begin(), stems.end(),
            [](const DimensionStem &a, const DimensionStem &b) {
              const int ra = stemOrderRank(a.id);
              const int rb = stemOrderRank(b.id);
              if (ra != rb)
                return ra < rb;
              return a.id < b.id;
            });
}

std::filesystem::path dimensionStorageDirectory(const std::filesystem::path &worldDir,
                                                std::string_view dimensionId) {
  const DimensionId parsed = parseDimensionId(dimensionId);
  std::filesystem::path out = worldDir / "dimensions" / parsed.ns;
  std::size_t start = 0;
  while (start < parsed.path.size()) {
    const std::size_t slash = parsed.path.find('/', start);
    const std::size_t end =
        slash == std::string_view::npos ? parsed.path.size() : slash;
    out /= parsed.path.substr(start, end - start);
    if (slash == std::string_view::npos)
      break;
    start = slash + 1;
  }
  return out;
}

std::filesystem::path dimensionRootDirectory(const std::filesystem::path &worldDir,
                                             std::string_view dimensionId) {
  const std::string normalized = normalizeDimensionId(dimensionId);
  const auto modern = dimensionStorageDirectory(worldDir, normalized);
  if (std::filesystem::is_directory(modern / "region"))
    return modern;
  if (const auto legacy = legacyDimensionRoot(worldDir, normalized)) {
    if (std::filesystem::is_directory(*legacy / "region"))
      return *legacy;
  }
  return modern;
}

std::filesystem::path dimensionRegionDirectory(const std::filesystem::path &worldDir,
                                               std::string_view dimensionId) {
  return dimensionRootDirectory(worldDir, dimensionId) / "region";
}

std::filesystem::path
resolveDimensionDataDirectory(const std::filesystem::path &regionDirectory) {
  if (regionDirectory.filename() == "region")
    return regionDirectory.parent_path();
  return regionDirectory;
}

} // namespace redstone
