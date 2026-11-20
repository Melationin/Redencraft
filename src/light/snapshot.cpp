#include "redstone/light/snapshot.hpp"

#include <stdexcept>
#include <utility>

#include "light/common.hpp"

namespace redstone {

LightLayerSnapshot::LightLayerSnapshot() = default;

LightLayerSnapshot::LightLayerSnapshot(const LightLayerSnapshot &other)
    : state_(other.stateByte()) {
  if (const auto page = other.packed_) {
    // Explicit compatibility copies allocate their own page; production
    // section/header copies share the descriptor and thus this page pointer.
    packed_ = std::make_shared<PackedPage>();
    for (std::size_t i = 0; i < kLightLayerBytes; ++i)
      packed_->bytes[i].store(
          page->bytes[i].load(std::memory_order_relaxed),
          std::memory_order_relaxed);
  }
}

LightLayerSnapshot &
LightLayerSnapshot::operator=(const LightLayerSnapshot &other) {
  if (this == &other)
    return *this;
  state_.store(other.stateByte(), std::memory_order_relaxed);
  packed_ = other.packed_;
  return *this;
}

LightLayerSnapshot::LightLayerSnapshot(LightLayerSnapshot &&other) noexcept
    : state_(other.stateByte()), packed_(std::move(other.packed_)) {
  other.state_.store(0, std::memory_order_relaxed);
}

LightLayerSnapshot &
LightLayerSnapshot::operator=(LightLayerSnapshot &&other) noexcept {
  if (this == &other)
    return *this;
  state_.store(other.stateByte(), std::memory_order_relaxed);
  packed_ = std::move(other.packed_);
  other.state_.store(0, std::memory_order_relaxed);
  return *this;
}

LightLayerSnapshot::~LightLayerSnapshot() = default;

LightLayerSnapshot LightLayerSnapshot::missing() { return {}; }

LightLayerSnapshot LightLayerSnapshot::uniform(std::uint8_t value) {
  if (!light_internal::validLightValue(value))
    throw std::invalid_argument("light value must be in 0..15");
  LightLayerSnapshot result;
  result.state_.store(
      static_cast<std::uint8_t>(kLightUniformFlag | (value & 15)),
      std::memory_order_relaxed);
  return result;
}

LightLayerSnapshot LightLayerSnapshot::mixedEmpty() {
  LightLayerSnapshot result;
  result.makeMixed();
  return result;
}

LightLayerSnapshot LightLayerSnapshot::mixedFilled(std::uint8_t value) {
  if (!light_internal::validLightValue(value))
    throw std::invalid_argument("light value must be in 0..15");
  const std::uint8_t packed =
      static_cast<std::uint8_t>((value << 4) | (value & 15));
  LightLayerSnapshot result;
  result.packed_ = std::make_shared<PackedPage>();
  for (auto &byte : result.packed_->bytes)
    byte.store(packed, std::memory_order_relaxed);
  return result;
}

LightLayerSnapshot LightLayerSnapshot::mixed(
    const std::array<std::uint8_t, kLightVolume> &values) {
  auto page = std::make_shared<PackedPage>();
  for (std::size_t byte = 0; byte < kLightLayerBytes; ++byte) {
    const std::uint8_t lo = values[byte * 2];
    const std::uint8_t hi = values[byte * 2 + 1];
    if (!light_internal::validLightValue(lo) ||
        !light_internal::validLightValue(hi))
      throw std::invalid_argument("light value must be in 0..15");
    page->bytes[byte].store(static_cast<std::uint8_t>(lo | (hi << 4)),
                            std::memory_order_relaxed);
  }
  LightLayerSnapshot result;
  result.packed_ = std::move(page);
  return result;
}

LightLayerSnapshot LightLayerSnapshot::mixedPacked(
    const std::array<std::uint8_t, kLightLayerBytes> &packed) {
  auto page = std::make_shared<PackedPage>();
  for (std::size_t i = 0; i < kLightLayerBytes; ++i)
    page->bytes[i].store(packed[i], std::memory_order_relaxed);
  LightLayerSnapshot result;
  result.packed_ = std::move(page);
  return result;
}

void LightLayerSnapshot::makeMixed() {
  state_.store(0, std::memory_order_relaxed);
  packed_ = std::make_shared<PackedPage>();
  for (auto &byte : packed_->bytes)
    byte.store(0, std::memory_order_relaxed);
}

std::uint8_t LightLayerSnapshot::get(int x, int y, int z) const {
  const Storage current = storage();
  if (current == Storage::Missing)
    return 0;
  if (current == Storage::Uniform)
    return uniformValue();
  const std::size_t index = lightIndex(x, y, z);
  return static_cast<std::uint8_t>(
      (packedByte(index >> 1) >> ((index & 1) * 4)) & 15);
}

void LightLayerSnapshot::writeCell(std::size_t index, std::uint8_t value) {
  if (index >= kLightVolume || value > 15 || !packed_)
    throw std::invalid_argument("invalid mixed light cell");
  const std::size_t byte = index >> 1;
  const unsigned shift = static_cast<unsigned>((index & 1) * 4);
  auto &slot = packed_->bytes[byte];
  const std::uint8_t old = slot.load(std::memory_order_relaxed);
  const std::uint8_t mask = static_cast<std::uint8_t>(15u << shift);
  slot.store(
      static_cast<std::uint8_t>((old & ~mask) | ((value & 15u) << shift)),
      std::memory_order_relaxed);
}

void LightLayerSnapshot::writePackedByte(std::size_t byte,
                                         std::uint8_t value) {
  if (byte >= kLightLayerBytes || !packed_)
    throw std::invalid_argument("invalid mixed light byte");
  packed_->bytes[byte].store(value, std::memory_order_relaxed);
}

void LightLayerSnapshot::readPacked(
    std::array<std::uint8_t, kLightLayerBytes> &out) const {
  if (!packed_) {
    out.fill(0);
    return;
  }
  for (std::size_t i = 0; i < kLightLayerBytes; ++i)
    out[i] = packed_->bytes[i].load(std::memory_order_relaxed);
}

bool LightLayerSnapshot::operator==(const LightLayerSnapshot &other) const {
  const Storage mine = storage();
  if (mine != other.storage())
    return false;
  if (mine == Storage::Uniform)
    return uniformValue() == other.uniformValue();
  if (mine != Storage::Mixed)
    return true;
  for (std::size_t i = 0; i < kLightLayerBytes; ++i)
    if (packedByte(i) != other.packedByte(i))
      return false;
  return true;
}

void LightLayerRef::writeCell(std::size_t index, std::uint8_t value) const {
  if (index >= kLightVolume || value > 15 || !packed_)
    throw std::invalid_argument("invalid mixed light cell");
  const std::size_t byte = index >> 1;
  const unsigned shift = static_cast<unsigned>((index & 1) * 4);
  auto &slot = packed_->bytes[byte];
  const std::uint8_t old = slot.load(std::memory_order_relaxed);
  const std::uint8_t mask = static_cast<std::uint8_t>(15u << shift);
  slot.store(
      static_cast<std::uint8_t>((old & ~mask) | ((value & 15u) << shift)),
      std::memory_order_relaxed);
}

void LightLayerRef::writePackedByte(std::size_t byte,
                                    std::uint8_t value) const {
  if (byte >= kLightLayerBytes || !packed_)
    throw std::invalid_argument("invalid mixed light byte");
  packed_->bytes[byte].store(value, std::memory_order_relaxed);
}

void LightLayerRef::readPacked(
    std::array<std::uint8_t, kLightLayerBytes> &out) const {
  if (!packed_) {
    out.fill(0);
    return;
  }
  for (std::size_t i = 0; i < kLightLayerBytes; ++i)
    out[i] = packed_->bytes[i].load(std::memory_order_relaxed);
}

bool LightLayerRef::operator==(const LightLayerRef &other) const {
  const auto mine = storage();
  if (mine != other.storage())
    return false;
  if (mine == LightLayerSnapshot::Storage::Uniform)
    return uniformValue() == other.uniformValue();
  if (mine != LightLayerSnapshot::Storage::Mixed)
    return true;
  if (packed_ == other.packed_)
    return true;
  for (std::size_t i = 0; i < kLightLayerBytes; ++i)
    if (packedByte(i) != other.packedByte(i))
      return false;
  return true;
}

LightSectionSnapshot::LightSectionSnapshot(
    int section, bool isEmpty,
    std::array<LightLayerRef, kLightLayerCount> lightLayers,
    std::uint16_t unsupported)
    : sectionY(section), empty(isEmpty), layers(std::move(lightLayers)),
      unsupportedCells(unsupported) {}

int LightChunkSnapshot::lightValue(LightLayer layer, int x, int y,
                                   int z) const {
  const int sectionY = y >> 4;
  const LightSectionSnapshot *current = section(sectionY);
  if (!current)
    return 0;
  const auto layerIndex = static_cast<std::size_t>(layer);
  if (!current->layers[layerIndex].isMissing())
    return current->light(layer, x & 15, y & 15, z & 15);
  if (layer == LightLayer::Block || !skyLightEnabled)
    return 0;
  for (int aboveY = sectionY + 1;
       aboveY < minBlockSectionY + blockSectionCount + 1; ++aboveY) {
    const LightSectionSnapshot *above = section(aboveY);
    if (above && !above->layers[layerIndex].isMissing())
      return above->light(LightLayer::Sky, x & 15, 0, z & 15);
  }
  return 15;
}

} // namespace redstone
