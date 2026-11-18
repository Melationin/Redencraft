#include "redstone/server_worlds.hpp"

#include "redstone/portal.hpp"
#include "redstone/worldgen/generator_factory.hpp"
#include "redstone/worldgen/noise_router.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace redstone {
namespace {

std::uint64_t fnv1a64(std::string_view text) noexcept {
  std::uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : text) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  // SplitMix64 finalizer so similar names ("minecraft:the_nether" / "minecraft:the_end")
  // diverge completely across all 64 bits.
  h += 0x9e3779b97f4a7c15ULL;
  h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
  h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
  return h ^ (h >> 31);
}

std::vector<PersistentTicketRecord>
buildPersistentTicketSnapshot(const WorldChunkManager &manager) {
  std::vector<PersistentTicketRecord> out;
  const auto active =
      manager.ticketSources().tickets().storage().persistentTickets();
  out.reserve(active.size());
  for (const auto &[pos, ticket] : active) {
    if (ticket.type().name == ticket_types::Forced.name ||
        ticket.type().name == ticket_types::Portal.name) {
      out.push_back(PersistentTicketRecord{
          .x = pos.x,
          .z = pos.z,
          .type = std::string(ticket.type().name),
          .level = ticket.level(),
          .ticksLeft = ticket.ticksLeft(),
      });
    }
  }
  return out;
}

} // namespace

std::uint64_t deriveDimensionSeed(std::uint64_t worldSeed,
                                  std::string_view dimensionId) {
  const std::string normalized = normalizeDimensionId(dimensionId);
  if (normalized == "minecraft:overworld")
    return worldSeed;
  return worldSeed ^ fnv1a64(normalized);
}

ServerWorldSet::~ServerWorldSet() noexcept = default;

ServerWorldSet ServerWorldSet::open(const std::filesystem::path &worldDir,
                                    const ServerWorldOptions &options) {
  ServerWorldSet set;
  set.worldDir_ = worldDir;
  set.metadata_ = loadWorldMetadata(worldDir);
  set.packSelection_ =
      selectWorldDataPacks(worldDir, set.metadata_.levelRoot);
  set.blockTagPacks_ = set.packSelection_.paths;
  for (const auto &extra : options.extraBlockTagPacks)
    set.blockTagPacks_.push_back(extra);

  std::unordered_set<std::string> filterSet;
  for (const auto &rawId : options.dimensionsFilter)
    filterSet.insert(normalizeDimensionId(rawId));

  if (!options.worldgenDirectory.empty()) {
    set.worldgenRegistries_ = std::make_shared<worldgen::WorldgenRegistries>(
        worldgen::WorldgenRegistries::loadFromDirectory(
            options.worldgenDirectory));
  }

  for (const DimensionStem &stem : set.metadata_.dimensions) {
    if (!filterSet.empty() && !filterSet.contains(stem.id))
      continue;
    auto level = std::make_unique<DimensionLevel>();
    level->stem = stem;
    level->regionDirectory = dimensionRegionDirectory(worldDir, stem.id);
    level->world = std::make_unique<World>(
        stem.type, options.maxNeighborUpdates, stem.id);
    level->world->setCleanupManager(set.cleanupManager_, false);
    level->seed = deriveDimensionSeed(
        static_cast<std::uint64_t>(set.metadata_.seed), stem.id);
    level->world->setSeed(level->seed);
    if (set.worldgenRegistries_) {
      level->generator = worldgen::createVanillaChunkGenerator(
          *set.worldgenRegistries_, stem.id, stem.type);
      if (!level->generator) {
        std::cerr << "WORLDGEN unsupported dimension " << stem.id
                  << ": missing chunks will not be generated" << std::endl;
      }
    }
    level->world->setGameTime(set.metadata_.gameTime);
    level->world->tntExplodes = set.metadata_.gameRules.tntExplodes;
    level->world->waterSourceConversion =
        set.metadata_.gameRules.waterSourceConversion;
    level->world->lavaSourceConversion =
        set.metadata_.gameRules.lavaSourceConversion;
    level->world->randomTickSpeed = set.metadata_.gameRules.randomTickSpeed;
    if (!set.blockTagPacks_.empty()) {
      const auto reload = level->world->reloadBlockTags(set.blockTagPacks_);
      if (!reload.applied) {
        std::string msg = "cannot load block tag packs for " + stem.id;
        if (!reload.diagnostics.empty())
          msg += ": " + reload.diagnostics.front();
        throw std::runtime_error(msg);
      }
      level->blockTagsLoaded = true;
      level->blockTagPackCount = set.blockTagPacks_.size();
    }
    // 启用世界生成时允许全新的世界目录：region/ 会在第一次写盘时创建。
    if (options.withChunkManagers &&
        (std::filesystem::is_directory(level->regionDirectory) ||
         level->generator != nullptr)) {
      const auto ticketPath = ticketDataPath(level->regionDirectory);
      level->initialTickets = loadPersistentTickets(ticketPath);
      level->chunkManager = std::make_unique<WorldChunkManager>(
          *level->world, level->regionDirectory, options.viewDistance,
          options.simulationDistance, EntityChunkDataProbe{},
          std::shared_ptr<RawEntityStore>{}, level->generator);
      if (!level->initialTickets.empty()) {
        std::vector<std::pair<ChunkTicketPos, Ticket>> restore;
        restore.reserve(level->initialTickets.size());
        for (const auto &record : level->initialTickets) {
          restore.emplace_back(
              ChunkTicketPos{record.x, record.z},
              Ticket(record.type == ticket_types::Forced.name
                         ? ticket_types::Forced
                         : ticket_types::Portal,
                     record.level, record.ticksLeft));
        }
        (void)level->chunkManager->restorePersistentTickets(restore);
      }
    }
    set.levels_.push_back(std::move(level));
  }
  return set;
}

DimensionLevel *ServerWorldSet::find(std::string_view dimensionId) {
  std::string normalized;
  try {
    normalized = normalizeDimensionId(dimensionId);
  } catch (const std::invalid_argument &) {
    return nullptr;
  }
  for (const auto &level : levels_) {
    if (level && level->stem.id == normalized)
      return level.get();
  }
  return nullptr;
}

const DimensionLevel *
ServerWorldSet::find(std::string_view dimensionId) const {
  return const_cast<ServerWorldSet *>(this)->find(dimensionId);
}

DimensionLevel *ServerWorldSet::overworld() {
  if (auto *ow = find("minecraft:overworld"))
    return ow;
  return levels_.empty() ? nullptr : levels_.front().get();
}

const DimensionLevel *ServerWorldSet::overworld() const {
  return const_cast<ServerWorldSet *>(this)->overworld();
}

void ServerWorldSet::enableLighting(LightWorkerOptions options) {
  for (const auto &level : levels_) {
    if (level && level->world)
      level->world->enableLighting(options);
  }
}

void ServerWorldSet::tick(bool runsNormally, bool tickChunks) {
  const auto tickStart = std::chrono::steady_clock::now();
  // Vanilla MinecraftServer.tickChildren advances WorldClocks once per server
  // tick rather than once per ServerLevel.
  metadata_.worldClocks.tick(runsNormally, metadata_.gameRules.advanceTime);
  const std::int64_t preTickTime = metadata_.gameTime;
  const std::int64_t nextTime =
      (runsNormally && tickChunks) ? (preTickTime + 1) : preTickTime;
  for (const auto &level : levels_) {
    if (!level || !level->world)
      continue;
    // World::tick() itself increments time_ before draining scheduled ticks,
    // so set preTickTime before tick and ensure nextTime afterward even if a
    // manager is paused or skipping chunk ticks.
    level->world->setGameTime(preTickTime);
    if (level->chunkManager) {
      level->chunkManager->serverTick(tickChunks, runsNormally);
    } else if (runsNormally && tickChunks) {
      level->world->tick();
    } else {
      level->world->drainLightPublications();
    }
    level->world->setGameTime(nextTime);
  }
  processPendingPortalTransfers(*this);
  metadata_.gameTime = nextTime;
  if (runsNormally && tickChunks && cleanupManager_) {
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tickStart)
            .count();
    cleanupManager_->advanceTick(elapsedMs);
  }
}

void ServerWorldSet::pumpAll() {
  for (const auto &level : levels_) {
    if (!level)
      continue;
    if (level->chunkManager)
      (void)level->chunkManager->pump();
    if (level->world)
      level->world->drainLightPublications();
  }
}

void ServerWorldSet::saveMetadataAndTickets() {
  if (worldDir_.empty())
    return;
  setWorldMetadataTime(metadata_, metadata_.gameTime);
  saveWorldMetadata(worldDir_, metadata_);
  (void)metadata_.worldClocks.saveIfDirty(
      worldDir_ / "data" / "minecraft" / "world_clocks.dat");
  for (const auto &level : levels_) {
    if (!level || !level->chunkManager)
      continue;
    const auto ticketPath = ticketDataPath(level->regionDirectory);
    const auto records = buildPersistentTicketSnapshot(*level->chunkManager);
    if (!records.empty() || std::filesystem::exists(ticketPath))
      savePersistentTickets(ticketPath, records);
  }
}

void ServerWorldSet::flushAndSaveOnShutdown() {
  for (const auto &level : levels_) {
    if (!level || !level->chunkManager)
      continue;
    const ChunkShutdownResult result = level->chunkManager->close();
    if (!result.complete) {
      throw std::runtime_error(
          "incomplete shutdown for dimension " + level->stem.id +
          (result.fatalError.empty() ? "" : ": " + result.fatalError));
    }
  }
  saveMetadataAndTickets();
}

} // namespace redstone
