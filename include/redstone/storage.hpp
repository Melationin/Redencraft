#pragma once
#include "redstone/engine.hpp"
#include "redstone/nbt.hpp"
#include "redstone/tickets.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace redstone {
struct SavedTickData {
  Pos pos;
  std::string type;
  int delay{}, priority{};
  bool fluid{};
  // Litematica records the original ScheduledTick.subTickOrder as SubTick;
  // vanilla's own chunk format does not, and rebuilds -N..-1 in saved-list
  // order instead (LevelChunkTicks.unpack). Absent means "use the vanilla
  // per-chunk sequence", present means "keep the schematic's own order", so
  // the two rules never get mixed (LitematicaSchematic.java:1498,2834).
  std::optional<std::int64_t> subTick;
};
struct WorldGameRules {
  bool advanceTime = true;
  bool tntExplodes = true;
  bool waterSourceConversion = true;
  bool lavaSourceConversion = false;
};

struct WorldClockState {
  std::int64_t totalTicks = 0;
  float partialTick = 0.0f;
  float rate = 1.0f;
  bool paused = false;
};
enum class BuiltinWorldClock : std::uint8_t { Overworld, TheEnd };
// ServerClockManager kernel and the supported part of the 26.1
// data/minecraft/world_clocks.dat SavedData. Unrecognised top-level data,
// clock entries and fields remain in the loaded NBT document and are never
// discarded when a supported clock changes.
class WorldClocks {
public:
  WorldClocks();
  static WorldClocks load(const std::filesystem::path &path);
  const WorldClockState &state(BuiltinWorldClock clock) const;
  // Returns true only when the requested state differs. The next successful
  // saveIfDirty writes the change.
  bool setClock(BuiltinWorldClock clock, const WorldClockState &state);
  // MinecraftServer.tickChildren calls clocks.tick only when the tick-rate
  // manager runs normally, then ServerClockManager.tick applies advance_time.
  void tick(bool runsNormally, bool advanceTime);
  bool dirty() const { return dirty_; }
  // False means the on-disk bytes were left untouched. A successful write
  // clears dirty; I/O and encoding errors propagate and keep it dirty.
  bool saveIfDirty(const std::filesystem::path &path);
private:
  std::array<WorldClockState, 2> states_{};
  nbt::RootTag document_;
  bool dirty_ = false;
};

struct WorldMetadata {
  std::int64_t gameTime = 0;
  std::int64_t seed = 0;
  WorldGameRules gameRules;
  WorldClocks worldClocks;
  // Parsed level.dat root remains the sole writable metadata template. The
  // read path validates DataVersion/Data.Time before returning it.
  nbt::RootTag levelRoot;
};
// Loading is read-only. Updating Time mutates levelRoot in place; persistence
// uses a temporary gzip file plus rename, retaining level.dat_old when a
// current file exists. Failures leave the current level.dat untouched.
void setWorldMetadataTime(WorldMetadata &metadata, std::int64_t gameTime);
void saveWorldMetadata(const std::filesystem::path &worldDir,
                       const WorldMetadata &metadata);
// ticket path is <selected dimension root>/data/minecraft/chunk_tickets.dat.
std::filesystem::path ticketDataPath(const std::filesystem::path &regionDirectory);
struct PersistentTicketRecord {
  std::int32_t x = 0, z = 0;
  std::string type;
  std::int32_t level = 0;
  std::int64_t ticksLeft = 0;
};
// Missing is an empty list; any malformed envelope or unsupported ticket type
// is an error and must be rejected before startup installs any demand.
std::vector<PersistentTicketRecord>
loadPersistentTickets(const std::filesystem::path &path);
// Caller skips this when the file is absent and the list is empty. Writes the
// exact 26.1 SavedDataStorage gzip envelope and replaces only that file.
void savePersistentTickets(const std::filesystem::path &path,
                           std::span<const PersistentTicketRecord> tickets);
struct LoadedWorld {
  std::int64_t gameTime{};
  int minY = -64, maxY = 320;
  std::map<Pos, BlockState> blocks;
  std::vector<std::pair<int, int>> chunks;
  std::vector<SavedTickData> ticks;
  std::vector<nbt::Tag> blockEntities;
  std::vector<nbt::Tag> entities;
  std::unordered_map<std::uint64_t, PersistedChunkLight> persistedLights;
  std::int64_t seed = 0;
};
// 26.1's namespaced overworld path takes precedence over legacy world/region.
std::filesystem::path defaultRegionDirectory(const std::filesystem::path &worldDir);
// regionDir is the selected dimension's region directory; its sibling entities
// directory is read without generating/loading additional terrain chunks.
// worldDir contains level.dat and namespaced data/minecraft saved data.
// Reads level.dat and namespaced seed metadata only; no terrain scan.
WorldMetadata loadWorldMetadata(const std::filesystem::path &worldDir);
// Reads only the existing 26.1 namespaced game_rules SavedData. Missing
// returns the vanilla defaults; malformed present data is an error.
WorldGameRules loadWorldGameRules(const std::filesystem::path &path);
LoadedWorld loadAnvilWorld(const std::filesystem::path &worldDir,
                           const std::filesystem::path &regionDir,
                           int minY = -64, int maxY = 320);
void importWorld(const LoadedWorld &input, World &world);
} // namespace redstone
