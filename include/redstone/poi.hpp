#pragma once

#include "redstone/block.hpp"
#include "redstone/flat_hash_map.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {

// Vanilla 26.1 net.minecraft.world.entity.ai.village.poi.PoiTypes (21 types).
enum class PoiTypeId : std::uint8_t {
  None = 0,
  Armorer = 1,
  Butcher = 2,
  Cartographer = 3,
  Cleric = 4,
  Farmer = 5,
  Fisherman = 6,
  Fletcher = 7,
  Leatherworker = 8,
  Librarian = 9,
  Mason = 10,
  Shepherd = 11,
  Toolsmith = 12,
  Weaponsmith = 13,
  Home = 14,
  Meeting = 15,
  Beehive = 16,
  BeeNest = 17,
  NetherPortal = 18,
  Lodestone = 19,
  LightningRod = 20,
  TestInstance = 21,
};

inline constexpr std::size_t kPoiTypeCount = 21;
inline constexpr std::size_t kPoiSectionCapacity = 24;

constexpr std::uint32_t poiTypeMask(PoiTypeId id) noexcept {
  const unsigned index = static_cast<unsigned>(id);
  return (index >= 1u && index <= kPoiTypeCount)
             ? (std::uint32_t{1} << (index - 1u))
             : 0u;
}

inline constexpr std::uint32_t kAcquirableJobSitePoiMask =
    poiTypeMask(PoiTypeId::Armorer) | poiTypeMask(PoiTypeId::Butcher) |
    poiTypeMask(PoiTypeId::Cartographer) | poiTypeMask(PoiTypeId::Cleric) |
    poiTypeMask(PoiTypeId::Farmer) | poiTypeMask(PoiTypeId::Fisherman) |
    poiTypeMask(PoiTypeId::Fletcher) | poiTypeMask(PoiTypeId::Leatherworker) |
    poiTypeMask(PoiTypeId::Librarian) | poiTypeMask(PoiTypeId::Mason) |
    poiTypeMask(PoiTypeId::Shepherd) | poiTypeMask(PoiTypeId::Toolsmith) |
    poiTypeMask(PoiTypeId::Weaponsmith);

inline constexpr std::uint32_t kVillagePoiMask =
    kAcquirableJobSitePoiMask | poiTypeMask(PoiTypeId::Home) |
    poiTypeMask(PoiTypeId::Meeting);

inline constexpr std::uint32_t kBeeHomePoiMask =
    poiTypeMask(PoiTypeId::Beehive) | poiTypeMask(PoiTypeId::BeeNest);

inline constexpr std::uint32_t kAllPoiMask =
    (std::uint32_t{1} << kPoiTypeCount) - 1u;

struct PoiTypeSpec {
  PoiTypeId id = PoiTypeId::None;
  std::string_view name;
  std::uint8_t maxTickets = 0;
  std::uint8_t validRange = 0;
};

struct PoiPos {
  int x = 0;
  int y = 0;
  int z = 0;
  auto operator<=>(const PoiPos &) const = default;
};

enum class PoiOccupancy : std::uint8_t {
  HasSpace = 0,
  IsOccupied = 1,
  Any = 2,
};

struct PoiMatch {
  PoiPos pos;
  PoiTypeId type = PoiTypeId::None;
  std::uint8_t freeTickets = 0;
};

// 4-byte packed POI record aligned with PoiSection.
// Bits [0..11]  : local cell (localY << 8) | (localZ << 4) | localX  (0..4095)
// Bits [12..16] : PoiTypeId (1..21)
// Bits [17..22] : freeTickets (0..63)
struct PackedPoiEntry {
  std::uint32_t word = 0;

  static constexpr PackedPoiEntry make(std::uint16_t localPos, PoiTypeId type,
                                       std::uint8_t freeTickets) noexcept {
    return PackedPoiEntry{
        (static_cast<std::uint32_t>(localPos) & 0xFFFu) |
        ((static_cast<std::uint32_t>(type) & 0x1Fu) << 12u) |
        ((static_cast<std::uint32_t>(freeTickets) & 0x3Fu) << 17u)};
  }

  constexpr std::uint16_t localPos() const noexcept {
    return static_cast<std::uint16_t>(word & 0xFFFu);
  }
  constexpr PoiTypeId type() const noexcept {
    return static_cast<PoiTypeId>((word >> 12u) & 0x1Fu);
  }
  constexpr std::uint8_t freeTickets() const noexcept {
    return static_cast<std::uint8_t>((word >> 17u) & 0x3Fu);
  }
  constexpr void setFreeTickets(std::uint8_t value) noexcept {
    word = (word & ~(std::uint32_t{0x3Fu} << 17u)) |
           ((static_cast<std::uint32_t>(value) & 0x3Fu) << 17u);
  }
};
static_assert(sizeof(PackedPoiEntry) == 4);

namespace detail {
extern const std::uint8_t *gPoiStateData;
extern std::uint32_t gPoiStateCount;
void initPoiStateTable();
} // namespace detail

const PoiTypeSpec &poiTypeSpec(PoiTypeId id);
PoiTypeId poiTypeFromName(std::string_view name);
void ensurePoiStateTable();

inline PoiTypeId poiTypeForState(BlockState::Id stateId) {
  if (detail::gPoiStateCount == 0)
    detail::initPoiStateTable();
  if (stateId >= detail::gPoiStateCount)
    return PoiTypeId::None;
  return static_cast<PoiTypeId>(detail::gPoiStateData[stateId]);
}

inline PoiTypeId poiTypeForState(const BlockState &state) {
  return poiTypeForState(state.id());
}

// Fast lookup used after PoiStorage construction guarantees the table exists.
inline PoiTypeId fastPoiTypeForState(BlockState::Id stateId) noexcept {
  return (stateId < detail::gPoiStateCount)
             ? static_cast<PoiTypeId>(detail::gPoiStateData[stateId])
             : PoiTypeId::None;
}

bool paletteHasAnyPoi(std::span<const BlockState::Id> palette) noexcept;

struct Uint16Hash {
  std::uint64_t operator()(std::uint16_t value) const noexcept {
    std::uint64_t h = static_cast<std::uint64_t>(value) * 0x9E3779B97F4A7C15ULL;
    return h ^ (h >> 32);
  }
};

struct ChunkKeyHash {
  std::uint64_t operator()(std::uint64_t key) const noexcept {
    key += 0x9E3779B97F4A7C15ULL;
    key = (key ^ (key >> 30)) * 0xBF58476D1CE4E5B9ULL;
    key = (key ^ (key >> 27)) * 0x94D049BB133111EBULL;
    return key ^ (key >> 31);
  }
};

struct PoiSection {
  static constexpr std::size_t kLinearScanLimit = 32;

  bool valid = false;
  std::uint32_t typeMask = 0;
  std::array<std::uint16_t, kPoiTypeCount> typeCounts{};
  std::vector<PackedPoiEntry> entries;
  FlatHashMap<std::uint16_t, std::uint16_t, Uint16Hash> indexByLocal;

  bool empty() const noexcept { return entries.empty(); }
  bool matchesType(std::uint32_t queryMask) const noexcept {
    return (typeMask & queryMask) != 0;
  }

  std::optional<std::size_t> findIndex(std::uint16_t localPos) const noexcept;
  bool add(std::uint16_t localPos, PoiTypeId type, std::uint8_t freeTickets,
           bool *replacedMismatch = nullptr);
  bool remove(std::uint16_t localPos, PoiTypeId *removedType = nullptr);
  void clear() noexcept;

  static bool matchesOccupancy(PackedPoiEntry entry,
                               PoiOccupancy occupancy) noexcept;
};

struct PoiChunkColumn {
  std::uint32_t validMask = 0;
  std::uint32_t nonEmptyMask = 0;
  std::uint32_t typeMask = 0;
  std::array<std::uint32_t, kPoiTypeCount> sectionMasksByType{};
  std::array<std::unique_ptr<PoiSection>, kPoiSectionCapacity> sections{};
  bool dirty = false;

  std::uint32_t candidateSections(std::uint32_t queryTypeMask) const noexcept;
};

struct DecodedPoiSection {
  int sectionY = 0;
  bool valid = false;
  std::vector<PackedPoiEntry> entries;
};

struct DecodedPoiChunk {
  enum class Status { Ok, Missing, Unsupported, Corrupt };
  Status status = Status::Corrupt;
  std::string detail;
  int dataVersion = 4786;
  std::vector<DecodedPoiSection> sections;
};

// Vanilla 26.1 Anvil poi/r.x.z.mca chunk codec (DataVersion 4786).
DecodedPoiChunk decodePoiChunk(std::span<const std::uint8_t> nbtBytes,
                               int minSectionY, int maxSectionYInclusive);
std::vector<std::uint8_t> encodePoiChunk(int chunkX, int chunkZ, int minSectionY,
                                         int sectionCount,
                                         const PoiChunkColumn &column);
DecodedPoiChunk loadPoiChunkFromRegion(const std::filesystem::path &poiDir,
                                       int chunkX, int chunkZ, int minSectionY,
                                       int maxSectionYInclusive);
void savePoiChunkToRegion(const std::filesystem::path &poiDir, int chunkX,
                          int chunkZ, int minSectionY, int sectionCount,
                          const PoiChunkColumn &column);

// Per-dimension point-of-interest storage with per-type section bitmasks and
// concentric-ring L2 spatial queries.
//
// TODO: Village DistanceTracker (SectionTracker range 0..6 for raids,
// wandering trader and cat spawning) is not implemented yet.
class PoiStorage {
public:
  using PosFilter = std::function<bool(PoiPos)>;
  using PortalFilter = std::function<bool(const PoiMatch &)>;

  PoiStorage(int minY = -64, int maxY = 320);

  int minY() const noexcept { return minY_; }
  int maxY() const noexcept { return maxY_; }
  int sectionCount() const noexcept { return sectionCount_; }
  int minSectionY() const noexcept { return minY_ >> 4; }
  std::size_t chunkCount() const noexcept { return columns_.size(); }

  // Point mutations & maintenance
  bool add(PoiPos pos, PoiTypeId type);
  bool addWithTickets(PoiPos pos, PoiTypeId type, std::uint8_t freeTickets);
  bool remove(PoiPos pos);
  void onBlockStateChange(PoiPos pos, BlockState::Id oldStateId,
                          BlockState::Id newStateId);

  // Vanilla checkConsistencyWithBlocks primitives for a section [0..sectionCount).
  bool isSectionValid(int chunkX, int chunkZ, int sectionIndex) const noexcept;
  void markSectionValidEmpty(int chunkX, int chunkZ, int sectionIndex);
  // Replaces or reconciles entries in a section from a terrain block scan.
  // Preserves freeTickets for any pre-existing entry with identical (localPos, type)
  // when the section was previously loaded from disk with Valid=0.
  void reconcileSectionFromScan(int chunkX, int chunkZ, int sectionIndex,
                                std::span<const std::pair<std::uint16_t, PoiTypeId>> found,
                                bool markDirtyOnChange = true);
  // Applies decoded poi/r.x.z.mca records before terrain consistency check.
  void applyDecodedChunk(int chunkX, int chunkZ,
                         const DecodedPoiChunk &decoded);

  void removeChunk(int chunkX, int chunkZ);
  void clear();

  // Ticket occupancy
  bool exists(PoiPos pos, std::uint32_t typeMask = kAllPoiMask) const noexcept;
  bool existsAtPosition(PoiTypeId type, PoiPos pos) const noexcept;
  PoiTypeId getType(PoiPos pos) const noexcept;
  std::uint8_t freeTickets(PoiPos pos) const noexcept;
  bool acquireTicket(PoiPos pos);
  bool releaseTicket(PoiPos pos);
  std::optional<PoiPos> take(PoiPos center, int radius, std::uint32_t typeMask,
                             const std::function<bool(PoiTypeId, PoiPos)> &filter = {});

  // Dirty tracking for poi/r.x.z.mca persistence
  bool hasDirtyChunks() const noexcept;
  std::vector<std::pair<int, int>> dirtyChunks() const;
  void clearChunkDirty(int chunkX, int chunkZ) noexcept;
  const PoiChunkColumn *findColumn(int chunkX, int chunkZ) const noexcept;

  // Queries
  void collectInChunk(int chunkX, int chunkZ, std::uint32_t typeMask,
                      PoiOccupancy occupancy, std::vector<PoiMatch> &out) const;
  void collectInSquare(PoiPos center, int radius, std::uint32_t typeMask,
                       PoiOccupancy occupancy, std::vector<PoiMatch> &out) const;
  void collectInRange(PoiPos center, int radius, std::uint32_t typeMask,
                      PoiOccupancy occupancy, std::vector<PoiMatch> &out) const;
  std::size_t countInRange(PoiPos center, int radius, std::uint32_t typeMask,
                           PoiOccupancy occupancy) const;

  std::optional<PoiPos> findFirst(PoiPos center, int radius,
                                  std::uint32_t typeMask,
                                  PoiOccupancy occupancy,
                                  const PosFilter &filter = {}) const;
  std::optional<PoiMatch> findClosestWithType(PoiPos center, int radius,
                                              std::uint32_t typeMask,
                                              PoiOccupancy occupancy,
                                              const PosFilter &filter = {}) const;
  std::optional<PoiPos> findClosest(PoiPos center, int radius,
                                    std::uint32_t typeMask,
                                    PoiOccupancy occupancy,
                                    const PosFilter &filter = {}) const;
  std::vector<PoiMatch> findClosestN(PoiPos center, int radius,
                                     std::uint32_t typeMask,
                                     PoiOccupancy occupancy, std::size_t limit,
                                     const PosFilter &filter = {}) const;

  // PortalForcer.findClosestPortalPosition query:
  // searches NETHER_PORTAL in square [-radius..+radius]^2 horizontally across
  // full world height, orders by (distSqr ASC, y ASC), and applies lazy filter.
  std::optional<PoiPos> findClosestPortal(PoiPos center, int radius,
                                          const PortalFilter &filter = {}) const;

  // Uniformly shuffles in-range matches and returns first passing filter.
  template <class Rng>
  std::optional<PoiPos> getRandom(PoiPos center, int radius,
                                  std::uint32_t typeMask,
                                  PoiOccupancy occupancy, Rng &rng,
                                  const PosFilter &filter = {}) const {
    collectInRange(center, radius, typeMask, occupancy, scratchMatches_);
    for (std::size_t i = scratchMatches_.size(); i > 0; --i) {
      const std::size_t pick =
          static_cast<std::size_t>(rng.nextInt(static_cast<int>(i)));
      std::swap(scratchMatches_[pick], scratchMatches_[i - 1]);
      const PoiPos candidate = scratchMatches_[i - 1].pos;
      if (!filter || filter(candidate))
        return candidate;
    }
    return std::nullopt;
  }

  static std::uint64_t chunkKey(int chunkX, int chunkZ) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunkX))
            << 32) |
           static_cast<std::uint32_t>(chunkZ);
  }

private:
  struct QueuedSection {
    std::int64_t minDistSq = 0;
    int chunkX = 0;
    int chunkZ = 0;
    std::uint8_t sectionIndex = 0;
    const PoiSection *section = nullptr;
  };

  int sectionIndexForY(int y) const noexcept { return (y - minY_) >> 4; }
  int sectionBaseY(int sectionIndex) const noexcept {
    return minY_ + sectionIndex * 16;
  }
  std::uint16_t localPosFor(PoiPos pos, int sectionIndex) const noexcept {
    const int lx = pos.x & 15;
    const int lz = pos.z & 15;
    const int ly = pos.y - sectionBaseY(sectionIndex);
    return static_cast<std::uint16_t>((ly << 8) | (lz << 4) | lx);
  }
  PoiPos worldPosFor(int chunkX, int chunkZ, int sectionIndex,
                     std::uint16_t localPos) const noexcept {
    return PoiPos{chunkX * 16 + static_cast<int>(localPos & 15u),
                  sectionBaseY(sectionIndex) +
                      static_cast<int>((localPos >> 8) & 15u),
                  chunkZ * 16 + static_cast<int>((localPos >> 4) & 15u)};
  }

  PoiChunkColumn &getOrCreateColumn(int chunkX, int chunkZ);
  void refreshColumnSectionMasks(PoiChunkColumn &col, int sectionIndex);
  PoiSection &getOrCreateSection(PoiChunkColumn &col, int sectionIndex,
                                 bool markValid = true);
  void enqueueColumnCandidateSections(int chunkX, int chunkZ,
                                      const PoiChunkColumn &col,
                                      std::uint32_t typeMask, PoiPos center,
                                      std::vector<QueuedSection> &queue) const;

  int minY_ = -64;
  int maxY_ = 320;
  int sectionCount_ = 24;
  FlatHashMap<std::uint64_t, PoiChunkColumn, ChunkKeyHash> columns_;
  mutable std::vector<QueuedSection> scratchSections_;
  mutable std::vector<PoiMatch> scratchMatches_;
};

} // namespace redstone
