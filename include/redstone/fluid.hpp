#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "redstone/block.hpp"

// net/minecraft/world/level/material/{Fluid,FluidState,Fluids,FlowingFluid}.java
//
// A fluid is a process-wide singleton (the static fields of Fluids), and a
// FluidState is the (fluid, amount, falling) triple a block state decodes to.
// Both are pure values: querying never writes a block and never schedules a
// tick. Flow simulation is a later task.
namespace redstone {

class Fluid;

// ---------------------------------------------------------------------------
// FluidState (net/minecraft/world/level/material/FluidState)
// ---------------------------------------------------------------------------
class FluidState {
public:
  FluidState() = default; // minecraft:empty, amount 0
  FluidState(const Fluid &type, std::uint8_t amount, bool falling)
      : type_(&type), amount_(amount), falling_(falling) {}
  const Fluid &getType() const;
  // FluidState.isSource: the Source variant, independent of amount.
  bool isSource() const;
  bool isSourceOfType(const Fluid &type) const;
  bool isEmpty() const;
  // FluidState.getAmount dispatches to the owning fluid: a Source answers 8 and
  // a Flowing variant answers its stored LEVEL property.
  std::uint8_t getAmount() const;
  // The stored LEVEL property value, before the fluid's getAmount() rule. The
  // Flowing variants read this; reading getAmount() from there would recurse.
  std::uint8_t rawAmount() const { return amount_; }
  // FluidState.isFull: amount == 8, independent of isSource().
  bool isFull() const { return getAmount() == 8; }
  bool isFalling() const { return falling_; }
  // FlowingFluid.getOwnHeight: amount / 9.
  float getOwnHeight() const;
  // FlowingFluid.getLegacyLevel: the block state level this fluid writes back.
  int getLegacyLevel() const;
  bool operator==(const FluidState &other) const;
  bool operator!=(const FluidState &other) const { return !(*this == other); }

private:
  const Fluid *type_ = nullptr;
  std::uint8_t amount_ = 0;
  bool falling_ = false;
};

// ---------------------------------------------------------------------------
// Fluid (net/minecraft/world/level/material/Fluid)
//
// One process-wide singleton per fluid type, like the static fields of Fluids.
// ---------------------------------------------------------------------------
class Fluid {
public:
  virtual ~Fluid() = default;
  const std::string &name() const { return name_; }
  std::string_view shortName() const;
  bool is(const Fluid &other) const { return this == &other; }
  // Fluid.isEmpty: only minecraft:empty.
  bool isEmpty() const { return empty_; }
  // Fluid.isSource: Source variants answer true, Flowing variants false.
  bool isSource() const { return source_; }
  // Fluid.isSame: the family test used by flowing logic and mixing.
  virtual bool isSame(const Fluid &other) const { return this == &other; }
  // Fluid.getAmount: 8 for a Source, the flowing level otherwise.
  virtual std::uint8_t getAmount(const FluidState &state) const = 0;
  virtual float getOwnHeight(const FluidState &state) const {
    return static_cast<float>(state.getAmount()) / 9.0f;
  }
  // Fluid.createLegacyBlock: the block state a fluid writes back.
  virtual BlockState createLegacyBlock(const FluidState &state) const = 0;
  // FlowingFluid.getLegacyLevel.
  virtual int getLegacyLevel(const FluidState &state) const;
  // Registered bucket item name; empty for the empty fluid.
  const std::string &bucketItem() const { return bucketItem_; }

protected:
  Fluid(std::string_view name, bool empty, bool source, std::string bucketItem);

private:
  std::string name_;
  std::string bucketItem_;
  bool empty_ = false;
  bool source_ = false;
};

// WaterFluid / LavaFluid differ in isSame() (family test), drop-off, slope
// distance, tick delay and source conversion. Only the state model is here.
class WaterFluid : public Fluid {
public:
  bool isSame(const Fluid &other) const override;
  std::uint8_t getAmount(const FluidState &state) const override;
  BlockState createLegacyBlock(const FluidState &state) const override;

protected:
  WaterFluid(std::string_view name, bool source);
};

class LavaFluid : public Fluid {
public:
  bool isSame(const Fluid &other) const override;
  std::uint8_t getAmount(const FluidState &state) const override;
  BlockState createLegacyBlock(const FluidState &state) const override;

protected:
  LavaFluid(std::string_view name, bool source);
};

// Fluids.java's static fields. Function-local statics keep registration order
// independent of translation-unit initialisation order.
namespace Fluids {
const Fluid &empty();
const WaterFluid &flowingWater();
const WaterFluid &water();
const LavaFluid &flowingLava();
const LavaFluid &lava();
} // namespace Fluids

const Fluid *findFluidByName(std::string_view name);

// BlockBehaviour.getFluidState for this registry subset: liquid blocks decode
// their `level` property. Non-fluid blocks return minecraft:empty.
// TODO: waterlogged hosts report their host fluid state; that is task F-5.
FluidState fluidStateOf(const BlockState &state);

} // namespace redstone
