#pragma once

#include "redstone/block.hpp"
#include "redstone/nbt.hpp"
#include "redstone/region.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace redstone::terrain {

enum class DecodeStatus {
  Ok,
  Missing,
  Corrupt,
  Unsupported,
};

struct PaletteEntry {
  std::string name;
  std::map<std::string, std::string> properties;
  BlockState state;
  bool supported = false;
};

struct PackedBlockStates {
  int bitsPerEntry = 0; // 0 for a single palette value with no data stream.
  std::vector<PaletteEntry> palette;
  std::vector<std::uint64_t> data;
  std::size_t cell(std::size_t index) const;
  static constexpr std::size_t CellCount = 4096;
};

struct Section {
  std::int8_t y = 0;
  std::optional<PackedBlockStates> blockStates;
  std::optional<std::array<std::uint8_t, 2048>> blockLight;
  std::optional<std::array<std::uint8_t, 2048>> skyLight;
};

struct SavedTick {
  std::string id;
  int x = 0, y = 0, z = 0;
  int delay = 0;
  int priority = 0;
};

struct MovingPistonEntity {
  int x = 0, y = 0, z = 0;
  BlockState movedState;
  std::uint8_t facing = 0;
  float progressO = 0.0f;
  bool extending = false;
  bool source = false;
};

struct DecodeOptions {
  // Fixture/debug callers may request the original DOM. Online managed loads
  // consume only the compact records and leave the root worker-owned.
  bool retainRawBlockEntities = false;
};

struct DecodedChunk {
  DecodeStatus status = DecodeStatus::Corrupt;
  std::string detail;
  std::int32_t chunkX = 0, chunkZ = 0;
  std::string statusName;
  bool isLightOn = false;
  std::vector<Section> sections;
  std::vector<SavedTick> blockTicks;
  std::vector<SavedTick> fluidTicks;
  std::vector<MovingPistonEntity> movingPistons;
  bool hasUnsupportedBlockEntities = false;
  std::string blockEntityError;
  std::vector<nbt::Tag> blockEntities;
  std::vector<nbt::Tag> entities;
  nbt::RootTag root;
};

DecodedChunk decodeChunkNbt(std::span<const std::uint8_t> nbtBytes,
                            DecodeOptions options = {});
DecodedChunk decodeChunkAt(const std::filesystem::path &regionDirectory,
                           int chunkX, int chunkZ,
                           DecodeOptions options = {});

struct BlockEdit {
  std::int8_t sectionY = 0;
  std::uint16_t localIndex = 0;
  BlockState state;
};

// Main-thread save transfer record. sectionY/localIndex select one cell in the
// owning chunk and stateId is the process-stable BlockState registry id. Eight
// bytes includes all fields and alignment padding.
struct CompactBlockEdit {
  std::int8_t sectionY = 0;
  std::uint16_t localIndex = 0;
  BlockState::Id stateId = 0;
};
static_assert(sizeof(CompactBlockEdit) == 8,
              "compact save edit must remain one 8-byte record");
inline std::uint32_t compactCellKey(const CompactBlockEdit &edit) {
  return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(edit.sectionY))
          << 12) |
         edit.localIndex;
}
inline CompactBlockEdit compactEdit(const BlockEdit &edit) {
  return {edit.sectionY, edit.localIndex, edit.state.id()};
}

struct PatchResult {
  bool edited = false;
  std::string error;
  std::size_t changedCells = 0;
  std::size_t repackedSections = 0;
};

// Mutates an owned, decoded chunk root in place. Edits are grouped by section,
// the last edit to a cell wins, and each affected section is repacked once.
// The root is never cloned. Structure and values representable by nbt::Tag are
// retained; byte-identical output is not promised for payloads the DOM cannot
// represent losslessly (for example, noncanonical IEEE NaN payloads).
// The root must have come from a successful decodeChunkNbt/decodeChunkAt.
PatchResult patchBlockStates(nbt::RootTag &root,
                             std::span<const BlockEdit> edits);
// Production save path. Resolves ids only while applying each selected cell and
// never materializes a BlockEdit vector.
PatchResult patchBlockStatesCompact(
    nbt::RootTag &root, std::span<const CompactBlockEdit> edits);

// Read-only validation using the same DataVersion/FULL, section palette,
// packed-storage, and supported-state rules as patchBlockStates/decode. It
// allocates no decoded section payload and does not broaden support.
bool validateChunkRootForSave(const nbt::RootTag &root, std::string &error);

struct TickListPatch {
  // nullopt leaves the corresponding saved list unchanged. An engaged empty
  // vector explicitly clears it.
  std::optional<std::vector<SavedTick>> blockTicks;
  std::optional<std::vector<SavedTick>> fluidTicks;
};
// Replaces only block_ticks/fluid_ticks in an owned chunk root. Every other NBT
// field remains untouched. Saved sub-tick order is never invented in NBT.
bool patchChunkTicks(nbt::RootTag &root, const TickListPatch &patch,
                     std::string &error);

// Replaces block_entities with the compact moving-piston list. An engaged list
// (including empty) is exact; other root fields are untouched. Validation is
// performed before the owned root is mutated.
bool patchMovingPistonEntities(
    nbt::RootTag &root,
    std::span<const MovingPistonEntity> entities,
    std::string &error);

struct SectionLightPatch {
  std::int8_t y = 0;
  std::optional<std::array<std::uint8_t, 2048>> blockLight;
  std::optional<std::array<std::uint8_t, 2048>> skyLight;
};

struct ChunkLightPatch {
  bool isLightOn = false;
  std::vector<SectionLightPatch> sections;
};

// Replaces isLightOn and per-section BlockLight/SkyLight nibble arrays per
// vanilla 26.1 SerializableChunkData rules (empty all-zero layers are omitted).
bool patchChunkLight(
    nbt::RootTag &root,
    const ChunkLightPatch &patch,
    std::string &error);

// Fixture compatibility only: this clones the complete root before editing.
// Production save paths must call patchBlockStates on their owned root.
struct EditResult {
  bool edited = false;
  std::string error;
  nbt::RootTag root;
};
EditResult setBlockState(const DecodedChunk &chunk, std::int8_t sectionY,
                         std::uint16_t localIndex, const BlockState &state);
// Moves the already-owned root through the compatibility result; no root copy.
EditResult setBlockState(DecodedChunk &&chunk, std::int8_t sectionY,
                         std::uint16_t localIndex, const BlockState &state);
std::vector<std::uint8_t> encodeChunkNbt(const nbt::RootTag &root);
std::vector<std::uint8_t> encodeChunkNbt(const DecodedChunk &chunk);

} // namespace redstone::terrain
