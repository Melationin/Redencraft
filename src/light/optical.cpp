#include "redstone/light/optical.hpp"
#include "redstone/light/snapshot.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "light/common.hpp"

namespace redstone {
namespace {

struct OfficialLightState {
  std::string_view key;
  std::uint8_t emission;
  std::uint8_t dampening;
  bool canOcclude;
  bool useShapeForLightOcclusion;
  bool propagatesSkylightDown;
};

struct LightFaceRect {
  std::int32_t u0 = 0;
  std::int32_t u1 = 0;
  std::int32_t v0 = 0;
  std::int32_t v1 = 0;
  bool operator==(const LightFaceRect &) const = default;
};

struct LightFaceShape {
  LightFaceRect down{};
  LightFaceRect up{};
  LightFaceRect north{};
  LightFaceRect south{};
  LightFaceRect west{};
  LightFaceRect east{};
  std::uint8_t present = 0;
  bool operator==(const LightFaceShape &) const = default;
};

struct OfficialLightFaceState {
  std::string_view key;
  std::uint16_t shape;
};

#include "../light_metadata.inc"
#include "../light_occlusion.inc"

const BlockLightProperties kUnsupportedLightProperties{};
constexpr int kFaceScale = 1'000'000;

const LightFaceRect *faceRect(const BlockLightProperties &properties,
                              int direction) {
  if (properties.occlusionShapeId >= std::size(kLightFaceShapes))
    return nullptr;
  const LightFaceShape &shape = kLightFaceShapes[properties.occlusionShapeId];
  if ((shape.present & (1u << direction)) == 0)
    return nullptr;
  switch (direction) {
  case 0:
    return &shape.down;
  case 1:
    return &shape.up;
  case 2:
    return &shape.north;
  case 3:
    return &shape.south;
  case 4:
    return &shape.west;
  default:
    return &shape.east;
  }
}

bool faceUnionCovers(const LightFaceRect *first, const LightFaceRect *second) {
  std::array<const LightFaceRect *, 2> rectangles{first, second};
  std::array<int, 6> xs{};
  std::size_t count = 2;
  xs[0] = 0;
  xs[1] = kFaceScale;
  for (const LightFaceRect *rectangle : rectangles) {
    if (!rectangle)
      continue;
    xs[count++] = std::clamp(rectangle->u0, 0, kFaceScale);
    xs[count++] = std::clamp(rectangle->u1, 0, kFaceScale);
  }
  std::sort(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(count));
  count = static_cast<std::size_t>(
      std::unique(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(count)) -
      xs.begin());
  for (std::size_t i = 1; i < count; ++i) {
    if (xs[i - 1] == xs[i])
      continue;
    const int midpoint = (xs[i - 1] + xs[i]) / 2;
    std::array<std::pair<int, int>, 2> intervals{};
    std::size_t intervalsCount = 0;
    for (const LightFaceRect *rectangle : rectangles) {
      if (!rectangle || midpoint < rectangle->u0 || midpoint >= rectangle->u1)
        continue;
      intervals[intervalsCount++] = {
          std::max(0, rectangle->v0), std::min(kFaceScale, rectangle->v1)};
    }
    std::sort(intervals.begin(),
              intervals.begin() + static_cast<std::ptrdiff_t>(intervalsCount));
    int covered = 0;
    for (std::size_t j = 0; j < intervalsCount; ++j) {
      if (intervals[j].first > covered)
        return false;
      covered = std::max(covered, intervals[j].second);
    }
    if (covered < kFaceScale)
      return false;
  }
  return true;
}

} // namespace

namespace light_internal {

bool hasLightFaceRect(const BlockLightProperties &properties, int direction) {
  return faceRect(properties, direction) != nullptr;
}

bool faceShapesOcclude(const BlockLightProperties &from,
                       const BlockLightProperties &to, int direction) {
  if (!from.supported || !to.supported)
    return true;
  const LightFaceRect *first = faceRect(from, direction);
  const LightFaceRect *second = faceRect(to, direction ^ 1);
  if (!first && !second)
    return false;
  return faceUnionCovers(first, second);
}

} // namespace light_internal

LightOpticalTable::LightOpticalTable() {
  classes_.push_back({{}, false}); // conservative unsupported sentinel
  stateClasses_.resize(detail::gStateCount, 0);
  std::unordered_map<std::uint64_t, std::uint8_t> byOptics;
  for (BlockState::Id stateId = 0; stateId < detail::gStateCount; ++stateId) {
    const BlockState state = BlockState::fromId(stateId);
    const BlockLightProperties properties = lightPropertiesOf(state);
    if (!properties.supported)
      continue;
    const std::uint64_t key =
        (static_cast<std::uint64_t>(
             std::bit_cast<std::uint32_t>(properties))
         << 1) |
        (state.isAir() ? 1u : 0u);
    const auto existing = byOptics.find(key);
    if (existing != byOptics.end()) {
      stateClasses_[stateId] = existing->second;
      continue;
    }
    if (classes_.size() >= 256)
      throw std::runtime_error("light optical class table exceeds uint8");
    const auto id = static_cast<std::uint8_t>(classes_.size());
    classes_.push_back({properties, state.isAir()});
    byOptics.emplace(key, id);
    stateClasses_[stateId] = id;
  }
}

std::shared_ptr<const LightOpticalTable> defaultLightOpticalTable() {
  static const auto table = std::make_shared<LightOpticalTable>();
  return table;
}

GeometrySection makeLightGeometrySection(const LightOpticalTable &optical,
                                         const LightSectionView &section) {
  GeometrySection result;
  if (section.bits == 0) {
    result.uniformClass = optical.classForState(section.single);
    const auto &klass = optical[result.uniformClass];
    result.nonAir = klass.air ? 0 : kLightVolume;
    result.empty = result.nonAir == 0;
    result.unknown = klass.properties.supported ? 0 : kLightVolume;
    return result;
  }
  if (section.bits != 8 && section.bits != 16)
    throw std::runtime_error("World section has invalid palette width");
  if (section.palette.empty())
    throw std::runtime_error("World section has an empty palette");

  std::vector<std::uint8_t> palette;
  palette.reserve(section.palette.size());
  for (BlockState::Id state : section.palette)
    palette.push_back(optical.classForState(state));

  result.cells = std::make_unique<GeometrySection::Cells>();
  for (std::size_t i = 0; i < kLightVolume; ++i) {
    const std::uint16_t index =
        section.bits == 8 ? section.cells8[i] : section.cells16[i];
    if (index >= palette.size())
      throw std::runtime_error("World section palette index out of range");
    (*result.cells)[i] = palette[index];
  }
  result.uniform = false;
  for (std::uint8_t classId : *result.cells) {
    const auto &klass = optical[classId];
    result.nonAir += klass.air ? 0 : 1;
    result.unknown += klass.properties.supported ? 0 : 1;
  }
  result.empty = result.nonAir == 0;
  return result;
}

const BlockLightProperties &unsupportedLightProperties() noexcept {
  return kUnsupportedLightProperties;
}

BlockLightProperties officialLightProperties(std::string_view blockName,
                                             std::string_view properties) {
  std::string key;
  key.reserve(blockName.size() + properties.size() + 1);
  key.append(blockName);
  key.push_back('\x1f');
  key.append(properties);
  const auto found = std::lower_bound(
      std::begin(kOfficialLightStates), std::end(kOfficialLightStates), key,
      [](const OfficialLightState &entry, const std::string &wanted) {
        return entry.key < wanted;
      });
  if (found == std::end(kOfficialLightStates) || found->key != key)
    return unsupportedLightProperties();
  BlockLightProperties result;
  result.emission = found->emission;
  result.dampening = found->dampening;
  result.canOcclude = found->canOcclude;
  result.useShapeForLightOcclusion = found->useShapeForLightOcclusion;
  result.propagatesSkylightDown = found->propagatesSkylightDown;
  result.supported = true;
  if (result.useShapeForLightOcclusion && result.canOcclude) {
    const auto face = std::lower_bound(
        std::begin(kOfficialLightFaceStates),
        std::end(kOfficialLightFaceStates), key,
        [](const OfficialLightFaceState &entry, const std::string &wanted) {
          return entry.key < wanted;
        });
    if (face == std::end(kOfficialLightFaceStates) || face->key != key) {
      result.supported = false;
      return result;
    }
    result.occlusionShapeId = face->shape;
  }
  return result;
}

bool hasDifferentLightProperties(const BlockState &oldState,
                                 const BlockState &newState) {
  if (oldState == newState)
    return false;
  return hasDifferentLightProperties(lightPropertiesOf(oldState),
                                     lightPropertiesOf(newState));
}

bool hasDifferentLightProperties(const BlockLightProperties &oldProperties,
                                 const BlockLightProperties &newProperties) {
  if (!oldProperties.supported || !newProperties.supported)
    return true;
  return oldProperties.emission != newProperties.emission ||
         oldProperties.dampening != newProperties.dampening ||
         oldProperties.useShapeForLightOcclusion ||
         newProperties.useShapeForLightOcclusion;
}

bool hasDifferentSectionLightProperties(bool wasEmpty, bool isEmpty) {
  return wasEmpty != isEmpty;
}

bool lightFaceShapesOcclude(const BlockLightProperties &from,
                            const BlockLightProperties &to,
                            LightDirection direction) {
  return light_internal::faceShapesOcclude(from, to,
                                           static_cast<int>(direction));
}

} // namespace redstone
