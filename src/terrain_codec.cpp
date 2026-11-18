#include "redstone/terrain_codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <bitset>
#include <limits>
#include <set>
#include <tuple>

namespace redstone::terrain {
namespace {

constexpr std::int32_t kDataVersion = 4786;
constexpr std::size_t kCells = 4096;
constexpr std::int32_t kMinChunkCoordinate = -1875000;
constexpr std::int32_t kMaxChunkCoordinate = 1874999;

const nbt::Tag *field(const nbt::Tag &root, std::string_view key) {
  return root.get(std::string(key));
}
std::optional<int> integer(const nbt::Tag &root, std::string_view key) {
  const nbt::Tag *value = field(root, key);
  if (!value || value->type != 3)
    return std::nullopt;
  return static_cast<int>(value->number);
}
std::string stringValue(const nbt::Tag &root, std::string_view key) {
  const nbt::Tag *value = field(root, key);
  return value && value->type == 8 ? value->text : std::string{};
}
bool parseSectionY(const nbt::Tag &section, std::int8_t &y) {
  const nbt::Tag *value = field(section, "Y");
  if (!value || value->type != 1)
    return false;
  y = static_cast<std::int8_t>(value->number);
  return true;
}
std::optional<std::size_t> packedCell(std::size_t bits,
                                      std::span<const std::uint64_t> data,
                                      std::size_t index) {
  if (!bits || bits > 32 || data.empty())
    return std::nullopt;
  const std::size_t perLong = 64 / bits;
  const std::size_t longIndex = index / perLong;
  if (longIndex >= data.size())
    return std::nullopt;
  const std::size_t offset = (index % perLong) * bits;
  if (offset + bits > 64)
    return std::nullopt;
  return static_cast<std::size_t>((data[longIndex] >> offset) &
                                  ((std::uint64_t{1} << bits) - 1));
}
bool parseBlockState(const nbt::Tag &tag, PaletteEntry &entry,
                     bool allowUnimplementedBlocks = false,
                     bool *resolved = nullptr) {
  if (resolved)
    *resolved = false;
  if (tag.type != 10)
    return false;
  entry.name = stringValue(tag, "Name");
  if (entry.name.empty())
    return false;
  entry.properties.clear();
  if (const nbt::Tag *properties = field(tag, "Properties")) {
    if (properties->type != 10)
      return false;
    for (const auto &[name, value] : properties->compound) {
      if (value.type != 8)
        return false;
      entry.properties[name] = value.text;
    }
  }
  const Block *block = findBlockByName(entry.name);
  // Registry identity and state schemas may be present before simulation is
  // implemented. Do not make such chunks writable as if behavior were complete.
  // TODO: provide a read-only representation for unsupported block behavior.
  if (!block || (!allowUnimplementedBlocks && !block->supported())) {
    entry.supported = false;
    entry.state = {};
    return true;
  }
  std::uint32_t variant = 0;
  if (!block->stateDefinition().variantOf(entry.properties, variant)) {
    entry.supported = false;
    entry.state = {};
    return true;
  }
  if (resolved)
    *resolved = true;
  entry.supported = block->supported();
  entry.state = BlockState::fromId(block->firstState() + variant);
  return true;
}
std::optional<PackedBlockStates>
parseBlockStates(const nbt::Tag &sectionTag, std::string &error,
                 bool &unsupportedBlock,
                 bool allowUnimplementedBlocks = false) {
  const nbt::Tag *container = field(sectionTag, "block_states");
  if (!container)
    return std::nullopt;
  if (container->type != 10) {
    error = "block_states is not a compound";
    return std::nullopt;
  }
  const nbt::Tag *palette = field(*container, "palette");
  if (!palette || palette->type != 9 ||
      palette->listElementType != 10 || palette->list.empty()) {
    error = "block_states palette is invalid";
    return std::nullopt;
  }
  PackedBlockStates result;
  if (palette->list.size() > 4096) {
    error = "block_states palette exceeds 4096 entries";
    return std::nullopt;
  }
  result.palette.reserve(palette->list.size());
  for (const nbt::Tag &entry : palette->list) {
    PaletteEntry decoded;
    bool resolved = false;
    if (!parseBlockState(entry, decoded, allowUnimplementedBlocks, &resolved)) {
      error = "block_states palette entry is invalid";
      return std::nullopt;
    }
    if (!resolved || (!allowUnimplementedBlocks && !decoded.supported)) {
      error = "unsupported block state: " + decoded.name;
      unsupportedBlock = true;
      return std::nullopt;
    }
    result.palette.push_back(std::move(decoded));
  }
  const nbt::Tag *storage = field(*container, "data");
  if (result.palette.size() == 1) {
    if (storage && (storage->type != 12 || !storage->longs.empty())) {
      error = "single palette has unexpected data";
      return std::nullopt;
    }
    result.bitsPerEntry = 0;
    return result;
  }
  if (!storage || storage->type != 12) {
    error = "multi palette has no data";
    return std::nullopt;
  }
  unsigned bits = std::max(
      4u, static_cast<unsigned>(std::bit_width(result.palette.size() - 1)));
  if (bits > 32) {
    error = "palette bit width is unsupported";
    return std::nullopt;
  }
  const std::size_t perLong = 64 / bits;
  if (storage->longs.size() != (kCells + perLong - 1) / perLong) {
    error = "palette data length is invalid";
    return std::nullopt;
  }
  result.bitsPerEntry = static_cast<int>(bits);
  result.data.assign(storage->longs.begin(), storage->longs.end());
  for (std::size_t i = 0; i < kCells; ++i) {
    const auto value = packedCell(bits, result.data, i);
    if (!value || *value >= result.palette.size()) {
      error = "palette index is out of range";
      return std::nullopt;
    }
  }
  return result;
}

constexpr std::size_t kLightNibbleBytes = 2048;

bool isValidChunkStatusName(std::string_view name) {
  static constexpr std::array<std::string_view, 12> kShortNames{
      "empty",
      "structure_starts",
      "structure_references",
      "biomes",
      "noise",
      "surface",
      "carvers",
      "features",
      "initialize_light",
      "light",
      "spawn",
      "full",
  };
  std::string_view stripped = name;
  if (stripped.rfind("minecraft:", 0) == 0)
    stripped.remove_prefix(10);
  for (std::string_view s : kShortNames) {
    if (stripped == s)
      return true;
  }
  return false;
}

std::optional<PackedBiomes>
parseBiomes(const nbt::Tag &sectionTag, std::string &error) {
  const nbt::Tag *container = field(sectionTag, "biomes");
  if (!container)
    return std::nullopt;
  if (container->type != 10) {
    error = "biomes is not a compound";
    return std::nullopt;
  }
  const nbt::Tag *palette = field(*container, "palette");
  if (!palette)
    return std::nullopt;
  if (palette->type != 9 || palette->list.empty()) {
    error = "biomes palette is invalid";
    return std::nullopt;
  }
  if (palette->list.size() > PackedBiomes::CellCount) {
    error = "biomes palette exceeds 64 entries";
    return std::nullopt;
  }
  PackedBiomes result;
  result.palette.reserve(palette->list.size());
  for (const nbt::Tag &entry : palette->list) {
    if (entry.type != 8 || entry.text.empty()) {
      error = "biomes palette entry is not a valid string";
      return std::nullopt;
    }
    result.palette.push_back(entry.text);
  }
  const nbt::Tag *storage = field(*container, "data");
  if (result.palette.size() == 1) {
    if (storage && (storage->type != 12 || !storage->longs.empty())) {
      error = "single biome palette has unexpected data";
      return std::nullopt;
    }
    result.bitsPerEntry = 0;
    return result;
  }
  if (!storage || storage->type != 12) {
    error = "multi biome palette has no data";
    return std::nullopt;
  }
  const unsigned bits =
      static_cast<unsigned>(std::bit_width(result.palette.size() - 1));
  const std::size_t perLong = 64 / bits;
  const std::size_t expectedLongs =
      (PackedBiomes::CellCount + perLong - 1) / perLong;
  if (storage->longs.size() != expectedLongs) {
    error = "biome palette data length is invalid";
    return std::nullopt;
  }
  result.bitsPerEntry = static_cast<int>(bits);
  result.data.assign(storage->longs.begin(), storage->longs.end());
  for (std::size_t i = 0; i < PackedBiomes::CellCount; ++i) {
    const auto value = packedCell(bits, result.data, i);
    if (!value || *value >= result.palette.size()) {
      error = "biome palette index is out of range";
      return std::nullopt;
    }
  }
  return result;
}

bool parseLightLayer(const nbt::Tag &sectionTag, std::string_view key,
                     std::optional<std::array<std::uint8_t, kLightNibbleBytes>> &out,
                     std::string &error) {
  out.reset();
  const nbt::Tag *tag = field(sectionTag, key);
  if (!tag)
    return true;
  if (tag->type != 7 || tag->bytes.size() != kLightNibbleBytes) {
    error = std::string(key) + " is not a 2048-byte array";
    return false;
  }
  auto &arr = out.emplace();
  for (std::size_t i = 0; i < kLightNibbleBytes; ++i)
    arr[i] = static_cast<std::uint8_t>(tag->bytes[i]);
  return true;
}

bool isAllZeroLight(const std::array<std::uint8_t, kLightNibbleBytes> &bytes) {
  for (std::uint8_t b : bytes) {
    if (b != 0)
      return false;
  }
  return true;
}

void setCompoundField(nbt::Tag &compound, std::string key, nbt::Tag value) {
  if (auto it = compound.compound.find(key); it != compound.compound.end()) {
    it->second = std::move(value);
    return;
  }
  compound.compoundOrder.push_back(key);
  compound.compound.emplace(std::move(key), std::move(value));
}

void eraseCompoundField(nbt::Tag &compound, std::string_view key) {
  auto erased = compound.compound.erase(std::string(key));
  if (erased > 0)
    std::erase(compound.compoundOrder, key);
}

nbt::Tag makeLightByteArrayTag(
    const std::array<std::uint8_t, kLightNibbleBytes> &bytes) {
  nbt::Tag tag;
  tag.type = 7;
  tag.bytes.resize(kLightNibbleBytes);
  for (std::size_t i = 0; i < kLightNibbleBytes; ++i)
    tag.bytes[i] = static_cast<std::int8_t>(bytes[i]);
  return tag;
}

bool parseMovingPistonEntity(const nbt::Tag &entry, int chunkX, int chunkZ,
                                MovingPistonEntity &out, bool &supported,
                                std::string &error) {
  supported = false;
  if (entry.type != 10) {
    error = "block entity is not a compound";
    return false;
  }
  const nbt::Tag *id = field(entry, "id");
  if (!id || id->type != 8)
    return false;
  if (id->text != "minecraft:piston")
    return true;

  supported = true;
  const nbt::Tag *x = field(entry, "x");
  const nbt::Tag *y = field(entry, "y");
  const nbt::Tag *z = field(entry, "z");
  const nbt::Tag *state = field(entry, "blockState");
  const nbt::Tag *facing = field(entry, "facing");
  const nbt::Tag *progress = field(entry, "progress");
  const nbt::Tag *extending = field(entry, "extending");
  const nbt::Tag *source = field(entry, "source");
  if (!x || x->type != 3 || !y || y->type != 3 || !z || z->type != 3 ||
      !state || state->type != 10 || !facing || facing->type != 1 ||
      !progress || progress->type != 5 || !extending ||
      extending->type != 1 || !source || source->type != 1) {
    error = "moving piston block entity is missing or has invalid fields";
    return false;
  }
  out.x = static_cast<int>(x->number);
  out.y = static_cast<int>(y->number);
  out.z = static_cast<int>(z->number);
  if ((out.x >> 4) != chunkX || (out.z >> 4) != chunkZ) {
    error = "moving piston block entity is outside its chunk";
    return false;
  }
  PaletteEntry parsed;
  if (!parseBlockState(*state, parsed) || !parsed.supported) {
    error = "moving piston movedState is unknown or unsupported";
    return false;
  }
  out.movedState = parsed.state;
  const int facingValue = static_cast<int>(facing->number);
  if (facingValue < 0 || facingValue > 5) {
    error = "moving piston facing is outside Direction legacy ids";
    return false;
  }
  out.facing = static_cast<std::uint8_t>(facingValue);
  if (!std::isfinite(progress->real)) {
    error = "moving piston progress is not finite";
    return false;
  }
  out.progressO = static_cast<float>(progress->real);
  if ((extending->number != 0 && extending->number != 1) ||
      (source->number != 0 && source->number != 1)) {
    error = "moving piston extending/source is not a boolean byte";
    return false;
  }
  out.extending = extending->number != 0;
  out.source = source->number != 0;
  if (const nbt::Tag *keepPacked = field(entry, "keepPacked")) {
    if (keepPacked->type != 1 || keepPacked->number != 0) {
      error = "keepPacked moving piston entities are unsupported";
      return false;
    }
  }
  if (const nbt::Tag *components = field(entry, "components")) {
    if (components->type != 10 || !components->compound.empty()) {
      error = "moving piston components are unsupported";
      return false;
    }
  }
  static const std::set<std::string> allowed{
      "id", "x", "y", "z", "keepPacked", "components",
      "blockState", "facing", "progress", "extending", "source"};
  for (const auto &[key, value] : entry.compound) {
    (void)value;
    if (!allowed.contains(key)) {
      error = "moving piston block entity has unknown field: " + key;
      return false;
    }
  }
  return true;
}

std::set<std::tuple<int, int, int>>
movingPistonTerrainCells(const std::vector<Section> &sections, int chunkX,
                         int chunkZ) {
  std::set<std::tuple<int, int, int>> result;
  for (const Section &section : sections) {
    if (!section.blockStates)
      continue;
    bool hasMoving = false;
    for (const PaletteEntry &entry : section.blockStates->palette)
      hasMoving = hasMoving || entry.name == "minecraft:moving_piston";
    if (!hasMoving)
      continue;
    for (std::size_t cell = 0; cell < PackedBlockStates::CellCount; ++cell) {
      const std::size_t value = section.blockStates->cell(cell);
      if (value >= section.blockStates->palette.size() ||
          section.blockStates->palette[value].name !=
              "minecraft:moving_piston")
        continue;
      result.emplace(
          static_cast<int>(static_cast<std::int64_t>(chunkX) * 16 +
                           static_cast<int>(cell & 15)),
          static_cast<int>(section.y) * 16 +
              static_cast<int>((cell >> 8) & 15),
          static_cast<int>(static_cast<std::int64_t>(chunkZ) * 16 +
                           static_cast<int>((cell >> 4) & 15)));
    }
  }
  return result;
}

std::vector<SavedTick> parseTickList(const nbt::RootTag &root,
                                     std::string_view key, std::string &error) {
  std::vector<SavedTick> result;
  const nbt::Tag *list = field(root.value, key);
  if (!list)
    return result;
  if (list->type != 9 ||
      (!list->list.empty() && list->listElementType != 10)) {
    error = std::string(key) + " is not a compound list";
    return {};
  }
  for (const nbt::Tag &tick : list->list) {
    const nbt::Tag *id = field(tick, "i");
    const nbt::Tag *x = field(tick, "x");
    const nbt::Tag *y = field(tick, "y");
    const nbt::Tag *z = field(tick, "z");
    const nbt::Tag *delay = field(tick, "t");
    const nbt::Tag *priority = field(tick, "p");
    if (!id || id->type != 8 || !x || x->type != 3 || !y || y->type != 3 ||
        !z || z->type != 3 || !delay || delay->type != 3 || !priority ||
        priority->type != 3) {
      error = std::string(key) + " contains an invalid tick";
      return {};
    }
    result.push_back(
        {id->text, static_cast<int>(x->number), static_cast<int>(y->number),
         static_cast<int>(z->number), static_cast<int>(delay->number),
         std::clamp(static_cast<int>(priority->number), -3, 3)});
  }
  return result;
}
nbt::Tag *mutableField(nbt::Tag &root, std::string_view key) {
  return root.get(std::string(key));
}
nbt::Tag *sectionAt(nbt::RootTag &root, std::int8_t y) {
  nbt::Tag *sections = mutableField(root.value, "sections");
  if (!sections || sections->type != 9)
    return nullptr;
  for (nbt::Tag &section : sections->list) {
    std::int8_t found = 0;
    if (parseSectionY(section, found) && found == y)
      return &section;
  }
  return nullptr;
}
struct SectionPatchGroup {
  nbt::Tag *section = nullptr;
  nbt::Tag *container = nullptr;
  nbt::Tag *palette = nullptr;
  nbt::Tag *data = nullptr;
  std::vector<std::size_t> editIndices;
  std::map<BlockState::Id, std::size_t> paletteIndex;
};

class EditView {
public:
  explicit EditView(std::span<const BlockEdit> edits) : expanded_(edits) {}
  explicit EditView(std::span<const CompactBlockEdit> edits)
      : compact_(edits) {}
  std::size_t size() const {
    return compact_.empty() ? expanded_.size() : compact_.size();
  }
  bool empty() const { return size() == 0; }
  std::int8_t sectionY(std::size_t i) const {
    return compact_.empty() ? expanded_[i].sectionY : compact_[i].sectionY;
  }
  std::uint16_t localIndex(std::size_t i) const {
    return compact_.empty() ? expanded_[i].localIndex
                            : compact_[i].localIndex;
  }
  BlockState state(std::size_t i) const {
    return compact_.empty() ? expanded_[i].state
                            : BlockState::fromId(compact_[i].stateId);
  }
  BlockState::Id stateId(std::size_t i) const {
    return compact_.empty() ? expanded_[i].state.id() : compact_[i].stateId;
  }
  bool compact() const { return !compact_.empty(); }
private:
  std::span<const BlockEdit> expanded_;
  std::span<const CompactBlockEdit> compact_;
};

unsigned bitsForPalette(std::size_t count) {
  return count <= 1
             ? 0
             : std::max(4u, static_cast<unsigned>(std::bit_width(count - 1)));
}

std::size_t longCountForBits(unsigned bits) {
  const std::size_t perLong = 64 / bits;
  return (kCells + perLong - 1) / perLong;
}

bool validateEditableRoot(const nbt::RootTag &root, std::string &error) {
  if (root.value.type != 10) {
    error = "chunk root is not a compound";
    return false;
  }
  const auto version = integer(root.value, "DataVersion");
  const auto x = integer(root.value, "xPos");
  const auto z = integer(root.value, "zPos");
  const std::string status = stringValue(root.value, "Status");
  if (!version || *version != kDataVersion || !x || !z ||
      *x < kMinChunkCoordinate || *x > kMaxChunkCoordinate ||
      *z < kMinChunkCoordinate || *z > kMaxChunkCoordinate ||
      (status != "minecraft:full" && status != "full")) {
    error = "root is not a validated in-range 4786 FULL chunk";
    return false;
  }
  const nbt::Tag *sections = field(root.value, "sections");
  if (!sections || sections->type != 9 ||
      sections->listElementType != 10) {
    error = "sections is not a compound list";
    return false;
  }
  return true;
}

nbt::Tag makePaletteEntry(const BlockState &state) {
  nbt::Tag entry;
  entry.type = 10;
  nbt::Tag name;
  name.type = 8;
  name.text = state.name();
  entry.compound.emplace("Name", std::move(name));
  entry.compoundOrder.emplace_back("Name");
  auto properties = state.properties();
  if (!properties.empty()) {
    nbt::Tag values;
    values.type = 10;
    for (auto &[key, value] : properties) {
      nbt::Tag tag;
      tag.type = 8;
      tag.text = std::move(value);
      values.compound.emplace(key, std::move(tag));
      values.compoundOrder.push_back(key);
    }
    entry.compound.emplace("Properties", std::move(values));
    entry.compoundOrder.emplace_back("Properties");
  }
  return entry;
}

std::vector<std::size_t>
selectedSectionEdits(const SectionPatchGroup &group, const EditView &edits) {
  std::vector<std::size_t> selected;
  std::bitset<kCells> seen;
  for (auto it = group.editIndices.rbegin(); it != group.editIndices.rend();
       ++it) {
    const std::uint16_t cell = edits.localIndex(*it);
    if (!seen.test(cell)) {
      seen.set(cell);
      selected.push_back(*it);
    }
  }
  std::reverse(selected.begin(), selected.end());
  return selected;
}

bool prepareSectionPatch(SectionPatchGroup &group, const EditView &edits,
                         std::string &error) {
  if (group.palette->list.empty() ||
      group.palette->list.size() > kCells) {
    error = "section block_states palette is invalid";
    return false;
  }
  group.paletteIndex.clear();
  for (std::size_t i = 0; i < group.palette->list.size(); ++i) {
    PaletteEntry parsed;
    // 世界生成区块可以包含“已注册但未实现模拟”的状态；补丁只需保留它们。
    // 未注册名称仍然拒绝。
    if (!parseBlockState(group.palette->list[i], parsed,
                         /*allowUnimplementedBlocks=*/true) ||
        findBlockByName(parsed.name) == nullptr) {
      error = "target section has an invalid or unknown palette state";
      return false;
    }
    group.paletteIndex.emplace(parsed.state.id(), i);
  }

  const unsigned oldBits =
      bitsForPalette(group.palette->list.size());
  if (oldBits) {
    if (!group.data || group.data->type != 12 ||
        group.data->longs.size() != longCountForBits(oldBits)) {
      error = "existing palette data length is invalid";
      return false;
    }
    const std::size_t perLong = 64 / oldBits;
    const std::uint64_t mask = (std::uint64_t{1} << oldBits) - 1;
    std::array<std::uint16_t, kCells> cells{};
    for (std::size_t i = 0; i < kCells; ++i) {
      const std::uint64_t word =
          static_cast<std::uint64_t>(group.data->longs[i / perLong]);
      const auto value = static_cast<std::size_t>(
          (word >> ((i % perLong) * oldBits)) & mask);
      if (value >= group.palette->list.size()) {
        error = "existing palette index is out of range";
        return false;
      }
      cells[i] = static_cast<std::uint16_t>(value);
    }
  } else if (group.data &&
             (group.data->type != 12 || !group.data->longs.empty())) {
    error = "single palette has unexpected data";
    return false;
  }

  std::set<BlockState::Id> missingStates;
  for (std::size_t editIndex : selectedSectionEdits(group, edits)) {
    const BlockState::Id id = edits.stateId(editIndex);
    if (!group.paletteIndex.contains(id))
      missingStates.insert(id);
  }
  if (group.palette->list.size() + missingStates.size() > kCells) {
    error = "block_states palette would exceed 4096 entries";
    return false;
  }
  return true;
}

bool applySectionPatch(SectionPatchGroup &group, const EditView &edits,
                       std::size_t &changedCells, std::string &error) {
  const unsigned oldBits =
      bitsForPalette(group.palette->list.size());
  std::array<std::uint16_t, kCells> cells{};
  if (oldBits) {
    const std::size_t perLong = 64 / oldBits;
    const std::uint64_t mask = (std::uint64_t{1} << oldBits) - 1;
    for (std::size_t i = 0; i < kCells; ++i) {
      const std::uint64_t word =
          static_cast<std::uint64_t>(group.data->longs[i / perLong]);
      const auto value = static_cast<std::size_t>(
          (word >> ((i % perLong) * oldBits)) & mask);
      if (value >= group.palette->list.size()) {
        error = "existing palette index is out of range";
        return false;
      }
      cells[i] = static_cast<std::uint16_t>(value);
    }
  }

  const std::vector<std::size_t> selected =
      selectedSectionEdits(group, edits);

  std::map<BlockState::Id, std::size_t> targetIndex = group.paletteIndex;
  std::vector<nbt::Tag> addedPalette;
  std::size_t changed = 0;
  for (std::size_t editIndex : selected) {
    const BlockState state = edits.state(editIndex);
    auto found = targetIndex.find(state.id());
    if (found == targetIndex.end()) {
      if (group.palette->list.size() + addedPalette.size() >= kCells) {
        error = "block_states palette would exceed 4096 entries";
        return false;
      }
      const std::size_t index =
          group.palette->list.size() + addedPalette.size();
      addedPalette.push_back(makePaletteEntry(state));
      found = targetIndex.emplace(state.id(), index).first;
    }
    const std::uint16_t cell = edits.localIndex(editIndex);
    if (cells[cell] != found->second) {
      cells[cell] = static_cast<std::uint16_t>(found->second);
      ++changed;
    }
  }
  if (changed == 0)
    return true;

  for (nbt::Tag &entry : addedPalette)
    group.palette->list.push_back(std::move(entry));

  const unsigned bits = bitsForPalette(group.palette->list.size());
  if (bits == 0) {
    if (group.data) {
      group.data->type = 12;
      group.data->longs.clear();
    }
    changedCells += changed;
    return true;
  }
  if (!group.data) {
    nbt::Tag fresh;
    fresh.type = 12;
    group.container->compound.emplace("data", std::move(fresh));
    group.container->compoundOrder.emplace_back("data");
    group.data = mutableField(*group.container, "data");
  }
  group.data->type = 12;
  group.data->longs.assign(longCountForBits(bits), 0);
  const std::size_t perLong = 64 / bits;
  const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
  for (std::size_t i = 0; i < kCells; ++i) {
    auto &word = group.data->longs[i / perLong];
    const std::size_t offset = (i % perLong) * bits;
    word = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(word) & ~(mask << offset)) |
        (static_cast<std::uint64_t>(cells[i]) << offset));
  }
  changedCells += changed;
  return true;
}

bool validateSectionsReadOnly(const nbt::Tag &root, std::string &error,
                              bool allowRegisteredUnsupported) {
  const nbt::Tag *sections = field(root, "sections");
  if (!sections || sections->type != 9 ||
      sections->listElementType != 10) {
    error = "sections is not a compound list";
    return false;
  }
  std::set<int> sectionYs;
  for (const nbt::Tag &section : sections->list) {
    std::int8_t y = 0;
    if (!parseSectionY(section, y)) {
      error = "section Y is missing";
      return false;
    }
    if (!sectionYs.insert(y).second) {
      error = "duplicate section Y";
      return false;
    }
    std::optional<std::array<std::uint8_t, kLightNibbleBytes>> dummyLight;
    if (!parseLightLayer(section, "BlockLight", dummyLight, error) ||
        !parseLightLayer(section, "SkyLight", dummyLight, error)) {
      return false;
    }
    const nbt::Tag *container = field(section, "block_states");
    if (!container)
      continue;
    if (container->type != 10) {
      error = "block_states is not a compound";
      return false;
    }
    const nbt::Tag *palette = field(*container, "palette");
    if (!palette || palette->type != 9 ||
        palette->listElementType != 10 || palette->list.empty()) {
      error = "block_states palette is invalid";
      return false;
    }
    if (palette->list.size() > kCells) {
      error = "block_states palette exceeds 4096 entries";
      return false;
    }
    for (const nbt::Tag &entry : palette->list) {
      PaletteEntry parsed;
      if (!parseBlockState(entry, parsed, allowRegisteredUnsupported)) {
        error = "block_states palette entry is invalid";
        return false;
      }
      if (allowRegisteredUnsupported) {
        // 只接受已注册名称；未注册方块仍然拒绝。
        if (findBlockByName(parsed.name) == nullptr) {
          error = "unknown block state: " + parsed.name;
          return false;
        }
      } else if (!parsed.supported) {
        error = "unsupported block state: " + parsed.name;
        return false;
      }
    }
    const nbt::Tag *data = field(*container, "data");
    if (palette->list.size() == 1) {
      if (data && (data->type != 12 || !data->longs.empty())) {
        error = "single palette has unexpected data";
        return false;
      }
      continue;
    }
    if (!data || data->type != 12) {
      error = "multi palette has no data";
      return false;
    }
    const unsigned bits = bitsForPalette(palette->list.size());
    if (bits > 32 ||
        data->longs.size() != longCountForBits(bits)) {
      error = "palette data length is invalid";
      return false;
    }
    const std::size_t perLong = 64 / bits;
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    for (std::size_t i = 0; i < kCells; ++i) {
      const std::uint64_t word =
          static_cast<std::uint64_t>(data->longs[i / perLong]);
      const auto value = static_cast<std::size_t>(
          (word >> ((i % perLong) * bits)) & mask);
      if (value >= palette->list.size()) {
        error = "palette index is out of range";
        return false;
      }
    }
  }
  return true;
}

} // namespace

std::size_t PackedBlockStates::cell(std::size_t index) const {
  if (bitsPerEntry == 0)
    return 0;
  const std::size_t bits = static_cast<std::size_t>(bitsPerEntry);
  const std::size_t perLong = 64 / bits;
  if (index >= CellCount || index / perLong >= data.size())
    return std::numeric_limits<std::size_t>::max();
  return static_cast<std::size_t>((data[index / perLong] >>
                                   ((index % perLong) * bits)) &
                                  ((std::uint64_t{1} << bits) - 1));
}

std::size_t PackedBiomes::cell(std::size_t index) const {
  if (bitsPerEntry == 0)
    return 0;
  const std::size_t bits = static_cast<std::size_t>(bitsPerEntry);
  const std::size_t perLong = 64 / bits;
  if (index >= CellCount || index / perLong >= data.size())
    return std::numeric_limits<std::size_t>::max();
  return static_cast<std::size_t>((data[index / perLong] >>
                                   ((index % perLong) * bits)) &
                                  ((std::uint64_t{1} << bits) - 1));
}

DecodedChunk decodeChunkNbt(std::span<const std::uint8_t> bytes,
                            DecodeOptions options) {
  DecodedChunk result;
  try {
    result.root = nbt::parseRoot(bytes);
  } catch (const std::exception &error) {
    result.status = DecodeStatus::Corrupt;
    result.detail = error.what();
    return result;
  }
  const nbt::Tag &root = result.root.value;
  const auto version = integer(root, "DataVersion");
  if (!version || *version != kDataVersion) {
    result.status = DecodeStatus::Unsupported;
    result.detail = "chunk DataVersion is not 4786";
    return result;
  }
  const auto x = integer(root, "xPos");
  const auto z = integer(root, "zPos");
  if (!x || !z) {
    result.status = DecodeStatus::Corrupt;
    result.detail = "chunk xPos/zPos missing";
    return result;
  }
  result.chunkX = *x;
  result.chunkZ = *z;
  if (result.chunkX < kMinChunkCoordinate ||
      result.chunkX > kMaxChunkCoordinate ||
      result.chunkZ < kMinChunkCoordinate ||
      result.chunkZ > kMaxChunkCoordinate) {
    result.status = DecodeStatus::Unsupported;
    result.detail = "chunk xPos/zPos outside supported world bounds";
    return result;
  }
  result.statusName = stringValue(root, "Status");
  if (options.requireFullStatus) {
    if (result.statusName != "minecraft:full" && result.statusName != "full") {
      result.status = DecodeStatus::Unsupported;
      result.detail = "chunk is not full";
      return result;
    }
  } else if (!isValidChunkStatusName(result.statusName)) {
    result.status = DecodeStatus::Unsupported;
    result.detail = "chunk Status is unknown: " + result.statusName;
    return result;
  }
  if (const nbt::Tag *inhabited = field(root, "InhabitedTime")) {
    if (inhabited->type != 4) {
      result.status = DecodeStatus::Corrupt;
      result.detail = "InhabitedTime is not a long";
      return result;
    }
    result.inhabitedTime = inhabited->number;
  }
  if (const nbt::Tag *lightOn = field(root, "isLightOn")) {
    if (lightOn->type != 1) {
      result.status = DecodeStatus::Corrupt;
      result.detail = "isLightOn is not a byte";
      return result;
    }
    result.isLightOn = lightOn->number != 0;
  }
  std::string error;
  const nbt::Tag *sections = field(root, "sections");
  if (!sections || sections->type != 9 ||
      ((options.requireFullStatus || !sections->list.empty()) &&
       sections->listElementType != 10)) {
    result.status = DecodeStatus::Corrupt;
    result.detail = "sections is not a compound list";
    return result;
  }
  std::set<int> sectionYs;
  for (const nbt::Tag &sectionTag : sections->list) {
    std::int8_t y = 0;
    if (!parseSectionY(sectionTag, y)) {
      result.status = DecodeStatus::Corrupt;
      result.detail = "section Y is missing";
      return result;
    }
    if (!sectionYs.insert(y).second) {
      result.status = DecodeStatus::Corrupt;
      result.detail = "duplicate section Y";
      return result;
    }
    Section section;
    section.y = y;
    bool unsupportedBlock = false;
    section.blockStates = parseBlockStates(
        sectionTag, error, unsupportedBlock, options.allowUnimplementedBlocks);
    if (!error.empty()) {
      result.status = unsupportedBlock ? DecodeStatus::Unsupported
                                       : DecodeStatus::Corrupt;
      result.detail = error;
      return result;
    }
    section.biomes = parseBiomes(sectionTag, error);
    if (!error.empty()) {
      result.status = DecodeStatus::Corrupt;
      result.detail = error;
      return result;
    }
    if (!parseLightLayer(sectionTag, "BlockLight", section.blockLight, error) ||
        !parseLightLayer(sectionTag, "SkyLight", section.skyLight, error)) {
      result.status = DecodeStatus::Corrupt;
      result.detail = error;
      return result;
    }
    result.sections.push_back(std::move(section));
  }
  if (const nbt::Tag *carving = field(root, "carving_mask")) {
    if (carving->type != 12) {
      result.status = DecodeStatus::Corrupt;
      result.detail = "carving_mask is not a long array";
      return result;
    }
    result.carvingMask.reserve(carving->longs.size());
    for (std::int64_t v : carving->longs)
      result.carvingMask.push_back(static_cast<std::uint64_t>(v));
  }
  if (const nbt::Tag *postProc = field(root, "PostProcessing")) {
    if (postProc->type != 9) {
      result.status = DecodeStatus::Corrupt;
      result.detail = "PostProcessing is not a list";
      return result;
    }
    result.postProcessing.resize(postProc->list.size());
    for (std::size_t i = 0; i < postProc->list.size(); ++i) {
      const nbt::Tag &secTag = postProc->list[i];
      if (secTag.type != 9) {
        result.status = DecodeStatus::Corrupt;
        result.detail = "PostProcessing section entry is not a list";
        return result;
      }
      auto &dst = result.postProcessing[i];
      dst.reserve(secTag.list.size());
      for (const nbt::Tag &offsetTag : secTag.list) {
        if (offsetTag.type != 2) {
          result.status = DecodeStatus::Corrupt;
          result.detail = "PostProcessing offset entry is not a short";
          return result;
        }
        dst.push_back(static_cast<std::int16_t>(offsetTag.number));
      }
    }
  }
  if (const nbt::Tag *heightmaps = field(root, "Heightmaps")) {
    if (heightmaps->type != 10) {
      result.status = DecodeStatus::Corrupt;
      result.detail = "Heightmaps is not a compound";
      return result;
    }
    static constexpr std::array<HeightmapType, 6> kAllHeightmapTypes{
        HeightmapType::WorldSurfaceWG, HeightmapType::WorldSurface,
        HeightmapType::OceanFloorWG,   HeightmapType::OceanFloor,
        HeightmapType::MotionBlocking, HeightmapType::MotionBlockingNoLeaves,
    };
    for (HeightmapType hmType : kAllHeightmapTypes) {
      const nbt::Tag *hmTag =
          field(*heightmaps, heightmapSerializationKey(hmType));
      if (!hmTag)
        continue;
      if (hmTag->type != 12) {
        result.status = DecodeStatus::Corrupt;
        result.detail = "Heightmap entry is not a long array";
        return result;
      }
      std::vector<std::uint64_t> words;
      words.reserve(hmTag->longs.size());
      for (std::int64_t v : hmTag->longs)
        words.push_back(static_cast<std::uint64_t>(v));
      result.heightmaps.emplace(hmType, std::move(words));
    }
  }
  result.blockTicks = parseTickList(result.root, "block_ticks", error);
  if (!error.empty()) {
    result.status = DecodeStatus::Corrupt;
    result.detail = error;
    return result;
  }
  result.fluidTicks = parseTickList(result.root, "fluid_ticks", error);
  if (!error.empty()) {
    result.status = DecodeStatus::Corrupt;
    result.detail = error;
    return result;
  }
  if (const nbt::Tag *blockEntities = field(root, "block_entities")) {
    if (blockEntities->type != 9 ||
        (!blockEntities->list.empty() &&
         blockEntities->listElementType != 10)) {
      result.status = DecodeStatus::Corrupt;
      result.detail = "block_entities is invalid";
      return result;
    }
    std::set<std::tuple<int, int, int>> entityPositions;
    for (const nbt::Tag &entry : blockEntities->list) {
      MovingPistonEntity piston;
      bool supported = false;
      std::string blockEntityError;
      if (!parseMovingPistonEntity(entry, result.chunkX, result.chunkZ,
                                   piston, supported, blockEntityError)) {
        result.hasUnsupportedBlockEntities = true;
        result.blockEntityError = blockEntityError.empty()
                                      ? "invalid block entity"
                                      : std::move(blockEntityError);
        break;
      }
      if (!supported) {
        result.hasUnsupportedBlockEntities = true;
        result.blockEntityError = "unsupported persisted block entities";
        break;
      }
      if (!entityPositions.emplace(piston.x, piston.y, piston.z).second) {
        result.hasUnsupportedBlockEntities = true;
        result.blockEntityError =
            "duplicate moving piston block entity position";
        break;
      }
      result.movingPistons.push_back(piston);
    }
    if (!result.hasUnsupportedBlockEntities) {
      const auto cells =
          movingPistonTerrainCells(result.sections, result.chunkX, result.chunkZ);
      if (cells != entityPositions) {
        result.hasUnsupportedBlockEntities = true;
        result.blockEntityError =
            "moving_piston terrain cells and block entities do not match";
      }
    }
    if (options.retainRawBlockEntities)
      result.blockEntities.assign(blockEntities->list.begin(),
                                  blockEntities->list.end());
  } else {
    const auto cells =
        movingPistonTerrainCells(result.sections, result.chunkX, result.chunkZ);
    if (!cells.empty()) {
      result.hasUnsupportedBlockEntities = true;
      result.blockEntityError =
          "moving_piston terrain cells have no block entity";
    }
  }
  if (const nbt::Tag *entities = field(root, "entities")) {
    if (entities->type != 9 ||
        (!entities->list.empty() && entities->listElementType != 10)) {
      result.status = DecodeStatus::Corrupt;
      result.detail = "entities is invalid";
      return result;
    }
    result.entities.assign(entities->list.begin(), entities->list.end());
  }
  result.status = DecodeStatus::Ok;
  return result;
}

DecodedChunk decodeChunkAt(const std::filesystem::path &regionDirectory,
                           int chunkX, int chunkZ, DecodeOptions options) {
  const region::ReadResult read =
      region::readChunkNbt(regionDirectory, chunkX, chunkZ);
  if (!read) {
    DecodedChunk result;
    result.status = read.status == region::ReadStatus::Missing
                        ? DecodeStatus::Missing
                        : read.status == region::ReadStatus::Unsupported
                              ? DecodeStatus::Unsupported
                              : DecodeStatus::Corrupt;
    result.detail = read.detail;
    return result;
  }
  DecodedChunk result = decodeChunkNbt(read.nbt, options);
  if (result.status == DecodeStatus::Ok &&
      (result.chunkX != chunkX || result.chunkZ != chunkZ)) {
    result.status = DecodeStatus::Corrupt;
    result.detail = "chunk NBT coordinates do not match requested slot";
  }
  return result;
}

bool validateChunkRootForSave(const nbt::RootTag &root, std::string &error,
                              bool allowRegisteredUnsupported) {
  error.clear();
  return validateEditableRoot(root, error) &&
         validateSectionsReadOnly(root.value, error,
                                  allowRegisteredUnsupported);
}

namespace {
nbt::Tag savedTickTag(const SavedTick &tick) {
  nbt::Tag entry;
  entry.type = 10;
  auto add = [&](std::string key, nbt::Tag value) {
    entry.compoundOrder.push_back(key);
    entry.compound.emplace(std::move(key), std::move(value));
  };
  nbt::Tag id;
  id.type = 8;
  id.text = tick.id;
  add("i", std::move(id));
  auto integerTag = [](std::int64_t value) {
    nbt::Tag tag;
    tag.type = 3;
    tag.number = value;
    return tag;
  };
  add("x", integerTag(tick.x));
  add("y", integerTag(tick.y));
  add("z", integerTag(tick.z));
  add("t", integerTag(tick.delay));
  add("p", integerTag(std::clamp(tick.priority, -3, 3)));
  return entry;
}

void replaceRootList(nbt::RootTag &root, std::string_view key,
                     const std::vector<SavedTick> &ticks) {
  nbt::Tag replacement;
  replacement.type = 9;
  replacement.listElementType = 10;
  replacement.list.reserve(ticks.size());
  for (const SavedTick &tick : ticks)
    replacement.list.push_back(savedTickTag(tick));
  const std::string name(key);
  auto found = root.value.compound.find(name);
  if (found == root.value.compound.end()) {
    root.value.compoundOrder.push_back(name);
    root.value.compound.emplace(name, std::move(replacement));
  } else {
    found->second = std::move(replacement);
  }
}
} // namespace

bool patchChunkTicks(nbt::RootTag &root, const TickListPatch &patch,
                     std::string &error) {
  error.clear();
  if (root.value.type != 10) {
    error = "chunk root is not a compound";
    return false;
  }
  for (const auto *ticks : {patch.blockTicks ? &*patch.blockTicks
                                            : nullptr,
                            patch.fluidTicks ? &*patch.fluidTicks : nullptr}) {
    if (!ticks)
      continue;
    for (const SavedTick &tick : *ticks) {
      if (tick.id.empty()) {
        error = "saved tick has an empty id";
        return false;
      }
    }
  }
  if (patch.blockTicks)
    replaceRootList(root, "block_ticks", *patch.blockTicks);
  if (patch.fluidTicks)
    replaceRootList(root, "fluid_ticks", *patch.fluidTicks);
  return true;
}

bool patchMovingPistonEntities(
    nbt::RootTag &root, std::span<const MovingPistonEntity> entities,
    std::string &error) {
  error.clear();
  if (!validateEditableRoot(root, error))
    return false;
  const int chunkX = integer(root.value, "xPos").value();
  const int chunkZ = integer(root.value, "zPos").value();
  std::set<std::tuple<int, int, int>> expected;
  for (const MovingPistonEntity &entity : entities) {
    if ((entity.x >> 4) != chunkX || (entity.z >> 4) != chunkZ) {
      error = "moving piston entity is outside its chunk";
      return false;
    }
    if (entity.facing > 5 || !std::isfinite(entity.progressO)) {
      error = "moving piston entity has invalid facing/progress";
      return false;
    }
    if (!isSupportedBlock(entity.movedState.name()) ||
        !entity.movedState.isSchemaState()) {
      error = "moving piston movedState is unsupported";
      return false;
    }
    if (!expected.emplace(entity.x, entity.y, entity.z).second) {
      error = "duplicate moving piston entity position";
      return false;
    }
  }

  std::set<std::tuple<int, int, int>> terrainPositions;
  const nbt::Tag *sections = field(root.value, "sections");
  for (const nbt::Tag &section : sections->list) {
    const nbt::Tag *states = field(section, "block_states");
    if (!states)
      continue;
    const nbt::Tag *palette = field(*states, "palette");
    const nbt::Tag *data = field(*states, "data");
    if (!palette || palette->type != 9 || palette->list.empty())
      continue;
    bool movingInPalette = false;
    for (const nbt::Tag &entry : palette->list)
      movingInPalette =
          movingInPalette || stringValue(entry, "Name") == "minecraft:moving_piston";
    if (!movingInPalette)
      continue;
    std::int8_t sectionY = 0;
    if (!parseSectionY(section, sectionY)) {
      error = "moving_piston section Y is invalid";
      return false;
    }
    const unsigned bits =
        palette->list.size() <= 1
            ? 0
            : std::max(4u, static_cast<unsigned>(
                               std::bit_width(palette->list.size() - 1)));
    const std::size_t perLong = bits == 0 ? 1 : 64 / bits;
    if (bits != 0 && (!data || data->type != 12 ||
                      data->longs.size() != (kCells + perLong - 1) / perLong)) {
      error = "moving_piston section data is invalid";
      return false;
    }
    for (std::size_t cell = 0; cell < kCells; ++cell) {
      const std::size_t value =
          bits == 0 ? 0
                    : static_cast<std::size_t>(
                          (static_cast<std::uint64_t>(data->longs[cell / perLong]) >>
                           ((cell % perLong) * bits)) &
                          ((std::uint64_t{1} << bits) - 1));
      if (value >= palette->list.size()) {
        error = "moving_piston palette index is out of range";
        return false;
      }
      if (stringValue(palette->list[value], "Name") !=
          "minecraft:moving_piston")
        continue;
      const std::int64_t blockX =
          static_cast<std::int64_t>(chunkX) * 16 +
          static_cast<int>(cell & 15);
      const std::int64_t blockZ =
          static_cast<std::int64_t>(chunkZ) * 16 +
          static_cast<int>((cell >> 4) & 15);
      if (blockX < std::numeric_limits<int>::min() ||
          blockX > std::numeric_limits<int>::max() ||
          blockZ < std::numeric_limits<int>::min() ||
          blockZ > std::numeric_limits<int>::max()) {
        error = "moving_piston coordinate overflows int";
        return false;
      }
      terrainPositions.emplace(
          static_cast<int>(blockX),
          sectionY * 16 + static_cast<int>((cell >> 8) & 15),
          static_cast<int>(blockZ));
    }
  }
  if (terrainPositions != expected) {
    error = "moving_piston terrain cells and block entities do not match";
    return false;
  }

  nbt::Tag replacement;
  replacement.type = 9;
  replacement.listElementType = 10;
  replacement.list.reserve(entities.size());
  for (const MovingPistonEntity &entity : entities) {
    nbt::Tag entry;
    entry.type = 10;
    auto add = [&](std::string key, nbt::Tag value) {
      entry.compoundOrder.push_back(key);
      entry.compound.emplace(std::move(key), std::move(value));
    };
    auto integerTag = [](std::int64_t value) {
      nbt::Tag tag;
      tag.type = 3;
      tag.number = value;
      return tag;
    };
    auto byteTag = [](std::int8_t value) {
      nbt::Tag tag;
      tag.type = 1;
      tag.number = value;
      return tag;
    };
    nbt::Tag id;
    id.type = 8;
    id.text = "minecraft:piston";
    add("id", std::move(id));
    add("x", integerTag(entity.x));
    add("y", integerTag(entity.y));
    add("z", integerTag(entity.z));
    add("keepPacked", byteTag(0));
    nbt::Tag components;
    components.type = 10;
    add("components", std::move(components));
    add("blockState", makePaletteEntry(entity.movedState));
    add("facing", byteTag(static_cast<std::int8_t>(entity.facing)));
    nbt::Tag progress;
    progress.type = 5;
    progress.real = entity.progressO;
    add("progress", std::move(progress));
    add("extending", byteTag(entity.extending ? 1 : 0));
    add("source", byteTag(entity.source ? 1 : 0));
    replacement.list.push_back(std::move(entry));
  }
  auto found = root.value.compound.find("block_entities");
  if (found == root.value.compound.end()) {
    root.value.compoundOrder.push_back("block_entities");
    root.value.compound.emplace("block_entities", std::move(replacement));
  } else {
    found->second = std::move(replacement);
  }
  return true;
}

bool patchChunkLight(nbt::RootTag &root, const ChunkLightPatch &patch,
                     std::string &error) {
  error.clear();
  if (!validateEditableRoot(root, error))
    return false;
  std::set<std::int8_t> seenPatchYs;
  for (const SectionLightPatch &entry : patch.sections) {
    if (!seenPatchYs.insert(entry.y).second) {
      error = "duplicate light section Y";
      return false;
    }
  }
  nbt::Tag *sections = mutableField(root.value, "sections");
  std::map<std::int8_t, std::size_t> existingIndex;
  for (std::size_t i = 0; i < sections->list.size(); ++i) {
    std::int8_t y = 0;
    if (!parseSectionY(sections->list[i], y)) {
      error = "section Y is missing";
      return false;
    }
    if (!existingIndex.emplace(y, i).second) {
      error = "duplicate section Y";
      return false;
    }
  }

  nbt::Tag lightOnTag;
  lightOnTag.type = 1;
  lightOnTag.number = patch.isLightOn ? 1 : 0;
  setCompoundField(root.value, "isLightOn", std::move(lightOnTag));

  if (!patch.isLightOn) {
    for (nbt::Tag &sec : sections->list) {
      eraseCompoundField(sec, "BlockLight");
      eraseCompoundField(sec, "SkyLight");
    }
  } else {
    std::map<std::int8_t, const SectionLightPatch *> patchByY;
    for (const SectionLightPatch &entry : patch.sections)
      patchByY.emplace(entry.y, &entry);

    for (nbt::Tag &sec : sections->list) {
      std::int8_t y = 0;
      parseSectionY(sec, y);
      auto it = patchByY.find(y);
      if (it == patchByY.end()) {
        eraseCompoundField(sec, "BlockLight");
        eraseCompoundField(sec, "SkyLight");
        continue;
      }
      const SectionLightPatch &entry = *it->second;
      if (entry.blockLight && !isAllZeroLight(*entry.blockLight)) {
        setCompoundField(sec, "BlockLight",
                         makeLightByteArrayTag(*entry.blockLight));
      } else {
        eraseCompoundField(sec, "BlockLight");
      }
      if (entry.skyLight && !isAllZeroLight(*entry.skyLight)) {
        setCompoundField(sec, "SkyLight",
                         makeLightByteArrayTag(*entry.skyLight));
      } else {
        eraseCompoundField(sec, "SkyLight");
      }
    }

    for (const SectionLightPatch &entry : patch.sections) {
      if (existingIndex.contains(entry.y))
        continue;
      const bool hasBlock =
          entry.blockLight && !isAllZeroLight(*entry.blockLight);
      const bool hasSky = entry.skyLight && !isAllZeroLight(*entry.skyLight);
      if (!hasBlock && !hasSky)
        continue;
      nbt::Tag sec;
      sec.type = 10;
      nbt::Tag yTag;
      yTag.type = 1;
      yTag.number = entry.y;
      setCompoundField(sec, "Y", std::move(yTag));
      if (hasBlock) {
        setCompoundField(sec, "BlockLight",
                         makeLightByteArrayTag(*entry.blockLight));
      }
      if (hasSky) {
        setCompoundField(sec, "SkyLight",
                         makeLightByteArrayTag(*entry.skyLight));
      }
      auto insertPos = sections->list.begin();
      while (insertPos != sections->list.end()) {
        std::int8_t curY = 0;
        if (parseSectionY(*insertPos, curY) && curY > entry.y)
          break;
        ++insertPos;
      }
      sections->list.insert(insertPos, std::move(sec));
    }
  }

  // Prune empty light-only sections (only have "Y") so repeated saves stay tidy.
  std::erase_if(sections->list, [](const nbt::Tag &sec) {
    return sec.type == 10 && sec.compound.size() == 1 &&
           sec.compound.contains("Y");
  });
  return true;
}

PatchResult patchBlockStatesImpl(nbt::RootTag &root,
                                 const EditView &edits) {
  PatchResult result;
  if (edits.empty())
    return result;
  if (!validateEditableRoot(root, result.error))
    return result;

  std::vector<SectionPatchGroup> groups;
  for (std::size_t i = 0; i < edits.size(); ++i) {
    const std::uint16_t localIndex = edits.localIndex(i);
    if (localIndex >= kCells) {
      result.error = "local block index is out of range";
      return result;
    }
    const BlockState state = edits.state(i);
    if (edits.compact() && state.id() != edits.stateId(i)) {
      result.error = "unknown block state id";
      return result;
    }
    if (!isSupportedBlock(state.name()) || !state.isSchemaState()) {
      result.error = "unknown or unsupported block state";
      return result;
    }
    nbt::Tag *section = sectionAt(root, edits.sectionY(i));
    if (!section || section->type != 10) {
      result.error = "section Y is missing";
      return result;
    }
    auto group = std::find_if(
        groups.begin(), groups.end(),
        [section](const SectionPatchGroup &candidate) {
          return candidate.section == section;
        });
    if (group == groups.end()) {
      nbt::Tag *container = mutableField(*section, "block_states");
      if (!container || container->type != 10) {
        result.error = "section has no block_states";
        return result;
      }
      nbt::Tag *palette = mutableField(*container, "palette");
      if (!palette || palette->type != 9 ||
          palette->listElementType != 10 || palette->list.empty()) {
        result.error = "section block_states palette is invalid";
        return result;
      }
      SectionPatchGroup prepared;
      prepared.section = section;
      prepared.container = container;
      prepared.palette = palette;
      prepared.data = mutableField(*container, "data");
      groups.push_back(std::move(prepared));
      group = std::prev(groups.end());
    }
    group->editIndices.push_back(i);
  }

  // Validate every target section before touching the owned root. Preparation
  // decodes indices into a bounded scratch array but does not retain or clone
  // any section payload.
  for (SectionPatchGroup &group : groups)
    if (!prepareSectionPatch(group, edits, result.error))
      return result;

  for (SectionPatchGroup &group : groups) {
    const std::size_t before = result.changedCells;
    if (!applySectionPatch(group, edits, result.changedCells, result.error))
      return result;
    if (result.changedCells != before)
      ++result.repackedSections;
  }
  if (result.changedCells == 0)
    return result;

  result.edited = true;
  if (nbt::Tag *light = mutableField(root.value, "isLightOn"))
    light->type = 1, light->number = 0;
  if (nbt::Tag *heightmaps = mutableField(root.value, "Heightmaps"))
    heightmaps->compound.clear();
  return result;
}

PatchResult patchBlockStates(nbt::RootTag &root,
                             std::span<const BlockEdit> edits) {
  return patchBlockStatesImpl(root, EditView(edits));
}

PatchResult patchBlockStatesCompact(
    nbt::RootTag &root, std::span<const CompactBlockEdit> edits) {
  return patchBlockStatesImpl(root, EditView(edits));
}

EditResult setBlockState(const DecodedChunk &chunk, std::int8_t sectionY,
                         std::uint16_t localIndex, const BlockState &state) {
  EditResult result;
  if (chunk.status != DecodeStatus::Ok) {
    result.error = "chunk is not a validated FULL chunk";
    return result;
  }
  result.root = chunk.root;
  const BlockEdit edit{sectionY, localIndex, state};
  PatchResult patch = patchBlockStates(result.root,
                                       std::span<const BlockEdit>(&edit, 1));
  result.edited = patch.edited;
  result.error = std::move(patch.error);
  return result;
}

EditResult setBlockState(DecodedChunk &&chunk, std::int8_t sectionY,
                         std::uint16_t localIndex, const BlockState &state) {
  EditResult result;
  if (chunk.status != DecodeStatus::Ok) {
    result.error = "chunk is not a validated FULL chunk";
    return result;
  }
  result.root = std::move(chunk.root);
  const BlockEdit edit{sectionY, localIndex, state};
  PatchResult patch = patchBlockStates(result.root,
                                       std::span<const BlockEdit>(&edit, 1));
  result.edited = patch.edited;
  result.error = std::move(patch.error);
  return result;
}

std::vector<std::uint8_t> encodeChunkNbt(const nbt::RootTag &root) {
  return nbt::writeRoot(root);
}
std::vector<std::uint8_t> encodeChunkNbt(const DecodedChunk &chunk) {
  return encodeChunkNbt(chunk.root);
}

} // namespace redstone::terrain
