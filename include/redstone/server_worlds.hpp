#pragma once
#include "redstone/dimension.hpp"
#include "redstone/engine.hpp"
#include "redstone/storage.hpp"
#include "redstone/world_chunk_manager.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {

// Per-dimension runtime equivalent of vanilla ServerLevel + LevelChunk cache.
struct DimensionLevel {
  DimensionStem stem;
  std::filesystem::path regionDirectory;
  std::unique_ptr<World> world;
  std::unique_ptr<WorldChunkManager> chunkManager;
  std::vector<PersistentTicketRecord> initialTickets;
  std::uint64_t seed = 0;
  bool blockTagsLoaded = false;
  std::size_t blockTagPackCount = 0;
};

struct ServerWorldOptions {
  bool withChunkManagers = true;
  bool lowMemoryMode = false;
  int viewDistance = 3;
  int simulationDistance = 10;
  int maxNeighborUpdates = 1000000;
  std::vector<std::filesystem::path> extraBlockTagPacks;
  // Optional filter; empty loads every dimension listed in world metadata.
  std::vector<std::string> dimensionsFilter;
};

// Owns shared server metadata (level.dat Time, GameRules, WorldClocks) and all
// active DimensionLevel instances for a multi-dimension 26.1 world directory.
class ServerWorldSet {
public:
  ServerWorldSet() = default;
  ~ServerWorldSet() noexcept;
  ServerWorldSet(const ServerWorldSet &) = delete;
  ServerWorldSet &operator=(const ServerWorldSet &) = delete;
  ServerWorldSet(ServerWorldSet &&) noexcept = default;
  ServerWorldSet &operator=(ServerWorldSet &&) noexcept = default;

  static ServerWorldSet open(const std::filesystem::path &worldDir,
                             const ServerWorldOptions &options = {});

  const std::filesystem::path &worldDir() const noexcept { return worldDir_; }
  WorldMetadata &metadata() noexcept { return metadata_; }
  const WorldMetadata &metadata() const noexcept { return metadata_; }
  const WorldDataPackSelection &dataPackSelection() const noexcept {
    return packSelection_;
  }
  const std::vector<std::filesystem::path> &blockTagPacks() const noexcept {
    return blockTagPacks_;
  }

  const std::vector<std::unique_ptr<DimensionLevel>> &levels() const noexcept {
    return levels_;
  }
  std::vector<std::unique_ptr<DimensionLevel>> &levels() noexcept {
    return levels_;
  }

  DimensionLevel *find(std::string_view dimensionId);
  const DimensionLevel *find(std::string_view dimensionId) const;
  DimensionLevel *overworld();
  const DimensionLevel *overworld() const;

  void enableLighting(LightWorkerOptions options = {});
  void tick(bool runsNormally = true, bool tickChunks = true);
  void pumpAll();
  void flushAndSaveOnShutdown();
  void saveMetadataAndTickets();

private:
  std::filesystem::path worldDir_;
  WorldMetadata metadata_;
  WorldDataPackSelection packSelection_;
  std::vector<std::filesystem::path> blockTagPacks_;
  std::vector<std::unique_ptr<DimensionLevel>> levels_;
};

// Deterministic per-dimension RNG seed derivation from the shared world seed.
std::uint64_t deriveDimensionSeed(std::uint64_t worldSeed,
                                  std::string_view dimensionId);

} // namespace redstone
