#include "redstone/light/optical.hpp"
#include "redstone/light/snapshot.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "light/common.hpp"

namespace redstone {
namespace {

struct OfficialLightState {
  const char *key;
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

struct LightFaceRectSpan {
  std::uint16_t offset = 0;
  std::uint8_t count = 0;
  bool operator==(const LightFaceRectSpan &) const = default;
};

struct LightFaceShape {
  LightFaceRectSpan down{};
  LightFaceRectSpan up{};
  LightFaceRectSpan north{};
  LightFaceRectSpan south{};
  LightFaceRectSpan west{};
  LightFaceRectSpan east{};
  std::uint8_t present = 0;
  bool operator==(const LightFaceShape &) const = default;
};

struct OfficialLightFaceState {
  const char *key;
  std::uint16_t shape;
};

#include "../light_metadata.inc"
#include "../light_occlusion.inc"

const BlockLightProperties kUnsupportedLightProperties{};
constexpr int kFaceScale = 1'000'000;

std::span<const LightFaceRect> faceRects(const BlockLightProperties &properties,
                                         int direction) {
  if (properties.occlusionShapeId >= std::size(kLightFaceShapes))
    return {};
  const LightFaceShape &shape = kLightFaceShapes[properties.occlusionShapeId];
  if ((shape.present & (1u << direction)) == 0)
    return {};
  const LightFaceRectSpan *span = nullptr;
  switch (direction) {
  case 0:
    span = &shape.down;
    break;
  case 1:
    span = &shape.up;
    break;
  case 2:
    span = &shape.north;
    break;
  case 3:
    span = &shape.south;
    break;
  case 4:
    span = &shape.west;
    break;
  default:
    span = &shape.east;
    break;
  }
  return {kLightFaceRects + span->offset, span->count};
}

bool faceUnionCovers(std::span<const LightFaceRect> first,
                     std::span<const LightFaceRect> second) {
  std::vector<int> xs;
  xs.reserve(2 + (first.size() + second.size()) * 2);
  xs.push_back(0);
  xs.push_back(kFaceScale);
  for (const auto &r : first) {
    xs.push_back(std::clamp(r.u0, 0, kFaceScale));
    xs.push_back(std::clamp(r.u1, 0, kFaceScale));
  }
  for (const auto &r : second) {
    xs.push_back(std::clamp(r.u0, 0, kFaceScale));
    xs.push_back(std::clamp(r.u1, 0, kFaceScale));
  }
  std::sort(xs.begin(), xs.end());
  xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  std::vector<std::pair<int, int>> intervals;
  intervals.reserve(first.size() + second.size());
  for (std::size_t i = 1; i < xs.size(); ++i) {
    if (xs[i - 1] == xs[i])
      continue;
    const int midpoint = (xs[i - 1] + xs[i]) / 2;
    intervals.clear();
    for (const auto &r : first) {
      if (midpoint >= r.u0 && midpoint < r.u1)
        intervals.emplace_back(std::max(0, r.v0), std::min(kFaceScale, r.v1));
    }
    for (const auto &r : second) {
      if (midpoint >= r.u0 && midpoint < r.u1)
        intervals.emplace_back(std::max(0, r.v0), std::min(kFaceScale, r.v1));
    }
    std::sort(intervals.begin(), intervals.end());
    int covered = 0;
    for (const auto &[v0, v1] : intervals) {
      if (v0 > covered)
        return false;
      covered = std::max(covered, v1);
    }
    if (covered < kFaceScale)
      return false;
  }
  return true;
}

} // namespace

namespace light_internal {

bool hasLightFaceRect(const BlockLightProperties &properties, int direction) {
  return !faceRects(properties, direction).empty();
}

bool faceShapesOcclude(const BlockLightProperties &from,
                       const BlockLightProperties &to, int direction) {
  if (!from.supported || !to.supported)
    return true;
  const auto first = faceRects(from, direction);
  const auto second = faceRects(to, direction ^ 1);
  if (first.empty() && second.empty())
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
  const bool validBits =
      (section.bits >= 1 && section.bits <= 8) || section.bits == 16;
  if (!validBits)
    throw std::runtime_error("World section has invalid palette width");
  if (section.palette.empty())
    throw std::runtime_error("World section has an empty palette");
  if (section.bits == 8 && section.cells8.size() < kLightVolume)
    throw std::runtime_error("World section has truncated 8-bit indices");
  if (section.bits == 16 && section.cells16.size() < kLightVolume)
    throw std::runtime_error("World section has truncated 16-bit indices");
  if (section.bits >= 1 && section.bits <= 7) {
    const std::size_t perWord = std::size_t{64} / section.bits;
    const std::size_t requiredWords = (kLightVolume + perWord - 1) / perWord;
    if (section.packedWords.size() < requiredWords)
      throw std::runtime_error("World section has truncated packed words");
  }

  std::vector<std::uint8_t> palette;
  palette.reserve(section.palette.size());
  for (BlockState::Id state : section.palette)
    palette.push_back(optical.classForState(state));

  result.cells = std::make_unique<GeometrySection::Cells>();
  for (std::size_t i = 0; i < kLightVolume; ++i) {
    const std::uint16_t index = section.paletteIndexAt(i);
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
        return std::string_view(entry.key) < wanted;
      });
  if (found == std::end(kOfficialLightStates) ||
      std::string_view(found->key) != key)
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
          return std::string_view(entry.key) < wanted;
        });
    if (face == std::end(kOfficialLightFaceStates) ||
        std::string_view(face->key) != key) {
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
