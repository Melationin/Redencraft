#include "redstone/chunk_section.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace redstone {
namespace {

unsigned biomeBitsForPaletteSize(std::size_t size) noexcept {
  if (size <= 1)
    return 0;
  return static_cast<unsigned>(std::bit_width(size - 1));
}

std::uint8_t nextSectionBitsForWidth(std::uint8_t bits) noexcept {
  if (bits < 7)
    return static_cast<std::uint8_t>(bits + 1);
  if (bits == 7)
    return 8;
  return 16;
}

void updateSectionCounters(
    std::uint16_t &nonEmpty, std::uint16_t &tickingBlockCount,
    std::array<std::uint8_t, kRandomTickMinisectionCount> &minisections,
    int index, BlockState::Id previous, BlockState::Id state) {
  const bool previousAir = BlockState::fromId(previous).isAir();
  const bool stateAir = BlockState::fromId(state).isAir();
  std::uint16_t nextNonEmpty = nonEmpty;
  if (previousAir && !stateAir)
    ++nextNonEmpty;
  else if (!previousAir && stateAir)
    --nextNonEmpty;
  if (nextNonEmpty != nonEmpty)
    std::atomic_ref<std::uint16_t>(nonEmpty).store(nextNonEmpty,
                                                   std::memory_order_relaxed);
  const bool prevTicking = stateIsRandomlyTickingId(previous);
  const bool nextTicking = stateIsRandomlyTickingId(state);
  if (prevTicking != nextTicking) {
    const std::size_t mini =
        static_cast<std::size_t>(index) / kRandomTickMinisectionSize;
    if (nextTicking) {
      ++tickingBlockCount;
      ++minisections[mini];
    } else {
      --tickingBlockCount;
      --minisections[mini];
    }
  }
}

} // namespace

std::shared_ptr<SectionStorage>
makeSectionStorageForBits(std::uint8_t bits) {
  switch (bits) {
  case 1:
    return std::make_shared<SectionStoragePacked<1>>();
  case 2:
    return std::make_shared<SectionStoragePacked<2>>();
  case 3:
    return std::make_shared<SectionStoragePacked<3>>();
  case 4:
    return std::make_shared<SectionStoragePacked<4>>();
  case 5:
    return std::make_shared<SectionStoragePacked<5>>();
  case 6:
    return std::make_shared<SectionStoragePacked<6>>();
  case 7:
    return std::make_shared<SectionStoragePacked<7>>();
  case 8:
    return std::make_shared<SectionStorage8>();
  case 16:
    return std::make_shared<SectionStorage16>();
  default:
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// BiomePaletteContainer（Section::biomeData 的非拥有视图）
// ---------------------------------------------------------------------------
BiomePaletteContainer::BiomePaletteContainer(
    std::shared_ptr<SectionBiomes> &storage, std::string defaultBiome)
    : storage_(storage),
      defaultBiome_(defaultBiome.empty() ? std::string("minecraft:plains")
                                         : std::move(defaultBiome)) {}

SectionBiomes &BiomePaletteContainer::ensure() {
  if (!storage_) {
    storage_ = std::make_shared<SectionBiomes>();
    storage_->palette.push_back(defaultBiome_);
    storage_->indices.fill(0);
  }
  return *storage_;
}

const SectionBiomes *BiomePaletteContainer::peek() const {
  return storage_.get();
}

std::uint8_t BiomePaletteContainer::intern(std::string_view biomeId) {
  SectionBiomes &data = ensure();
  for (std::size_t i = 0; i < data.palette.size(); ++i) {
    if (data.palette[i] == biomeId)
      return static_cast<std::uint8_t>(i);
  }
  if (data.palette.size() >= kCellCount) {
    // 4x4x4 容器最多 64 个不同值；先按仍被引用的项压缩，再尝试复用。
    std::array<bool, kCellCount> used{};
    for (std::uint8_t idx : data.indices) {
      if (idx < data.palette.size())
        used[idx] = true;
    }
    std::array<std::uint8_t, kCellCount> remap{};
    std::vector<std::string> compacted;
    compacted.reserve(data.palette.size());
    for (std::size_t i = 0; i < data.palette.size(); ++i) {
      if (used[i]) {
        remap[i] = static_cast<std::uint8_t>(compacted.size());
        compacted.push_back(std::move(data.palette[i]));
      }
    }
    for (std::uint8_t &idx : data.indices)
      idx = remap[idx];
    data.palette = std::move(compacted);
    for (std::size_t i = 0; i < data.palette.size(); ++i) {
      if (data.palette[i] == biomeId)
        return static_cast<std::uint8_t>(i);
    }
  }
  const std::uint8_t id = static_cast<std::uint8_t>(data.palette.size());
  data.palette.emplace_back(biomeId);
  return id;
}

const std::string &BiomePaletteContainer::get(int qx, int qy, int qz) const {
  const SectionBiomes *data = peek();
  if (!data || data->palette.empty())
    return defaultBiome_;
  const std::uint8_t pIdx = data->indices[index(qx, qy, qz)];
  return data->palette[pIdx < data->palette.size() ? pIdx : 0];
}

void BiomePaletteContainer::set(int qx, int qy, int qz,
                                std::string_view biomeId) {
  SectionBiomes &data = ensure();
  const std::size_t cell = index(qx, qy, qz);
  const std::uint8_t prev = data.indices[cell];
  if (prev < data.palette.size() && data.palette[prev] == biomeId)
    return;
  data.indices[cell] = intern(biomeId);
}

void BiomePaletteContainer::fill(std::string_view biomeId) {
  storage_ = std::make_shared<SectionBiomes>();
  storage_->palette.emplace_back(
      biomeId.empty() ? std::string_view{"minecraft:plains"} : biomeId);
  storage_->indices.fill(0);
}

bool BiomePaletteContainer::isUniform() const {
  const SectionBiomes *data = peek();
  return !data || data->palette.size() <= 1;
}

const std::vector<std::string> &BiomePaletteContainer::palette() {
  return ensure().palette;
}

const std::array<std::uint8_t, BiomePaletteContainer::kCellCount> &
BiomePaletteContainer::indices() {
  return ensure().indices;
}

void BiomePaletteContainer::pack(std::vector<std::string> &outPalette,
                                 std::vector<std::uint64_t> &outData,
                                 int &outBits) const {
  outPalette.clear();
  outData.clear();
  const SectionBiomes *data = peek();
  if (!data || data->palette.empty()) {
    outPalette.push_back(defaultBiome_);
    outBits = 0;
    return;
  }

  std::array<std::int16_t, 256> remap{};
  remap.fill(-1);
  std::array<std::uint8_t, kCellCount> remappedCells{};
  for (std::size_t i = 0; i < kCellCount; ++i) {
    const std::uint8_t raw = data->indices[i];
    if (remap[raw] < 0) {
      remap[raw] = static_cast<std::int16_t>(outPalette.size());
      outPalette.push_back(
          raw < data->palette.size() ? data->palette[raw] : defaultBiome_);
    }
    remappedCells[i] = static_cast<std::uint8_t>(remap[raw]);
  }

  const unsigned bits = biomeBitsForPaletteSize(outPalette.size());
  outBits = static_cast<int>(bits);
  if (bits == 0)
    return;

  const std::size_t perLong = 64u / bits;
  const std::size_t longCount = (kCellCount + perLong - 1u) / perLong;
  outData.assign(longCount, 0ull);
  for (std::size_t i = 0; i < kCellCount; ++i) {
    const std::size_t wordIdx = i / perLong;
    const std::size_t bitOffset = (i % perLong) * bits;
    outData[wordIdx] |=
        static_cast<std::uint64_t>(remappedCells[i]) << bitOffset;
  }
}

bool BiomePaletteContainer::unpack(std::span<const std::string> palette,
                                   std::span<const std::uint64_t> data,
                                   std::string &error) {
  if (palette.empty() || palette.size() > kCellCount) {
    error = "biomes palette size must be in 1..64";
    return false;
  }
  const unsigned bits = biomeBitsForPaletteSize(palette.size());
  if (bits == 0) {
    if (!data.empty()) {
      error = "single biome palette has unexpected data";
      return false;
    }
    ensure().palette.assign(palette.begin(), palette.end());
    storage_->indices.fill(0);
    return true;
  }
  const std::size_t perLong = 64u / bits;
  const std::size_t expectedLongs = (kCellCount + perLong - 1u) / perLong;
  if (data.size() != expectedLongs) {
    error = "biomes palette data length is invalid";
    return false;
  }
  const std::uint64_t mask = (std::uint64_t{1} << bits) - 1ull;
  std::array<std::uint8_t, kCellCount> parsed{};
  for (std::size_t i = 0; i < kCellCount; ++i) {
    const std::size_t wordIdx = i / perLong;
    const std::size_t bitOffset = (i % perLong) * bits;
    const std::size_t val =
        static_cast<std::size_t>((data[wordIdx] >> bitOffset) & mask);
    if (val >= palette.size()) {
      error = "biomes palette index is out of range";
      return false;
    }
    parsed[i] = static_cast<std::uint8_t>(val);
  }
  ensure().palette.assign(palette.begin(), palette.end());
  storage_->indices = parsed;
  return true;
}

// ---------------------------------------------------------------------------
// Section
// ---------------------------------------------------------------------------
BlockState::Id BlockSection::get(int index) const {
  const SectionStorage *base = storage.get();
  if (!base)
    return single;
  const auto cell = static_cast<std::size_t>(index);
  return visitSectionStorage(base, [cell](const auto *block) {
    return block->palette[block->indexAt(cell)];
  });
}

bool BlockSection::tryGetRandomlyTicking(int index, BlockState::Id &outId) const {
  const SectionStorage *base = storage.get();
  if (!base) {
    if (!stateIsRandomlyTickingId(single))
      return false;
    outId = single;
    return true;
  }
  const auto cell = static_cast<std::size_t>(index);
  return visitSectionStorage(base, [cell, &outId](const auto *block) {
    const auto idx = block->indexAt(cell);
    if (!block->paletteTicking[idx])
      return false;
    outId = block->palette[idx];
    return true;
  });
}

void BlockSection::initSingleRandomTick(BlockState::Id state) {
  const bool ticking = stateIsRandomlyTickingId(state);
  tickingBlockCount = ticking ? static_cast<std::uint16_t>(kLightVolume) : 0;
  if (ticking) {
    randomTickByMinisection.fill(
        static_cast<std::uint8_t>(kRandomTickMinisectionSize));
    randomTickByMinisection.back() =
        static_cast<std::uint8_t>(kRandomTickLastMinisectionSize);
  } else {
    randomTickByMinisection.fill(0);
  }
}

BlockState::Id BlockSection::set(int index, BlockState::Id state, bool compact,
                            std::shared_mutex *resizeMutex) {
  const auto previous = get(index);
  if (previous == state)
    return previous;
  const auto cell = static_cast<std::size_t>(index);
  SectionStorage *base = storage.get();
  if (!base) {
    auto next = makeSectionStorageForBits(compact ? 1 : 8);
    visitSectionStorage(next.get(), [&](auto *block) {
      block->palette[0] = single;
      block->paletteTicking[0] = stateIsRandomlyTickingId(single) ? 1 : 0;
      block->palette[1] = state;
      block->paletteTicking[1] = stateIsRandomlyTickingId(state) ? 1 : 0;
      block->paletteSize = 2;
      block->setIndexAt(cell, 1);
    });
    if (resizeMutex) {
      std::unique_lock lock(*resizeMutex);
      storage = std::move(next);
    } else {
      storage = std::move(next);
    }
  } else {
    visitSectionStorage(base, [&](auto *block) {
      using BlockT = std::remove_pointer_t<decltype(block)>;
      const auto end = block->palette.begin() + block->paletteSize;
      auto found = std::find(block->palette.begin(), end, state);
      std::uint16_t targetIdx = 0;
      if (found == end) {
        if (block->paletteSize < BlockT::kPaletteCapacity) {
          targetIdx = block->paletteSize;
          std::atomic_ref<BlockState::Id>(block->palette[targetIdx])
              .store(state, std::memory_order_release);
          block->paletteTicking[targetIdx] =
              stateIsRandomlyTickingId(state) ? 1 : 0;
          std::atomic_ref<std::uint16_t>(block->paletteSize)
              .store(static_cast<std::uint16_t>(targetIdx + 1),
                     std::memory_order_release);
        } else {
          if constexpr (std::is_same_v<BlockT, SectionStorage16>) {
            throw std::logic_error("section palette overflow");
          } else {
            auto nextStorage =
                makeSectionStorageForBits(nextSectionBitsForWidth(block->bits));
            visitSectionStorage(nextStorage.get(), [&](auto *wider) {
              std::copy(block->palette.begin(), end, wider->palette.begin());
              std::copy(block->paletteTicking.begin(),
                        block->paletteTicking.begin() + block->paletteSize,
                        wider->paletteTicking.begin());
              wider->palette[block->paletteSize] = state;
              wider->paletteTicking[block->paletteSize] =
                  stateIsRandomlyTickingId(state) ? 1 : 0;
              wider->paletteSize =
                  static_cast<std::uint16_t>(block->paletteSize + 1);
              for (std::size_t i = 0; i < kLightVolume; ++i)
                wider->setIndexAt(i, block->indexAt(i));
              wider->setIndexAt(cell, block->paletteSize);
            });
            if (resizeMutex) {
              std::unique_lock lock(*resizeMutex);
              storage = std::move(nextStorage);
            } else {
              storage = std::move(nextStorage);
            }
            return;
          }
        }
      } else {
        targetIdx = static_cast<std::uint16_t>(found - block->palette.begin());
      }
      if (resizeMutex)
        block->atomicSetIndexAt(cell, targetIdx);
      else
        block->setIndexAt(cell, targetIdx);
    });
  }
  updateSectionCounters(nonEmpty, tickingBlockCount, randomTickByMinisection,
                        index, previous, state);
  return previous;
}

BiomePaletteContainer BaseChunk::biomes(int sectionIndex,
                                       std::string_view defaultBiome) {
  const int index =
      sectionIndex >= 0 && sectionIndex < sectionCount() ? sectionIndex : 0;
  return BiomePaletteContainer(biomeSections[static_cast<std::size_t>(index)],
                               std::string(defaultBiome));
}

BlockState BaseChunk::getBlockState(int x, int y, int z) const {
  if (isOutsideBuildHeight(y))
    return defaultState("void_air");
  const BlockSection &sec =
      sections[static_cast<std::size_t>(sectionIndex(y))];
  if (sec.hasOnlyAir())
    return BlockState{};
  return sec.getBlockState(x & 15, y & 15, z & 15);
}

FluidState BaseChunk::getFluidState(int x, int y, int z) const {
  if (isOutsideBuildHeight(y))
    return FluidState{};
  const BlockSection &sec =
      sections[static_cast<std::size_t>(sectionIndex(y))];
  if (sec.hasOnlyAir())
    return FluidState{};
  return sec.getFluidState(x & 15, y & 15, z & 15);
}

} // namespace redstone
