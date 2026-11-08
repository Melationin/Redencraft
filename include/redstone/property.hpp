#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace redstone {

// Canonical block-state property kinds. Vanilla identifies a property by its
// object identity (BlockStateProperties.FACING, ...); this enum is that
// identity for the properties the simulator models.
//
// A kind owns its value space: every block that declares the property maps its
// own value list onto the canonical values, so a state's property value is
// stored in a bit field of BlockState::packed() and read/written with a shift
// and a mask instead of a name lookup.
enum class PropertyKind : std::uint8_t {
  Facing,
  Type,
  Shape,
  Instrument,
  Note,
  Level,
  Extended,
  InWall,
  Lit,
  Open,
  Powered,
  Short,
  Snowy,
  Unstable,
  Up,
  Waterlogged,
  WallEast,
  WallNorth,
  WallSouth,
  WallWest,
  // RotatedPillarBlock.AXIS: x / y / z. basalt is the first user (F-4).
  Axis,
  Count
};
inline constexpr std::size_t kPropertyKindCount =
    static_cast<std::size_t>(PropertyKind::Count);
// Canonical values never exceed this, so a kind's value set fits a bit mask.
inline constexpr std::uint8_t kMaxPropertyValue = 31;

enum class Facing : std::uint8_t { Down, Up, North, South, West, East };
enum class PistonType : std::uint8_t { Normal, Sticky };
enum class RailShape : std::uint8_t {
  NorthSouth,
  EastWest,
  AscendingEast,
  AscendingWest,
  AscendingNorth,
  AscendingSouth,
  SouthEast,
  SouthWest,
  NorthWest,
  NorthEast
};
enum class Instrument : std::uint8_t {
  Harp,
  Basedrum,
  Snare,
  Hat,
  Bass,
  Flute,
  Bell,
  Guitar,
  Chime,
  Xylophone,
  IronXylophone,
  CowBell,
  Didgeridoo,
  Bit,
  Banjo,
  Pling
};
enum class WallSide : std::uint8_t { None, Low, Tall };

// Registry-facing conversions. Both run only while blocks register, never on
// the simulation path.
bool propertyKindFromName(std::string_view name, PropertyKind &out);
bool propertyValueFromName(PropertyKind kind, std::string_view value,
                           std::uint8_t &out);

} // namespace redstone
