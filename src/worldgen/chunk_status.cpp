#include "redstone/worldgen/chunk_status.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace redstone::worldgen {
namespace {

constexpr std::uint32_t heightmapBit(HeightmapType type) {
  return std::uint32_t{1} << static_cast<std::uint8_t>(type);
}

constexpr std::uint32_t kWorldgenHeightmapMask =
    heightmapBit(HeightmapType::OceanFloorWG) |
    heightmapBit(HeightmapType::WorldSurfaceWG);

constexpr std::uint32_t kFinalHeightmapMask =
    heightmapBit(HeightmapType::OceanFloor) |
    heightmapBit(HeightmapType::WorldSurface) |
    heightmapBit(HeightmapType::MotionBlocking) |
    heightmapBit(HeightmapType::MotionBlockingNoLeaves);

constexpr std::array<HeightmapType, 2> kWorldgenHeightmapTypes{
    HeightmapType::OceanFloorWG,
    HeightmapType::WorldSurfaceWG,
};

constexpr std::array<HeightmapType, 4> kFinalHeightmapTypes{
    HeightmapType::OceanFloor,
    HeightmapType::WorldSurface,
    HeightmapType::MotionBlocking,
    HeightmapType::MotionBlockingNoLeaves,
};

} // namespace

struct ChunkStatus::Storage {
  std::array<ChunkStatus, kChunkStatusCount> statuses;
  std::array<const ChunkStatus *, kChunkStatusCount> ptrs;
};

ChunkStatus::ChunkStatus(ChunkStatusId id, const ChunkStatus *parent,
                         std::string_view shortName, std::string_view fullName,
                         ChunkType chunkType, std::uint32_t heightmapMask)
    : id_(id), parent_(parent ? parent : this), shortName_(shortName),
      fullName_(fullName), chunkType_(chunkType),
      heightmapMask_(heightmapMask) {}

const ChunkStatus::Storage &statusStorage() {
  static const ChunkStatus::Storage linked = []() {
    auto make = [](ChunkStatusId id, const ChunkStatus *parent,
                   std::string_view shortName, std::string_view fullName,
                   ChunkType type, std::uint32_t mask) {
      return ChunkStatus(id, parent, shortName, fullName, type, mask);
    };
    ChunkStatus::Storage st{
        {make(ChunkStatusId::Empty, nullptr, "empty", "minecraft:empty",
              ChunkType::ProtoChunk, kWorldgenHeightmapMask),
         make(ChunkStatusId::StructureStarts, nullptr, "structure_starts",
              "minecraft:structure_starts", ChunkType::ProtoChunk,
              kWorldgenHeightmapMask),
         make(ChunkStatusId::StructureReferences, nullptr,
              "structure_references", "minecraft:structure_references",
              ChunkType::ProtoChunk, kWorldgenHeightmapMask),
         make(ChunkStatusId::Biomes, nullptr, "biomes", "minecraft:biomes",
              ChunkType::ProtoChunk, kWorldgenHeightmapMask),
         make(ChunkStatusId::Noise, nullptr, "noise", "minecraft:noise",
              ChunkType::ProtoChunk, kWorldgenHeightmapMask),
         make(ChunkStatusId::Surface, nullptr, "surface", "minecraft:surface",
              ChunkType::ProtoChunk, kWorldgenHeightmapMask),
         make(ChunkStatusId::Carvers, nullptr, "carvers", "minecraft:carvers",
              ChunkType::ProtoChunk, kFinalHeightmapMask),
         make(ChunkStatusId::Features, nullptr, "features",
              "minecraft:features", ChunkType::ProtoChunk, kFinalHeightmapMask),
         make(ChunkStatusId::InitializeLight, nullptr, "initialize_light",
              "minecraft:initialize_light", ChunkType::ProtoChunk,
              kFinalHeightmapMask),
         make(ChunkStatusId::Light, nullptr, "light", "minecraft:light",
              ChunkType::ProtoChunk, kFinalHeightmapMask),
         make(ChunkStatusId::Spawn, nullptr, "spawn", "minecraft:spawn",
              ChunkType::ProtoChunk, kFinalHeightmapMask),
         make(ChunkStatusId::Full, nullptr, "full", "minecraft:full",
              ChunkType::LevelChunk, kFinalHeightmapMask)},
        {},
    };
    for (std::size_t i = 0; i < kChunkStatusCount; ++i) {
      const ChunkStatus *parent = i == 0 ? &st.statuses[0] : &st.statuses[i - 1];
      st.statuses[i] = make(st.statuses[i].id(), parent,
                            st.statuses[i].shortName(), st.statuses[i].name(),
                            st.statuses[i].chunkType(),
                            st.statuses[i].heightmapsAfterMask());
      st.ptrs[i] = &st.statuses[i];
    }
    return st;
  }();
  return linked;
}

const ChunkStatus &ChunkStatus::empty() { return byId(ChunkStatusId::Empty); }
const ChunkStatus &ChunkStatus::structureStarts() {
  return byId(ChunkStatusId::StructureStarts);
}
const ChunkStatus &ChunkStatus::structureReferences() {
  return byId(ChunkStatusId::StructureReferences);
}
const ChunkStatus &ChunkStatus::biomes() { return byId(ChunkStatusId::Biomes); }
const ChunkStatus &ChunkStatus::noise() { return byId(ChunkStatusId::Noise); }
const ChunkStatus &ChunkStatus::surface() {
  return byId(ChunkStatusId::Surface);
}
const ChunkStatus &ChunkStatus::carvers() {
  return byId(ChunkStatusId::Carvers);
}
const ChunkStatus &ChunkStatus::features() {
  return byId(ChunkStatusId::Features);
}
const ChunkStatus &ChunkStatus::initializeLight() {
  return byId(ChunkStatusId::InitializeLight);
}
const ChunkStatus &ChunkStatus::light() { return byId(ChunkStatusId::Light); }
const ChunkStatus &ChunkStatus::spawn() { return byId(ChunkStatusId::Spawn); }
const ChunkStatus &ChunkStatus::full() { return byId(ChunkStatusId::Full); }

const ChunkStatus &ChunkStatus::byId(ChunkStatusId id) {
  const auto idx = static_cast<std::size_t>(id);
  if (idx >= kChunkStatusCount)
    throw std::out_of_range("invalid ChunkStatusId");
  return statusStorage().statuses[idx];
}

const ChunkStatus *ChunkStatus::byName(std::string_view name) {
  for (const ChunkStatus *status : statusList()) {
    if (name == status->name() || name == status->shortName())
      return status;
  }
  return nullptr;
}

const std::array<const ChunkStatus *, kChunkStatusCount> &
ChunkStatus::statusList() {
  return statusStorage().ptrs;
}

std::span<const HeightmapType> ChunkStatus::heightmapsAfter() const noexcept {
  if (heightmapMask_ == kWorldgenHeightmapMask)
    return kWorldgenHeightmapTypes;
  return kFinalHeightmapTypes;
}

ChunkDependencies::ChunkDependencies(
    std::vector<ChunkStatusId> dependencyByRadius)
    : dependencyByRadius_(std::move(dependencyByRadius)) {
  const std::size_t size =
      dependencyByRadius_.empty()
          ? 0
          : static_cast<std::size_t>(dependencyByRadius_.front()) + 1;
  radiusByDependency_.assign(size, 0);
  for (std::size_t radius = 0; radius < dependencyByRadius_.size(); ++radius) {
    const int depIdx = static_cast<int>(dependencyByRadius_[radius]);
    for (int s = 0; s <= depIdx; ++s) {
      radiusByDependency_[static_cast<std::size_t>(s)] =
          static_cast<int>(radius);
    }
  }
}

ChunkStatusId ChunkDependencies::get(int distance) const {
  if (distance < 0 ||
      static_cast<std::size_t>(distance) >= dependencyByRadius_.size()) {
    throw std::out_of_range("ChunkDependencies distance out of range");
  }
  return dependencyByRadius_[static_cast<std::size_t>(distance)];
}

int ChunkDependencies::getRadiusOf(ChunkStatusId status) const {
  const auto idx = static_cast<std::size_t>(status);
  if (idx >= radiusByDependency_.size()) {
    throw std::invalid_argument(
        "Requesting a ChunkStatus outside of dependency range");
  }
  return radiusByDependency_[idx];
}

ChunkStep::Builder::Builder(ChunkStatusId status) : status_(status) {
  const ChunkStatus &cs = ChunkStatus::byId(status);
  if (&cs.parent() != &cs) {
    throw std::invalid_argument("Not starting with ChunkStatus::EMPTY");
  }
}

ChunkStep::Builder::Builder(ChunkStatusId status, const ChunkStep &parent)
    : status_(status), parent_(&parent) {
  if (static_cast<int>(parent.targetStatus()) !=
      static_cast<int>(status) - 1) {
    throw std::invalid_argument("Out of order ChunkStatus in ChunkStep");
  }
  directDependenciesByRadius_.push_back(parent.targetStatus());
}

ChunkStep::Builder &ChunkStep::Builder::addRequirement(ChunkStatusId status,
                                                       int radius) {
  if (static_cast<int>(status) >= static_cast<int>(status_)) {
    throw std::invalid_argument(
        "Dependency status must precede target status");
  }
  if (radius < 0)
    throw std::invalid_argument("Requirement radius must be non-negative");
  const std::size_t newLength = static_cast<std::size_t>(radius) + 1;
  const std::size_t oldLength = directDependenciesByRadius_.size();
  if (newLength > oldLength) {
    directDependenciesByRadius_.resize(newLength, status);
  }
  const std::size_t limit = std::min(newLength, oldLength);
  for (std::size_t i = 0; i < limit; ++i) {
    if (static_cast<int>(status) >
        static_cast<int>(directDependenciesByRadius_[i])) {
      directDependenciesByRadius_[i] = status;
    }
  }
  return *this;
}

ChunkStep::Builder &ChunkStep::Builder::blockStateWriteRadius(int radius) {
  blockStateWriteRadius_ = radius;
  return *this;
}

int ChunkStep::Builder::getRadiusOfParent(ChunkStatusId parentStatus) const {
  for (int i = static_cast<int>(directDependenciesByRadius_.size()) - 1;
       i >= 0; --i) {
    if (static_cast<int>(
            directDependenciesByRadius_[static_cast<std::size_t>(i)]) >=
        static_cast<int>(parentStatus)) {
      return i;
    }
  }
  return 0;
}

std::vector<ChunkStatusId>
ChunkStep::Builder::buildAccumulatedDependencies() const {
  if (!parent_)
    return directDependenciesByRadius_;
  const int radiusOfParent = getRadiusOfParent(parent_->targetStatus());
  const ChunkDependencies &parentDeps = parent_->accumulatedDependencies();
  const std::size_t outSize = std::max(
      static_cast<std::size_t>(radiusOfParent) + parentDeps.size(),
      directDependenciesByRadius_.size());
  std::vector<ChunkStatusId> accumulated(outSize, ChunkStatusId::Empty);
  for (std::size_t d = 0; d < outSize; ++d) {
    const int dInParent = static_cast<int>(d) - radiusOfParent;
    if (dInParent < 0 ||
        static_cast<std::size_t>(dInParent) >= parentDeps.size()) {
      accumulated[d] = directDependenciesByRadius_[d];
    } else if (d >= directDependenciesByRadius_.size()) {
      accumulated[d] = parentDeps.get(dInParent);
    } else {
      const ChunkStatusId a = directDependenciesByRadius_[d];
      const ChunkStatusId b = parentDeps.get(dInParent);
      accumulated[d] = static_cast<int>(a) > static_cast<int>(b) ? a : b;
    }
  }
  return accumulated;
}

ChunkStep ChunkStep::Builder::build() const {
  return ChunkStep(status_, ChunkDependencies(directDependenciesByRadius_),
                   ChunkDependencies(buildAccumulatedDependencies()),
                   blockStateWriteRadius_);
}

int ChunkStep::getAccumulatedRadiusOf(ChunkStatusId status) const {
  return status == targetStatus_
             ? 0
             : accumulatedDependencies_.getRadiusOf(status);
}

const ChunkPyramid &ChunkPyramid::generationPyramid() {
  static const ChunkPyramid pyramid = []() {
    std::vector<ChunkStep> steps;
    steps.reserve(kChunkStatusCount);
    auto add = [&](ChunkStatusId id, auto &&fn) {
      ChunkStep::Builder b = steps.empty() ? ChunkStep::Builder(id)
                                           : ChunkStep::Builder(id, steps.back());
      fn(b);
      steps.push_back(b.build());
    };
    add(ChunkStatusId::Empty, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::StructureStarts, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::StructureReferences, [](ChunkStep::Builder &b) {
      b.addRequirement(ChunkStatusId::StructureStarts, 8);
    });
    add(ChunkStatusId::Biomes, [](ChunkStep::Builder &b) {
      b.addRequirement(ChunkStatusId::StructureStarts, 8);
    });
    add(ChunkStatusId::Noise, [](ChunkStep::Builder &b) {
      b.addRequirement(ChunkStatusId::StructureStarts, 8)
          .addRequirement(ChunkStatusId::Biomes, 1)
          .blockStateWriteRadius(0);
    });
    add(ChunkStatusId::Surface, [](ChunkStep::Builder &b) {
      b.addRequirement(ChunkStatusId::StructureStarts, 8)
          .addRequirement(ChunkStatusId::Biomes, 1)
          .blockStateWriteRadius(0);
    });
    add(ChunkStatusId::Carvers, [](ChunkStep::Builder &b) {
      b.addRequirement(ChunkStatusId::StructureStarts, 8)
          .blockStateWriteRadius(0);
    });
    add(ChunkStatusId::Features, [](ChunkStep::Builder &b) {
      b.addRequirement(ChunkStatusId::StructureStarts, 8)
          .addRequirement(ChunkStatusId::Carvers, 1)
          .blockStateWriteRadius(1);
    });
    add(ChunkStatusId::InitializeLight, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::Light, [](ChunkStep::Builder &b) {
      b.addRequirement(ChunkStatusId::InitializeLight, 1);
    });
    add(ChunkStatusId::Spawn, [](ChunkStep::Builder &b) {
      b.addRequirement(ChunkStatusId::Biomes, 1);
    });
    add(ChunkStatusId::Full, [](ChunkStep::Builder &) {});
    return ChunkPyramid(std::move(steps));
  }();
  return pyramid;
}

const ChunkPyramid &ChunkPyramid::loadingPyramid() {
  static const ChunkPyramid pyramid = []() {
    std::vector<ChunkStep> steps;
    steps.reserve(kChunkStatusCount);
    auto add = [&](ChunkStatusId id, auto &&fn) {
      ChunkStep::Builder b = steps.empty() ? ChunkStep::Builder(id)
                                           : ChunkStep::Builder(id, steps.back());
      fn(b);
      steps.push_back(b.build());
    };
    add(ChunkStatusId::Empty, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::StructureStarts, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::StructureReferences, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::Biomes, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::Noise, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::Surface, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::Carvers, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::Features, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::InitializeLight, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::Light, [](ChunkStep::Builder &b) {
      b.addRequirement(ChunkStatusId::InitializeLight, 1);
    });
    add(ChunkStatusId::Spawn, [](ChunkStep::Builder &) {});
    add(ChunkStatusId::Full, [](ChunkStep::Builder &) {});
    return ChunkPyramid(std::move(steps));
  }();
  return pyramid;
}

const ChunkStep &ChunkPyramid::getStepTo(ChunkStatusId status) const {
  return steps_.at(static_cast<std::size_t>(status));
}

std::optional<ChunkStatusId> generationStatusForLevel(int level) {
  const int distanceToFull = level - ticket_levels::FullChunkLevel;
  const ChunkStep &fullStep =
      ChunkPyramid::generationPyramid().getStepTo(ChunkStatusId::Full);
  if (distanceToFull > fullStep.accumulatedDependencies().getRadius())
    return std::nullopt;
  if (distanceToFull <= 0)
    return ChunkStatusId::Full;
  return fullStep.accumulatedDependencies().get(distanceToFull);
}

int chunkLevelForStatus(ChunkStatusId status) {
  const ChunkStep &fullStep =
      ChunkPyramid::generationPyramid().getStepTo(ChunkStatusId::Full);
  return ticket_levels::FullChunkLevel +
         fullStep.getAccumulatedRadiusOf(status);
}

} // namespace redstone::worldgen
