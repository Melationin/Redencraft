#include "redstone/fluid.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace redstone {
namespace {
// Level.canonicalName lives in engine.hpp; fluids must stay free of the World
// header, so normalise here. Fluid names are all in the default namespace.
std::string canonicalFluidName(std::string_view name) {
  constexpr std::string_view prefix = "minecraft:";
  if (name.empty())
    return "minecraft:empty";
  return name.starts_with(prefix) ? std::string(name)
                                  : "minecraft:" + std::string(name);
}
} // namespace

// ---------------------------------------------------------------------------
// Fluid
// ---------------------------------------------------------------------------
Fluid::Fluid(std::string_view name, bool empty, bool source,
             std::string bucketItem)
    : name_(canonicalFluidName(name)),
      bucketItem_(std::move(bucketItem)), empty_(empty), source_(source) {}

std::string_view Fluid::shortName() const {
  constexpr std::string_view prefix = "minecraft:";
  const std::string_view n = name_;
  return n.starts_with(prefix) ? n.substr(prefix.size()) : n;
}

// FlowingFluid.getLegacyLevel: a source writes level 0; a flowing fluid writes
// 8 - min(amount, 8), plus 8 when it is falling.
int Fluid::getLegacyLevel(const FluidState &state) const {
  if (state.isSource())
    return 0;
  const int amount = std::min<int>(state.getAmount(), 8);
  return 8 - amount + (state.isFalling() ? 8 : 0);
}

// ---------------------------------------------------------------------------
// Water
// ---------------------------------------------------------------------------
WaterFluid::WaterFluid(std::string_view name, bool source)
    : Fluid(name, false, source, "minecraft:water_bucket") {}

bool WaterFluid::isSame(const Fluid &other) const {
  return other.is(Fluids::water()) || other.is(Fluids::flowingWater());
}

std::uint8_t WaterFluid::getAmount(const FluidState &state) const {
  // WaterFluid.Source.getAmount is the constant 8; Flowing reads the property.
  // It must read the raw property, since FluidState::getAmount dispatches back
  // into this function.
  return isSourceVariant() ? std::uint8_t{8} : state.rawAmount();
}

BlockState WaterFluid::createLegacyBlock(const FluidState &state) const {
  return Blocks::WATER.defaultState().with(
      PropertyKind::Level, static_cast<std::uint8_t>(state.getLegacyLevel()));
}

// ---------------------------------------------------------------------------
// Lava
// ---------------------------------------------------------------------------
LavaFluid::LavaFluid(std::string_view name, bool source)
    : Fluid(name, false, source, "minecraft:lava_bucket") {}

bool LavaFluid::isSame(const Fluid &other) const {
  return other.is(Fluids::lava()) || other.is(Fluids::flowingLava());
}

std::uint8_t LavaFluid::getAmount(const FluidState &state) const {
  return isSourceVariant() ? std::uint8_t{8} : state.rawAmount();
}

BlockState LavaFluid::createLegacyBlock(const FluidState &state) const {
  return Blocks::LAVA.defaultState().with(
      PropertyKind::Level, static_cast<std::uint8_t>(state.getLegacyLevel()));
}

namespace {
// EmptyFluid: no amount, no height, and air as its legacy block.
class EmptyFluid final : public Fluid {
public:
  EmptyFluid() : Fluid("minecraft:empty", true, false, {}) {}
  std::uint8_t getAmount(const FluidState &) const override { return 0; }
  float getOwnHeight(const FluidState &) const override { return 0.0f; }
  BlockState createLegacyBlock(const FluidState &) const override {
    return Blocks::AIR.defaultState();
  }
  int getLegacyLevel(const FluidState &) const override { return 0; }
};

// WaterFluid.Flowing / .Source differ only in isSource(), which the base
// constructor encodes; WaterFluid::getAmount splits on it as well.
class FlowingWater final : public WaterFluid {
public:
  FlowingWater() : WaterFluid("minecraft:flowing_water", false) {}
};
class SourceWater final : public WaterFluid {
public:
  SourceWater() : WaterFluid("minecraft:water", true) {}
};
class FlowingLava final : public LavaFluid {
public:
  FlowingLava() : LavaFluid("minecraft:flowing_lava", false) {}
};
class SourceLava final : public LavaFluid {
public:
  SourceLava() : LavaFluid("minecraft:lava", true) {}
};
} // namespace

// Fluids.java's static fields never terminate, so the five singletons are
// intentionally leaked: a function-local static would be destroyed at exit, and
// any later FluidState::getType() (which falls back to Fluids::empty()) would
// then read a destroyed object. Do not turn these into stack or scoped statics.
namespace Fluids {
const Fluid &empty() {
  static const EmptyFluid *instance = new EmptyFluid();
  return *instance;
}
const WaterFluid &flowingWater() {
  static const FlowingWater *instance = new FlowingWater();
  return *instance;
}
const WaterFluid &water() {
  static const SourceWater *instance = new SourceWater();
  return *instance;
}
const LavaFluid &flowingLava() {
  static const FlowingLava *instance = new FlowingLava();
  return *instance;
}
const LavaFluid &lava() {
  static const SourceLava *instance = new SourceLava();
  return *instance;
}
} // namespace Fluids

const Fluid *findFluidByName(std::string_view name) {
  const std::string canonical = canonicalFluidName(name);
  const Fluid *const all[] = {&Fluids::empty(),       &Fluids::flowingWater(),
                              &Fluids::water(),       &Fluids::flowingLava(),
                              &Fluids::lava()};
  for (const Fluid *fluid : all)
    if (fluid->name() == canonical)
      return fluid;
  return nullptr;
}

// ---------------------------------------------------------------------------
// FluidState
// ---------------------------------------------------------------------------
const Fluid &FluidState::getType() const {
  return type_ ? *type_ : Fluids::empty();
}
// FluidState.java:37-43: both entry points hand the state to the fluid, so the
// stateful isSource(FluidState) overload is the one actually exercised.
bool FluidState::isSource() const { return getType().isSource(*this); }
bool FluidState::isSourceOfType(const Fluid &type) const {
  return type_ == &type && type.isSource(*this);
}
bool FluidState::isEmpty() const { return getType().isEmpty(); }
std::uint8_t FluidState::getAmount() const {
  return type_ ? type_->getAmount(*this) : std::uint8_t{0};
}
float FluidState::getOwnHeight() const { return getType().getOwnHeight(*this); }
int FluidState::getLegacyLevel() const { return getType().getLegacyLevel(*this); }
bool FluidState::operator==(const FluidState &other) const {
  return getType().is(other.getType()) && getAmount() == other.getAmount() &&
         falling_ == other.falling_;
}

// LiquidBlock.getFluidState: `level` is clamped onto the 9-entry state cache
// (Math.min(level, 8)), so levels 8..15 all read the falling full state while
// the block state keeps its own property value.
FluidState fluidStateOf(const BlockState &state) {
  if (!isLiquidBlock(state))
    return FluidState();
  const int level = static_cast<int>(propertyValue(state, PropertyKind::Level));
  const int clamped = std::min(level, 8);
  const bool water = state.is(Blocks::WATER);
  if (clamped == 0)
    return water ? FluidState(Fluids::water(), 8, false)
                 : FluidState(Fluids::lava(), 8, false);
  if (clamped < 8) {
    const auto amount = static_cast<std::uint8_t>(8 - clamped);
    return water ? FluidState(Fluids::flowingWater(), amount, false)
                 : FluidState(Fluids::flowingLava(), amount, false);
  }
  return water ? FluidState(Fluids::flowingWater(), 8, true)
               : FluidState(Fluids::flowingLava(), 8, true);
}

// ---------------------------------------------------------------------------
// FluidRef: a Fluid pointer plus a name resolver. Unlike blockByName it never
// registers an unknown fluid; an unknown name stays null so F-6 can report it.
// ---------------------------------------------------------------------------
FluidRef::FluidRef(std::string_view name) : fluid_(findFluidByName(name)) {}
const std::string &FluidRef::str() const {
  static const std::string kEmpty;
  return fluid_ ? fluid_->name() : kEmpty;
}

} // namespace redstone

namespace redstone {
// FluidTags.WATER / FluidTags.LAVA. Only the four flowing/source entries carry a
// tag; minecraft:empty belongs to none.
bool fluidHasTag(const Fluid &fluid, FluidTag tag) {
  switch (tag) {
  case FluidTag::Water:
    return fluid.is(Fluids::water()) || fluid.is(Fluids::flowingWater());
  case FluidTag::Lava:
    return fluid.is(Fluids::lava()) || fluid.is(Fluids::flowingLava());
  }
  return false;
}
} // namespace redstone
