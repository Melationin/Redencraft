#include "redstone/engine.hpp"
#include <algorithm>
#include <deque>
#include <string_view>
#include <utility>

namespace redstone {
std::string canonicalName(std::string n) {
  if (n.find(':') == std::string::npos)
    n = "minecraft:" + n;
  return n;
}

namespace {
// Imported properties outside a block's StateDefinition are kept as extension
// states so their values survive serialization (vanilla rejects them instead).
struct Extension {
  std::map<std::string, std::string> properties;
};
} // namespace
} // namespace redstone

namespace redstone::detail {
// Block/StateDefinition registry. States are stored in registration order, so a
// registry id indexes the table directly.
struct Tables {
  std::vector<const Block *> blocks;
  std::vector<BlockState> states;
  std::unordered_map<std::string_view, const Block *> byName;
  std::deque<Block> owned; // blocks registered after startup (imported names)
  std::unordered_map<BlockState::Id, Extension> extensions;
};

Tables &tables() {
  static Tables table;
  return table;
}

namespace {
// Zeroed BlockState {nullptr, 0}: fromId can clamp to index 0 before any block
// has registered, and the registry table is never a null pointer.
const std::uint64_t kNullState[2] = {0, 0};
} // namespace

const BlockState *gStateData =
    reinterpret_cast<const BlockState *>(kNullState);
std::uint32_t gStateCount = 0;
const Block *gAirBlock = nullptr;
} // namespace redstone::detail

namespace redstone {
const Block &airBlock() {
  static const Block *air = &blockByName("minecraft:air");
  return *air;
}

// ---------------------------------------------------------------------------
// BlockRef: a Block object handle. Copying is a pointer copy and identity is the
// address of the Block singleton, like ScheduledTick<Block> in vanilla.
// ---------------------------------------------------------------------------
const std::string &BlockRef::str() const {
  static const std::string empty;
  return block_ ? block_->name() : empty;
}
BlockRef::BlockRef(std::string_view name) {
  if (!name.empty())
    block_ = &blockByName(name);
}

// ---------------------------------------------------------------------------
// Canonical property kinds and values (StateDefinition's enum path)
// ---------------------------------------------------------------------------
namespace {
template <std::size_t N>
bool lookupName(const std::string_view (&names)[N], std::string_view value,
                std::uint8_t &out) {
  for (std::size_t i = 0; i < N; ++i)
    if (names[i] == value) {
      out = static_cast<std::uint8_t>(i);
      return true;
    }
  return false;
}
bool parseSmallNumber(std::string_view text, std::uint8_t &out) {
  if (text.empty() || text.size() > 3)
    return false;
  unsigned value = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  out = static_cast<std::uint8_t>(value);
  return true;
}
} // namespace

bool propertyKindFromName(std::string_view name, PropertyKind &out) {
  // Order must match PropertyKind.
  static constexpr std::string_view names[] = {
      "facing",   "type",     "shape",      "instrument", "note",
      "level",    "extended", "in_wall",    "lit",        "open",
      "powered",  "short",    "snowy",      "unstable",   "up",
      "waterlogged", "east",  "north",      "south",      "west",
      "axis", "stage", "distance", "persistent", "triggered", "part", "occupied", "half",
      "age", "attached", "attachment", "berries", "bites", "bloom", "bottom",
      "can_summon", "candles", "charges", "conditional", "copper_golem_pose",
      "cracked", "crafting", "creaking_heart_state", "delay", "disarmed", "down",
      "drag", "dusted", "eggs", "enabled", "eye", "face", "flower_amount",
      "hanging", "has_book", "has_bottle_0", "has_bottle_1", "has_bottle_2",
      "has_record", "hatch", "hinge", "honey_level", "hydration", "inverted",
      "layers", "leaves", "locked", "mode", "moisture", "natural", "ominous",
      "orientation", "pickles", "power", "rotation", "sculk_sensor_phase",
      "segment_amount", "shrieking", "side_chain", "signal_fire",
      "slot_0_occupied", "slot_1_occupied", "slot_2_occupied", "slot_3_occupied",
      "slot_4_occupied", "slot_5_occupied", "thickness", "tilt", "tip",
      "trial_spawner_state", "vault_state", "vertical_direction"};
  static_assert(std::size(names) == kPropertyKindCount);
  for (std::size_t i = 0; i < std::size(names); ++i)
    if (names[i] == name) {
      out = static_cast<PropertyKind>(i);
      return true;
    }
  return false;
}

bool propertyValueFromName(PropertyKind kind, std::string_view value,
                           std::uint8_t &out) {
  switch (kind) {
  case PropertyKind::Facing: {
    static constexpr std::string_view names[] = {"down", "up",   "north",
                                                 "south", "west", "east"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Type: {
    static constexpr std::string_view names[] = {
        "normal", "sticky", "top", "bottom", "double", "single", "left", "right"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Axis: {
    // RotatedPillarBlock.AXIS.
    static constexpr std::string_view names[] = {"x", "y", "z"};
    return lookupName(names, value, out);
  }
  case PropertyKind::BedPart: {
    static constexpr std::string_view names[] = {"head", "foot"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Half: {
    static constexpr std::string_view names[] = {"upper", "lower", "top", "bottom"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Shape: {
    static constexpr std::string_view names[] = {
        "north_south",     "east_west",       "ascending_east",
        "ascending_west",  "ascending_north", "ascending_south",
        "south_east",      "south_west",      "north_west",
        "north_east",      "straight",        "inner_left",
        "inner_right",     "outer_left",      "outer_right"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Instrument: {
    static constexpr std::string_view names[] = {
        "harp",  "basedrum", "snare",        "hat",     "bass",
        "flute", "bell",     "guitar",       "chime",   "xylophone",
        "iron_xylophone",    "cow_bell",     "didgeridoo",
        "bit",               "banjo",        "pling",
        "trumpet", "trumpet_exposed", "trumpet_oxidized", "trumpet_weathered",
        "zombie", "skeleton", "creeper", "dragon", "wither_skeleton", "piglin", "custom_head"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Note:
    return parseSmallNumber(value, out) && out <= 24;
  case PropertyKind::Age:
    return parseSmallNumber(value, out) && out <= 25;
  case PropertyKind::Level:
  case PropertyKind::Power:
  case PropertyKind::Rotation:
    return parseSmallNumber(value, out) && out <= 15;
  case PropertyKind::Stage:
    return parseSmallNumber(value, out) && out <= 1;
  case PropertyKind::Distance:
  case PropertyKind::Moisture:
    return parseSmallNumber(value, out) && out <= 7;
  case PropertyKind::Bites:
    return parseSmallNumber(value, out) && out <= 6;
  case PropertyKind::HoneyLevel:
    return parseSmallNumber(value, out) && out <= 5;
  case PropertyKind::Charges:
    return parseSmallNumber(value, out) && out <= 4;
  case PropertyKind::Dusted:
  case PropertyKind::Hydration:
    return parseSmallNumber(value, out) && out <= 3;
  case PropertyKind::Hatch:
    return parseSmallNumber(value, out) && out <= 2;
  case PropertyKind::Layers:
    return parseSmallNumber(value, out) && out >= 1 && out <= 8;
  case PropertyKind::Candles:
  case PropertyKind::Delay:
  case PropertyKind::Eggs:
  case PropertyKind::FlowerAmount:
  case PropertyKind::Pickles:
  case PropertyKind::SegmentAmount:
    return parseSmallNumber(value, out) && out >= 1 && out <= 4;
  case PropertyKind::WallEast:
  case PropertyKind::WallNorth:
  case PropertyKind::WallSouth:
  case PropertyKind::WallWest: {
    static constexpr std::string_view names[] = {
        "none", "low", "tall", "false", "true", "side", "up"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Attachment: {
    static constexpr std::string_view names[] = {
        "floor", "ceiling", "single_wall", "double_wall"};
    return lookupName(names, value, out);
  }
  case PropertyKind::CopperGolemPose: {
    static constexpr std::string_view names[] = {
        "standing", "sitting", "running", "star"};
    return lookupName(names, value, out);
  }
  case PropertyKind::CreakingHeartState: {
    static constexpr std::string_view names[] = {
        "uprooted", "dormant", "awake"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Face: {
    static constexpr std::string_view names[] = {"floor", "wall", "ceiling"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Hinge: {
    static constexpr std::string_view names[] = {"left", "right"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Leaves: {
    static constexpr std::string_view names[] = {"none", "small", "large"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Mode: {
    static constexpr std::string_view names[] = {
        "compare", "subtract", "save", "load", "corner",
        "data", "start", "log", "fail", "accept"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Orientation: {
    static constexpr std::string_view names[] = {
        "down_east", "down_north", "down_south", "down_west",
        "up_east", "up_north", "up_south", "up_west",
        "west_up", "east_up", "north_up", "south_up"};
    return lookupName(names, value, out);
  }
  case PropertyKind::SculkSensorPhase: {
    static constexpr std::string_view names[] = {
        "inactive", "active", "cooldown"};
    return lookupName(names, value, out);
  }
  case PropertyKind::SideChain: {
    static constexpr std::string_view names[] = {
        "unconnected", "right", "center", "left"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Thickness: {
    static constexpr std::string_view names[] = {
        "tip_merge", "tip", "frustum", "middle", "base"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Tilt: {
    static constexpr std::string_view names[] = {
        "none", "unstable", "partial", "full"};
    return lookupName(names, value, out);
  }
  case PropertyKind::TrialSpawnerState: {
    static constexpr std::string_view names[] = {
        "inactive", "waiting_for_players", "active",
        "waiting_for_reward_ejection", "ejecting_reward", "cooldown"};
    return lookupName(names, value, out);
  }
  case PropertyKind::VaultState: {
    static constexpr std::string_view names[] = {
        "inactive", "active", "unlocking", "ejecting"};
    return lookupName(names, value, out);
  }
  case PropertyKind::VerticalDirection: {
    static constexpr std::string_view names[] = {"up", "down"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Extended:
  case PropertyKind::InWall:
  case PropertyKind::Lit:
  case PropertyKind::Open:
  case PropertyKind::Powered:
  case PropertyKind::Short:
  case PropertyKind::Snowy:
  case PropertyKind::Unstable:
  case PropertyKind::Up:
  case PropertyKind::Waterlogged:
  case PropertyKind::Persistent:
  case PropertyKind::Occupied:
  case PropertyKind::Triggered:
  case PropertyKind::Attached:
  case PropertyKind::Berries:
  case PropertyKind::Bloom:
  case PropertyKind::Bottom:
  case PropertyKind::CanSummon:
  case PropertyKind::Conditional:
  case PropertyKind::Cracked:
  case PropertyKind::Crafting:
  case PropertyKind::Disarmed:
  case PropertyKind::Down:
  case PropertyKind::Drag:
  case PropertyKind::Enabled:
  case PropertyKind::Eye:
  case PropertyKind::Hanging:
  case PropertyKind::HasBook:
  case PropertyKind::HasBottle0:
  case PropertyKind::HasBottle1:
  case PropertyKind::HasBottle2:
  case PropertyKind::HasRecord:
  case PropertyKind::Inverted:
  case PropertyKind::Locked:
  case PropertyKind::Natural:
  case PropertyKind::Ominous:
  case PropertyKind::Shrieking:
  case PropertyKind::SignalFire:
  case PropertyKind::Slot0Occupied:
  case PropertyKind::Slot1Occupied:
  case PropertyKind::Slot2Occupied:
  case PropertyKind::Slot3Occupied:
  case PropertyKind::Slot4Occupied:
  case PropertyKind::Slot5Occupied:
  case PropertyKind::Tip: {
    static constexpr std::string_view names[] = {"false", "true"};
    return lookupName(names, value, out);
  }
  case PropertyKind::Count:
    break;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Property / StateDefinition (net/minecraft/world/level/block/state)
// ---------------------------------------------------------------------------
Property::Property(std::string name, std::vector<std::string> values,
                   std::string defaultValue)
    : name_(std::move(name)), values_(std::move(values)) {
  if (values_.size() > kMaxPropertyValue + 1)
    throw std::invalid_argument("block property has too many values: " +
                                name_);
  if (!propertyKindFromName(name_, kind_))
    throw std::invalid_argument("unknown block property: " + name_);
  valueByLocal_.fill(0);
  localByValue_.fill(kAbsent);
  for (std::size_t i = 0; i < values_.size(); ++i) {
    valueIndex_.emplace(values_[i], static_cast<std::uint32_t>(i));
    std::uint8_t canonical = 0;
    if (!propertyValueFromName(kind_, values_[i], canonical))
      throw std::invalid_argument("unknown value '" + values_[i] +
                                  "' for block property " + name_);
    valueByLocal_[i] = canonical;
    localByValue_[canonical] = static_cast<std::uint8_t>(i);
  }
  const auto found = valueIndex_.find(defaultValue);
  defaultIndex_ = found == valueIndex_.end() ? 0 : found->second;
}

bool Property::indexOf(std::string_view value, std::uint32_t &index) const {
  const auto found = valueIndex_.find(value);
  if (found == valueIndex_.end())
    return false;
  index = found->second;
  return true;
}

StateDefinition::StateDefinition(Properties properties)
    : properties_(std::move(properties)) {
  std::sort(properties_.begin(), properties_.end(),
            [](const Property &a, const Property &b) {
              return a.name() < b.name();
            });
  const std::size_t count = properties_.size();
  stride_.assign(count, 1);
  for (std::size_t i = count; i-- > 1;)
    stride_[i - 1] =
        stride_[i] *
        static_cast<std::uint32_t>(properties_[i].values().size());
  stateCount_ =
      count ? stride_[0] *
                  static_cast<std::uint32_t>(properties_[0].values().size())
            : 1;
  std::uint32_t variant = 0;
  for (std::size_t i = 0; i < count; ++i)
    variant += properties_[i].defaultIndex() * stride_[i];
  defaultVariant_ = variant;
  // Canonical bit fields: property i occupies `width` bits at `shift`, where
  // width covers the largest canonical value this block accepts. The dense
  // variant stays independent (it is the registry id's base), so packed() and
  // variant() are two encodings of the same combination.
  std::uint32_t shift = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const Property &property = properties_[i];
    PropertyLayout &slot = layout_[static_cast<std::size_t>(property.kind())];
    if (slot.property)
      throw std::invalid_argument("duplicate property kind in one definition");
    std::uint8_t maxValue = 0;
    for (std::size_t v = 0; v < property.values().size(); ++v)
      maxValue = std::max(
          maxValue,
          property.canonicalValue(static_cast<std::uint32_t>(v)));
    std::uint8_t width = 0;
    while ((1u << width) <= maxValue)
      ++width;
    slot.property = &property;
    slot.shift = static_cast<std::uint8_t>(shift);
    slot.width = width;
    slot.count = static_cast<std::uint8_t>(property.values().size());
    slot.stride = stride_[i];
    slot.mask = (1u << width) - 1u;
    shift += width;
  }
  if (shift > 32)
    throw std::invalid_argument("block state needs more than 32 packed bits");
}

std::uint32_t StateDefinition::packedOf(std::uint32_t variant) const {
  std::uint32_t packed = 0;
  for (std::size_t i = 0; i < properties_.size(); ++i) {
    const Property &property = properties_[i];
    const PropertyLayout &slot =
        layout_[static_cast<std::size_t>(property.kind())];
    const std::uint32_t local =
        (variant / stride_[i]) %
        static_cast<std::uint32_t>(property.values().size());
    packed |= static_cast<std::uint32_t>(property.canonicalValue(local))
              << slot.shift;
  }
  return packed;
}

bool StateDefinition::withPacked(std::uint32_t packed, std::uint32_t variant,
                                 PropertyKind kind, std::uint8_t value,
                                 std::uint32_t &nextVariant,
                                 std::uint32_t &nextPacked) const {
  const PropertyLayout &slot = layout_[static_cast<std::size_t>(kind)];
  if (!slot.property || value > kMaxPropertyValue)
    return false;
  const std::uint8_t local = slot.property->localIndex(value);
  if (local == Property::kAbsent)
    return false;
  const std::uint32_t current = (packed >> slot.shift) & slot.mask;
  if (current == value) {
    nextVariant = variant;
    nextPacked = packed;
    return true;
  }
  const std::uint8_t currentLocal =
      slot.property->localIndex(static_cast<std::uint32_t>(current));
  // Unsigned wraparound is exact here: the result is the dense variant of the
  // target combination.
  nextVariant = variant + (static_cast<std::uint32_t>(local) -
                           static_cast<std::uint32_t>(currentLocal)) *
                              slot.stride;
  nextPacked = packed ^ ((current ^ value) << slot.shift);
  return true;
}

const Property *StateDefinition::find(std::string_view name) const {
  const auto it = std::lower_bound(
      properties_.begin(), properties_.end(), name,
      [](const Property &property, std::string_view key) {
        return property.name() < key;
      });
  return it != properties_.end() && it->name() == name ? &*it : nullptr;
}

bool StateDefinition::variantOf(
    const std::map<std::string, std::string> &values,
    std::uint32_t &variant) const {
  // A supplied property that is not part of the definition cannot be
  // represented; it must not silently take a default.
  for (const auto &[key, value] : values) {
    (void)value;
    if (!find(key))
      return false;
  }
  variant = 0;
  for (std::size_t i = 0; i < properties_.size(); ++i) {
    const auto &property = properties_[i];
    const auto found = values.find(property.name());
    std::uint32_t index = 0;
    if (!property.indexOf(found == values.end()
                              ? property.values()[property.defaultIndex()]
                              : found->second,
                          index))
      return false;
    variant += index * stride_[i];
  }
  return true;
}

bool StateDefinition::with(std::uint32_t variant, std::string_view key,
                           std::string_view value,
                           std::uint32_t &result) const {
  const Property *property = find(key);
  if (!property)
    return false;
  std::uint32_t index = 0;
  if (!property->indexOf(value, index))
    return false;
  const auto i = static_cast<std::size_t>(property - properties_.data());
  const auto count = static_cast<std::uint32_t>(property->values().size());
  const auto current = (variant / stride_[i]) % count;
  if (current == index) {
    result = variant;
    return true;
  }
  result = variant + (index - current) * stride_[i];
  return true;
}

std::string_view StateDefinition::value(std::uint32_t variant,
                                        std::string_view key,
                                        std::string_view fallback) const {
  const Property *property = find(key);
  if (!property)
    return fallback;
  const auto i = static_cast<std::size_t>(property - properties_.data());
  const auto &values = property->values();
  return values[(variant / stride_[i]) % values.size()];
}

std::map<std::string, std::string>
StateDefinition::values(std::uint32_t variant) const {
  std::map<std::string, std::string> result;
  for (std::size_t i = 0; i < properties_.size(); ++i) {
    const auto &property = properties_[i];
    const auto &values = property.values();
    result.emplace(property.name(),
                   values[(variant / stride_[i]) % values.size()]);
  }
  return result;
}

Block::Block(std::string_view name, BlockMetadata metadata,
             StateDefinition definition)
    : name_(canonicalName(std::string(name))), metadata_(std::move(metadata)),
      definition_(std::move(definition)) {
  detail::registerBlock(*this);
}

// ---------------------------------------------------------------------------
// Block registration (Block constructor + Block.BLOCK_STATE_REGISTRY)
// ---------------------------------------------------------------------------
namespace detail {
void registerBlock(Block &block) {
  Tables &table = tables();
  const std::string_view name = block.name_;
  const auto endsWith = [&](std::string_view suffix) {
    return name.size() >= suffix.size() &&
           name.substr(name.size() - suffix.size()) == suffix;
  };
  // Block class stand-ins: vanilla resolves these with instanceof at runtime.
  if (name == "minecraft:air")
    gAirBlock = &block;
  block.airLike_ = block.hasClassFlag(BlockClassFlag::AirLike) ||
                   name == "minecraft:air" || name == "minecraft:cave_air" ||
                   name == "minecraft:void_air";
  block.rail_ = block.hasClassFlag(BlockClassFlag::Rail);
  block.liquid_ = block.hasClassFlag(BlockClassFlag::Liquid);
  block.carpet_ = block.hasClassFlag(BlockClassFlag::Carpet);
  block.wall_ = block.hasClassFlag(BlockClassFlag::Wall);
  block.coralWallFan_ = block.hasClassFlag(BlockClassFlag::CoralWallFan);
  block.coralFan_ = block.hasClassFlag(BlockClassFlag::CoralFan);
  (void)endsWith;
  block.index_ = static_cast<std::uint32_t>(table.blocks.size());
  block.firstState_ = gStateCount;
  block.stateCount_ = block.definition_.stateCount();
  block.defaultStateId_ = block.firstState_ + block.definition_.defaultVariant();
  block.lightProperties_.reserve(block.stateCount_);
  for (std::uint32_t variant = 0; variant < block.stateCount_; ++variant) {
    std::string properties;
    for (const Property &property : block.definition_.properties()) {
      if (!properties.empty())
        properties.push_back(',');
      properties += property.name();
      properties.push_back('=');
      properties += block.definition_.value(variant, property.name());
    }
    block.lightProperties_.push_back(
        officialLightProperties(block.name_, properties));
  }
  table.blocks.push_back(&block);
  table.byName.emplace(std::string_view(block.name_), &block);
  for (std::uint32_t variant = 0; variant < block.stateCount_; ++variant)
    table.states.push_back(BlockState(&block, block.firstState_ + variant,
                                      block.definition_.packedOf(variant)));
  gStateData = table.states.data();
  gStateCount = static_cast<std::uint32_t>(table.states.size());
  bindBlockBehaviours(block);
  bindRandomTickBehaviour(block);
}

std::uint32_t internState(const Block *block) {
  Tables &table = tables();
  const auto id = gStateCount;
  table.states.push_back(BlockState(block, id));
  gStateData = table.states.data();
  gStateCount = static_cast<std::uint32_t>(table.states.size());
  return id;
}
} // namespace detail

const BlockLightProperties &Block::lightProperties(
    std::uint32_t variant) const {
  return variant < lightProperties_.size() ? lightProperties_[variant]
                                            : unsupportedLightProperties();
}

const BlockLightProperties &lightPropertiesOf(const BlockState &state) {
  return state.block().lightProperties(state.variant());
}

const Block *findBlockByName(std::string_view name) {
  auto &table = detail::tables();
  const auto found = table.byName.find(name);
  if (found != table.byName.end())
    return found->second;
  if (name.find(':') == std::string_view::npos) {
    std::string canonical = "minecraft:";
    canonical.append(name);
    const auto prefixed = table.byName.find(canonical);
    if (prefixed != table.byName.end())
      return prefixed->second;
  }
  return nullptr;
}

const Block &blockByName(std::string_view name) {
  if (const Block *found = findBlockByName(name))
    return *found;
  auto &table = detail::tables();
  // A Block singleton for a name outside the registry (save/litematic import).
  table.owned.emplace_back(std::string(name),
                           BlockMetadata{0, false, "unsupported", false},
                           StateDefinition{});
  return table.owned.back();
}

// ---------------------------------------------------------------------------
// BlockState: a registered state owned by one Block. Copying is two words,
// equality is registry id equality, and state.is(Blocks.X) is a pointer test.
// ---------------------------------------------------------------------------
BlockState::BlockState(std::string name) {
  *this = blockByName(name).defaultState();
}

BlockState::BlockState(std::string name,
                       std::map<std::string, std::string> properties) {
  const Block &owner = blockByName(name);
  std::uint32_t variant = 0;
  if (owner.stateDefinition().variantOf(properties, variant)) {
    *this = BlockState::fromId(owner.firstState() + variant);
    return;
  }
  const auto id = detail::internState(&owner);
  *this = BlockState(&owner, id);
  detail::tables().extensions.emplace(id, Extension{std::move(properties)});
}

std::string_view BlockState::extensionValue(std::string_view key,
                                            std::string_view fallback) const {
  auto &table = detail::tables();
  const auto record = table.extensions.find(id_);
  if (record == table.extensions.end())
    return fallback;
  const auto found = record->second.properties.find(std::string(key));
  return found == record->second.properties.end()
             ? fallback
             : std::string_view(found->second);
}

bool BlockState::extensionHas(std::string_view key) const {
  auto &table = detail::tables();
  const auto record = table.extensions.find(id_);
  return record != table.extensions.end() &&
         record->second.properties.contains(std::string(key));
}

std::map<std::string, std::string> BlockState::extensionProperties() const {
  auto &table = detail::tables();
  const auto record = table.extensions.find(id_);
  return record == table.extensions.end() ? std::map<std::string, std::string>{}
                                          : record->second.properties;
}

BlockState BlockState::with(std::string_view key,
                            std::string_view value) const {
  if (!block_)
    return *this;
  if (isSchemaState()) {
    std::uint32_t next = 0;
    if (block_->stateDefinition().with(variant(), key, value, next))
      return BlockState::fromId(block_->firstState() + next);
  }
  // A state change is only allowed inside the block's own StateDefinition. The
  // registry no longer invents out-of-schema states: a property or value the
  // block does not declare is a programming error, exactly like vanilla's
  // IllegalArgumentException from StateHolder.setValue.
  std::string message = "unknown block property '";
  message.append(key).append("'='").append(value).append("' for ");
  message.append(block_->name());
  throw std::invalid_argument(message);
}

BlockState BlockState::with(PropertyKind kind, std::uint8_t value) const {
  if (!block_)
    return *this;
  std::uint32_t nextVariant = 0;
  std::uint32_t nextPacked = 0;
  if (!isSchemaState() ||
      !block_->stateDefinition().withPacked(packed_, variant(), kind, value,
                                            nextVariant, nextPacked)) {
    std::string message = "property not valid for ";
    message.append(block_->name());
    throw std::invalid_argument(message);
  }
  if (nextVariant == variant())
    return *this;
  return BlockState(block_, block_->firstState() + nextVariant, nextPacked);
}

std::vector<BlockState> Block::states() const {
  std::vector<BlockState> result;
  result.reserve(stateCount_);
  for (std::uint32_t variant = 0; variant < stateCount_; ++variant)
    result.push_back(BlockState::fromId(firstState_ + variant));
  return result;
}

// ---------------------------------------------------------------------------
// Registry queries.
// ---------------------------------------------------------------------------
bool stateNamed(const BlockState &state, std::string_view shortName) {
  return state.shortName() == shortName;
}

bool stateIsAir(const BlockState &state) { return state.isAir(); }

BlockState defaultState(const std::string &name) {
  return blockByName(name).defaultState();
}

bool isSupportedBlock(const std::string &name) {
  const Block *block = findBlockByName(name);
  return block && block->supported();
}

std::size_t registeredStateCount() { return detail::gStateCount; }

const std::vector<BlockRegistration> &blockRegistry() {
  static const std::vector<BlockRegistration> entries = [] {
    std::vector<BlockRegistration> result;
    for (const Block *block : detail::tables().blocks)
      if (block->supported())
        result.push_back({block->name(), block->metadata()});
    return result;
  }();
  return entries;
}
} // namespace redstone
