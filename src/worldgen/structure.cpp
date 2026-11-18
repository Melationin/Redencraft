#include "redstone/worldgen/structure.hpp"

#include "picojson.h"
#include "redstone/worldgen/biome_source.hpp"
#include "redstone/worldgen/noise_router.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace redstone::worldgen {
namespace {

constexpr double kPiDouble = 3.14159265358979323846;

int floorDivInt(int a, int b) noexcept {
  const int q = a / b;
  const int r = a % b;
  return (r != 0 && ((a ^ b) < 0)) ? q - 1 : q;
}

std::string normId(std::string_view id) {
  if (id.find(':') != std::string_view::npos)
    return std::string(id);
  return "minecraft:" + std::string(id);
}

const picojson::object &asObj(const picojson::value &v, std::string_view ctx) {
  if (!v.is<picojson::object>())
    throw std::runtime_error("Expected object in " + std::string(ctx));
  return v.get<picojson::object>();
}

const picojson::value &req(const picojson::object &obj, const char *key,
                           std::string_view ctx) {
  auto it = obj.find(key);
  if (it == obj.end())
    throw std::runtime_error("Missing '" + std::string(key) + "' in " +
                             std::string(ctx));
  return it->second;
}

double asNum(const picojson::value &v, std::string_view ctx) {
  if (!v.is<double>())
    throw std::runtime_error("Expected number in " + std::string(ctx));
  return v.get<double>();
}

int asInt(const picojson::value &v, std::string_view ctx) {
  return static_cast<int>(asNum(v, ctx));
}

bool asBoolVal(const picojson::value &v, std::string_view ctx) {
  if (!v.is<bool>())
    throw std::runtime_error("Expected bool in " + std::string(ctx));
  return v.get<bool>();
}

std::string asStr(const picojson::value &v, std::string_view ctx) {
  if (!v.is<std::string>())
    throw std::runtime_error("Expected string in " + std::string(ctx));
  return v.get<std::string>();
}

double fastInvSqrt(double x) noexcept {
  const double xhalf = 0.5 * x;
  std::uint64_t i = std::bit_cast<std::uint64_t>(x);
  i = 6910469410427058090ULL - (i >> 1);
  x = std::bit_cast<double>(i);
  return x * (1.5 - xhalf * x * x);
}

const std::array<float, 13824> &beardKernel() {
  static const std::array<float, 13824> kKernel = []() {
    std::array<float, 13824> arr{};
    for (int zi = 0; zi < 24; ++zi) {
      for (int xi = 0; xi < 24; ++xi) {
        for (int yi = 0; yi < 24; ++yi) {
          const double dx = static_cast<double>(xi - 12);
          const double dy = static_cast<double>(yi - 12) + 0.5;
          const double dz = static_cast<double>(zi - 12);
          const double distSq = dx * dx + dy * dy + dz * dz;
          arr[static_cast<std::size_t>(zi * 576 + xi * 24 + yi)] =
              static_cast<float>(std::exp(-distSq / 16.0));
        }
      }
    }
    return arr;
  }();
  return kKernel;
}

double getBuryContribution(double dx, double dy, double dz) noexcept {
  const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  return worldgen_math::clampedLerp(distance / 6.0, 1.0, 0.0);
}

double getBeardContribution(int dx, int dy, int dz, int yToGround) noexcept {
  const int xi = dx + 12;
  const int yi = dy + 12;
  const int zi = dz + 12;
  if (xi >= 0 && xi < 24 && yi >= 0 && yi < 24 && zi >= 0 && zi < 24) {
    const double dyWithOffset = static_cast<double>(yToGround) + 0.5;
    const double distSq = static_cast<double>(dx) * dx +
                          dyWithOffset * dyWithOffset +
                          static_cast<double>(dz) * dz;
    const double value = -dyWithOffset * fastInvSqrt(distSq / 2.0) / 2.0;
    return value *
           static_cast<double>(
               beardKernel()[static_cast<std::size_t>(zi * 576 + xi * 24 + yi)]);
  }
  return 0.0;
}

int evaluateSpread(SpreadType type, RandomSource &rng, int limit) {
  if (limit <= 0)
    return 0;
  if (type == SpreadType::Triangular) {
    const int a = rng.nextInt(limit);
    const int b = rng.nextInt(limit);
    return (a + b) / 2;
  }
  return rng.nextInt(limit);
}

StructureDefinition::PoolAliasEntry parsePoolAlias(const picojson::value &v,
                                                   std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  const std::string type = normId(asStr(req(obj, "type", ctx), ctx));
  StructureDefinition::PoolAliasEntry out;
  if (type == "minecraft:direct") {
    out.kind = StructureDefinition::PoolAliasEntry::Kind::Direct;
    out.alias = normId(asStr(req(obj, "alias", ctx), ctx));
    out.target = normId(asStr(req(obj, "target", ctx), ctx));
  } else if (type == "minecraft:random") {
    out.kind = StructureDefinition::PoolAliasEntry::Kind::Random;
    out.alias = normId(asStr(req(obj, "alias", ctx), ctx));
    const auto &targets = req(obj, "targets", ctx);
    if (!targets.is<picojson::array>())
      throw std::runtime_error("Expected targets array in " + std::string(ctx));
    for (const auto &t : targets.get<picojson::array>()) {
      const auto &to = asObj(t, ctx);
      out.weighted.push_back(
          {normId(asStr(req(to, "data", ctx), ctx)),
           asInt(req(to, "weight", ctx), ctx)});
    }
  } else if (type == "minecraft:random_group") {
    out.kind = StructureDefinition::PoolAliasEntry::Kind::RandomGroup;
    const auto &groups = req(obj, "groups", ctx);
    if (!groups.is<picojson::array>())
      throw std::runtime_error("Expected groups array in " + std::string(ctx));
    for (const auto &g : groups.get<picojson::array>()) {
      const auto &go = asObj(g, ctx);
      StructureDefinition::PoolAliasEntry::GroupEntry ge;
      ge.weight = asInt(req(go, "weight", ctx), ctx);
      const auto &dataArr = req(go, "data", ctx);
      if (dataArr.is<picojson::array>()) {
        for (const auto &sub : dataArr.get<picojson::array>())
          ge.entries.push_back(parsePoolAlias(sub, ctx));
      }
      out.groups.push_back(std::move(ge));
    }
  }
  return out;
}

HeightmapType parseHeightmapTypeStr(std::string_view s) {
  if (s == "WORLD_SURFACE_WG")
    return HeightmapType::WorldSurfaceWG;
  if (s == "WORLD_SURFACE")
    return HeightmapType::WorldSurface;
  if (s == "OCEAN_FLOOR_WG")
    return HeightmapType::OceanFloorWG;
  if (s == "OCEAN_FLOOR")
    return HeightmapType::OceanFloor;
  if (s == "MOTION_BLOCKING")
    return HeightmapType::MotionBlocking;
  if (s == "MOTION_BLOCKING_NO_LEAVES")
    return HeightmapType::MotionBlockingNoLeaves;
  throw std::runtime_error("Unknown HeightmapType: " + std::string(s));
}

} // namespace

DecorationStep parseDecorationStep(std::string_view name) {
  if (name == "raw_generation")
    return DecorationStep::RawGeneration;
  if (name == "lakes")
    return DecorationStep::Lakes;
  if (name == "local_modifications")
    return DecorationStep::LocalModifications;
  if (name == "underground_structures")
    return DecorationStep::UndergroundStructures;
  if (name == "surface_structures")
    return DecorationStep::SurfaceStructures;
  if (name == "strongholds")
    return DecorationStep::Strongholds;
  if (name == "underground_ores")
    return DecorationStep::UndergroundOres;
  if (name == "underground_decoration")
    return DecorationStep::UndergroundDecoration;
  if (name == "fluid_springs")
    return DecorationStep::FluidSprings;
  if (name == "vegetal_decoration")
    return DecorationStep::VegetalDecoration;
  if (name == "top_layer_modification")
    return DecorationStep::TopLayerModification;
  throw std::runtime_error("Unknown DecorationStep: " + std::string(name));
}

TerrainAdjustment parseTerrainAdjustment(std::string_view name) {
  if (name == "none")
    return TerrainAdjustment::None;
  if (name == "bury")
    return TerrainAdjustment::Bury;
  if (name == "beard_thin")
    return TerrainAdjustment::BeardThin;
  if (name == "beard_box")
    return TerrainAdjustment::BeardBox;
  if (name == "encapsulate")
    return TerrainAdjustment::Encapsulate;
  throw std::runtime_error("Unknown TerrainAdjustment: " + std::string(name));
}

BoundingBox BoundingBox::fromCorners(Pos a, Pos b) noexcept {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z),
          std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

BoundingBox BoundingBox::orientBox(int footX, int footY, int footZ, int offX,
                                   int offY, int offZ, int width, int height,
                                   int depth, Direction dir) noexcept {
  switch (dir) {
  case Direction::North:
    return {footX + offX, footY + offY, footZ - depth + 1 + offZ,
            footX + width - 1 + offX, footY + height - 1 + offY, footZ + offZ};
  case Direction::West:
    return {footX - depth + 1 + offZ, footY + offY, footZ + offX,
            footX + offZ, footY + height - 1 + offY, footZ + width - 1 + offX};
  case Direction::East:
    return {footX + offZ, footY + offY, footZ + offX,
            footX + depth - 1 + offZ, footY + height - 1 + offY,
            footZ + width - 1 + offX};
  default:
    return {footX + offX, footY + offY, footZ + offZ,
            footX + width - 1 + offX, footY + height - 1 + offY,
            footZ + depth - 1 + offZ};
  }
}

void BoundingBox::encapsulate(const BoundingBox &o) noexcept {
  minX = std::min(minX, o.minX);
  minY = std::min(minY, o.minY);
  minZ = std::min(minZ, o.minZ);
  maxX = std::max(maxX, o.maxX);
  maxY = std::max(maxY, o.maxY);
  maxZ = std::max(maxZ, o.maxZ);
}

void BoundingBox::encapsulate(Pos p) noexcept {
  minX = std::min(minX, p.x);
  minY = std::min(minY, p.y);
  minZ = std::min(minZ, p.z);
  maxX = std::max(maxX, p.x);
  maxY = std::max(maxY, p.y);
  maxZ = std::max(maxZ, p.z);
}

BoundingBox StructureStart::calculateBoundingBox() const noexcept {
  if (pieces.empty())
    return {};
  BoundingBox box = pieces.front().boundingBox;
  for (std::size_t i = 1; i < pieces.size(); ++i)
    box.encapsulate(pieces[i].boundingBox);
  return terrainAdjustment != TerrainAdjustment::None ? box.inflatedBy(12) : box;
}

std::shared_ptr<Beardifier> Beardifier::empty() {
  static const std::shared_ptr<Beardifier> kEmpty =
      std::make_shared<Beardifier>(std::vector<Rigid>{},
                                   std::vector<JigsawJunction>{},
                                   std::nullopt);
  return kEmpty;
}

std::shared_ptr<Beardifier>
Beardifier::forChunk(const std::vector<const StructureStart *> &starts,
                     ChunkTicketPos chunkPos) {
  if (starts.empty())
    return empty();
  const int chunkStartBlockX = chunkPos.x << 4;
  const int chunkStartBlockZ = chunkPos.z << 4;
  std::vector<Rigid> rigids;
  std::vector<JigsawJunction> junctions;
  std::optional<BoundingBox> anyBox;

  auto include = [&](const BoundingBox &b) {
    if (!anyBox.has_value())
      anyBox = b;
    else
      anyBox->encapsulate(b);
  };

  for (const StructureStart *start : starts) {
    if (!start || start->terrainAdjustment == TerrainAdjustment::None)
      continue;
    for (const StructurePiece &piece : start->pieces) {
      if (!piece.isCloseToChunk(chunkPos, 12))
        continue;
      if (piece.kind == StructurePiece::Kind::PoolElement) {
        if (piece.rigidProjection) {
          rigids.push_back(
              Rigid{piece.boundingBox, start->terrainAdjustment,
                    piece.groundLevelDelta});
          include(piece.boundingBox);
        }
        for (const JigsawJunction &j : piece.junctions) {
          if (j.sourceX > chunkStartBlockX - 12 &&
              j.sourceZ > chunkStartBlockZ - 12 &&
              j.sourceX < chunkStartBlockX + 15 + 12 &&
              j.sourceZ < chunkStartBlockZ + 15 + 12) {
            junctions.push_back(j);
            include(BoundingBox::fromCorners(
                Pos{j.sourceX, j.sourceGroundY, j.sourceZ},
                Pos{j.sourceX, j.sourceGroundY, j.sourceZ}));
          }
        }
      } else {
        rigids.push_back(Rigid{piece.boundingBox, start->terrainAdjustment, 0});
        include(piece.boundingBox);
      }
    }
  }
  if (!anyBox.has_value())
    return empty();
  return std::make_shared<Beardifier>(
      std::move(rigids), std::move(junctions), anyBox->inflatedBy(24));
}

Beardifier::Beardifier(std::vector<Rigid> pieces,
                       std::vector<JigsawJunction> junctions,
                       std::optional<BoundingBox> affectedBox)
    : pieces_(std::move(pieces)), junctions_(std::move(junctions)),
      affectedBox_(affectedBox) {}

double Beardifier::compute(const DensityFunctionContext &ctx) {
  if (!affectedBox_.has_value())
    return 0.0;
  const int blockX = ctx.blockX;
  const int blockY = ctx.blockY;
  const int blockZ = ctx.blockZ;
  if (!affectedBox_->isInside(blockX, blockY, blockZ))
    return 0.0;

  double noiseValue = 0.0;
  for (const Rigid &rigid : pieces_) {
    const BoundingBox &box = rigid.box;
    const int dx = std::max(0, std::max(box.minX - blockX, blockX - box.maxX));
    const int dz = std::max(0, std::max(box.minZ - blockZ, blockZ - box.maxZ));
    const int groundY = box.minY + rigid.groundLevelDelta;
    const int dyToGround = blockY - groundY;

    int dy = 0;
    switch (rigid.terrainAdjustment) {
    case TerrainAdjustment::None:
      dy = 0;
      break;
    case TerrainAdjustment::Bury:
    case TerrainAdjustment::BeardThin:
      dy = dyToGround;
      break;
    case TerrainAdjustment::BeardBox:
      dy = std::max(0, std::max(groundY - blockY, blockY - box.maxY));
      break;
    case TerrainAdjustment::Encapsulate:
      dy = std::max(0, std::max(box.minY - blockY, blockY - box.maxY));
      break;
    }

    switch (rigid.terrainAdjustment) {
    case TerrainAdjustment::None:
      break;
    case TerrainAdjustment::Bury:
      noiseValue +=
          getBuryContribution(static_cast<double>(dx),
                              static_cast<double>(dy) / 2.0,
                              static_cast<double>(dz));
      break;
    case TerrainAdjustment::BeardThin:
    case TerrainAdjustment::BeardBox:
      noiseValue += getBeardContribution(dx, dy, dz, dyToGround) * 0.8;
      break;
    case TerrainAdjustment::Encapsulate:
      noiseValue += getBuryContribution(static_cast<double>(dx) / 2.0,
                                        static_cast<double>(dy) / 2.0,
                                        static_cast<double>(dz) / 2.0) *
                    0.8;
      break;
    }
  }

  for (const JigsawJunction &j : junctions_) {
    const int dx = blockX - j.sourceX;
    const int dy = blockY - j.sourceGroundY;
    const int dz = blockZ - j.sourceZ;
    noiseValue += getBeardContribution(dx, dy, dz, dy) * 0.4;
  }
  return noiseValue;
}

void Beardifier::fillArray(double *output, std::size_t count,
                           DensityContextProvider &provider) {
  if (!affectedBox_.has_value()) {
    std::fill_n(output, count, 0.0);
    return;
  }
  for (std::size_t i = 0; i < count; ++i)
    output[i] = compute(provider.forIndex(static_cast<int>(i)));
}

ChunkTicketPos StructurePlacementConfig::getPotentialStructureChunk(
    std::int64_t worldSeed, int sourceX, int sourceZ) const noexcept {
  const int spacedGridX = floorDivInt(sourceX, spacing);
  const int spacedGridZ = floorDivInt(sourceZ, spacing);
  LegacyRandomSource random(0);
  worldgen_random::setLargeFeatureWithSalt(random, worldSeed, spacedGridX,
                                           spacedGridZ, salt);
  const int limit = spacing - separation;
  const int spreadX = evaluateSpread(spreadType, random, limit);
  const int spreadZ = evaluateSpread(spreadType, random, limit);
  return {spacedGridX * spacing + spreadX, spacedGridZ * spacing + spreadZ};
}

bool StructurePlacementConfig::applyAdditionalChunkRestrictions(
    int sourceX, int sourceZ, std::int64_t worldSeed) const noexcept {
  if (!(frequency < 1.0f))
    return true;
  LegacyRandomSource random(0);
  switch (frequencyReductionMethod) {
  case FrequencyReductionMethod::Default:
    worldgen_random::setLargeFeatureWithSalt(random, worldSeed, salt, sourceX,
                                             sourceZ);
    return random.nextFloat() < frequency;
  case FrequencyReductionMethod::LegacyType1: {
    const int cx = sourceX >> 4;
    const int cz = sourceZ >> 4;
    const std::int64_t s = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(cx) ^
        (static_cast<std::uint64_t>(cz) << 4) ^
        static_cast<std::uint64_t>(worldSeed));
    random.setSeed(s);
    (void)random.nextInt();
    return random.nextInt(static_cast<int>(1.0f / frequency)) == 0;
  }
  case FrequencyReductionMethod::LegacyType2:
    worldgen_random::setLargeFeatureWithSalt(random, worldSeed, sourceX,
                                             sourceZ, 10387320);
    return random.nextFloat() < frequency;
  case FrequencyReductionMethod::LegacyType3:
    worldgen_random::setLargeFeatureSeed(random, worldSeed, sourceX, sourceZ);
    return random.nextDouble() < static_cast<double>(frequency);
  }
  return true;
}

ChunkGeneratorStructureState::ChunkGeneratorStructureState(
    std::int64_t levelSeed, std::int64_t concentricRingsSeed,
    const WorldgenRegistries &registries, const BiomeSource &biomeSource,
    const ClimateSampler &sampler, bool includeConcentricRings)
    : levelSeed_(levelSeed), concentricRingsSeed_(concentricRingsSeed),
      includeConcentricRings_(includeConcentricRings),
      registries_(registries), biomeSource_(biomeSource), sampler_(sampler) {
  const auto possibleList = biomeSource_.possibleBiomes();
  const std::set<std::string> possibleSet(possibleList.begin(),
                                          possibleList.end());
  for (const auto &[setId, setDef] : registries_.structureSets()) {
    (void)setId;
    if (!includeConcentricRings_ &&
        setDef.placement.kind == StructurePlacementKind::ConcentricRings) {
      continue;
    }
    bool matchesAny = false;
    for (const auto &entry : setDef.structures) {
      const StructureDefinition *sDef =
          registries_.findStructure(entry.structureId);
      if (!sDef)
        continue;
      for (const auto &sel : sDef->biomeSelectors) {
        for (const auto &b : possibleSet) {
          if (registries_.biomeMatches(b, sel)) {
            matchesAny = true;
            break;
          }
        }
        if (matchesAny)
          break;
      }
      if (matchesAny)
        break;
    }
    if (matchesAny)
      possibleStructureSets_.push_back(&setDef);
  }
}

void ChunkGeneratorStructureState::ensureStructuresGenerated() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generated_)
    return;
  for (const StructureSetDefinition *set : possibleStructureSets_) {
    if (set->placement.kind == StructurePlacementKind::ConcentricRings) {
      ringPositions_.emplace(set->id, generateRingPositions(set->placement));
    }
  }
  generated_ = true;
}

std::vector<ChunkTicketPos>
ChunkGeneratorStructureState::generateRingPositions(
    const StructurePlacementConfig &placement) const {
  if (placement.count == 0)
    return {};
  const int distance = placement.distance;
  const int count = placement.count;
  int spread = placement.spread;

  LegacyRandomSource random(0);
  random.setSeed(concentricRingsSeed_);
  double angle = random.nextDouble() * kPiDouble * 2.0;
  int positionInCircle = 0;
  int circle = 0;

  // 预先把生物群系选择器编译成整数位掩码，避免 41 万个采样点每次都做字符串查找。
  std::vector<std::uint64_t> preferredMask;
  const bool hasMask = registries_.buildBiomeSelectorMask(
      std::span<const std::string>(&placement.preferredBiomesSelector, 1),
      preferredMask);
  auto preferredPred = [&](std::string_view biome) {
    if (!hasMask)
      return registries_.biomeMatches(biome, placement.preferredBiomesSelector);
    return WorldgenRegistries::biomeMatchesMask(registries_.findBiomeId(biome),
                                                preferredMask);
  };

  // 对齐 26.1 ChunkGeneratorStructureState.generateRingPositions：
  // 先在主循环中按固定顺序派生子随机源，再把 128 个独立的大半径群系搜索并行执行；
  // 每个搜索只读写自己的 RandomSource，结果按原索引收集，保持与原版一致的确定性。
  struct RingSearchTask {
    int initialX = 0;
    int initialZ = 0;
    LegacyRandomSource rng{0};
  };
  std::vector<RingSearchTask> tasks;
  tasks.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    const double dist = 4.0 * distance + distance * circle * 6.0 +
                        (random.nextDouble() - 0.5) * (distance * 2.5);
    RingSearchTask task;
    task.initialX = static_cast<int>(std::llround(std::cos(angle) * dist));
    task.initialZ = static_cast<int>(std::llround(std::sin(angle) * dist));
    task.rng = random.fork();
    tasks.push_back(std::move(task));

    angle += (kPiDouble * 2.0) / static_cast<double>(spread);
    if (++positionInCircle == spread) {
      ++circle;
      positionInCircle = 0;
      spread += 2 * spread / (circle + 1);
      spread = std::min(spread, count - i);
      angle += random.nextDouble() * kPiDouble * 2.0;
    }
  }

  std::vector<ChunkTicketPos> positions(tasks.size());
  std::vector<std::exception_ptr> failures(tasks.size());
  auto runTask = [&](std::size_t index) noexcept {
    try {
      RingSearchTask &task = tasks[index];
      const auto found = biomeSource_.findBiomeHorizontal(
          (task.initialX << 4) + 8, 0, (task.initialZ << 4) + 8, 112,
          preferredPred, task.rng, sampler_);
      positions[index] =
          found.has_value()
              ? ChunkTicketPos{found->pos.x >> 4, found->pos.z >> 4}
              : ChunkTicketPos{task.initialX, task.initialZ};
    } catch (...) {
      failures[index] = std::current_exception();
    }
  };

  const std::size_t hardware =
      std::max(1u, std::thread::hardware_concurrency());
  const std::size_t workerCount = std::min<std::size_t>(hardware, tasks.size());
  if (workerCount <= 1) {
    for (std::size_t i = 0; i < tasks.size(); ++i)
      runTask(i);
  } else {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount - 1);
    auto workerLoop = [&]() {
      for (;;) {
        const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= tasks.size())
          return;
        runTask(index);
      }
    };
    for (std::size_t w = 1; w < workerCount; ++w)
      workers.emplace_back(workerLoop);
    workerLoop();
    for (std::thread &worker : workers)
      worker.join();
  }
  for (const std::exception_ptr &failure : failures) {
    if (failure)
      std::rethrow_exception(failure);
  }
  return positions;
}

const std::vector<ChunkTicketPos> *
ChunkGeneratorStructureState::getRingPositionsFor(
    const StructureSetDefinition &set) const {
  ensureStructuresGenerated();
  auto it = ringPositions_.find(set.id);
  return it == ringPositions_.end() ? nullptr : &it->second;
}

bool ChunkGeneratorStructureState::isStructureChunk(
    const StructureSetDefinition &set, int chunkX, int chunkZ) const {
  bool isPlacement = false;
  if (set.placement.kind == StructurePlacementKind::ConcentricRings) {
    const auto *list = getRingPositionsFor(set);
    if (list) {
      const ChunkTicketPos target{chunkX, chunkZ};
      isPlacement =
          std::find(list->begin(), list->end(), target) != list->end();
    }
  } else {
    const ChunkTicketPos p =
        set.placement.getPotentialStructureChunk(levelSeed_, chunkX, chunkZ);
    isPlacement = (p.x == chunkX && p.z == chunkZ);
  }
  if (!isPlacement)
    return false;
  if (!set.placement.applyAdditionalChunkRestrictions(chunkX, chunkZ,
                                                      levelSeed_)) {
    return false;
  }
  if (set.placement.exclusionZone.has_value()) {
    if (hasStructureChunkInRange(set.placement.exclusionZone->otherSetId,
                                 chunkX, chunkZ,
                                 set.placement.exclusionZone->chunkCount)) {
      return false;
    }
  }
  return true;
}

bool ChunkGeneratorStructureState::hasStructureChunkInRange(
    std::string_view structureSetId, int chunkX, int chunkZ, int range) const {
  const StructureSetDefinition *otherSet =
      registries_.findStructureSet(structureSetId);
  if (!otherSet)
    return false;
  for (int tx = chunkX - range; tx <= chunkX + range; ++tx) {
    for (int tz = chunkZ - range; tz <= chunkZ + range; ++tz) {
      if (isStructureChunk(*otherSet, tx, tz))
        return true;
    }
  }
  return false;
}

StructureDefinition parseStructureDefinitionJson(const picojson::value &v,
                                                 std::string_view id) {
  const auto &obj = asObj(v, id);
  StructureDefinition def;
  def.id = normId(id);
  const std::string type = normId(asStr(req(obj, "type", id), id));
  if (type == "minecraft:jigsaw") {
    def.type = StructureType::Jigsaw;
    def.terrainAdjustment = TerrainAdjustment::None;
  } else if (type == "minecraft:buried_treasure") {
    def.type = StructureType::BuriedTreasure;
  } else if (type == "minecraft:desert_pyramid") {
    def.type = StructureType::DesertPyramid;
  } else if (type == "minecraft:end_city") {
    def.type = StructureType::EndCity;
  } else if (type == "minecraft:fortress") {
    def.type = StructureType::Fortress;
  } else if (type == "minecraft:igloo") {
    def.type = StructureType::Igloo;
  } else if (type == "minecraft:jungle_temple") {
    def.type = StructureType::JungleTemple;
  } else if (type == "minecraft:mineshaft") {
    def.type = StructureType::Mineshaft;
  } else if (type == "minecraft:nether_fossil") {
    def.type = StructureType::NetherFossil;
  } else if (type == "minecraft:ocean_monument") {
    def.type = StructureType::OceanMonument;
  } else if (type == "minecraft:ocean_ruin") {
    def.type = StructureType::OceanRuin;
  } else if (type == "minecraft:ruined_portal") {
    def.type = StructureType::RuinedPortal;
  } else if (type == "minecraft:shipwreck") {
    def.type = StructureType::Shipwreck;
  } else if (type == "minecraft:stronghold") {
    def.type = StructureType::Stronghold;
  } else if (type == "minecraft:swamp_hut") {
    def.type = StructureType::SwampHut;
  } else if (type == "minecraft:woodland_mansion") {
    def.type = StructureType::WoodlandMansion;
  } else {
    throw std::runtime_error("Unknown structure type '" + type + "' in " +
                             std::string(id));
  }

  const auto &biomesVal = req(obj, "biomes", id);
  if (biomesVal.is<std::string>()) {
    const std::string s = biomesVal.get<std::string>();
    def.biomeSelectors.push_back(
        (!s.empty() && s.front() == '#') ? ("#" + normId(s.substr(1)))
                                         : normId(s));
  } else if (biomesVal.is<picojson::array>()) {
    for (const auto &el : biomesVal.get<picojson::array>()) {
      const std::string s = asStr(el, id);
      def.biomeSelectors.push_back(
          (!s.empty() && s.front() == '#') ? ("#" + normId(s.substr(1)))
                                           : normId(s));
    }
  }
  def.step = parseDecorationStep(asStr(req(obj, "step", id), id));
  if (auto it = obj.find("terrain_adaptation"); it != obj.end())
    def.terrainAdjustment = parseTerrainAdjustment(asStr(it->second, id));

  if (def.type == StructureType::Jigsaw) {
    def.startPool = normId(asStr(req(obj, "start_pool", id), id));
    if (auto it = obj.find("start_jigsaw_name"); it != obj.end())
      def.startJigsawName = normId(asStr(it->second, id));
    def.size = asInt(req(obj, "size", id), id);
    def.startHeight = parseHeightProviderJson(req(obj, "start_height", id), id);
    def.useExpansionHack =
        asBoolVal(req(obj, "use_expansion_hack", id), id);
    if (auto it = obj.find("project_start_to_heightmap"); it != obj.end()) {
      def.projectStartToHeightmap =
          parseHeightmapTypeStr(asStr(it->second, id));
    }
    if (auto it = obj.find("max_distance_from_center"); it != obj.end()) {
      if (it->second.is<double>()) {
        def.maxDistanceHorizontal = asInt(it->second, id);
        def.maxDistanceVertical = def.maxDistanceHorizontal;
      } else if (it->second.is<picojson::object>()) {
        const auto &md = it->second.get<picojson::object>();
        def.maxDistanceHorizontal = asInt(req(md, "horizontal", id), id);
        if (auto vit = md.find("vertical"); vit != md.end())
          def.maxDistanceVertical = asInt(vit->second, id);
      }
    }
    if (auto it = obj.find("dimension_padding"); it != obj.end()) {
      if (it->second.is<double>()) {
        def.dimensionPaddingBottom = asInt(it->second, id);
        def.dimensionPaddingTop = def.dimensionPaddingBottom;
      } else if (it->second.is<picojson::object>()) {
        const auto &dp = it->second.get<picojson::object>();
        if (auto bit = dp.find("bottom"); bit != dp.end())
          def.dimensionPaddingBottom = asInt(bit->second, id);
        if (auto tit = dp.find("top"); tit != dp.end())
          def.dimensionPaddingTop = asInt(tit->second, id);
      }
    }
    if (auto it = obj.find("liquid_settings"); it != obj.end()) {
      if (asStr(it->second, id) == "ignore_waterlogging")
        def.liquidSettings = LiquidSettings::IgnoreWaterlogging;
    }
    if (auto it = obj.find("pool_aliases");
        it != obj.end() && it->second.is<picojson::array>()) {
      for (const auto &al : it->second.get<picojson::array>())
        def.poolAliases.push_back(parsePoolAlias(al, id));
    }
  } else if (def.type == StructureType::NetherFossil) {
    def.fossilHeight = parseHeightProviderJson(req(obj, "height", id), id);
  } else if (def.type == StructureType::Shipwreck) {
    if (auto it = obj.find("is_beached"); it != obj.end())
      def.isBeached = asBoolVal(it->second, id);
  } else if (def.type == StructureType::OceanRuin) {
    def.oceanRuinTemp = asStr(req(obj, "biome_temp", id), id);
    def.largeProbability =
        static_cast<float>(asNum(req(obj, "large_probability", id), id));
    def.clusterProbability =
        static_cast<float>(asNum(req(obj, "cluster_probability", id), id));
  } else if (def.type == StructureType::RuinedPortal) {
    if (auto it = obj.find("setups");
        it != obj.end() && it->second.is<picojson::array>()) {
      for (const auto &s : it->second.get<picojson::array>()) {
        const auto &so = asObj(s, id);
        StructureDefinition::RuinedPortalSetup rp;
        rp.placement = asStr(req(so, "placement", id), id);
        rp.airPocketProbability = static_cast<float>(
            asNum(req(so, "air_pocket_probability", id), id));
        rp.mossiness =
            static_cast<float>(asNum(req(so, "mossiness", id), id));
        rp.overgrown = asBoolVal(req(so, "overgrown", id), id);
        rp.vines = asBoolVal(req(so, "vines", id), id);
        rp.canBeCold = asBoolVal(req(so, "can_be_cold", id), id);
        rp.replaceWithBlackstone =
            asBoolVal(req(so, "replace_with_blackstone", id), id);
        rp.weight = static_cast<float>(asNum(req(so, "weight", id), id));
        def.portalSetups.push_back(std::move(rp));
      }
    }
  }
  return def;
}

StructureSetDefinition parseStructureSetJson(const picojson::value &v,
                                             std::string_view id) {
  const auto &obj = asObj(v, id);
  StructureSetDefinition out;
  out.id = normId(id);

  const auto &arr = req(obj, "structures", id);
  if (!arr.is<picojson::array>())
    throw std::runtime_error("Expected structures array in " + std::string(id));
  for (const auto &el : arr.get<picojson::array>()) {
    const auto &eo = asObj(el, id);
    out.structures.push_back(
        {normId(asStr(req(eo, "structure", id), id)),
         asInt(req(eo, "weight", id), id)});
  }

  const auto &p = asObj(req(obj, "placement", id), id);
  const std::string ptype = normId(asStr(req(p, "type", id), id));
  if (auto it = p.find("salt"); it != p.end())
    out.placement.salt = asInt(it->second, id);
  if (auto it = p.find("frequency"); it != p.end())
    out.placement.frequency = static_cast<float>(asNum(it->second, id));
  if (auto it = p.find("frequency_reduction_method"); it != p.end()) {
    const std::string m = asStr(it->second, id);
    if (m == "legacy_type_1")
      out.placement.frequencyReductionMethod =
          FrequencyReductionMethod::LegacyType1;
    else if (m == "legacy_type_2")
      out.placement.frequencyReductionMethod =
          FrequencyReductionMethod::LegacyType2;
    else if (m == "legacy_type_3")
      out.placement.frequencyReductionMethod =
          FrequencyReductionMethod::LegacyType3;
    else
      out.placement.frequencyReductionMethod =
          FrequencyReductionMethod::Default;
  }
  if (auto it = p.find("locate_offset");
      it != p.end() && it->second.is<picojson::array>()) {
    const auto &a = it->second.get<picojson::array>();
    if (a.size() == 3) {
      out.placement.locateOffset = Pos{asInt(a[0], id), asInt(a[1], id),
                                       asInt(a[2], id)};
    }
  }
  if (auto it = p.find("exclusion_zone");
      it != p.end() && it->second.is<picojson::object>()) {
    const auto &ez = it->second.get<picojson::object>();
    out.placement.exclusionZone = ExclusionZoneConfig{
        normId(asStr(req(ez, "other_set", id), id)),
        asInt(req(ez, "chunk_count", id), id),
    };
  }

  if (ptype == "minecraft:random_spread") {
    out.placement.kind = StructurePlacementKind::RandomSpread;
    out.placement.spacing = asInt(req(p, "spacing", id), id);
    out.placement.separation = asInt(req(p, "separation", id), id);
    if (auto it = p.find("spread_type"); it != p.end()) {
      out.placement.spreadType =
          asStr(it->second, id) == "triangular" ? SpreadType::Triangular
                                                : SpreadType::Linear;
    }
  } else if (ptype == "minecraft:concentric_rings") {
    out.placement.kind = StructurePlacementKind::ConcentricRings;
    out.placement.distance = asInt(req(p, "distance", id), id);
    out.placement.spread = asInt(req(p, "spread", id), id);
    out.placement.count = asInt(req(p, "count", id), id);
    const std::string pb = asStr(req(p, "preferred_biomes", id), id);
    out.placement.preferredBiomesSelector =
        (!pb.empty() && pb.front() == '#') ? ("#" + normId(pb.substr(1)))
                                           : normId(pb);
  } else {
    throw std::runtime_error("Unsupported StructurePlacement '" + ptype +
                             "' in " + std::string(id));
  }
  return out;
}

} // namespace redstone::worldgen
