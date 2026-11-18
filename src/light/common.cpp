#include "light/common.hpp"

#include "redstone/chunk_section.hpp"

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

bool LightEngineHost::hasChunkGeometry(std::uint64_t key) const {
  const auto it = directChunks.find(key);
  return it != directChunks.end() &&
         (it->second.chunk != nullptr || directWorld.context != nullptr);
}

LightSectionView sectionViewOfLiveChunk(const BaseChunk &chunk, int sectionY,
                                        int minBlockSectionY,
                                        int blockSectionCount) {
  LightSectionView view;
  const int index = sectionY - minBlockSectionY;
  if (index < 0 || index >= blockSectionCount ||
      index >= chunk.sectionCount())
    return view;
  const BlockSection &section = chunk.sections[static_cast<std::size_t>(index)];
  const std::uint16_t nonEmpty =
      std::atomic_ref<std::uint16_t>(
          const_cast<std::uint16_t &>(section.nonEmpty))
          .load(std::memory_order_relaxed);
  view.empty = nonEmpty == 0;
  const SectionStorage *base = section.storage.get();
  if (!base) {
    view.bits = 0;
    view.single = section.single;
    return view;
  }
  const std::uint16_t paletteSize =
      std::atomic_ref<std::uint16_t>(
          const_cast<std::uint16_t &>(base->paletteSize))
          .load(std::memory_order_acquire);
  view.bits = base->bits;
  view.storageOwner = section.storage;
  visitSectionStorage(base, [&](const auto *typed) {
    view.palette =
        std::span<const BlockState::Id>(typed->palette.data(), paletteSize);
    using BlockT =
        std::remove_cv_t<std::remove_pointer_t<decltype(typed)>>;
    if constexpr (std::is_same_v<BlockT, SectionStorage8>) {
      view.cells8 = std::span<const std::uint8_t>(typed->indices.data(),
                                                  typed->indices.size());
    } else if constexpr (std::is_same_v<BlockT, SectionStorage16>) {
      view.cells16 = std::span<const std::uint16_t>(typed->indices.data(),
                                                    typed->indices.size());
    } else {
      view.packedWords = std::span<const std::uint64_t>(typed->words.data(),
                                                        typed->words.size());
    }
  });
  return view;
}

const LightEngineHost::DirectSectionCacheEntry *
LightEngineHost::lookupDirectSectionCache(std::uint64_t key,
                                          int sectionY) const {
  const int index = sectionY - minBlockSectionY;
  if (index < 0 || index >= blockSectionCount)
    return nullptr;
  const std::size_t slot =
      (chunkL1Slot(key) ^ (static_cast<std::size_t>(sectionY) * 29u)) & 63u;
  DirectSectionCacheEntry &entry = directSectionCache_[slot];
  if (entry.valid && entry.key == key && entry.sectionY == sectionY)
    return entry.present ? &entry : nullptr;
  entry.key = key;
  entry.sectionY = sectionY;
  entry.valid = true;
  const auto metaIt = directChunks.find(key);
  if (metaIt == directChunks.end()) {
    entry.present = false;
    entry.view = LightSectionView{};
    return nullptr;
  }
  if (metaIt->second.chunk) {
    entry.present = true;
    entry.view = sectionViewOfLiveChunk(*metaIt->second.chunk, sectionY,
                                        minBlockSectionY, blockSectionCount);
    return &entry;
  }
  if (!directWorld.context) {
    entry.present = false;
    entry.view = LightSectionView{};
    return nullptr;
  }
  entry.present = true;
  entry.view = directWorld.readSection(
      directWorld.context, metaIt->second.chunkX, metaIt->second.chunkZ,
      sectionY);
  return &entry;
}

std::optional<LightSectionView>
LightEngineHost::directSectionView(std::uint64_t key, int sectionY) const {
  const auto *entry = lookupDirectSectionCache(key, sectionY);
  if (!entry)
    return std::nullopt;
  return entry->view;
}

std::optional<bool>
LightEngineHost::sectionNonEmpty(std::uint64_t key, int sectionY) const {
  if (const auto *entry = lookupDirectSectionCache(key, sectionY))
    return !entry->view.empty;
  return std::nullopt;
}

const BlockLightProperties *
LightEngineHost::sectionPropertiesAt(std::uint64_t key, int x, int y,
                                     int z) const {
  if (y < minBlockSectionY * 16 ||
      y >= (minBlockSectionY + blockSectionCount) * 16)
    return &emptyLightProperties();
  const int sectionY = y >> 4;
  if (const auto *entry = lookupDirectSectionCache(key, sectionY)) {
    const BlockState::Id state = entry->view.atomicStateAt(lightIndex(x, y, z));
    return &propertiesForState(state);
  }
  return nullptr;
}

const BlockLightProperties &
LightEngineHost::blockPropertiesAt(int x, int y, int z,
                                   bool &incomplete) const {
  const BlockLightProperties *props =
      sectionPropertiesAt(chunkKey(x >> 4, z >> 4), x, y, z);
  if (!props)
    return unsupportedLightProperties();
  if (!props->supported)
    incomplete = true;
  return *props;
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
