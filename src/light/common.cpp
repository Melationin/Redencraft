#include "light/common.hpp"

#include <limits>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

namespace redstone::light_internal {

double currentThreadCpuMs() {
#ifdef _WIN32
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
    return 0;
  const auto ticks = [](const FILETIME &value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
           value.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#elif defined(__linux__)
  timespec time{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0)
    return 0;
  return static_cast<double>(time.tv_sec) * 1000.0 + time.tv_nsec / 1.0e6;
#else
  return 0;
#endif
}

#if defined(_WIN32) || defined(__linux__)
ThreadAffinityResult applyThreadAffinity(std::thread::native_handle_type handle,
                                         std::uintptr_t mask) {
#ifdef _WIN32
  if (!mask)
    return {};
  const HANDLE thread = static_cast<HANDLE>(handle);
  GROUP_AFFINITY current{};
  if (!GetThreadGroupAffinity(thread, &current))
    return {0, GetLastError()};
  GROUP_AFFINITY requested = current;
  requested.Mask = static_cast<KAFFINITY>(mask);
  if (!SetThreadGroupAffinity(thread, &requested, nullptr))
    return {0, GetLastError()};
  GROUP_AFFINITY group{};
  return {GetThreadGroupAffinity(thread, &group)
              ? static_cast<std::uint64_t>(group.Mask)
              : static_cast<std::uint64_t>(mask),
          0};
#else
  if (!mask)
    return {};
  cpu_set_t requested;
  CPU_ZERO(&requested);
  for (std::size_t cpu = 0; cpu < std::numeric_limits<std::uintptr_t>::digits;
       ++cpu)
    if ((mask >> cpu) & 1)
      CPU_SET(cpu, &requested);
  if (const int error =
          pthread_setaffinity_np(handle, sizeof(requested), &requested))
    return {0, static_cast<std::uint32_t>(error)};
  cpu_set_t actual;
  CPU_ZERO(&actual);
  if (const int error = pthread_getaffinity_np(handle, sizeof(actual), &actual))
    return {0, static_cast<std::uint32_t>(error)};
  std::uint64_t actualMask = 0;
  for (std::size_t cpu = 0; cpu < std::numeric_limits<std::uint64_t>::digits;
       ++cpu)
    if (CPU_ISSET(cpu, &actual))
      actualMask |= std::uint64_t{1} << cpu;
  return {actualMask, 0};
#endif
}
#endif

const BlockLightProperties &emptyLightProperties() {
  static const BlockLightProperties value = [] {
    BlockLightProperties properties;
    properties.supported = true;
    return properties;
  }();
  return value;
}

LightSectionSnapshot *WorkingChunk::sectionForEdit(int sectionY) {
  if (!snapshot)
    return nullptr;
  const auto index = vectorIndex(snapshot->sections, sectionY);
  if (index >= snapshot->sections.size())
    return nullptr;
  mutableLayerSnapshot(snapshot);
  auto &slot = snapshot->sections[index];
  if (!slot)
    return nullptr;
  if (slot.use_count() > 1)
    slot = std::make_shared<LightSectionSnapshot>(*slot);
  return const_cast<LightSectionSnapshot *>(slot.get());
}

LightValues::LightValues() = default;

LightValues LightValues::fromLayer(const LightLayerRef &layer) {
  LightValues result;
  result.layer_ = layer;
  return result;
}

void LightValues::fill(std::uint8_t value) {
  const std::uint8_t clamped = value & 15u;
  if (layer_.isUniform() && layer_.uniformValue() == clamped)
    return;
  layer_ = LightLayerSnapshot::uniform(clamped);
}

void LightValues::fillInheritedMixed(
    const std::array<std::uint8_t, 256> &cols) {
  std::array<std::uint8_t, 128> row{};
  for (int byte = 0; byte < 128; ++byte) {
    const int base = byte << 1;
    row[static_cast<std::size_t>(byte)] = static_cast<std::uint8_t>(
        (cols[static_cast<std::size_t>(base)] & 15u) |
        ((cols[static_cast<std::size_t>(base + 1)] & 15u) << 4));
  }
  layer_ = LightLayerSnapshot::mixedEmpty();
  for (int y = 0; y < 16; ++y) {
    const std::size_t planeOffset = static_cast<std::size_t>(y) << 7;
    for (int byte = 0; byte < 128; ++byte)
      layer_.writePackedByte(planeOffset + static_cast<std::size_t>(byte),
                             row[static_cast<std::size_t>(byte)]);
  }
}

std::optional<std::uint8_t> LightValues::uniformValue() const {
  if (layer_.isMissing())
    return std::nullopt;
  if (layer_.isUniform())
    return layer_.uniformValue();
  const auto &page = layer_.shared();
  if (!page)
    return std::nullopt;
  const std::uint8_t firstByte = page->bytes[0].load(std::memory_order_relaxed);
  const std::uint8_t lo = static_cast<std::uint8_t>(firstByte & 15u);
  const std::uint8_t hi = static_cast<std::uint8_t>(firstByte >> 4);
  if (lo != hi)
    return std::nullopt;
  for (std::size_t i = 1; i < kLightLayerBytes; ++i) {
    if (page->bytes[i].load(std::memory_order_relaxed) != firstByte)
      return std::nullopt;
  }
  return lo;
}

void LightValues::ensureMixed() {
  if (layer_.isUniform()) {
    const std::uint8_t fillValue = layer_.uniformValue();
    LightLayerRef mixed = LightLayerSnapshot::mixedEmpty();
    const std::uint8_t packed =
        static_cast<std::uint8_t>((fillValue << 4) | (fillValue & 15));
    for (std::size_t byte = 0; byte < kLightLayerBytes; ++byte)
      mixed.writePackedByte(byte, packed);
    layer_ = std::move(mixed);
  } else if (layer_.isMissing()) {
    layer_ = LightLayerSnapshot::mixedEmpty();
  }
}

const GeometrySection *LightEngineHost::geometrySection(std::uint64_t key,
                                                        int sectionY) const {
  const GeometryChunk *chunk = geometryChunk(key);
  if (!chunk)
    return nullptr;
  const int index = sectionY - minBlockSectionY;
  if (index < 0 || index >= blockSectionCount)
    return nullptr;
  return &chunk->sections[static_cast<std::size_t>(index)];
}

const BlockLightProperties &
LightEngineHost::blockPropertiesAt(int x, int y, int z,
                                   bool &incomplete) const {
  if (y < minBlockSectionY * 16 ||
      y >= (minBlockSectionY + blockSectionCount) * 16)
    return emptyLightProperties();
  const GeometrySection *geometry =
      geometrySection(chunkKey(x >> 4, z >> 4), y >> 4);
  if (!geometry)
    return unsupportedLightProperties();
  const std::uint16_t classId =
      geometry->uniform ? geometry->uniformClass
                        : geometry->at(lightIndex(x, y, z));
  const auto &properties = opticalClass(classId).properties;
  if (!properties.supported)
    incomplete = true;
  return properties;
}

const std::shared_ptr<LightChunkSnapshot> &
LightEngineHost::storedLayerSnapshot(LightLayer layer,
                                     std::uint64_t key) const {
  static const std::shared_ptr<LightChunkSnapshot> empty;
  const auto &source =
      layer == LightLayer::Block ? lastReadyBlock : lastReadySky;
  const auto found = source.find(key);
  return found == source.end() ? empty : found->second;
}

} // namespace redstone::light_internal
