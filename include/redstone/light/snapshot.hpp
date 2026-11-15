#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "redstone/light/optical.hpp"
#include "redstone/light/types.hpp"

namespace redstone {

static_assert(sizeof(std::atomic<std::uint8_t>) == 1,
              "published light state and page bytes must stay one byte");
static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
              "published light nibbles require lock-free atomic bytes");

// One independently owned light-layer descriptor. The state byte's high nibble
// is the uniform flag and its low nibble is the uniform value. Uniform and
// missing descriptors allocate no page. Mixed owns exactly one shared
// atomic<uint8_t>[2048] nibble page; its sole layer writer and all readers use
// relaxed atomic byte loads/stores. Topology changes replace the descriptor in
// a new section header, never its page pointer in place.
class LightLayerSnapshot {
public:
  enum class Storage : std::uint8_t { Missing, Uniform, Mixed };
  struct PackedPage {
    std::atomic<std::uint8_t> bytes[kLightLayerBytes];
  };
  static_assert(sizeof(PackedPage) == kLightLayerBytes);
  using PackedPagePtr = std::shared_ptr<PackedPage>;

  LightLayerSnapshot();
  LightLayerSnapshot(const LightLayerSnapshot &other);
  LightLayerSnapshot &operator=(const LightLayerSnapshot &other);
  LightLayerSnapshot(LightLayerSnapshot &&other) noexcept;
  LightLayerSnapshot &operator=(LightLayerSnapshot &&other) noexcept;
  ~LightLayerSnapshot();

  static LightLayerSnapshot missing();
  static LightLayerSnapshot uniform(std::uint8_t value);
  static LightLayerSnapshot mixed(
      const std::array<std::uint8_t, kLightVolume> &values);
  static LightLayerSnapshot mixedPacked(
      const std::array<std::uint8_t, kLightLayerBytes> &packed);
  // Allocates the single 2048-byte page for a worker-owned mixed layer.
  static LightLayerSnapshot mixedEmpty();

  Storage storage() const {
    const std::uint8_t state =
        state_.load(std::memory_order_acquire);
    if ((state & kLightUniformFlag) != 0)
      return Storage::Uniform;
    return packed_ ? Storage::Mixed : Storage::Missing;
  }
  bool isMissing() const { return storage() == Storage::Missing; }
  bool isUniform() const { return storage() == Storage::Uniform; }
  bool isMixed() const { return storage() == Storage::Mixed; }
  std::uint8_t stateByte() const {
    return state_.load(std::memory_order_relaxed);
  }
  const PackedPagePtr &packedPtr() const { return packed_; }
  PackedPagePtr takePackedPtr() && { return std::move(packed_); }
  std::uint8_t uniformValue() const {
    return static_cast<std::uint8_t>(
        stateByte() & static_cast<std::uint8_t>(~kLightUniformFlag & 15));
  }
  std::uint8_t get(int x, int y, int z) const;
  std::size_t packedByteCount() const {
    return isMixed() ? kLightLayerBytes : 0;
  }
  std::uint8_t packedByte(std::size_t index) const {
    if (index >= kLightLayerBytes || !packed_)
      return 0;
    return packed_->bytes[index].load(std::memory_order_relaxed);
  }
  // Only the layer's sole worker calls these writers. No CAS is required.
  void writeCell(std::size_t index, std::uint8_t value);
  void writePackedByte(std::size_t byte, std::uint8_t value);
  void readPacked(std::array<std::uint8_t, kLightLayerBytes> &out) const;
  bool operator==(const LightLayerSnapshot &other) const;

private:
  void makeMixed();

  std::atomic<std::uint8_t> state_{0};
  PackedPagePtr packed_;
};

// Flat value descriptor for a light layer. Missing and Uniform allocate zero
// heap objects; Mixed holds a single shared PackedPage (2048 atomic bytes)
// directly with no intermediate LightLayerSnapshot heap node.
class LightLayerRef {
public:
  LightLayerRef() = default;
  LightLayerRef(const LightLayerSnapshot &value)
      : state_(value.stateByte()), packed_(value.packedPtr()) {}
  LightLayerRef(LightLayerSnapshot &&value) noexcept
      : state_(value.stateByte()),
        packed_(std::move(value).takePackedPtr()) {}
  explicit LightLayerRef(LightLayerSnapshot::PackedPagePtr page) noexcept
      : state_(0), packed_(std::move(page)) {}

  LightLayerRef &operator=(const LightLayerSnapshot &value) {
    state_ = value.stateByte();
    packed_ = value.packedPtr();
    return *this;
  }
  LightLayerRef &operator=(LightLayerSnapshot &&value) noexcept {
    state_ = value.stateByte();
    packed_ = std::move(value).takePackedPtr();
    return *this;
  }
  LightLayerRef &operator=(LightLayerSnapshot::PackedPagePtr page) noexcept {
    state_ = 0;
    packed_ = std::move(page);
    return *this;
  }

  LightLayerSnapshot::Storage storage() const {
    if ((state_ & kLightUniformFlag) != 0)
      return LightLayerSnapshot::Storage::Uniform;
    return packed_ ? LightLayerSnapshot::Storage::Mixed
                   : LightLayerSnapshot::Storage::Missing;
  }
  bool isMissing() const {
    return (state_ & kLightUniformFlag) == 0 && !packed_;
  }
  bool isUniform() const { return (state_ & kLightUniformFlag) != 0; }
  bool isMixed() const {
    return (state_ & kLightUniformFlag) == 0 && packed_ != nullptr;
  }
  std::uint8_t stateByte() const { return state_; }
  std::uint8_t uniformValue() const {
    return isUniform()
               ? static_cast<std::uint8_t>(
                     state_ & static_cast<std::uint8_t>(~kLightUniformFlag & 15))
               : 0;
  }
  std::uint8_t get(int x, int y, int z) const {
    if ((state_ & kLightUniformFlag) != 0)
      return static_cast<std::uint8_t>(
          state_ & static_cast<std::uint8_t>(~kLightUniformFlag & 15));
    if (!packed_)
      return 0;
    const std::size_t index = lightIndex(x, y, z);
    const std::uint8_t packed =
        packed_->bytes[index >> 1].load(std::memory_order_relaxed);
    return static_cast<std::uint8_t>((packed >> ((index & 1) * 4)) & 15);
  }
  std::uint8_t packedByte(std::size_t index) const {
    if (index >= kLightLayerBytes || !packed_)
      return 0;
    return packed_->bytes[index].load(std::memory_order_relaxed);
  }
  void writeCell(std::size_t index, std::uint8_t value) const;
  void writePackedByte(std::size_t byte, std::uint8_t value) const;
  void readPacked(std::array<std::uint8_t, kLightLayerBytes> &out) const;
  const LightLayerSnapshot::PackedPagePtr &shared() const { return packed_; }
  bool samePayload(const LightLayerRef &other) const {
    return state_ == other.state_ && packed_ == other.packed_;
  }
  std::size_t packedByteCount() const {
    return isMixed() ? kLightLayerBytes : 0;
  }
  bool operator==(const LightLayerRef &other) const;
  bool operator!=(const LightLayerRef &other) const {
    return !(*this == other);
  }

private:
  std::uint8_t state_{0};
  LightLayerSnapshot::PackedPagePtr packed_;
};

// A section header has two independent layer slots. Production views populate
// only their own slot and leave the other Missing; optical direction masks are
// owned by the coordinator geometry rather than copied into light publication.
struct LightSectionSnapshot {
  explicit LightSectionSnapshot(
      int sectionY = 0, bool empty = true,
      std::array<LightLayerRef, kLightLayerCount> layers = {},
      std::uint16_t unsupportedCells = 0);

  int sectionY = 0;
  bool empty = true;
  std::array<LightLayerRef, kLightLayerCount> layers{};
  std::uint16_t unsupportedCells = 0;

  std::uint8_t light(LightLayer layer, int x, int y, int z) const {
    return layers[static_cast<std::size_t>(layer)].get(x, y, z);
  }
};

struct PersistedLightSection {
  int sectionY = 0;
  std::optional<std::array<std::uint8_t, 2048>> blockLight;
  std::optional<std::array<std::uint8_t, 2048>> skyLight;
};

struct PersistedChunkLight {
  bool isLightOn = false;
  std::vector<PersistedLightSection> sections;
};

struct LightChunkInput {
  int chunkX = 0;
  int chunkZ = 0;
  // Optional world-owned lifecycle token. Leave zero for a standalone service;
  // World supplies a token that changes on unload/reload so queued cell records
  // from the old lifetime cannot be applied to the new one.
  std::uint64_t lifecycleEpoch = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  // Chunk load input has exactly blockSectionCount entries converted directly
  // from World::Chunk::Section or test LightSectionView fixtures unless
  // directGeometry is set (in which case geometrySections is omitted).
  bool directGeometry = false;
  std::vector<GeometrySection> geometrySections;
  std::optional<PersistedChunkLight> persistedLight;

  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
};

struct LightChunkSnapshot {
  struct LayerChange {
    int sectionY = 0;
    bool sky = false;
    bool block = false;
  };
  int chunkX = 0;
  int chunkZ = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  // Latest input request that touched this chunk.
  std::uint64_t generation = 0;
  // Lifecycle rejects unload/reload results; revision distinguishes completed
  // results within one loaded lifetime and is ordered by the single writer,
  // independently of the unordered batch's input generation.
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t revision = 0;
  bool skyLightEnabled = true;
  std::optional<LightLayer> layer;
  bool blockLightComplete = false;
  bool skyLightComplete = false;
  bool propagationComplete = false;
  // Set only when every loaded chunk has complete optical metadata and the
  // layer converged. Protocol encoding never exposes an unpublished result.
  bool protocolReady = false;
  std::uint32_t unsupportedCells = 0;
  std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;
  // changedSections is a ready-to-ready delta. deltaBaseRevision identifies
  // the last protocol-ready snapshot it was computed from; incomplete drained
  // revisions do not advance that base.
  std::uint64_t deltaBaseRevision = 0;
  std::vector<LayerChange> changedSections;

  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
  const LightSectionSnapshot *section(int sectionY) const {
    const int index = sectionY - (minBlockSectionY - 1);
    if (index >= 0 && static_cast<std::size_t>(index) < sections.size()) {
      const auto &entry = sections[static_cast<std::size_t>(index)];
      if (entry && entry->sectionY == sectionY)
        return entry.get();
    }
    if (sections.empty() || !sections.front())
      return nullptr;
    const int fallback = sectionY - sections.front()->sectionY;
    if (fallback < 0 || static_cast<std::size_t>(fallback) >= sections.size())
      return nullptr;
    const auto &entry = sections[static_cast<std::size_t>(fallback)];
    return (entry && entry->sectionY == sectionY) ? entry.get() : nullptr;
  }
  // Sky queries follow SkyLightSectionStorage: a missing section takes the
  // bottom plane of the nearest stored section above it. Raw layer access stays
  // available through section().Missing sky data therefore differs from a
  // deliberately stored zero layer.
  int lightValue(LightLayer layer, int x, int y, int z) const;
};

} // namespace redstone
