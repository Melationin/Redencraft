#pragma once
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "redstone/property.hpp"

namespace redstone {
class Block;
class BlockRef;
class BlockState;
class Random;
class RandomTickAccess;
class World;
enum class Direction;
struct Pos;
enum class RandomTickResult { Applied, Unsupported };

namespace detail {
// Registry tables are published as a flat pointer plus count so that
// BlockState::fromId is one indexed load. Both are zero initialized before any
// static constructor runs and are refreshed after every registration, so a
// state can be resolved before, during and after block registration.
extern const BlockState *gStateData;
extern std::uint32_t gStateCount;
extern const std::uint8_t *const gRandomTickStateFlags;
extern const std::uint32_t gRandomTickStateCount;
// The air singleton, published as soon as minecraft:air registers, so a default
// constructed BlockState is the registered air state (not a null placeholder).
extern const Block *gAirBlock;
void registerBlock(Block &block);
void bindBlockBehaviours(Block &block);
void bindRandomTickBehaviour(Block &block);
std::uint32_t internState(const Block *block);
struct Tables;
Tables &tables();
} // namespace detail

// The air singleton; used for a default constructed state before registration.
const Block &airBlock();

// ---------------------------------------------------------------------------
// BlockState (net/minecraft/world/level/block/state/BlockBehaviour.BlockStateBase)
//
// A state is a registered singleton owned by exactly one Block. It carries the
// owner reference and the registry id: copying is two words, equality is id
// equality, and state.is(Blocks.SAND) is the reference comparison vanilla puts
// in getBlock().
// ---------------------------------------------------------------------------
class BlockState {
public:
  using Id = std::uint32_t;
  BlockState() : block_(detail::gAirBlock) {} // minecraft:air, id 0
  explicit BlockState(std::string name);
  BlockState(std::string name, std::map<std::string, std::string> properties);
  static BlockState fromId(Id value) {
    // Single clamped load: the table always has a null state at index 0, so an
    // out-of-range id resolves to air without constructing a temporary.
    return detail::gStateData[value < detail::gStateCount ? value : 0u];
  }
  Id id() const { return id_; }
  // Canonical property bit fields (shift/width per PropertyKind). Zero for the
  // default state of a block without properties; irrelevant for states outside
  // the block's StateDefinition, which is why lookups check isSchemaState().
  std::uint32_t packed() const { return packed_; }
  const Block *blockOrNull() const { return block_; }
  // AbstractBlockState.getBlock: the owner reference.
  const Block &block() const;
  // BlockState.is(Block): identity, the vanilla "instanceof" replacement.
  bool is(const Block &other) const { return block_ == &other; }
  bool sameBlock(const BlockState &other) const {
    return block_ == other.block_;
  }
  bool isAir() const;
  const std::string &name() const; // canonical "minecraft:" name
  std::string_view nameView() const;
  std::string_view shortName() const;
  std::map<std::string, std::string> properties() const;
  std::string get(const std::string &key,
                  const std::string &fallback = "") const;
  std::string_view getView(std::string_view key,
                           std::string_view fallback = std::string_view{}) const;
  bool hasProperty(std::string_view key) const;
  // StateHolder.setValue: transition to the registered neighbour state.
  BlockState with(std::string_view key, std::string_view value) const;
  // Same transition through the canonical property kind: one field update on
  // packed(), then the dense variant/id is derived from the property strides.
  // Values outside the block's own list leave the state unchanged.
  BlockState with(PropertyKind kind, std::uint8_t value) const;
  // Registry ids identify a state uniquely (one state per id).
  bool operator==(const BlockState &other) const { return id_ == other.id_; }
  // Index of this state inside its block's dense variant space. States past the
  // block's StateDefinition are the out-of-schema extension states.
  std::uint32_t variant() const;
  bool isSchemaState() const;
  // nullopt means the block's random-tick declaration is not yet modeled.
  std::optional<bool> isRandomlyTicking() const;
  RandomTickResult randomTick(RandomTickAccess &level, Pos pos, Random &random) const;

private:
  friend void detail::registerBlock(Block &);
  friend std::uint32_t detail::internState(const Block *);
  BlockState(const Block *owner, Id id, std::uint32_t packed = 0)
      : block_(owner), id_(id), packed_(packed) {}
  std::string_view extensionValue(std::string_view key,
                                  std::string_view fallback) const;
  bool extensionHas(std::string_view key) const;
  std::map<std::string, std::string> extensionProperties() const;
  const Block *block_ = nullptr;
  Id id_ = 0;
  std::uint32_t packed_ = 0;
};
// detail::kNullState is two uint64 words reinterpreted as a BlockState, and the
// registry table is indexed by id, so the layout must stay two words plus the
// packed property field inside the second word.
static_assert(sizeof(BlockState) == 16 && alignof(BlockState) == 8,
              "BlockState must stay 16 bytes");

// ---------------------------------------------------------------------------
// StateDefinition (net/minecraft/world/level/block/state/StateDefinition)
//
// A block owns an ordered property set; its states are the cartesian product of
// the property values and are registered before the simulation starts. Every
// state has a dense variant index, so StateHolder.setValue is stride arithmetic
// that returns an already registered neighbour state.
// ---------------------------------------------------------------------------
class Property {
public:
  static constexpr std::uint8_t kAbsent = 0xFF;
  Property() = default;
  Property(std::string name, std::vector<std::string> values,
           std::string defaultValue = {});
  const std::string &name() const { return name_; }
  const std::vector<std::string> &values() const { return values_; }
  std::uint32_t defaultIndex() const { return defaultIndex_; }
  bool indexOf(std::string_view value, std::uint32_t &index) const;
  // Canonical identity and value mapping. Registration fills both: local order
  // is the block's own declaration order, canonical order is shared by every
  // block that uses the kind.
  PropertyKind kind() const { return kind_; }
  std::uint8_t canonicalValue(std::uint32_t localIndex) const {
    return valueByLocal_[localIndex];
  }
  std::uint8_t localIndex(std::uint32_t canonicalValue) const {
    return localByValue_[canonicalValue];
  }

private:
  struct ValueHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const {
      return std::hash<std::string_view>{}(value);
    }
  };
  std::string name_;
  std::vector<std::string> values_;
  std::unordered_map<std::string, std::uint32_t, ValueHash, std::equal_to<>>
      valueIndex_;
  std::uint32_t defaultIndex_ = 0;
  PropertyKind kind_ = PropertyKind::Count;
  std::array<std::uint8_t, kMaxPropertyValue + 1> valueByLocal_{};
  std::array<std::uint8_t, kMaxPropertyValue + 1> localByValue_{};
};

// One property's placement inside BlockState::packed(): the canonical value
// occupies `width` bits at `shift`, and `stride` is its dense variant weight.
struct PropertyLayout {
  const Property *property = nullptr; // null when the block has no such kind
  std::uint32_t stride = 0;
  std::uint32_t mask = 0;
  std::uint8_t shift = 0;
  std::uint8_t width = 0;
  std::uint8_t count = 0;
};

class StateDefinition {
public:
  using Properties = std::vector<Property>;
  StateDefinition() = default;
  explicit StateDefinition(Properties properties);
  StateDefinition(std::initializer_list<Property> properties)
      : StateDefinition(Properties(properties)) {}
  const Properties &properties() const { return properties_; }
  std::uint32_t stateCount() const { return stateCount_; }
  std::uint32_t defaultVariant() const { return defaultVariant_; }
  const Property *find(std::string_view name) const;
  // Missing values take their default; a value that is not part of the
  // definition is rejected instead of silently mapping onto the default.
  bool variantOf(const std::map<std::string, std::string> &values,
                 std::uint32_t &variant) const;
  bool with(std::uint32_t variant, std::string_view key, std::string_view value,
            std::uint32_t &result) const;
  std::string_view value(std::uint32_t variant, std::string_view key,
                          std::string_view fallback = {}) const;
  std::map<std::string, std::string> values(std::uint32_t variant) const;
  // Enum path: one bit field per property, no name lookup.
  const PropertyLayout &layout(PropertyKind kind) const {
    return layout_[static_cast<std::size_t>(kind)];
  }
  // Canonical bit fields for a dense variant.
  std::uint32_t packedOf(std::uint32_t variant) const;
  // Replaces one property field. Returns false when the block has no such
  // property or the canonical value is not part of this block's value list.
  bool withPacked(std::uint32_t packed, std::uint32_t variant, PropertyKind kind,
                  std::uint8_t value, std::uint32_t &nextVariant,
                  std::uint32_t &nextPacked) const;

private:
  Properties properties_; // sorted by property name
  std::vector<std::uint32_t> stride_;
  std::uint32_t stateCount_ = 1;
  std::uint32_t defaultVariant_ = 0;
  std::array<PropertyLayout, kPropertyKindCount> layout_{};
};

// BlockBehaviour.BlockStateBase light fields. canOcclude and
// useShapeForLightOcclusion intentionally remain separate from collision
// geometry. Face masks reserve exact directional occlusion data for L-2a.
struct BlockLightProperties {
  std::uint8_t emission = 0;
  std::uint8_t dampening = 0;
  // Index into the immutable official directional face-shape table (0..63).
  // Zero is the empty face shape; shape identity is independent from collision.
  std::uint16_t occlusionShapeId : 6 = 0;
  std::uint16_t supported : 1 = 0;
  std::uint16_t canOcclude : 1 = 0;
  std::uint16_t useShapeForLightOcclusion : 1 = 0;
  std::uint16_t propagatesSkylightDown : 1 = 1;
  std::uint16_t : 6; // reserved; keeps the whole struct at four bytes
  bool operator==(const BlockLightProperties &) const = default;
};
static_assert(sizeof(BlockLightProperties) == 4,
              "BlockLightProperties must stay four bytes");

enum class BlockClassFlag : std::uint64_t {
  None = 0,
  AirLike = 1ULL << 0,
  Rail = 1ULL << 1,
  Liquid = 1ULL << 2,
  Carpet = 1ULL << 3,
  Wall = 1ULL << 4,
  Fence = 1ULL << 5,
  FenceGate = 1ULL << 6,
  Stairs = 1ULL << 7,
  Slab = 1ULL << 8,
  Door = 1ULL << 9,
  Trapdoor = 1ULL << 10,
  Button = 1ULL << 11,
  PressurePlate = 1ULL << 12,
  Sign = 1ULL << 13,
  Leaves = 1ULL << 14,
  Sapling = 1ULL << 15,
  Falling = 1ULL << 16,
  CoralFan = 1ULL << 17,
  CoralWallFan = 1ULL << 18,
  Snowy = 1ULL << 19,
  Flower = 1ULL << 20,
  Vegetation = 1ULL << 21,
  DryVegetation = 1ULL << 22,
  RotatedPillar = 1ULL << 23,
  Bed = 1ULL << 24,
  Chest = 1ULL << 25,
  ShulkerBox = 1ULL << 26,
  Piston = 1ULL << 27,
  MovingPiston = 1ULL << 28,
  CopperBulb = 1ULL << 29,
  Candle = 1ULL << 30,
  Cauldron = 1ULL << 31,
  Banner = 1ULL << 32,
  Skull = 1ULL << 33,
  WaterloggedHost = 1ULL << 34,
  HasBlockEntity = 1ULL << 35,
};

// BlockBehaviour.Properties subset (see docs/compatibility.md).
struct BlockMetadata {
  float hardness;
  bool conductor;
  std::string pushReaction;
  bool supported = false;
  float friction = 0.6f;
  float speedFactor = 1.0f;
  float explosionResistance = 0.0f;
  bool liquid = false;
  // BlockBehaviour.Properties.forceSolidOn(): the state counts as solid no
  // matter what its collision shape says. 26.1 sets it on the fence gates and on
  // moving_piston, so an opened gate still blocks fluid and a moving piston is
  // never treated as an empty cell.
  bool forceSolid = false;
  // Block instanceof LiquidBlockContainer. Every block declaring the
  // waterlogged property in this registry subset is a SimpleWaterloggedBlock,
  // which is the only LiquidBlockContainer that exists here; the flag keeps the
  // instanceof test a single load instead of a string compare on the property
  // declaration (FlowingFluid.java:410-435).
  bool liquidContainer = false;
  bool replaceable = false;
  bool ignitedByLava = false;
  Instrument instrument = Instrument::Harp;
  std::uint64_t classFlags = 0;
};

// Placeholder identity used for imported states outside the official metadata
// table. Unknown states are never assigned guessed light values.
const BlockLightProperties &unsupportedLightProperties() noexcept;
const BlockLightProperties &lightPropertiesOf(const BlockState &state);
BlockLightProperties officialLightProperties(
    std::string_view blockName, std::string_view properties);
// BlockBehaviour.BlockStateBase.blocksMotion for the registered subset.
bool blocksMotion(const BlockState &state);
bool stateIsRandomlyTicking(const BlockState &state);
inline bool stateIsRandomlyTickingId(BlockState::Id id) {
  return id < detail::gRandomTickStateCount &&
         detail::gRandomTickStateFlags[id] != 0;
}
bool stateBlocksMotion(const BlockState &state);
bool stateIsRedstoneConductor(const BlockState &state);
struct BlockRegistration {
  std::string name;
  BlockMetadata metadata;
};

// BlockBehaviour per-block dispatch table. Vanilla Block subclasses override
// virtual callbacks on BlockBehaviour; here each Block binds static function
// pointers once during registration, leaving high-frequency checks (air,
// fluids, pistons, rails) as inline fast paths in World.
struct BlockBehaviours {
  using OnPlaceFn = void (*)(World &world, Pos pos, const BlockState &state,
                             const BlockState &oldState, bool movedByPiston);
  using AffectRemovalFn = void (*)(World &world, Pos pos,
                                   const BlockState &state,
                                   bool movedByPiston);
  using NeighborChangedFn = void (*)(World &world, Pos pos,
                                     const BlockState &state,
                                     const BlockRef &source,
                                     bool movedByPiston);
  using UpdateShapeFn = BlockState (*)(World &world, Pos pos,
                                       const BlockState &state,
                                       Direction direction, Pos neighborPos,
                                       const BlockState &neighbor);
  using ScheduledTickFn = void (*)(World &world, Pos pos,
                                   const BlockState &state);
  using BlockEventFn = bool (*)(World &world, Pos pos, const BlockState &state,
                                int type, int data);
  using RandomTickFn = RandomTickResult (*)(RandomTickAccess &level, Pos pos,
                                            const BlockState &state,
                                            Random &random);

  OnPlaceFn onPlace = nullptr;
  AffectRemovalFn affectNeighborsAfterRemoval = nullptr;
  NeighborChangedFn neighborChanged = nullptr;
  UpdateShapeFn updateShape = nullptr;
  ScheduledTickFn tick = nullptr;
  BlockEventFn triggerEvent = nullptr;
  RandomTickFn randomTick = nullptr;
};

// ---------------------------------------------------------------------------
// Block (net/minecraft/world/level/block/Block)
//
// One process-wide singleton per block type, exactly like the static fields of
// net/minecraft/world/level/block/Blocks. The constructor registers the block
// and all of its states, so block identity is object identity and a state test
// never touches a string.
// ---------------------------------------------------------------------------
class Block {
public:
  Block(const Block &) = delete;
  Block &operator=(const Block &) = delete;
  Block(std::string_view name, BlockMetadata metadata = {},
        StateDefinition definition = {});
  const std::string &name() const { return name_; }
  std::string_view shortName() const;
  bool is(const Block &other) const { return this == &other; }
  const BlockMetadata &metadata() const { return metadata_; }
  const StateDefinition &stateDefinition() const { return definition_; }
  BlockState defaultState() const;
  std::vector<BlockState> states() const;
  BlockState::Id firstState() const { return firstState_; }
  std::uint32_t stateCount() const { return stateCount_; }
  std::uint32_t index() const { return index_; }
  // Block class stand-ins: vanilla instanceof tests, evaluated once per block.
  bool supported() const { return metadata_.supported; }
  bool isAirLike() const { return airLike_; }
  bool isRail() const { return rail_; }
  bool isLiquid() const { return liquid_; }
  bool isCarpet() const { return carpet_; }
  bool isWall() const { return wall_; }
  bool isCoralFan() const { return coralFan_; }
  bool isCoralWallFan() const { return coralWallFan_; }
  bool hasClassFlag(BlockClassFlag flag) const {
    return (metadata_.classFlags & static_cast<std::uint64_t>(flag)) != 0;
  }
  bool isFence() const { return hasClassFlag(BlockClassFlag::Fence); }
  bool isFenceGate() const { return hasClassFlag(BlockClassFlag::FenceGate); }
  bool isStairs() const { return hasClassFlag(BlockClassFlag::Stairs); }
  bool isSlab() const { return hasClassFlag(BlockClassFlag::Slab); }
  bool isDoor() const { return hasClassFlag(BlockClassFlag::Door); }
  bool isTrapdoor() const { return hasClassFlag(BlockClassFlag::Trapdoor); }
  bool isButton() const { return hasClassFlag(BlockClassFlag::Button); }
  bool isPressurePlate() const { return hasClassFlag(BlockClassFlag::PressurePlate); }
  bool isSign() const { return hasClassFlag(BlockClassFlag::Sign); }
  bool isLeaves() const { return hasClassFlag(BlockClassFlag::Leaves); }
  bool isSapling() const { return hasClassFlag(BlockClassFlag::Sapling); }
  bool isFalling() const { return hasClassFlag(BlockClassFlag::Falling); }
  bool isSnowy() const { return hasClassFlag(BlockClassFlag::Snowy); }
  bool isFlower() const { return hasClassFlag(BlockClassFlag::Flower); }
  bool isVegetation() const { return hasClassFlag(BlockClassFlag::Vegetation); }
  bool isDryVegetation() const { return hasClassFlag(BlockClassFlag::DryVegetation); }
  bool isRotatedPillar() const { return hasClassFlag(BlockClassFlag::RotatedPillar); }
  bool isBed() const { return hasClassFlag(BlockClassFlag::Bed); }
  bool isChest() const { return hasClassFlag(BlockClassFlag::Chest); }
  bool isShulkerBox() const { return hasClassFlag(BlockClassFlag::ShulkerBox); }
  bool isPiston() const { return hasClassFlag(BlockClassFlag::Piston); }
  bool isMovingPiston() const { return hasClassFlag(BlockClassFlag::MovingPiston); }
  bool isCopperBulb() const { return hasClassFlag(BlockClassFlag::CopperBulb); }
  bool isCandle() const { return hasClassFlag(BlockClassFlag::Candle); }
  bool isCauldron() const { return hasClassFlag(BlockClassFlag::Cauldron); }
  bool isBanner() const { return hasClassFlag(BlockClassFlag::Banner); }
  bool isSkull() const { return hasClassFlag(BlockClassFlag::Skull); }
  bool isWaterloggedHost() const { return hasClassFlag(BlockClassFlag::WaterloggedHost); }
  bool hasBlockEntity() const { return hasClassFlag(BlockClassFlag::HasBlockEntity); }
  // LiquidBlockContainer: the block can hold its own fluid independent of the
  // blocksMotion test (SimpleWaterloggedBlock.java:18-36).
  bool isLiquidContainer() const { return metadata_.liquidContainer; }
  const BlockLightProperties &lightProperties(std::uint32_t variant) const;
  const BlockBehaviours &behaviours() const { return behaviours_; }
  void onPlace(World &world, Pos pos, const BlockState &state,
               const BlockState &oldState, bool movedByPiston) const;
  void affectNeighborsAfterRemoval(World &world, Pos pos,
                                   const BlockState &state,
                                   bool movedByPiston) const;
  void neighborChanged(World &world, Pos pos, const BlockState &state,
                       const BlockRef &source, bool movedByPiston) const;
  BlockState updateShape(World &world, Pos pos, const BlockState &state,
                         Direction direction, Pos neighborPos,
                         const BlockState &neighbor) const;
  void tick(World &world, Pos pos, const BlockState &state) const;
  bool triggerEvent(World &world, Pos pos, const BlockState &state, int type,
                    int data) const;
  RandomTickResult randomTick(RandomTickAccess &level, Pos pos,
                              const BlockState &state, Random &random) const;

private:
  friend void detail::registerBlock(Block &block);
  friend void detail::bindBlockBehaviours(Block &block);
  friend void detail::bindRandomTickBehaviour(Block &block);
  std::string name_;
  BlockMetadata metadata_;
  StateDefinition definition_;
  BlockState::Id firstState_ = 0;
  BlockState::Id defaultStateId_ = 0;
  std::uint32_t stateCount_ = 0;
  std::uint32_t index_ = 0;
  bool airLike_ = false;
  bool rail_ = false;
  bool liquid_ = false;
  bool carpet_ = false;
  bool wall_ = false;
  bool coralFan_ = false;
  bool coralWallFan_ = false;
  BlockBehaviours behaviours_{};
  std::vector<BlockLightProperties> lightProperties_;
};

// Block object handle (ScheduledTick.block / BlockEvent.block / update source).
// Copying and comparing a handle is a pointer copy: vanilla stores the Block
// object itself.
class BlockRef {
public:
  BlockRef() = default;
  BlockRef(const Block &block) : block_(&block) {}
  // Resolves (and registers) by canonical name. The char*/std::string overloads
  // avoid a second user-defined conversion when callers pass a name string.
  BlockRef(std::string_view name);
  BlockRef(const char *name) : BlockRef(std::string_view(name ? name : "")) {}
  BlockRef(const std::string &name) : BlockRef(std::string_view(name)) {}
  const Block *block() const { return block_; }
  const Block &operator*() const { return *block_; }
  const std::string &str() const;
  std::string_view view() const { return str(); }
  bool empty() const { return block_ == nullptr; }
  bool equals(const std::string &other) const { return str() == other; }
  bool operator==(const BlockRef &other) const = default;

private:
  const Block *block_ = nullptr;
};

// ---------------------------------------------------------------------------
// Inline state accessors (need the complete Block type).
// ---------------------------------------------------------------------------
inline const Block &BlockState::block() const {
  return block_ ? *block_ : airBlock();
}
inline std::uint32_t BlockState::variant() const {
  return id_ - block_->firstState();
}
inline bool BlockState::isSchemaState() const {
  return block_ != nullptr && variant() < block_->stateCount();
}
inline bool BlockState::isAir() const {
  return block_ == nullptr || block_->isAirLike();
}
inline const std::string &BlockState::name() const { return block().name(); }
inline std::string_view BlockState::nameView() const { return name(); }
inline std::string_view BlockState::shortName() const {
  return block().shortName();
}
inline bool BlockState::hasProperty(std::string_view key) const {
  if (!block_)
    return false;
  return isSchemaState() ? block_->stateDefinition().find(key) != nullptr
                         : extensionHas(key);
}
inline std::string_view BlockState::getView(std::string_view key,
                                            std::string_view fallback) const {
  if (!block_)
    return fallback;
  if (isSchemaState())
    return block_->stateDefinition().value(variant(), key, fallback);
  return extensionValue(key, fallback);
}
inline std::string BlockState::get(const std::string &key,
                                   const std::string &fallback) const {
  return std::string(getView(key, fallback));
}
inline std::map<std::string, std::string> BlockState::properties() const {
  if (!block_)
    return {};
  return isSchemaState() ? block_->stateDefinition().values(variant())
                         : extensionProperties();
}
inline std::string_view Block::shortName() const {
  constexpr std::string_view prefix = "minecraft:";
  const std::string_view n = name_;
  return n.starts_with(prefix) ? n.substr(prefix.size()) : n;
}
inline BlockState Block::defaultState() const {
  return BlockState::fromId(defaultStateId_);
}
inline BlockState airState() { return BlockState{}; }

// ---------------------------------------------------------------------------
// Enum property accessors. Each is one shift and one mask on
// BlockState::packed(); the string form (getView) stays for names that cross
// the registry boundary (import/export, protocol, diagnostics).
// `fallback` is returned for blocks that do not declare the property, matching
// the old getView(key, fallback) behaviour.
// ---------------------------------------------------------------------------
inline std::uint32_t propertyValue(const BlockState &state, PropertyKind kind,
                                   std::uint32_t fallback = 0) {
  if (!state.isSchemaState())
    return fallback;
  const PropertyLayout &layout = state.block().stateDefinition().layout(kind);
  return layout.property ? (state.packed() >> layout.shift) & layout.mask
                         : fallback;
}

inline Facing facingOf(const BlockState &state, Facing fallback = Facing::North) {
  return static_cast<Facing>(propertyValue(
      state, PropertyKind::Facing, static_cast<std::uint32_t>(fallback)));
}
inline PistonType pistonTypeOf(const BlockState &state) {
  return static_cast<PistonType>(propertyValue(state, PropertyKind::Type));
}
inline WallSide wallSideOf(const BlockState &state, PropertyKind side) {
  return static_cast<WallSide>(propertyValue(state, side));
}
inline bool poweredOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Powered) != 0;
}
inline bool litOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Lit) != 0;
}
inline bool openOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Open) != 0;
}
inline bool waterloggedOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Waterlogged) != 0;
}
inline bool extendedOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Extended) != 0;
}
inline bool snowyOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Snowy) != 0;
}
inline bool unstableOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Unstable) != 0;
}
inline bool upOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Up) != 0;
}
inline bool inWallOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::InWall) != 0;
}
inline bool shortOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Short) != 0;
}

// BlockBehaviour.Properties subset of a state: per-block data, no cache.
inline const BlockMetadata &blockMetadata(const BlockState &state) {
  return state.block().metadata();
}
inline const BlockLightProperties &blockLightProperties(const BlockState &state) {
  return lightPropertiesOf(state);
}

// ---------------------------------------------------------------------------
// Block registry (net/minecraft/world/level/block/Blocks).
// ---------------------------------------------------------------------------
namespace Blocks {
#include "redstone/blocks_decl.inc"
} // namespace Blocks

// Name lookup: findBlockByName never registers, blockByName registers an
// unsupported Block for names outside the registry (save/litematic import).
const Block *findBlockByName(std::string_view name);
const Block &blockByName(std::string_view name);

bool stateNamed(const BlockState &state, std::string_view shortName);
bool stateIsAir(const BlockState &state);
BlockState defaultState(const std::string &name);
bool isSupportedBlock(const std::string &name);
std::size_t registeredStateCount();
const std::vector<BlockRegistration> &blockRegistry();

// BlockStateBase.isRedstoneConductor: the extended piston base is not one.
inline bool isRedstoneConductor(const BlockState &state) {
  const Block *block = state.blockOrNull();
  if (!block || !block->metadata().conductor)
    return false;
  if ((block->is(Blocks::PISTON) || block->is(Blocks::STICKY_PISTON)) &&
      state.getView("extended", "false") == "true")
    return false;
  return true;
}
inline bool isRegisteredSmallFlower(const BlockState &state) {
  return state.is(Blocks::DANDELION) ||
         state.is(Blocks::GOLDEN_DANDELION) ||
         state.is(Blocks::TORCHFLOWER) ||
         state.is(Blocks::POPPY) ||
         state.is(Blocks::BLUE_ORCHID) ||
         state.is(Blocks::ALLIUM) ||
         state.is(Blocks::AZURE_BLUET) ||
         state.is(Blocks::RED_TULIP) ||
         state.is(Blocks::ORANGE_TULIP) ||
         state.is(Blocks::WHITE_TULIP) ||
         state.is(Blocks::PINK_TULIP) ||
         state.is(Blocks::OXEYE_DAISY) ||
         state.is(Blocks::CORNFLOWER) ||
         state.is(Blocks::WITHER_ROSE) ||
         state.is(Blocks::LILY_OF_THE_VALLEY);
}
inline bool isWoolBlock(const BlockState &state) {
  return state.is(Blocks::WHITE_WOOL) ||
         state.is(Blocks::ORANGE_WOOL) ||
         state.is(Blocks::MAGENTA_WOOL) ||
         state.is(Blocks::LIGHT_BLUE_WOOL) ||
         state.is(Blocks::YELLOW_WOOL) ||
         state.is(Blocks::LIME_WOOL) ||
         state.is(Blocks::PINK_WOOL) ||
         state.is(Blocks::GRAY_WOOL) ||
         state.is(Blocks::LIGHT_GRAY_WOOL) ||
         state.is(Blocks::CYAN_WOOL) ||
         state.is(Blocks::PURPLE_WOOL) ||
         state.is(Blocks::BLUE_WOOL) ||
         state.is(Blocks::BROWN_WOOL) ||
         state.is(Blocks::GREEN_WOOL) ||
         state.is(Blocks::RED_WOOL) ||
         state.is(Blocks::BLACK_WOOL);
}
inline bool isWoolCarpet(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isCarpet() && block->supported();
}
inline bool isWallBlock(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isWall();
}
inline bool isFenceGate(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isFenceGate();
}
inline bool isDeadCoralWallFan(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isCoralWallFan() && block->supported();
}
inline bool isDeadCoralFan(const BlockState &state) {
  // Wall fans are coral fans too: matches the pre-registry predicate.
  const Block *block = state.blockOrNull();
  return block && (block->isCoralFan() || block->isCoralWallFan()) &&
         block->supported();
}
inline bool isRailBlock(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isRail();
}
inline bool isLiquidBlock(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isLiquid();
}
} // namespace redstone
