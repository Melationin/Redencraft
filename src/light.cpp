#include "redstone/light.hpp"

#include <algorithm>
#include <bit>
#include <array>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

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

#include "light_metadata.inc"
#include "light_occlusion.inc"

const BlockLightProperties kUnsupportedLightProperties{};

struct ThreadAffinityResult {
  std::uint64_t mask = 0;
  std::uint32_t error = 0;
};

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
ThreadAffinityResult applyThreadAffinity(std::thread::native_handle_type threadHandle,
                                         std::uintptr_t mask) {
#ifdef _WIN32
  if (!mask)
    return {};
  const HANDLE thread = static_cast<HANDLE>(threadHandle);
  GROUP_AFFINITY current{};
  if (!GetThreadGroupAffinity(thread, &current))
    return {0, GetLastError()};
  GROUP_AFFINITY requested = current;
  requested.Mask = static_cast<KAFFINITY>(mask);
  if (!SetThreadGroupAffinity(thread, &requested, nullptr)) {
    return {0, GetLastError()};
  }
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
  for (std::size_t cpu = 0; cpu < std::numeric_limits<std::uintptr_t>::digits; ++cpu)
    if ((mask >> cpu) & 1)
      CPU_SET(cpu, &requested);
  if (const int error =
          pthread_setaffinity_np(threadHandle, sizeof(requested), &requested))
    return {0, static_cast<std::uint32_t>(error)};
  cpu_set_t actual;
  CPU_ZERO(&actual);
  if (const int error =
          pthread_getaffinity_np(threadHandle, sizeof(actual), &actual))
    return {0, static_cast<std::uint32_t>(error)};
  std::uint64_t actualMask = 0;
  for (std::size_t cpu = 0; cpu < std::numeric_limits<std::uint64_t>::digits; ++cpu)
    if (CPU_ISSET(cpu, &actual))
      actualMask |= std::uint64_t{1} << cpu;
  return {actualMask, 0};
#endif
}
#endif

std::uint64_t chunkKey(int x, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
         static_cast<std::uint32_t>(z);
}

bool validLightValue(std::uint8_t value) { return value <= 15; }

constexpr int kFaceScale = 1'000'000;

bool emptyShape(const BlockLightProperties &properties) {
  return !properties.supported || !properties.canOcclude ||
         !properties.useShapeForLightOcclusion;
}

const LightFaceRect *faceRect(const BlockLightProperties &properties,
                              int direction) {
  if (properties.occlusionShapeId >= std::size(kLightFaceShapes))
    return nullptr;
  const LightFaceShape &shape = kLightFaceShapes[properties.occlusionShapeId];
  if ((shape.present & (1u << direction)) == 0)
    return nullptr;
  switch (direction) {
  case 0: return &shape.down;
  case 1: return &shape.up;
  case 2: return &shape.north;
  case 3: return &shape.south;
  case 4: return &shape.west;
  default: return &shape.east;
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

int lightOpacity(const BlockLightProperties &properties) {
  return properties.supported ? std::max(1, int(properties.dampening)) : 16;
}

const BlockLightProperties &emptyLightProperties() {
  static const BlockLightProperties value = [] {
    BlockLightProperties properties;
    properties.supported = true;
    return properties;
  }();
  return value;
}

struct LightPos {
  int x = 0;
  int y = 0;
  int z = 0;
  bool operator==(const LightPos &) const = default;
};

struct LightPosHash {
  std::size_t operator()(const LightPos &position) const noexcept {
    std::uint64_t value = static_cast<std::uint32_t>(position.x);
    value = value * 0x9E3779B185EBCA87ull ^
            static_cast<std::uint32_t>(position.y);
    value = value * 0xC2B2AE3D27D4EB4Full ^
            static_cast<std::uint32_t>(position.z);
    return static_cast<std::size_t>(value ^ (value >> 32));
  }
};

template <class T> class SpscRing {
public:
  explicit SpscRing(std::size_t capacity)
      : slots_(std::max<std::size_t>(capacity, 2)) {}

  bool push(T value) {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= slots_.size())
      return false;
    slots_[static_cast<std::size_t>(head % slots_.size())] =
        std::move(value);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  const T *front() const {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto head = head_.load(std::memory_order_acquire);
    if (tail == head)
      return nullptr;
    return &slots_[static_cast<std::size_t>(tail % slots_.size())];
  }

  bool pop(T &value) {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto head = head_.load(std::memory_order_acquire);
    if (tail == head)
      return false;
    value = std::move(slots_[static_cast<std::size_t>(tail % slots_.size())]);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }
  bool full() const {
    return head_.load(std::memory_order_relaxed) -
               tail_.load(std::memory_order_acquire) >=
           slots_.size();
  }
  std::size_t size() const {
    return static_cast<std::size_t>(
        head_.load(std::memory_order_acquire) -
        tail_.load(std::memory_order_acquire));
  }

private:
  std::vector<T> slots_;
  std::atomic<std::uint64_t> head_{0};
  std::atomic<std::uint64_t> tail_{0};
};

// SPSC queue whose logical capacity grows by appending fixed-size segments.
// The producer owns write_ and links a fully allocated next segment with a
// release store. The consumer follows that link with an acquire load and frees
// only segments it has drained. Existing slots never move, so growth does not
// copy queued values and does not add a mutex to the per-record fast path.
template <class T> class GrowableSpscRing {
  struct Segment {
    explicit Segment(std::size_t capacity) : slots(capacity) {}
    std::vector<T> slots;
    std::atomic<std::size_t> head{0};
    std::atomic<std::size_t> tail{0};
    std::atomic<Segment *> next{nullptr};
  };

public:
  explicit GrowableSpscRing(std::size_t capacity)
      : read_(new Segment(std::max<std::size_t>(capacity, 2))),
        write_(read_) {}
  ~GrowableSpscRing() {
    Segment *segment = read_;
    while (segment) {
      Segment *next = segment->next.load(std::memory_order_acquire);
      delete segment;
      segment = next;
    }
  }
  GrowableSpscRing(const GrowableSpscRing &) = delete;
  GrowableSpscRing &operator=(const GrowableSpscRing &) = delete;

  bool push(T value) {
    Segment *segment = write_;
    auto head = segment->head.load(std::memory_order_relaxed);
    if (head == segment->slots.size()) {
      Segment *next = new Segment(segment->slots.size());
      write_ = next;
      segment->next.store(next, std::memory_order_release);
      segment = next;
      head = 0;
    }
    segment->slots[head] = std::move(value);
    segment->head.store(head + 1, std::memory_order_release);
    published_.store(published_.load(std::memory_order_relaxed) + 1,
                     std::memory_order_release);
    return true;
  }

  const T *front() {
    for (;;) {
      Segment *segment = read_;
      const auto tail = segment->tail.load(std::memory_order_relaxed);
      const auto head = segment->head.load(std::memory_order_acquire);
      if (tail != head)
        return &segment->slots[tail];
      Segment *next = segment->next.load(std::memory_order_acquire);
      if (!next)
        return nullptr;
      read_ = next;
      delete segment;
    }
  }

  bool pop(T &value) {
    if (!front())
      return false;
    Segment *segment = read_;
    const auto tail = segment->tail.load(std::memory_order_relaxed);
    value = std::move(segment->slots[tail]);
    segment->tail.store(tail + 1, std::memory_order_release);
    consumed_.store(consumed_.load(std::memory_order_relaxed) + 1,
                    std::memory_order_release);
    return true;
  }

  bool empty() const { return size() == 0; }
  std::size_t size() const {
    // Read the consumer first so a concurrent producer can only make the
    // result larger, never underflow the unsigned subtraction.
    const auto consumed = consumed_.load(std::memory_order_acquire);
    const auto published = published_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(published - consumed);
  }

private:
  Segment *read_ = nullptr;
  Segment *write_ = nullptr;
  std::atomic<std::uint64_t> published_{0};
  std::atomic<std::uint64_t> consumed_{0};
};

template <class T> class TaskQueue {
public:
  TaskQueue() { values_.reserve(4096); }
  bool empty() const { return head_ == values_.size(); }
  const T &front() const { return values_[head_]; }
  void push(T value) { values_.push_back(std::move(value)); }
  T pop() {
    T result = std::move(values_[head_++]);
    if (head_ >= 4096 && head_ * 2 >= values_.size()) {
      values_.erase(values_.begin(), values_.begin() +
                                           static_cast<std::ptrdiff_t>(head_));
      head_ = 0;
    }
    return result;
  }

private:
  std::vector<T> values_;
  std::size_t head_ = 0;
};

struct GeometryInput {
  std::uint64_t sequence = 0;
  LightCellUpdate update;
};
static_assert(sizeof(GeometryInput) == 32);

struct SectionPos3 {
  int x = 0;
  int y = 0;
  int z = 0;
  bool operator==(const SectionPos3 &) const = default;
};

struct SectionPos3Hash {
  std::size_t operator()(const SectionPos3 &position) const noexcept {
    std::uint64_t value = static_cast<std::uint32_t>(position.x);
    value = value * 0x9E3779B185EBCA87ull ^
            static_cast<std::uint32_t>(position.y);
    value = value * 0xC2B2AE3D27D4EB4Full ^
            static_cast<std::uint32_t>(position.z);
    return static_cast<std::size_t>(value ^ (value >> 32));
  }
};

inline constexpr std::array<LightPos, 6> kDirectionOffsets{{
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};

LightPos offset(LightPos position, int direction) {
  const LightPos delta = kDirectionOffsets[static_cast<std::size_t>(direction)];
  return {position.x + delta.x, position.y + delta.y, position.z + delta.z};
}

template <class T>
std::size_t vectorIndex(const std::vector<T> &values, int sectionY) {
  if (values.empty())
    return values.size();
  const int minY = values.front()->sectionY;
  const int index = sectionY - minY;
  return index >= 0 && static_cast<std::size_t>(index) < values.size()
             ? static_cast<std::size_t>(index)
             : values.size();
}

} // namespace

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

std::uint8_t
LightOpticalTable::classForState(BlockState::Id state) const noexcept {
  return state < stateClasses_.size() ? stateClasses_[state] : 0;
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

  // This palette is temporary and private to the conversion. It maps a World
  // palette index to the compact optical-class id.
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
  if (!validLightValue(value))
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

LightLayerSnapshot LightLayerSnapshot::mixed(
    const std::array<std::uint8_t, kLightVolume> &values) {
  LightLayerSnapshot result = mixedEmpty();
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!validLightValue(values[i]))
      throw std::invalid_argument("light value must be in 0..15");
    result.writeCell(i, values[i]);
  }
  return result;
}

LightLayerSnapshot LightLayerSnapshot::mixedPacked(
    const std::array<std::uint8_t, kLightLayerBytes> &packed) {
  LightLayerSnapshot result = mixedEmpty();
  for (std::size_t i = 0; i < packed.size(); ++i)
    result.writePackedByte(i, packed[i]);
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
  slot.store(static_cast<std::uint8_t>((old & ~mask) |
                                      ((value & 15u) << shift)),
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

const BlockLightProperties &unsupportedLightProperties() noexcept {
  return kUnsupportedLightProperties;
}

BlockLightProperties officialLightProperties(
    std::string_view blockName, std::string_view properties) {
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
      // The optical scalar values are known, but propagation must explicitly
      // report the missing directional geometry rather than treating it as air.
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
  // Unknown imported states cannot be proven equivalent. Report a change so a
  // future propagation stage must re-check instead of silently retaining stale
  // light. Exact Java conditions below remain independent of that fallback.
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
  return faceShapesOcclude(from, to, static_cast<int>(direction));
}

LightSectionSnapshot::LightSectionSnapshot(
    int section, bool is_empty, std::vector<BlockLightProperties> lightPalette,
    std::vector<std::uint16_t> lightCells,
    std::array<LightLayerRef, kLightLayerCount> lightLayers)
    : sectionY(section), empty(is_empty), palette(std::move(lightPalette)),
      cells(std::move(lightCells)), layers(std::move(lightLayers)) {
  if (palette.empty())
    throw std::invalid_argument("light section needs a palette");
  if (!cells.empty() && cells.size() != kLightVolume)
    throw std::invalid_argument(
        "light section needs zero or exactly 4096 cells");
  if (cells.empty() && palette.size() != 1)
    throw std::invalid_argument(
        "uniform light section needs exactly one palette value");
  if (cells.empty()) {
    if (!palette.front().supported)
      unsupportedCells = kLightVolume;
    return;
  }
  for (std::uint16_t index : cells) {
    if (index >= palette.size())
      throw std::invalid_argument("light section cell has invalid palette index");
  }
  std::vector<std::uint16_t> counts(palette.size(), 0);
  for (std::uint16_t index : cells)
    ++counts[index];
  for (std::size_t i = 0; i < palette.size(); ++i)
    if (!palette[i].supported)
      unsupportedCells += counts[i];
}

const BlockLightProperties &LightSectionSnapshot::at(int x, int y,
                                                     int z) const {
  const auto index = cells.empty() ? 0 : cells.at(lightIndex(x, y, z));
  return palette.at(index);
}

std::uint8_t LightSectionSnapshot::light(LightLayer layer, int x, int y,
                                         int z) const {
  return layers.at(static_cast<std::size_t>(layer)).get(x, y, z);
}

const LightSectionSnapshot *LightChunkInput::section(int sectionY) const {
  if (sections.empty() || !sections.front())
    return nullptr;
  const auto index = vectorIndex(sections, sectionY);
  return index < sections.size() ? sections[index].get() : nullptr;
}

const LightSectionSnapshot *LightChunkSnapshot::section(int sectionY) const {
  if (sections.empty() || !sections.front())
    return nullptr;
  const auto index = vectorIndex(sections, sectionY);
  return index < sections.size() ? sections[index].get() : nullptr;
}

int LightChunkSnapshot::lightValue(LightLayer layer, int x, int y, int z) const {
  if (sections.empty()) {
    const LightChunkSnapshot *view =
        layer == LightLayer::Block ? blockLayerSnapshot.get()
                                   : skyLayerSnapshot.get();
    if (!view)
      return layer == LightLayer::Block ? 0 : (skyLightEnabled ? 15 : 0);
    const int sectionY = y >> 4;
    const auto layerIndex = static_cast<std::size_t>(layer);
    const LightSectionSnapshot *current = view->section(sectionY);
    if (!current)
      return 0;
    if (!current->layers[layerIndex].isMissing())
      return current->light(layer, x & 15, y & 15, z & 15);
    if (layer == LightLayer::Block)
      return 0;
    if (!skyLightEnabled)
      return 0;
    for (int aboveY = sectionY + 1;
         aboveY < minBlockSectionY + blockSectionCount + 1; ++aboveY) {
      const LightSectionSnapshot *above = view->section(aboveY);
      if (above && !above->layers[layerIndex].isMissing())
        return above->light(LightLayer::Sky, x & 15, 0, z & 15);
    }
    return 15;
  }
  const int sectionY = y >> 4;
  const LightSectionSnapshot *current = section(sectionY);
  if (!current)
    return 0;
  const auto layerIndex = static_cast<std::size_t>(layer);
  if (!current->layers[layerIndex].isMissing())
    return current->light(layer, x & 15, y & 15, z & 15);
  if (layer == LightLayer::Block)
    return 0;
  if (!skyLightEnabled)
    return 0;
  for (int aboveY = sectionY + 1;
       aboveY < minBlockSectionY + blockSectionCount + 1; ++aboveY) {
    const LightSectionSnapshot *above = section(aboveY);
    if (above && !above->layers[layerIndex].isMissing())
      return above->light(LightLayer::Sky, x & 15, 0, z & 15);
  }
  return 15;
}

class LightService::Impl {
  friend class LightService;

  struct OwnedCell {
    std::uint64_t sequence = 0;
    BlockState::Id state = 0;
  };
  struct SectionStatusValue {
    std::uint64_t sequence = 0;
    bool empty = false;
  };
  struct CoalescedCell {
    std::uint16_t localIndex = 0;
    OwnedCell value;
  };
  struct CoalescedSection {
    std::uint64_t chunkKey = 0;
    int sectionY = 0;
    std::uint64_t sectionSequence = 0;
    std::vector<CoalescedCell> cells;
  };

  struct PendingChunk {
    std::uint64_t key = 0;
    std::uint64_t generation = 0;
    // Generation of the newest full authoritative snapshot. Every record at or
    // below this sequence is already represented by that snapshot.
    std::uint64_t inputSequence = 0;
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t revision = 0;
    std::unique_ptr<LightChunkInput> load;
    bool unload = false;
    std::unordered_map<std::uint32_t, LightBlockChange> changes;
    // Owned hot-path hand-off: one latest value per dirty cell. A full ring
    // merges here under the same sequence ordering as the normal worker drain.
    std::unordered_map<std::uint32_t, OwnedCell> ownedCells;
    std::unordered_map<int, SectionStatusValue> sectionStatus;
  };

  struct WorkingChunk {
    std::uint64_t key = 0;
    std::uint64_t lifecycleEpoch = 0;
    int chunkX = 0;
    int chunkZ = 0;
    std::shared_ptr<LightChunkSnapshot> snapshot;
    std::unordered_map<int, std::shared_ptr<LightSectionSnapshot>>
        mutableSections;
    // Real writes performed by this layer's propagator. This, rather than a
    // comparison with a previous page, drives protocol section deltas.
    std::unordered_set<int> dirtySections;
    bool dirty = false;

    LightSectionSnapshot *sectionForEdit(int sectionY) {
      const auto working = mutableSections.find(sectionY);
      if (working != mutableSections.end())
        return working->second.get();
      const auto index = vectorIndex(snapshot->sections, sectionY);
      if (index >= snapshot->sections.size())
        return nullptr;
      auto copy = std::make_shared<LightSectionSnapshot>(
          *snapshot->sections[index]);
      snapshot->sections[index] = copy;
      mutableSections.emplace(sectionY, copy);
      return copy.get();
    }
  };

  struct PreparedBatch {
    bool rawGeometry = false;
    std::unordered_map<std::uint64_t, WorkingChunk> chunks;
    std::unordered_set<std::uint64_t> removed;
    std::unordered_set<std::uint64_t> replaced;
    std::unordered_set<std::uint64_t> loaded;
    std::unordered_set<std::uint64_t> affected;
    std::vector<LightPos> changedPositions;
    std::uint64_t maxGeneration = 0;
  };

  struct LayerWorkspace {
    std::unordered_map<std::uint64_t, WorkingChunk> chunks;
    std::unordered_set<std::uint64_t> removed;
  };

  struct LayerOutcome {
    LayerWorkspace layers;
    bool complete = false;
  };

  struct LayerResult {
    bool hasLayer = false;
    LightLayer layer = LightLayer::Block;
    std::uint64_t key = 0;
    std::shared_ptr<LightChunkSnapshot> snapshot;
  };
  static_assert(sizeof(LayerResult) == 32);

  struct GeometryChunk {
    int chunkX = 0;
    int chunkZ = 0;
    std::uint64_t lifecycleEpoch = 0;
    std::vector<GeometrySection> sections;
  };

public:
  Impl(int minSection, int sectionCount, LightWorkerOptions workerOptions)
      : minBlockSectionY(minSection), blockSectionCount(sectionCount),
        options(workerOptions), paused(workerOptions.startPausedForTesting) {
    if (sectionCount <= 0)
      throw std::invalid_argument("light service needs block sections");
    if (options.batchSize == 0 || options.maxQueuedCommands == 0)
      throw std::invalid_argument("light worker limits must be positive");
    if (options.cellQueueCapacity != 0 && options.cellQueueCapacity < 2)
      throw std::invalid_argument("light cell queue capacity must be at least two");
    const std::size_t geometryCapacity =
        options.cellQueueCapacity != 0
            ? options.cellQueueCapacity
            : std::max<std::size_t>(2, options.geometryQueueBytes /
                                          sizeof(GeometryInput));
    inputQueue = std::make_unique<GrowableSpscRing<GeometryInput>>(
        geometryCapacity);
    resultQueue = std::make_unique<GrowableSpscRing<LayerResult>>(
        std::max<std::size_t>(2, options.resultQueueBytes /
                                    sizeof(LayerResult)));
    controlQueue = std::make_unique<SpscRing<PendingChunk>>(
        std::max<std::size_t>(2, options.maxQueuedCommands));
    opticalTable = defaultLightOpticalTable();
    worker = std::thread([this] { run(); });
#if defined(_WIN32) || defined(__linux__)
    const auto affinity =
        applyThreadAffinity(worker.native_handle(), options.workerAffinityMask);
    statsWorkerAffinity.store(affinity.mask, std::memory_order_relaxed);
    statsWorkerAffinityError.store(affinity.error,
                                   std::memory_order_relaxed);
    statsBlockWorkerAffinity.store(affinity.mask, std::memory_order_relaxed);
    statsBlockWorkerAffinityError.store(affinity.error,
                                        std::memory_order_relaxed);
#endif
  }

  ~Impl() { close(); }

  bool enqueueControl(PendingChunk state) {
    if (controlQueue->full() || !controlQueue->push(std::move(state))) {
      ++statsRejected;
      return false;
    }
    ++statsSubmitted;
    signalWorkerLocked(true);
    return true;
  }

  std::uint64_t submitChunk(LightChunkInput input) {
    if (!validInput(input)) {
      ++statsRejected;
      return 0;
    }
    if (!accepting || controlQueue->full()) {
      ++statsRejected;
      return 0;
    }
    const auto key = chunkKey(input.chunkX, input.chunkZ);
    auto &active = versions[key];
    const bool newLifetime = active.inactive || active.requestRevision == 0;
    if (newLifetime) {
      if (input.lifecycleEpoch != 0)
        active.epoch = input.lifecycleEpoch;
      else
        ++active.epoch;
      active.requestRevision = 0;
      latest.erase(key);
      latestBlock.erase(key);
      latestSky.erase(key);
    } else if (input.lifecycleEpoch != 0 &&
               input.lifecycleEpoch != active.epoch) {
      ++statsRejected;
      return 0;
    }
    const auto generation = nextRequestGeneration();
    ++active.requestRevision;
    active.inactive = false;
    mainVersions[key] = active;
    PendingChunk state;
    state.key = key;
    state.generation = generation;
    state.inputSequence = generation;
    state.lifecycleEpoch = active.epoch;
    state.revision = active.requestRevision;
    state.load = std::make_unique<LightChunkInput>(std::move(input));
    return enqueueControl(std::move(state)) ? generation : 0;
  }

  bool reportSectionStatus(int chunkX, int chunkZ, int sectionY, bool empty) {
    if (!validSection(sectionY)) {
      ++statsRejected;
      return false;
    }
    const auto key = chunkKey(chunkX, chunkZ);
    const auto active = versions.find(key);
    if (!accepting || controlQueue->full() || active == versions.end() ||
        active->second.inactive) {
      ++statsRejected;
      return false;
    }
    const auto generation = nextRequestGeneration();
    auto &version = active->second;
    ++version.requestRevision;
    PendingChunk state;
    state.key = key;
    state.generation = generation;
    state.lifecycleEpoch = version.epoch;
    state.revision = version.requestRevision;
    state.sectionStatus[sectionY] = {generation, empty};
    return enqueueControl(std::move(state));
  }

  bool reportBlockChange(LightBlockChange change) {
    if (!validSection(change.sectionY) || change.localIndex >= kLightVolume) {
      ++statsRejected;
      return false;
    }
    if (!hasDifferentLightProperties(change.oldProperties, change.newProperties) &&
        !hasDifferentSectionLightProperties(change.wasEmpty, change.isEmpty))
      return true;
    return applyBlockChange(change);
  }

  // Bulk form: the caller coalesced a tick's writes into one batch grouped by
  // chunk, so the per-cell version/pending/section lookups collapse into one set
  // of lookups per chunk-section and the mutex is taken once for the whole tick.
  void reportBlockChanges(const std::vector<LightBlockChange> &changes) {
    // Compatibility path for attribute-level probes. Production World writes
    // use the raw 16-byte reportBlockCell ring.
    for (const auto &change : changes)
      if (!applyBlockChange(change))
        ++statsRejected;
  }

  std::uint64_t nextRequestGeneration() {
    return nextGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

  const LightOpticalClass &opticalClass(std::uint16_t id) const {
    return (*opticalTable)[static_cast<std::uint8_t>(id)];
  }

  std::uint8_t classForState(BlockState::Id stateId) const {
    return opticalTable->classForState(stateId);
  }

  void loadGeometry(std::uint64_t key, LightChunkInput &input) {
    GeometryChunk chunk;
    chunk.chunkX = input.chunkX;
    chunk.chunkZ = input.chunkZ;
    chunk.lifecycleEpoch = input.lifecycleEpoch;
    chunk.sections = std::move(input.geometrySections);
    geometry[key] = std::move(chunk);
  }

  GeometrySection *geometrySection(std::uint64_t key, int sectionY) {
    const auto chunk = geometry.find(key);
    if (chunk == geometry.end())
      return nullptr;
    const int index = sectionY - minBlockSectionY;
    if (index < 0 || index >= blockSectionCount)
      return nullptr;
    return &chunk->second.sections[static_cast<std::size_t>(index)];
  }

  const GeometrySection *geometrySection(std::uint64_t key,
                                         int sectionY) const {
    const auto chunk = geometry.find(key);
    if (chunk == geometry.end())
      return nullptr;
    const int index = sectionY - minBlockSectionY;
    if (index < 0 || index >= blockSectionCount)
      return nullptr;
    return &chunk->second.sections[static_cast<std::size_t>(index)];
  }

  const std::shared_ptr<const LightChunkSnapshot> &
  storedLayerSnapshot(LightLayer layer, std::uint64_t key) const {
    static const std::shared_ptr<const LightChunkSnapshot> empty;
    const auto &source =
        layer == LightLayer::Block ? lastReadyBlock : lastReadySky;
    const auto found = source.find(key);
    if (found != source.end())
      return found->second;
    // Attribute-level compatibility fixtures still use the legacy combined
    // owner state. Raw production never stores an entry there.
    const auto legacy = internal.find(key);
    return legacy == internal.end() ? empty : legacy->second;
  }

  struct RawCellResult {
    bool opticalChanged = false;
    bool emptyChanged = false;
  };

  RawCellResult applyRawGeometryCell(std::uint64_t key, int sectionY,
                                     std::uint16_t localIndex,
                                     BlockState::Id stateId) {
    RawCellResult result;
    GeometrySection *section = geometrySection(key, sectionY);
    if (!section)
      return result;
    const std::uint16_t oldClass = section->at(localIndex);
    const std::uint16_t newClass = classForState(stateId);
    const auto &oldOptical = opticalClass(oldClass);
    const auto &newOptical = opticalClass(newClass);
    result.opticalChanged =
        oldOptical.properties != newOptical.properties ||
        oldOptical.air != newOptical.air;
    if (!result.opticalChanged)
      return result;
    const bool oldEmpty = section->empty;
    if (!oldOptical.air)
      --section->nonAir;
    if (!oldOptical.properties.supported)
      --section->unknown;
    if (!newOptical.air)
      ++section->nonAir;
    if (!newOptical.properties.supported)
      ++section->unknown;
    if (section->uniform && oldClass == section->uniformClass) {
      if (newClass == section->uniformClass) {
        // Equal class path handled above; this is unreachable on a change.
      } else {
        section->cells = std::make_unique<GeometrySection::Cells>();
        section->cells->fill(
            static_cast<std::uint8_t>(section->uniformClass));
        (*section->cells)[localIndex] = static_cast<std::uint8_t>(newClass);
        section->uniform = false;
      }
    } else {
      (*section->cells)[localIndex] = static_cast<std::uint8_t>(newClass);
    }
    section->empty = section->nonAir == 0;
    result.emptyChanged = oldEmpty != section->empty;
    return result;
  }

  // Atomic wait is used for the cell ring so the producer does not take the
  // service mutex per block write. The sequence increment is published after
  // the ring head; a worker that captured an older sequence cannot miss it.
  void notifyWorkerSleeping() {
    wakeSequence.fetch_add(1, std::memory_order_release);
    workerSleeping.store(false, std::memory_order_release);
    // The flag is only an optimization: an unconditional wake closes the
    // producer/awaitWork state-transition window where a notification could
    // otherwise be lost while the worker is about to enter atomic wait.
    wakeSequence.notify_one();
  }

  void signalWorkerLocked(bool batchWindow = false) {
    notifyWorkerSleeping();
    if (batchWindow)
      wake.notify_all();
  }

  bool applyOwnedCellLocked(std::uint64_t sequence,
                            const LightCellUpdate &update) {
    const int sectionY = update.y >> 4;
    if (!validSection(sectionY)) {
      ++statsRejected;
      return false;
    }
    const auto key = chunkKey(update.x >> 4, update.z >> 4);
    auto active = workerVersions.find(key);
    if (active == workerVersions.end() || active->second.inactive) {
      ++statsRejected;
      return false;
    }
    auto stateIt = pending.find(key);
    if (stateIt == pending.end())
      stateIt = pending.emplace(key, PendingChunk{}).first;
    PendingChunk &state = stateIt->second;
    state.key = key;
    if (sequence <= state.inputSequence)
      return true;
    state.generation = sequence;
    state.lifecycleEpoch = active->second.epoch;
    state.revision = ++active->second.requestRevision;
    const std::uint16_t localIndex =
        static_cast<std::uint16_t>(
            lightIndex(update.x, update.y, update.z));
    auto &owned = state.ownedCells[changeSlot(sectionY, localIndex)];
    if (sequence > owned.sequence)
      owned = {sequence, update.state};
    notePendingLocked();
    return true;
  }

  bool applyBlockChange(LightBlockChange change) {
    const auto key = chunkKey(change.chunkX, change.chunkZ);
    const auto active = versions.find(key);
    if (!accepting || controlQueue->full() || active == versions.end() ||
        active->second.inactive) {
      ++statsRejected;
      return false;
    }
    change.generation = nextRequestGeneration();
    auto &version = active->second;
    ++version.requestRevision;
    PendingChunk state;
    state.key = key;
    state.generation = change.generation;
    state.lifecycleEpoch = version.epoch;
    state.revision = version.requestRevision;
    state.sectionStatus[change.sectionY] = {
        change.generation, change.isEmpty};
    const auto slot = changeSlot(change.sectionY, change.localIndex);
    state.changes.emplace(slot, std::move(change));
    return enqueueControl(std::move(state));
  }

  bool unloadChunk(int chunkX, int chunkZ) {
    const auto key = chunkKey(chunkX, chunkZ);
    const auto active = versions.find(key);
    if (!accepting || controlQueue->full() || active == versions.end()) {
      ++statsRejected;
      return false;
    }
    const auto generation = nextRequestGeneration();
    auto &version = active->second;
    ++version.epoch;
    version.requestRevision = 0;
    version.inactive = true;
    latest.erase(key);
    latestBlock.erase(key);
    latestSky.erase(key);
    mainVersions[key] = version;
    PendingChunk state;
    state.key = key;
    state.generation = generation;
    state.lifecycleEpoch = version.epoch;
    state.unload = true;
    return enqueueControl(std::move(state));
  }

  std::shared_ptr<const LightChunkSnapshot> mainLayerView(
      const LightChunkSnapshot &worker) const {
    auto result = std::make_shared<LightChunkSnapshot>(worker);
    result->sections.clear();
    result->sections.reserve(worker.sections.size());
    result->blockLayerSnapshot.reset();
    result->skyLayerSnapshot.reset();
    const auto layer = result->layer;
    if (!layer) {
      result->sections = worker.sections;
      return result;
    }
    const std::size_t index = static_cast<std::size_t>(*layer);
    for (const auto &source : worker.sections) {
      if (!source) {
        result->sections.push_back(nullptr);
        continue;
      }
      // Main owns this tiny section header. Only the layer descriptor and its
      // optional atomic page are shared with the worker.
      auto section = std::make_shared<LightSectionSnapshot>(
          source->sectionY, source->empty,
          std::vector<BlockLightProperties>{emptyLightProperties()},
          std::vector<std::uint16_t>{});
      section->unsupportedCells = source->unsupportedCells;
      section->layers[index] = source->layers[index];
      result->sections.push_back(std::move(section));
    }
    return result;
  }

  void drainPublished() {
    std::vector<LayerResult> ready;
    LayerResult result;
    while (resultQueue->pop(result))
      ready.push_back(std::move(result));

    std::map<std::tuple<int, std::uint64_t, std::uint64_t>, std::size_t>
        latestResult;
    std::vector<LayerResult> coalesced;
    for (LayerResult &item : ready) {
      if (!item.snapshot) {
        coalesced.push_back(std::move(item));
        continue;
      }
      // The queued object is still the worker's persistent header. Main makes
      // its own small header before merging dirty metadata; section payloads
      // remain shared and no light page is copied.
      item.snapshot = std::make_shared<LightChunkSnapshot>(*item.snapshot);
      const auto identity = std::make_tuple(
          item.hasLayer ? static_cast<int>(item.layer) : -1, item.key,
          item.snapshot->lifecycleEpoch);
      const auto found = latestResult.find(identity);
      if (found == latestResult.end()) {
        latestResult.emplace(identity, coalesced.size());
        coalesced.push_back(std::move(item));
        continue;
      }
      LayerResult &current = coalesced[found->second];
      std::map<int, std::pair<bool, bool>> merged;
      for (const auto &change : current.snapshot->changedSections)
        merged[change.sectionY] = {change.sky, change.block};
      for (const auto &change : item.snapshot->changedSections) {
        auto &flags = merged[change.sectionY];
        flags.first |= change.sky;
        flags.second |= change.block;
      }
      if (item.snapshot->deltaBaseRevision ==
          current.snapshot->revision)
        item.snapshot->deltaBaseRevision =
            current.snapshot->deltaBaseRevision;
      item.snapshot->changedSections.clear();
      for (const auto &[sectionY, flags] : merged)
        item.snapshot->changedSections.push_back(
            {sectionY, flags.first, flags.second});
      current = std::move(item);
    }
    ready = std::move(coalesced);

    std::vector<std::shared_ptr<const LightChunkSnapshot>> callbacks;
    std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback =
        publishCallback;
    for (LayerResult &item : ready) {
      if (!item.snapshot) {
        ++statsStale;
        continue;
      }
      const auto active = mainVersions.find(item.key);
      if (active == mainVersions.end() || active->second.inactive ||
          active->second.epoch != item.snapshot->lifecycleEpoch) {
        ++statsStale;
        continue;
      }
      if (!item.hasLayer) {
        const auto current = latest.find(item.key);
        if (current == latest.end() ||
            current->second->revision <= item.snapshot->revision) {
          latest[item.key] = item.snapshot;
          for (const auto &change : item.snapshot->changedSections) {
            statsReceivedSkySections.fetch_add(change.sky ? 1 : 0,
                                               std::memory_order_relaxed);
            statsReceivedBlockSections.fetch_add(change.block ? 1 : 0,
                                                 std::memory_order_relaxed);
          }
          if (callback)
            callbacks.push_back(item.snapshot);
        }
        continue;
      }
      auto &latestForLayer =
          item.layer == LightLayer::Block ? latestBlock : latestSky;
      const auto current = latestForLayer.find(item.key);
      if (current != latestForLayer.end() &&
          current->second->revision > item.snapshot->revision)
        continue;
      auto main = mainLayerView(*item.snapshot);
      latestForLayer[item.key] = main;
      for (const auto &change : item.snapshot->changedSections) {
        statsReceivedBlockSections.fetch_add(change.block ? 1 : 0,
                                             std::memory_order_relaxed);
        statsReceivedSkySections.fetch_add(change.sky ? 1 : 0,
                                           std::memory_order_relaxed);
      }
      if (callback)
        callbacks.push_back(std::move(main));
    }
    if (callback)
      for (const auto &snapshot : callbacks)
        callback(snapshot);
  }

  std::shared_ptr<const LightChunkSnapshot> latestChunk(int x, int z) const {
    const auto block = latestBlock.find(chunkKey(x, z));
    if (block != latestBlock.end())
      return block->second;
    const auto found = latest.find(chunkKey(x, z));
    return found == latest.end() ? nullptr : found->second;
  }

  std::shared_ptr<const LightChunkSnapshot> latestLayerChunk(
      LightLayer layer, int x, int z) const {
    const auto &source = layer == LightLayer::Block ? latestBlock : latestSky;
    const auto found = source.find(chunkKey(x, z));
    return found == source.end() ? nullptr : found->second;
  }

  std::optional<std::array<int, 256>> skyColumnSourcesForTesting(
      int x, int z) const {
    std::lock_guard lock(mutex);
    if (busy || !pending.empty() || !ringEmpty())
      return std::nullopt;
    const auto found = skyColumnSources.find(chunkKey(x, z));
    if (found == skyColumnSources.end())
      return std::nullopt;
    return found->second;
  }

  std::shared_ptr<const LightOpticalTable> opticalTableForTesting() const {
    return opticalTable;
  }

  std::optional<BlockLightProperties> opticalPropertiesForTesting(
      int x, int y, int z) const {
    std::lock_guard lock(mutex);
    if (busy || !pending.empty() || !ringEmpty())
      return std::nullopt;
    const GeometrySection *section =
        geometrySection(chunkKey(x >> 4, z >> 4), y >> 4);
    if (!section)
      return std::nullopt;
    return opticalClass(section->at(lightIndex(x, y, z))).properties;
  }

  void setPublishCallback(
      std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback) {
    std::lock_guard lock(mutex);
    publishCallback = std::move(callback);
  }

  Stats stats() const {
    Stats result;
    result.workerCount = 1;
    result.submitted = statsSubmitted.load(std::memory_order_relaxed);
    result.rejected = statsRejected.load(std::memory_order_relaxed);
    result.processedBatches = statsBatches.load(std::memory_order_relaxed);
    result.processedBlockChanges = statsChanges.load(std::memory_order_relaxed);
    result.droppedGeometryTasks =
        statsDroppedGeometry.load(std::memory_order_relaxed);
    result.published = statsPublished.load(std::memory_order_relaxed);
    result.stalePublications = statsStale.load(std::memory_order_relaxed);
    result.queueHighWater = statsHighWater.load(std::memory_order_relaxed);
    result.lightQueueEntries =
        statsLightQueueEntries.load(std::memory_order_relaxed);
    result.skySectionChanges =
        statsSkyChanges.load(std::memory_order_relaxed);
    result.blockSectionChanges =
        statsBlockChanges.load(std::memory_order_relaxed);
    result.receivedSkySections =
        statsReceivedSkySections.load(std::memory_order_relaxed);
    result.receivedBlockSections =
        statsReceivedBlockSections.load(std::memory_order_relaxed);
    result.workerCpuMs =
        statsWorkerCpuMs.load(std::memory_order_relaxed);
    result.blockWorkerCpuMs =
        statsBlockWorkerCpuMs.load(std::memory_order_relaxed);
    result.skyWorkerCpuMs =
        statsSkyWorkerCpuMs.load(std::memory_order_relaxed);
    result.workerAffinityMask =
        statsWorkerAffinity.load(std::memory_order_relaxed);
    result.workerAffinityRequested =
        static_cast<std::uint64_t>(options.workerAffinityMask);
    result.workerAffinityError =
        statsWorkerAffinityError.load(std::memory_order_relaxed);
    result.blockWorkerAffinityRequested =
        static_cast<std::uint64_t>(options.workerAffinityMask);
    result.blockWorkerAffinityMask =
        statsBlockWorkerAffinity.load(std::memory_order_relaxed);
    result.blockWorkerAffinityError =
        statsBlockWorkerAffinityError.load(std::memory_order_relaxed);
    result.skyWorkerAffinityRequested =
        static_cast<std::uint64_t>(options.workerAffinityMask);
    result.skyWorkerAffinityMask =
        statsWorkerAffinity.load(std::memory_order_relaxed);
    result.skyWorkerAffinityError =
        statsWorkerAffinityError.load(std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    result.pendingChunks = pendingChunkCountLocked();
    result.pendingPublications = resultQueue->size();
    return result;
  }

  void resetMeasurementStats() {
    statsSubmitted.store(0, std::memory_order_relaxed);
    statsRejected.store(0, std::memory_order_relaxed);
    statsDroppedGeometry.store(0, std::memory_order_relaxed);
    statsBatches.store(0, std::memory_order_relaxed);
    statsChanges.store(0, std::memory_order_relaxed);
    statsPublished.store(0, std::memory_order_relaxed);
    statsStale.store(0, std::memory_order_relaxed);
    statsHighWater.store(0, std::memory_order_relaxed);
    statsLightQueueEntries.store(0, std::memory_order_relaxed);
    statsSkyChanges.store(0, std::memory_order_relaxed);
    statsBlockChanges.store(0, std::memory_order_relaxed);
    statsReceivedSkySections.store(0, std::memory_order_relaxed);
    statsReceivedBlockSections.store(0, std::memory_order_relaxed);
  }

  std::string workerError() const {
    std::lock_guard lock(mutex);
    return error;
  }

  bool waitIdleForTesting() {
    std::unique_lock lock(mutex);
    // The ring is drained by the worker, so idleness also means no queued cell
    // is still waiting to be coalesced into pending.
    idle.wait(lock, [&] {
      return !busy && pending.empty() && orderedInputEmptyLocked();
    });
    return error.empty();
  }

  bool waitWorkerSleepingForTesting() {
    std::unique_lock lock(mutex);
    sleepState.wait(lock, [&] {
      return workerSleeping.load(std::memory_order_acquire) || closing;
    });
    return workerSleeping.load(std::memory_order_acquire) && !closing;
  }

  void resumeForTesting() {
    std::lock_guard lock(mutex);
    paused = false;
    signalWorkerLocked(true);
  }

  void pauseForTesting() {
    std::lock_guard lock(mutex);
    paused = true;
  }

  bool processChunkBatchForTesting(int chunkX, int chunkZ) {
    const auto key = chunkKey(chunkX, chunkZ);
    std::unique_lock lock(mutex);
    if (!paused || busy || closing || forcedChunk)
      return false;
    // The paused worker cannot consume the ring. Coalesce its queued cells
    // under the same lock before checking for work on the requested chunk.
    drainCellChangesLocked();
    if (!pending.contains(key))
      return false;
    const auto before = statsBatches.load(std::memory_order_relaxed);
    forcedChunk = key;
    pauseAfterBatch = true;
    paused = false;
    signalWorkerLocked(true);
    batchDone.wait(lock, [&] {
      return statsBatches.load(std::memory_order_relaxed) > before || closing;
    });
    return error.empty() &&
           statsBatches.load(std::memory_order_relaxed) > before;
  }

  void close() {
    // Close the hot-path gate before joining the worker. Records already in
    // the ring remain owned input and are deliberately exempt from the later
    // `accepting == false` check during draining.
    acceptingCells.store(false, std::memory_order_release);
    std::thread selected;
    {
      std::lock_guard lock(mutex);
      if (!accepting && !worker.joinable())
        return;
      accepting = false;
      closing = true;
      signalWorkerLocked(true);
      selected = std::move(worker);
    }
    if (selected.joinable())
      selected.join();
  }

private:
  class LightValues {
  public:
    class CellRef {
    public:
      CellRef(LightValues &owner, std::size_t index)
          : owner_(owner), index_(index) {}
      operator std::uint8_t() const { return owner_.value(index_); }
      CellRef &operator=(std::uint8_t value) {
        owner_.write(index_, value);
        return *this;
      }

    private:
      LightValues &owner_;
      std::size_t index_;
    };

    LightValues()
        : layer_(std::make_shared<LightLayerSnapshot>(
              LightLayerSnapshot::missing())) {}
    LightValues(const LightValues &) = default;
    LightValues &operator=(const LightValues &) = default;
    LightValues(LightValues &&) noexcept = default;
    LightValues &operator=(LightValues &&) noexcept = default;

    static LightValues fromLayer(const LightLayerRef &layer) {
      LightValues result;
      if (auto shared = layer.shared())
        result.layer_ = std::move(shared);
      return result;
    }

    CellRef operator[](std::size_t index) { return {*this, index}; }
    std::uint8_t operator[](std::size_t index) const {
      return value(index);
    }
    void fill(std::uint8_t value) {
      // A whole-section fill is a topology conversion. Publish a tiny uniform
      // descriptor; a later cell write expands it once into a mixed page.
      layer_ = std::make_shared<LightLayerSnapshot>(
          LightLayerSnapshot::uniform(value));
    }
    std::optional<std::uint8_t> uniformValue() const {
      if (!layer_ || layer_->isMissing())
        return std::nullopt;
      if (layer_->isUniform())
        return layer_->uniformValue();
      const std::uint8_t first = (*this)[0];
      for (std::size_t i = 1; i < kLightVolume; ++i)
        if ((*this)[i] != first)
          return std::nullopt;
      return first;
    }
    const std::shared_ptr<LightLayerSnapshot> &shared() const {
      return layer_;
    }
    std::shared_ptr<LightLayerSnapshot> &shared() { return layer_; }

  private:
    std::uint8_t value(std::size_t index) const {
      return layer_ ? layer_->get(static_cast<int>(index & 15),
                                 static_cast<int>((index >> 8) & 15),
                                 static_cast<int>((index >> 4) & 15))
                    : 0;
    }
    void write(std::size_t index, std::uint8_t value) {
      if (!layer_)
        layer_ = std::make_shared<LightLayerSnapshot>(
            LightLayerSnapshot::missing());
      if (layer_->isUniform()) {
        const std::uint8_t fill = layer_->uniformValue();
        auto mixed = std::make_shared<LightLayerSnapshot>(
            LightLayerSnapshot::mixedEmpty());
        const std::uint8_t packed =
            static_cast<std::uint8_t>((fill << 4) | (fill & 15));
        for (std::size_t byte = 0; byte < kLightLayerBytes; ++byte)
          mixed->writePackedByte(byte, packed);
        layer_ = std::move(mixed);
      } else if (layer_->isMissing()) {
        layer_ = std::make_shared<LightLayerSnapshot>(
            LightLayerSnapshot::mixedEmpty());
      }
      layer_->writeCell(index, value);
    }

    std::shared_ptr<LightLayerSnapshot> layer_;
  };

  struct IncreaseTask {
    LightPos position;
    std::uint8_t level = 0;
    std::uint8_t directions = 0;
    bool fromEmpty = false;
    bool fromEmission = false;
  };

  struct DecreaseTask {
    LightPos position;
    std::uint8_t level = 0;
    std::uint8_t directions = 0;
  };

  class BlockLightPropagator {
  public:
    BlockLightPropagator(
        Impl &light,
        std::unordered_map<std::uint64_t, WorkingChunk> &workingChunks,
        const std::unordered_set<std::uint64_t> &removedChunks)
        : owner(light), working(workingChunks), removed(removedChunks) {}

    void enqueueCheck(LightPos position) {
      if (checks.insert(position).second)
        checkQueue.push_back(position);
    }

    void seedChunk(std::uint64_t key) {
      const LightChunkSnapshot *chunk = stateFor(key);
      if (!chunk)
        return;
      for (int sectionY = chunk->minLightSectionY();
           sectionY < chunk->minBlockSectionY + chunk->blockSectionCount + 1;
           ++sectionY) {
        const LightSectionSnapshot *data = chunk->section(sectionY);
        if (!data)
          continue;
        const bool inBlockRange =
            sectionY >= chunk->minBlockSectionY &&
            sectionY < chunk->minBlockSectionY + chunk->blockSectionCount;
        const GeometrySection *geometry =
            owner.geometrySection(key, sectionY);
        for (int index = 0; index < kLightVolume; ++index) {
          const int x = index & 15;
          const int z = (index >> 4) & 15;
          const int y = (index >> 8) & 15;
          const BlockLightProperties &properties =
              inBlockRange
                  ? (geometry
                         ? owner.opticalClass(
                               geometry->uniform
                                   ? geometry->uniformClass
                                   : geometry->at(
                                         static_cast<std::size_t>(index)))
                               .properties
                         : data->at(x, y, z))
                  : emptyLightProperties();
          if (inBlockRange && !properties.supported)
            incomplete = true;
          const LightPos position{chunk->chunkX * 16 + x,
                                  sectionY * 16 + y,
                                  chunk->chunkZ * 16 + z};
          if (inBlockRange && properties.supported && properties.emission > 0)
            enqueueIncrease(position, properties.emission, 63,
                            emptyShape(properties), true);
          const std::uint8_t stored =
              data->light(LightLayer::Block, x, y, z);
          if (stored > 0)
            enqueueIncrease(position, stored, 63, emptyShape(properties),
                            false);
        }
      }
    }

    void seedIncoming(std::uint64_t key) {
      const int chunkX = static_cast<int>(key >> 32);
      const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
      for (int direction = 2; direction < 6; ++direction) {
        const LightPos delta = kDirectionOffsets[direction];
        const std::uint64_t neighborKey =
            chunkKey(chunkX + delta.x, chunkZ + delta.z);
        const LightChunkSnapshot *neighbor = stateFor(neighborKey);
        if (!neighbor)
          continue;
        forEachFaceCell(neighbor, direction ^ 1, [&](LightPos position) {
          const int level = storedLevel(position);
          if (level > 1) {
            const auto &properties = stateAt(position);
            enqueueIncrease(position, static_cast<std::uint8_t>(level),
                            static_cast<std::uint8_t>(1u << (direction ^ 1)),
                            emptyShape(properties), false);
          }
        });
      }
    }

    void clearIncomingFrom(std::uint64_t removedKey) {
      const int chunkX = static_cast<int>(removedKey >> 32);
      const int chunkZ =
          static_cast<int>(static_cast<std::uint32_t>(removedKey));
      for (int direction = 2; direction < 6; ++direction) {
        const LightPos delta = kDirectionOffsets[direction];
        const std::uint64_t neighborKey =
            chunkKey(chunkX + delta.x, chunkZ + delta.z);
        const LightChunkSnapshot *neighbor = stateFor(neighborKey);
        if (!neighbor)
          continue;
        forEachFaceCell(neighbor, direction ^ 1, [&](LightPos position) {
          enqueueCheck(position);
        });
      }
    }

    void run() {
      while (!checkQueue.empty()) {
        const LightPos position = checkQueue.front();
        checkQueue.pop_front();
        checks.erase(position);
        checkNode(position);
      }
      while (!currentDecreaseQueue.empty() || !decreaseQueue.empty()) {
        if (currentDecreaseQueue.empty()) {
          const DecreaseTask first = decreaseQueue.pop();
          currentDecreaseQueue.push(std::move(first));
          activeDecreaseSection =
              sectionOf(currentDecreaseQueue.front().position);
        }
        DecreaseTask task = currentDecreaseQueue.pop();
        ++owner.statsLightQueueEntries;
        propagateDecrease(task);
        if (currentDecreaseQueue.empty())
          activeDecreaseSection.reset();
      }
      while (!currentIncreaseQueue.empty() || !increaseQueue.empty()) {
        if (currentIncreaseQueue.empty()) {
          const IncreaseTask first = increaseQueue.pop();
          currentIncreaseQueue.push(std::move(first));
          activeIncreaseSection =
              sectionOf(currentIncreaseQueue.front().position);
        }
        IncreaseTask task = currentIncreaseQueue.pop();
        ++owner.statsLightQueueEntries;
        propagateIncrease(task);
        if (currentIncreaseQueue.empty())
          activeIncreaseSection.reset();
      }
    }

    void commitLayers() {
      for (const SectionPos3 &position : dirtySections) {
        const std::uint64_t key = chunkKey(position.x, position.z);
        WorkingChunk *chunk = ensureWorking(key);
        if (!chunk)
          continue;
        LightSectionSnapshot *section = chunk->sectionForEdit(position.y);
        if (!section)
          continue;
        const auto found = lightValues.find(position);
        if (found == lightValues.end())
          continue;
        const LightValues &values = found->second;
        const auto uniform = values.uniformValue();
        const auto payload =
            uniform && values.shared()->isMixed()
                ? std::make_shared<LightLayerSnapshot>(
                      LightLayerSnapshot::uniform(*uniform))
                : values.shared();
        section->layers[static_cast<std::size_t>(LightLayer::Block)] = payload;
        chunk->dirtySections.insert(position.y);
        chunk->dirty = true;
      }
    }

    bool isComplete() const { return !incomplete; }

  private:
    const LightChunkSnapshot *stateFor(std::uint64_t key) const {
      if (removed.contains(key))
        return nullptr;
      const auto workingChunk = working.find(key);
      if (workingChunk != working.end())
        return workingChunk->second.snapshot.get();
      const auto &stored = owner.storedLayerSnapshot(LightLayer::Block, key);
      return stored ? stored.get() : nullptr;
    }

    const LightChunkSnapshot *stateFor(int chunkX, int chunkZ) const {
      return stateFor(chunkKey(chunkX, chunkZ));
    }

    WorkingChunk *ensureWorking(std::uint64_t key) {
      if (removed.contains(key))
        return nullptr;
      const auto found = working.find(key);
      if (found != working.end())
        return &found->second;
      const auto &stored = owner.storedLayerSnapshot(LightLayer::Block, key);
      if (!stored)
        return nullptr;
      WorkingChunk next;
      next.key = key;
      next.lifecycleEpoch = stored->lifecycleEpoch;
      next.chunkX = stored->chunkX;
      next.chunkZ = stored->chunkZ;
      next.snapshot = std::make_shared<LightChunkSnapshot>(*stored);
      auto inserted = working.emplace(key, std::move(next)).first;
      return &inserted->second;
    }

    const LightSectionSnapshot *sectionAt(LightPos position) const {
      if (position.y < (owner.minBlockSectionY - 1) * 16 ||
          position.y >=
              (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16)
        return nullptr;
      const LightChunkSnapshot *chunk =
          stateFor(position.x >> 4, position.z >> 4);
      return chunk ? chunk->section(position.y >> 4) : nullptr;
    }

    const BlockLightProperties &stateAt(LightPos position) const {
      if (position.y < owner.minBlockSectionY * 16 ||
          position.y >=
              (owner.minBlockSectionY + owner.blockSectionCount) * 16)
        return emptyLightProperties();
      const std::uint64_t key =
          chunkKey(position.x >> 4, position.z >> 4);
      if (const GeometrySection *geometry =
              owner.geometrySection(key, position.y >> 4)) {
        const std::uint16_t classId =
            geometry->uniform
                ? geometry->uniformClass
                : geometry->at(lightIndex(position.x, position.y, position.z));
        const auto &properties = owner.opticalClass(classId).properties;
        if (!properties.supported)
          incomplete = true;
        return properties;
      }
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return unsupportedLightProperties();
      const auto &properties =
          section->at(position.x & 15, position.y & 15, position.z & 15);
      if (!properties.supported)
        incomplete = true;
      return properties;
    }

    int storedLevel(LightPos position) const {
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return 0;
      const SectionPos3 key{position.x >> 4, position.y >> 4,
                            position.z >> 4};
      const auto found = lightValues.find(key);
      if (found != lightValues.end())
        return found->second[lightIndex(position.x, position.y, position.z)];
      return section->light(LightLayer::Block, position.x & 15,
                            position.y & 15, position.z & 15);
    }

    bool setStoredLevel(LightPos position, int level) {
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return false;
      const SectionPos3 key{position.x >> 4, position.y >> 4,
                            position.z >> 4};
      auto inserted = lightValues.try_emplace(
          key, LightValues::fromLayer(
                   section->layers[static_cast<std::size_t>(
                       LightLayer::Block)]));
      LightValues &values = inserted.first->second;
      const auto index = lightIndex(position.x, position.y, position.z);
      if (values[index] == level)
        return false;
      values[index] = static_cast<std::uint8_t>(level);
      dirtySections.insert(key);
      return true;
    }

    void checkNode(LightPos position) {
      if (!sectionAt(position))
        return;
      const auto &properties = stateAt(position);
      const int emission = properties.supported ? properties.emission : 0;
      const int oldLevel = storedLevel(position);
      if (emission < oldLevel) {
        setStoredLevel(position, 0);
        enqueueDecrease(position, static_cast<std::uint8_t>(oldLevel), 63);
      } else {
        enqueueDecrease(position, 1, 63);
      }
      if (emission > 0)
        enqueueIncrease(position, static_cast<std::uint8_t>(emission), 63,
                        emptyShape(properties), true);
    }

    void propagateIncrease(const IncreaseTask &task) {
      int fromLevel = storedLevel(task.position);
      const int targetLevel = task.level;
      if (task.fromEmission) {
        if (fromLevel < targetLevel) {
          setStoredLevel(task.position, targetLevel);
          fromLevel = targetLevel;
        }
      }
      // LightEngine.propagateIncreases reads the current stored level when the
      // task leaves the queue. A task seeded before a decrease must not carry
      // its old level into a neighbor after the source has already gone dark.
      if (fromLevel != targetLevel)
        return;
      const auto &fromQueued =
          task.fromEmpty ? emptyLightProperties() : stateAt(task.position);
      for (int direction = 0; direction < 6; ++direction) {
        if ((task.directions & (1u << direction)) == 0)
          continue;
        const LightPos to = offset(task.position, direction);
        const LightSectionSnapshot *toSection = sectionAt(to);
        if (!toSection)
          continue;
        const int toLevel = storedLevel(to);
        if (fromLevel - 1 <= toLevel)
          continue;
        const auto &toState = stateAt(to);
        const int newLevel = fromLevel - lightOpacity(toState);
        if (newLevel <= toLevel)
          continue;
        if (faceShapesOcclude(fromQueued, toState, direction))
          continue;
        setStoredLevel(to, newLevel);
        if (newLevel > 1) {
          enqueueIncrease(to, static_cast<std::uint8_t>(newLevel),
                          static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))),
                          emptyShape(toState), false);
        }
      }
    }

    void propagateDecrease(const DecreaseTask &task) {
      for (int direction = 0; direction < 6; ++direction) {
        if ((task.directions & (1u << direction)) == 0)
          continue;
        const LightPos to = offset(task.position, direction);
        if (!sectionAt(to))
          continue;
        const int toLevel = storedLevel(to);
        if (toLevel == 0)
          continue;
        if (toLevel <= int(task.level) - 1) {
          const auto &toState = stateAt(to);
          const int emission = toState.supported ? toState.emission : 0;
          setStoredLevel(to, 0);
          if (emission < toLevel)
            enqueueDecrease(to, static_cast<std::uint8_t>(toLevel),
                            static_cast<std::uint8_t>(63u &
                                                      ~(1u << (direction ^ 1))));
          if (emission > 0)
            enqueueIncrease(to, static_cast<std::uint8_t>(emission), 63,
                            emptyShape(toState), true);
        } else {
          enqueueIncrease(to, static_cast<std::uint8_t>(toLevel),
                          static_cast<std::uint8_t>(1u << (direction ^ 1)),
                          false, false);
        }
      }
    }

    template <class Visitor>
    void forEachFaceCell(const LightChunkSnapshot *chunk, int direction,
                         Visitor visitor) const {
      const int minX = chunk->chunkX * 16;
      const int minZ = chunk->chunkZ * 16;
      const int minY = (owner.minBlockSectionY - 1) * 16;
      const int maxY =
          (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16;
      if (direction == 0 || direction == 1) {
        const int y = direction == 0 ? minY : maxY - 1;
        for (int x = 0; x < 16; ++x)
          for (int z = 0; z < 16; ++z)
            visitor(LightPos{minX + x, y, minZ + z});
      } else if (direction == 2 || direction == 3) {
        const int z = direction == 2 ? minZ : minZ + 15;
        for (int y = minY; y < maxY; ++y)
          for (int x = 0; x < 16; ++x)
            visitor(LightPos{minX + x, y, z});
      } else {
        const int x = direction == 4 ? minX : minX + 15;
        for (int y = minY; y < maxY; ++y)
          for (int z = 0; z < 16; ++z)
            visitor(LightPos{x, y, minZ + z});
      }
    }

    static SectionPos3 sectionOf(LightPos position) {
      return {position.x >> 4, position.y >> 4, position.z >> 4};
    }

    void enqueueIncrease(LightPos position, std::uint8_t level,
                         std::uint8_t directions, bool fromEmpty,
                         bool fromEmission) {
      if (level == 0)
        return;
      IncreaseTask task{position, level, directions, fromEmpty, fromEmission};
      if (activeIncreaseSection &&
          sectionOf(position) == *activeIncreaseSection)
        currentIncreaseQueue.push(std::move(task));
      else
        increaseQueue.push(std::move(task));
    }

    void enqueueDecrease(LightPos position, std::uint8_t level,
                         std::uint8_t directions) {
      DecreaseTask task{position, level, directions};
      if (activeDecreaseSection &&
          sectionOf(position) == *activeDecreaseSection)
        currentDecreaseQueue.push(std::move(task));
      else
        decreaseQueue.push(std::move(task));
    }

    Impl &owner;
    std::unordered_map<std::uint64_t, WorkingChunk> &working;
    const std::unordered_set<std::uint64_t> &removed;
    std::unordered_set<LightPos, LightPosHash> checks;
    std::deque<LightPos> checkQueue;
    TaskQueue<DecreaseTask> decreaseQueue;
    TaskQueue<IncreaseTask> increaseQueue;
    TaskQueue<DecreaseTask> currentDecreaseQueue;
    TaskQueue<IncreaseTask> currentIncreaseQueue;
    std::optional<SectionPos3> activeDecreaseSection;
    std::optional<SectionPos3> activeIncreaseSection;
    std::unordered_map<SectionPos3, LightValues, SectionPos3Hash> lightValues;
    std::unordered_set<SectionPos3, SectionPos3Hash> dirtySections;
    mutable bool incomplete = false;
  };

  using SkyColumnSources = std::array<int, 256>;

  class SkyLightPropagator {
  public:
    SkyLightPropagator(
        Impl &light,
        std::unordered_map<std::uint64_t, WorkingChunk> &workingChunks,
        const std::unordered_set<std::uint64_t> &removedChunks,
        std::unordered_map<std::uint64_t, SkyColumnSources> &sources,
        const std::unordered_set<std::uint64_t> &affectedChunks)
        : owner(light), working(workingChunks), removed(removedChunks),
          sourceHeights(sources), affected(affectedChunks) {}

    void enqueueCheck(LightPos position) {
      if (checks.insert(position).second)
        checkQueue.push_back(position);
    }

    void seedChunk(std::uint64_t key) {
      WorkingChunk *chunk = workingChunk(key);
      if (!chunk)
        return;
      const SkyColumnSources sources = computeSources(*chunk);
      sourceHeights[key] = sources;
      clearLines(*chunk);
      const int minY = owner.minBlockSectionY * 16;
      const int lightBottom = (owner.minBlockSectionY - 1) * 16;
      const int sentinel = minY - 1;
      // Lowest y that still receives full sky light for each column. A column
      // without any obstruction is lit through the bottom padding as well.
      std::array<int, 256> litFrom{};
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
          const int source = sources[HeightmapColumnKey(x, z)];
          litFrom[HeightmapColumnKey(x, z)] =
              source == sentinel ? lightBottom : source;
        }
      // One scalar lookup per section instead of a chunk/section/value lookup
      // for every one of the 4096 cells. Every cell of every light section is
      // covered by its column's lit or dark range, so the working layer can be
      // filled directly; the per-cell stored-level comparison of the old path
      // correctly skipped the writes that already equalled the inherited value.
      for (int sectionY = owner.minBlockSectionY - 1;
           sectionY <= owner.minBlockSectionY + owner.blockSectionCount;
           ++sectionY) {
        LightValues *layer = bulkValues(key, sectionY);
        if (!layer)
          continue;
        const int baseY = sectionY * 16;
        int minFirst = std::numeric_limits<int>::max();
        int maxFirst = std::numeric_limits<int>::min();
        for (int column = 0; column < 256; ++column) {
          const int first = litFrom[column] - baseY;
          minFirst = std::min(minFirst, first);
          maxFirst = std::max(maxFirst, first);
        }
        if (maxFirst <= 0) {
          layer->fill(15);
        } else if (minFirst >= 16) {
          layer->fill(0);
        } else {
          for (int index = 0; index < kLightVolume; ++index) {
            const int x = index & 15;
            const int z = (index >> 4) & 15;
            const int y = (index >> 8) & 15;
            (*layer)[static_cast<std::size_t>(index)] =
                baseY + y >= litFrom[HeightmapColumnKey(x, z)] ? 15 : 0;
          }
        }
        dirtySections.insert(
            {static_cast<int>(key >> 32), sectionY,
             static_cast<int>(static_cast<std::uint32_t>(key))});
      }
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          seedColumn(key, chunk, x, z, sources[HeightmapColumnKey(x, z)]);
    }

    void seedIncoming(std::uint64_t key) {
      const LightChunkSnapshot *chunk = stateFor(key);
      if (!chunk)
        return;
      for (int direction = 2; direction < 6; ++direction) {
        const LightPos delta = kDirectionOffsets[direction];
        const std::uint64_t neighborKey =
            chunkKey(chunk->chunkX + delta.x, chunk->chunkZ + delta.z);
        const LightChunkSnapshot *neighbor = stateFor(neighborKey);
        if (!neighbor)
          continue;
        forEachFaceCell(neighbor, direction ^ 1, [&](LightPos position) {
          const int level = storedLevel(position);
          if (level > 1)
            enqueueIncrease(position, static_cast<std::uint8_t>(level),
                            static_cast<std::uint8_t>(1u << (direction ^ 1)),
                            emptyShape(stateAt(position)), false);
        });
      }
    }

    void clearIncomingFrom(std::uint64_t removedKey) {
      const int chunkX = static_cast<int>(removedKey >> 32);
      const int chunkZ =
          static_cast<int>(static_cast<std::uint32_t>(removedKey));
      for (int direction = 2; direction < 6; ++direction) {
        const LightPos delta = kDirectionOffsets[direction];
        const LightChunkSnapshot *neighbor =
            stateFor(chunkKey(chunkX + delta.x, chunkZ + delta.z));
        if (!neighbor)
          continue;
        forEachFaceCell(neighbor, direction ^ 1, [&](LightPos position) {
          enqueueCheck(position);
        });
      }
    }

    void syncStorageEligibility() {
      // affectedChunks is fixed from the batch's shared geometry before either
      // layer starts. It cannot depend on chunks materialized by block commit.
      storedEligible.clear();
      eligibleValid = false;
      // A section from an already-published chunk outside this batch's
      // authoritative 3x3 domains is still a writable destination. Its raw
      // layer proves the old storage rule admitted it; keeping that evidence
      // avoids treating a cross-edge target as air merely because this batch
      // did not recompute its block sections.
      const auto &storedSky =
          owner.lastReadySky.empty() ? owner.internal : owner.lastReadySky;
      for (const auto &[key, chunk] : storedSky) {
        if (affected.contains(key) || removed.contains(key))
          continue;
        for (int y = chunk->minLightSectionY();
             y < chunk->minBlockSectionY + chunk->blockSectionCount + 1;
             ++y) {
          const LightSectionSnapshot *section = chunk->section(y);
          if (section &&
              !section->layers[static_cast<std::size_t>(LightLayer::Sky)]
                   .isMissing())
            storedEligible.insert(
                {static_cast<int>(key >> 32), y,
                 static_cast<int>(static_cast<std::uint32_t>(key))});
        }
      }
      for (std::uint64_t key : affected)
        (void)ensureWorking(key);

      for (auto &[key, chunk] : working) {
        if (removed.contains(key))
          continue;
        if (!affected.contains(key)) {
          // Preserve the same published raw destination if it entered the
          // working set for a reason other than its own section transition.
          for (int y = chunk.snapshot->minLightSectionY();
               y < chunk.snapshot->minBlockSectionY +
                       chunk.snapshot->blockSectionCount + 1;
               ++y) {
            const LightSectionSnapshot *section = chunk.snapshot->section(y);
            if (section &&
                !section->layers[static_cast<std::size_t>(LightLayer::Sky)]
                     .isMissing())
              storedEligible.insert({chunk.chunkX, y, chunk.chunkZ});
          }
          continue;
        }
        const int sectionX = static_cast<int>(key >> 32);
        const int sectionZ =
            static_cast<int>(static_cast<std::uint32_t>(key));
        for (int y = chunk.snapshot->minLightSectionY();
             y < chunk.snapshot->minBlockSectionY +
                     chunk.snapshot->blockSectionCount + 1;
             ++y) {
          bool eligible = false;
          for (int dx = -1; dx <= 1 && !eligible; ++dx)
            for (int dz = -1; dz <= 1 && !eligible; ++dz)
              for (int dy = -1; dy <= 1 && !eligible; ++dy)
                eligible = blockSectionNonEmpty(sectionX + dx, y + dy,
                                                sectionZ + dz);
          const SectionPos3 sectionKey{sectionX, y, sectionZ};
          if (eligible) {
            storedEligible.insert(sectionKey);
            allocateSection(key, y);
          } else {
            dropWorkingLayer(key, y);
            invalidateInheritedBases(key);
            dirtySections.erase(sectionKey);
            const auto *old = chunk.snapshot->section(y);
            if (old &&
                !old->layers[static_cast<std::size_t>(LightLayer::Sky)]
                     .isMissing()) {
              auto *edit = chunk.sectionForEdit(y);
              edit->layers[static_cast<std::size_t>(LightLayer::Sky)] =
                  LightLayerSnapshot::missing();
              chunk.dirtySections.insert(y);
              chunk.dirty = true;
            }
          }
        }
      }
      storageReady = true;
    }

    int columnSource(std::uint64_t key, int localX, int localZ) {
      WorkingChunk *chunk = workingChunk(key);
      if (!chunk)
        return owner.minBlockSectionY * 16 - 1;
      return sourcesForWorking(key, *chunk)[
          HeightmapColumnKey(localX, localZ)];
    }

    SkyColumnSources &sourcesForWorking(std::uint64_t key,
                                        WorkingChunk &chunk) {
      auto found = sourceHeights.find(key);
      if (found == sourceHeights.end())
        found = sourceHeights.emplace(key, computeSources(chunk)).first;
      return found->second;
    }

    // Reconcile only the two vertical edges touched by this changed cell.
    // Every lookup reads the final optical batch snapshot. The caller defers
    // seedColumn until all changed columns have final heights.
    int updateColumnSource(std::uint64_t key, int localX, int localZ,
                           int changedY) {
      WorkingChunk *chunk = workingChunk(key);
      if (!chunk)
        return owner.minBlockSectionY * 16 - 1;
      auto &sources = sourcesForWorking(key, *chunk);
      const int column = HeightmapColumnKey(localX, localZ);
      int source = sources[column];
      if (changedY + 1 < source)
        return source;

      const auto updateEdge = [&](int edgeY) {
        if (edgeY < owner.minBlockSectionY * 16)
          return;
        const int upperY = edgeY + 1;
        const auto &top = sourcePropertyAt(*chunk, localX, upperY, localZ);
        const auto &bottom =
            sourcePropertyAt(*chunk, localX, edgeY, localZ);
        if (isSourceEdgeOccluded(top, bottom)) {
          if (upperY > source)
            source = upperY;
        } else if (upperY == source) {
          // Re-evaluate the next edge below the invalid one. This includes a
          // different cell changed in the same batch at edgeY - 1.
          source = findSourceBelow(*chunk, localX, localZ, edgeY);
        }
      };
      updateEdge(changedY);
      updateEdge(changedY - 1);
      sources[column] = source;
      return source;
    }

    void applyColumnSource(std::uint64_t key, int localX, int localZ,
                           int oldSource, int newSource) {
      WorkingChunk *chunk = workingChunk(key);
      if (!chunk || oldSource == newSource)
        return;
      const int blockMinY = owner.minBlockSectionY * 16;
      const int lightBottom = (owner.minBlockSectionY - 1) * 16;
      if (newSource > oldSource) {
        const int oldFirst =
            oldSource == blockMinY - 1 ? lightBottom : oldSource;
        for (int y = oldFirst; y < newSource; ++y) {
          const LightPos position{chunk->chunkX * 16 + localX, y,
                                  chunk->chunkZ * 16 + localZ};
          const int oldLevel = storedLevel(position);
          if (oldLevel > 0) {
            setStoredLevel(position, 0);
            enqueueDecrease(position,
                            static_cast<std::uint8_t>(oldLevel), 63);
          }
        }
      } else {
        const int newFirst =
            newSource == blockMinY - 1 ? lightBottom : newSource;
        for (int y = newFirst; y < oldSource; ++y) {
          const LightPos position{chunk->chunkX * 16 + localX, y,
                                  chunk->chunkZ * 16 + localZ};
          setStoredLevel(position, 15);
          enqueueIncrease(position, 15, 63, false, true);
        }
      }
      seedColumn(key, chunk, localX, localZ, newSource);
    }

    void run() {
      while (!checkQueue.empty()) {
        const LightPos position = checkQueue.front();
        checkQueue.pop_front();
        checks.erase(position);
        checkNode(position);
      }
      while (!currentDecreaseQueue.empty() || !decreaseQueue.empty()) {
        if (currentDecreaseQueue.empty()) {
          const DecreaseTask first = decreaseQueue.pop();
          currentDecreaseQueue.push(std::move(first));
          activeDecreaseSection =
              sectionOf(currentDecreaseQueue.front().position);
        }
        DecreaseTask task = currentDecreaseQueue.pop();
        ++owner.statsLightQueueEntries;
        propagateDecrease(task);
        if (currentDecreaseQueue.empty())
          activeDecreaseSection.reset();
      }
      while (!currentIncreaseQueue.empty() || !increaseQueue.empty()) {
        if (currentIncreaseQueue.empty()) {
          const IncreaseTask first = increaseQueue.pop();
          currentIncreaseQueue.push(std::move(first));
          activeIncreaseSection =
              sectionOf(currentIncreaseQueue.front().position);
        }
        IncreaseTask task = currentIncreaseQueue.pop();
        ++owner.statsLightQueueEntries;
        propagateIncrease(task);
        if (currentIncreaseQueue.empty())
          activeIncreaseSection.reset();
      }
    }

    void commitLayers() {
      std::vector<SectionPos3> ordered(dirtySections.begin(), dirtySections.end());
      std::sort(ordered.begin(), ordered.end(), [](const auto &left,
                                                   const auto &right) {
        if (left.x != right.x)
          return left.x < right.x;
        if (left.z != right.z)
          return left.z < right.z;
        return left.y > right.y;
      });
      for (const SectionPos3 &position : ordered) {
        WorkingChunk *chunk = ensureWorking(chunkKey(position.x, position.z));
        if (!chunk)
          continue;
        LightSectionSnapshot *section = chunk->sectionForEdit(position.y);
        if (!section)
          continue;
        const LightValues *layer =
            workingLayer(chunkKey(position.x, position.z), position.y);
        if (!layer)
          continue;
        const auto uniform = layer->uniformValue();
        const auto payload =
            uniform && layer->shared()->isMixed()
                ? std::make_shared<LightLayerSnapshot>(
                      LightLayerSnapshot::uniform(*uniform))
                : layer->shared();
        section->layers[static_cast<std::size_t>(LightLayer::Sky)] = payload;
        chunk->dirtySections.insert(position.y);
        chunk->dirty = true;
      }
    }

    bool isComplete() const { return !incomplete; }

  private:
    static int HeightmapColumnKey(int x, int z) { return x + z * 16; }
    static constexpr int kUnknownInheritedSection =
        std::numeric_limits<int>::min();
    static constexpr int kNoInheritedSection =
        std::numeric_limits<int>::max();

    // A chunk owns at most one 4096-cell array per allocated light section.
    // Empty sections stay null, so the direct index does not allocate storage
    // for every missing layer. inherited[] caches only the section number of
    // the nearest stored layer above; its bottom value is always read from the
    // current working or immutable raw layer.
    struct WorkingLayers {
      explicit WorkingLayers(std::size_t count)
          : sections(count), inherited(count, kUnknownInheritedSection) {}

      std::vector<std::unique_ptr<LightValues>> sections;
      mutable std::vector<int> inherited;
    };

    int workingSectionIndex(int sectionY) const {
      const std::int64_t index =
          static_cast<std::int64_t>(sectionY) -
          static_cast<std::int64_t>(owner.minBlockSectionY - 1);
      return index >= 0 && index < owner.blockSectionCount + 2
                 ? static_cast<int>(index)
                 : -1;
    }

    WorkingLayers &workingLayers(std::uint64_t chunkKeyValue) const {
      if (cachedLayersValid && cachedLayersKey == chunkKeyValue)
        return *cachedLayersPtr;
      const auto found = layersByChunk.find(chunkKeyValue);
      if (found != layersByChunk.end()) {
        cachedLayersKey = chunkKeyValue;
        cachedLayersPtr = &found->second;
        cachedLayersValid = true;
        return found->second;
      }
      cachedLayersValid = false;
      WorkingLayers &inserted =
          layersByChunk
              .try_emplace(
                  chunkKeyValue,
                  static_cast<std::size_t>(owner.blockSectionCount + 2))
              .first->second;
      cachedLayersKey = chunkKeyValue;
      cachedLayersPtr = &inserted;
      cachedLayersValid = true;
      return inserted;
    }

    LightValues *workingLayer(std::uint64_t chunkKeyValue, int sectionY) {
      return const_cast<LightValues *>(
          std::as_const(*this).workingLayer(chunkKeyValue, sectionY));
    }

    const LightValues *workingLayer(std::uint64_t chunkKeyValue,
                                    int sectionY) const {
      const int index = workingSectionIndex(sectionY);
      if (index < 0)
        return nullptr;
      const auto found = layersByChunk.find(chunkKeyValue);
      if (found == layersByChunk.end())
        return nullptr;
      return found->second.sections[static_cast<std::size_t>(index)].get();
    }

    void invalidateInheritedBases(std::uint64_t chunkKeyValue) const {
      const auto found = layersByChunk.find(chunkKeyValue);
      if (found == layersByChunk.end())
        return;
      std::fill(found->second.inherited.begin(),
                found->second.inherited.end(), kUnknownInheritedSection);
    }

    LightValues *insertWorkingLayer(std::uint64_t chunkKeyValue, int sectionY,
                                    LightValues values) {
      const int index = workingSectionIndex(sectionY);
      if (index < 0)
        return nullptr;
      WorkingLayers &layers = workingLayers(chunkKeyValue);
      std::unique_ptr<LightValues> &slot =
          layers.sections[static_cast<std::size_t>(index)];
      if (slot)
        return slot.get();
      slot = std::make_unique<LightValues>(std::move(values));
      invalidateInheritedBases(chunkKeyValue);
      cellCacheValid = false;
      return slot.get();
    }

    void dropWorkingLayer(std::uint64_t chunkKeyValue, int sectionY) {
      const int index = workingSectionIndex(sectionY);
      if (index < 0)
        return;
      const auto found = layersByChunk.find(chunkKeyValue);
      if (found == layersByChunk.end())
        return;
      auto &slot = found->second.sections[static_cast<std::size_t>(index)];
      if (!slot)
        return;
      slot.reset();
      invalidateInheritedBases(chunkKeyValue);
      cellCacheValid = false;
    }

    void dropAllWorkingLayers(std::uint64_t chunkKeyValue) {
      cachedLayersValid = false;
      cellCacheValid = false;
      layersByChunk.erase(chunkKeyValue);
    }

    int inheritedBase(WorkingLayers &layers,
                      const LightChunkSnapshot *chunk, int sectionY) const {
      const int index = workingSectionIndex(sectionY);
      if (index < 0)
        return kNoInheritedSection;
      const std::size_t cacheIndex = static_cast<std::size_t>(index);
      if (layers.inherited[cacheIndex] != kUnknownInheritedSection)
        return layers.inherited[cacheIndex];
      int result = kNoInheritedSection;
      for (int aboveY = sectionY + 1;
           aboveY <
           chunk->minBlockSectionY + chunk->blockSectionCount + 1;
           ++aboveY) {
        const int aboveIndex = workingSectionIndex(aboveY);
        if (aboveIndex < 0)
          continue;
        if (layers.sections[static_cast<std::size_t>(aboveIndex)]) {
          result = aboveY;
          break;
        }
        const LightSectionSnapshot *above = chunk->section(aboveY);
        if (above &&
            !above->layers[static_cast<std::size_t>(LightLayer::Sky)]
                 .isMissing()) {
          result = aboveY;
          break;
        }
      }
      layers.inherited[cacheIndex] = result;
      return result;
    }

    LightValues initialWorkingLayer(std::uint64_t chunkKeyValue,
                                    const LightChunkSnapshot *chunk,
                                    int sectionY) const {
      LightValues layer{};
      const LightSectionSnapshot *section = chunk->section(sectionY);
      if (section &&
          !section->layers[static_cast<std::size_t>(LightLayer::Sky)]
               .isMissing())
        return LightValues::fromLayer(
            section->layers[static_cast<std::size_t>(LightLayer::Sky)]);

      WorkingLayers &layers = workingLayers(chunkKeyValue);
      const int baseY = inheritedBase(layers, chunk, sectionY);
      const int baseIndex =
          baseY == kNoInheritedSection ? -1 : workingSectionIndex(baseY);
      const LightValues *workingBase =
          baseIndex < 0
              ? nullptr
              : layers.sections[static_cast<std::size_t>(baseIndex)].get();
      const LightSectionSnapshot *rawBase =
          baseY == kNoInheritedSection ? nullptr : chunk->section(baseY);
      for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
          std::uint8_t inherited = 15;
          if (workingBase)
            inherited = (*workingBase)[lightIndex(x, 0, z)];
          else if (rawBase &&
                   !rawBase->layers[static_cast<std::size_t>(LightLayer::Sky)]
                        .isMissing())
            inherited = rawBase->light(LightLayer::Sky, x, 0, z);
          for (int y = 0; y < 16; ++y)
            layer[lightIndex(x, y, z)] = inherited;
        }
      }
      return layer;
    }

    bool blockSectionNonEmpty(int x, int y, int z) const {
      const auto key = chunkKey(x, z);
      if (const GeometrySection *geometry = owner.geometrySection(key, y))
        return !geometry->empty;
      const LightChunkSnapshot *chunk = stateFor(key);
      if (!chunk || y < chunk->minBlockSectionY ||
          y >= chunk->minBlockSectionY + chunk->blockSectionCount)
        return false;
      const LightSectionSnapshot *section = chunk->section(y);
      return section && !section->empty;
    }

    WorkingChunk *workingChunk(std::uint64_t key) {
      const auto found = working.find(key);
      if (found != working.end())
        return &found->second;
      return ensureWorking(key);
    }

    const LightChunkSnapshot *stateFor(std::uint64_t key) const {
      if (cachedStateValid && cachedStateKey == key)
        return cachedStatePtr;
      cachedStateValid = false;
      if (removed.contains(key))
        return nullptr;
      const LightChunkSnapshot *result = nullptr;
      const auto current = working.find(key);
      if (current != working.end())
        result = current->second.snapshot.get();
      else {
        const auto &stored = owner.storedLayerSnapshot(LightLayer::Sky, key);
        if (stored)
          result = stored.get();
      }
      cachedStateKey = key;
      cachedStatePtr = result;
      cachedStateValid = true;
      return result;
    }

    WorkingChunk *ensureWorking(std::uint64_t key) {
      if (removed.contains(key))
        return nullptr;
      const auto current = working.find(key);
      if (current != working.end())
        return &current->second;
      const auto &stored = owner.storedLayerSnapshot(LightLayer::Sky, key);
      if (!stored)
        return nullptr;
      WorkingChunk next;
      next.key = key;
      next.lifecycleEpoch = stored->lifecycleEpoch;
      next.chunkX = stored->chunkX;
      next.chunkZ = stored->chunkZ;
      next.snapshot = std::make_shared<LightChunkSnapshot>(*stored);
      // This shallow snapshot copy shares the same immutable raw section
      // pointers. Work layers calculated for an out-of-domain destination
      // before it joins the batch are based on that same raw data, so retain
      // them for commit. clearLines invalidates them when raw data is actually
      // replaced by a seed/refresh.
      cachedStateValid = false;
      cellCacheValid = false;
      return &working.emplace(key, std::move(next)).first->second;
    }

    void refreshCellCache(LightPos position) const {
      const SectionPos3 section{position.x >> 4, position.y >> 4,
                                position.z >> 4};
      if (cellCacheValid && section == cachedCellSection)
        return;
      cachedCellSection = section;
      cachedCellChunk =
          chunkKey(position.x >> 4, position.z >> 4);
      WorkingLayers &layers = workingLayers(cachedCellChunk);
      const int index = workingSectionIndex(section.y);
      cachedCellValues =
          index < 0
              ? nullptr
              : layers.sections[static_cast<std::size_t>(index)].get();
      const LightChunkSnapshot *chunk = stateFor(cachedCellChunk);
      cachedCellSectionSnapshot =
          chunk ? chunk->section(section.y) : nullptr;
      cellCacheValid = true;
    }

    const LightSectionSnapshot *sectionAt(LightPos position) const {
      if (position.y < (owner.minBlockSectionY - 1) * 16 ||
          position.y >=
              (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16)
        return nullptr;
      refreshCellCache(position);
      return cachedCellSectionSnapshot;
    }

    const BlockLightProperties &stateAt(LightPos position) const {
      if (position.y < owner.minBlockSectionY * 16 ||
          position.y >=
              (owner.minBlockSectionY + owner.blockSectionCount) * 16)
        return emptyLightProperties();
      const std::uint64_t key =
          chunkKey(position.x >> 4, position.z >> 4);
      if (const GeometrySection *geometry =
              owner.geometrySection(key, position.y >> 4)) {
        const std::uint16_t classId =
            geometry->uniform
                ? geometry->uniformClass
                : geometry->at(lightIndex(position.x, position.y, position.z));
        const auto &properties = owner.opticalClass(classId).properties;
        if (!properties.supported)
          incomplete = true;
        return properties;
      }
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return unsupportedLightProperties();
      const auto &properties =
          section->at(position.x & 15, position.y & 15, position.z & 15);
      if (!properties.supported)
        incomplete = true;
      return properties;
    }

    int storedLevel(LightPos position) const {
      refreshCellCache(position);
      const int sectionY = position.y >> 4;
      if (cachedCellValues)
        return (*cachedCellValues)[lightIndex(position.x, position.y,
                                              position.z)];
      const LightChunkSnapshot *chunk = stateFor(cachedCellChunk);
      if (!chunk)
        return 0;
      const LightSectionSnapshot *section = cachedCellSectionSnapshot;
      if (section && !section->layers[static_cast<std::size_t>(LightLayer::Sky)]
                           .isMissing())
        return section->light(LightLayer::Sky, position.x & 15,
                              position.y & 15, position.z & 15);
      WorkingLayers &layers = workingLayers(cachedCellChunk);
      const int baseY = inheritedBase(layers, chunk, sectionY);
      if (baseY == kNoInheritedSection)
        return 15;
      const int baseIndex = workingSectionIndex(baseY);
      if (const LightValues *layer =
              baseIndex < 0
                  ? nullptr
                  : layers.sections[static_cast<std::size_t>(baseIndex)].get())
        return (*layer)[lightIndex(position.x, 0, position.z)];
      const LightSectionSnapshot *above = chunk->section(baseY);
      return above && !above->layers[static_cast<std::size_t>(LightLayer::Sky)]
                            .isMissing()
                 ? above->light(LightLayer::Sky, position.x & 15, 0,
                                position.z & 15)
                 : 15;
    }

    // Bulk seeding needs the working layer of a whole section without the
    // per-cell value lookup. The eligibility rule of setStoredLevel still
    // applies: before this batch's storage eligibility is known every section
    // is writable, afterwards only eligible sections (or sections that already
    // hold a working value) are.
    LightValues *bulkValues(std::uint64_t chunkKeyValue, int sectionY) {
      if (sectionY < owner.minBlockSectionY - 1 ||
          sectionY > owner.minBlockSectionY + owner.blockSectionCount)
        return nullptr;
      if (LightValues *layer = workingLayer(chunkKeyValue, sectionY))
        return layer;
      const int sectionX =
          static_cast<int>(static_cast<std::int32_t>(chunkKeyValue >> 32));
      const int sectionZ =
          static_cast<int>(static_cast<std::uint32_t>(chunkKeyValue));
      if (storageReady && !eligibleHere(sectionX, sectionY, sectionZ))
        return nullptr;
      const LightChunkSnapshot *chunk = stateFor(chunkKeyValue);
      const LightSectionSnapshot *section =
          chunk ? chunk->section(sectionY) : nullptr;
      if (!section)
        return nullptr;
      return insertWorkingLayer(
          chunkKeyValue, sectionY,
          initialWorkingLayer(chunkKeyValue, chunk, sectionY));
    }

    // Flat probe of the current batch's storedEligible set. A seeding pass
    // probes once per light section (26 times per chunk), so a single set
    // lookup is already cheap; the helper only keeps bulkValues readable.
    bool eligibleHere(int sectionX, int sectionY, int sectionZ) {
      const std::uint64_t packed =
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sectionX)) << 32) |
          static_cast<std::uint32_t>(sectionZ);
      if (eligibleValid && eligibleChunkKey == packed &&
          eligibleSectionY == sectionY)
        return eligibleResult;
      eligibleChunkKey = packed;
      eligibleSectionY = sectionY;
      eligibleResult = storedEligible.contains({sectionX, sectionY, sectionZ});
      eligibleValid = true;
      return eligibleResult;
    }

    bool setStoredLevel(LightPos position, int level) {
      refreshCellCache(position);
      const LightSectionSnapshot *section = cachedCellSectionSnapshot;
      if (!section)
        return false;
      const int current = storedLevel(position);
      if (current == level)
        return false;
      const SectionPos3 key{position.x >> 4, position.y >> 4,
                            position.z >> 4};
      const std::uint64_t chunkKeyValue =
          chunkKey(position.x >> 4, position.z >> 4);
      LightValues *layer = cachedCellValues;
      if (!layer)
        layer = workingLayer(chunkKeyValue, key.y);
      if (!layer) {
        if (storageReady && !eligibleHere(key.x, key.y, key.z))
          return false;
        const LightChunkSnapshot *chunk = stateFor(chunkKeyValue);
        if (!chunk)
          return false;
        layer = insertWorkingLayer(
            chunkKeyValue, key.y,
            initialWorkingLayer(chunkKeyValue, chunk, key.y));
        if (!layer)
          return false;
      }
      const auto index = lightIndex(position.x, position.y, position.z);
      (*layer)[index] = static_cast<std::uint8_t>(level);
      cachedCellValues = layer;
      dirtySections.insert(key);
      return true;
    }

    void allocateSection(std::uint64_t chunkKeyValue, int sectionY) {
      const SectionPos3 key{static_cast<int>(chunkKeyValue >> 32), sectionY,
                            static_cast<int>(
                                static_cast<std::uint32_t>(chunkKeyValue))};
      if (workingLayer(chunkKeyValue, sectionY))
        return;
      const LightChunkSnapshot *chunk = stateFor(chunkKeyValue);
      if (!chunk)
        return;
      const LightSectionSnapshot *section = chunk->section(sectionY);
      if (!section || !section->layers[static_cast<std::size_t>(LightLayer::Sky)]
                          .isMissing())
        return;
      insertWorkingLayer(
          chunkKeyValue, sectionY,
          initialWorkingLayer(chunkKeyValue, chunk, sectionY));
      dirtySections.insert(key);
    }

    const BlockLightProperties &sourcePropertyAt(
        const WorkingChunk &chunk, int x, int y, int z) const {
      const int minY = owner.minBlockSectionY * 16;
      const int maxY =
          (owner.minBlockSectionY + owner.blockSectionCount) * 16;
      if (y < minY || y >= maxY)
        return emptyLightProperties();
      if (const GeometrySection *geometry =
              owner.geometrySection(chunk.key, y >> 4)) {
        const std::uint16_t classId =
            geometry->uniform
                ? geometry->uniformClass
                : geometry->at(lightIndex(x, y, z));
        return owner.opticalClass(classId).properties;
      }
      const LightSectionSnapshot *section = chunk.snapshot->section(y >> 4);
      return section ? section->at(x, y & 15, z)
                     : unsupportedLightProperties();
    }

    static bool sourceEmptySection(const LightSectionSnapshot *section) {
      if (!section || !section->empty || !section->isUniform())
        return false;
      const auto &properties = section->palette.front();
      // emptyShape() is a logical flag. Face occlusion consults the actual
      // shape ID, so prove both vertical faces are absent before skipping.
      return properties.supported && properties.dampening == 0 &&
             faceRect(properties, 0) == nullptr &&
             faceRect(properties, 1) == nullptr;
    }

    static bool isSourceEdgeOccluded(
        const BlockLightProperties &top,
        const BlockLightProperties &bottom) {
      if (bottom.supported && bottom.dampening != 0)
        return true;
      return faceShapesOcclude(top, bottom, 0);
    }

    int findSourceBelow(const WorkingChunk &chunk, int x, int z,
                        int topY) const {
      const int minY = owner.minBlockSectionY * 16;
      const int firstBottomY = topY - 1;
      if (firstBottomY < minY)
        return minY - 1;
      const int firstSectionY = firstBottomY >> 4;
      const int bottomSectionY = minY >> 4;
      const auto &top = sourcePropertyAt(chunk, x, topY, z);
      const BlockLightProperties *topState = &top;
      for (int sectionY = firstSectionY; sectionY >= bottomSectionY;
           --sectionY) {
        const LightSectionSnapshot *section =
            chunk.snapshot->section(sectionY);
        const int baseY = sectionY * 16;
        const int localStart =
            sectionY == firstSectionY ? (firstBottomY & 15) : 15;
        if (sourceEmptySection(section)) {
          const auto &empty = emptyLightProperties();
          if (isSourceEdgeOccluded(*topState, empty))
            return baseY + localStart + 1;
          topState = &empty;
          continue;
        }
        if (!section)
          return baseY + localStart + 1;
        for (int localY = localStart; localY >= 0; --localY) {
          const auto &bottom =
              sourcePropertyAt(chunk, x, baseY + localY, z);
          if (isSourceEdgeOccluded(*topState, bottom))
            return baseY + localY + 1;
          topState = &bottom;
        }
      }
      return minY - 1;
    }

    int computeSource(WorkingChunk &chunk, int x, int z) const {
      const int minY = owner.minBlockSectionY * 16;
      const int topSectionY =
          owner.minBlockSectionY + owner.blockSectionCount - 1;
      const int bottomSectionY = owner.minBlockSectionY;
      const auto &empty = emptyLightProperties();
      const BlockLightProperties *top = &empty;
      for (int sectionY = topSectionY; sectionY >= bottomSectionY;
           --sectionY) {
        const LightSectionSnapshot *section =
            chunk.snapshot->section(sectionY);
        const int baseY = sectionY * 16;
        if (sourceEmptySection(section)) {
          if (isSourceEdgeOccluded(*top, empty))
            return baseY + 16;
          top = &empty;
          continue;
        }
        if (!section)
          return baseY + 16;
        for (int localY = 15; localY >= 0; --localY) {
          const auto &bottom =
              sourcePropertyAt(chunk, x, baseY + localY, z);
          if (isSourceEdgeOccluded(*top, bottom))
            return baseY + localY + 1;
          top = &bottom;
        }
      }
      return minY - 1;
    }

    SkyColumnSources computeSources(WorkingChunk &chunk) const {
      SkyColumnSources result{};
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          result[HeightmapColumnKey(x, z)] = computeSource(chunk, x, z);
      return result;
    }

    void clearLines(WorkingChunk &chunk) {
      for (auto &section : chunk.snapshot->sections) {
        auto copy = std::make_shared<LightSectionSnapshot>(*section);
        copy->layers[static_cast<std::size_t>(LightLayer::Sky)] =
            LightLayerSnapshot::missing();
        section = copy;
      }
      chunk.mutableSections.clear();
      dropAllWorkingLayers(chunkKey(chunk.chunkX, chunk.chunkZ));
      invalidateInheritedBases(chunkKey(chunk.chunkX, chunk.chunkZ));
      cachedStateValid = false;
      cellCacheValid = false;
    }

    int sourceFor(int blockX, int blockZ) {
      const int chunkX = blockX >> 4;
      const int chunkZ = blockZ >> 4;
      const std::uint64_t key = chunkKey(chunkX, chunkZ);
      auto found = sourceHeights.find(key);
      if (found == sourceHeights.end()) {
        const LightChunkSnapshot *state = stateFor(key);
        if (!state)
          return owner.minBlockSectionY * 16 - 1;
        WorkingChunk temp;
        temp.key = key;
        temp.chunkX = state->chunkX;
        temp.chunkZ = state->chunkZ;
        temp.snapshot = std::make_shared<LightChunkSnapshot>(*state);
        found = sourceHeights.emplace(key, computeSources(temp)).first;
      }
      return found->second[HeightmapColumnKey(blockX & 15, blockZ & 15)];
    }

    void seedColumn(std::uint64_t, WorkingChunk *chunk, int x, int z,
                    int source) {
      const int minY = owner.minBlockSectionY * 16;
      const int lightTop =
          (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16;
      const int sentinel = minY - 1;
      const int bx = chunk->chunkX * 16 + x;
      const int bz = chunk->chunkZ * 16 + z;
      const int north = sourceFor(bx, bz - 1);
      const int south = sourceFor(bx, bz + 1);
      const int west = sourceFor(bx - 1, bz);
      const int east = sourceFor(bx + 1, bz);
      const int neighborHighest = std::max({north, south, west, east});
      const int first = source == sentinel ? minY : source;
      for (int y = lightTop - 1; y >= first; --y) {
        if (y != source && y >= neighborHighest)
          continue;
        std::uint8_t directions = 0;
        if (y == source)
          directions |= 1u << 0;
        if (y < north)
          directions |= 1u << 2;
        if (y < south)
          directions |= 1u << 3;
        if (y < west)
          directions |= 1u << 4;
        if (y < east)
          directions |= 1u << 5;
        if (directions)
          enqueueIncrease({bx, y, bz}, 15, directions, false, true);
      }
    }

    bool isSource(LightPos position) {
      return position.y >= sourceFor(position.x, position.z);
    }

    void checkNode(LightPos position) {
      if (!sectionAt(position))
        return;
      if (isSource(position)) {
        enqueueDecrease(position, 15, 63u & ~(1u << 1));
        enqueueIncrease(position, 15, 63u & ~(1u << 1), false, true);
      } else {
        const int oldLevel = storedLevel(position);
        if (oldLevel > 0) {
          setStoredLevel(position, 0);
          enqueueDecrease(position, static_cast<std::uint8_t>(oldLevel), 63);
        } else {
          enqueueDecrease(position, 1, 63);
        }
      }
    }

    void propagateIncrease(const IncreaseTask &task) {
      int fromLevel = storedLevel(task.position);
      if (task.fromEmission) {
        if (fromLevel < task.level) {
          setStoredLevel(task.position, task.level);
          fromLevel = task.level;
        }
      }
      if (fromLevel != task.level)
        return;
      const int emptySectionsBelow =
          countEmptySectionsBelowIfAtBorder(task.position);
      const auto &fromQueued =
          task.fromEmpty ? emptyLightProperties() : stateAt(task.position);
      for (int direction = 0; direction < 6; ++direction) {
        if ((task.directions & (1u << direction)) == 0)
          continue;
        const LightPos to = offset(task.position, direction);
        if (!sectionAt(to))
          continue;
        const int toLevel = storedLevel(to);
        if (fromLevel - 1 <= toLevel)
          continue;
        const auto &toState = stateAt(to);
        const int newLevel = fromLevel - lightOpacity(toState);
        if (newLevel <= toLevel || faceShapesOcclude(fromQueued, toState, direction))
          continue;
        if (!setStoredLevel(to, newLevel))
          continue;
        if (newLevel > 1)
          enqueueIncrease(to, static_cast<std::uint8_t>(newLevel),
                          static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))),
                          emptyShape(toState), false);
        propagateFromEmptySections(to, direction, newLevel, true,
                                   emptySectionsBelow);
      }
    }

    void propagateDecrease(const DecreaseTask &task) {
      const int emptySectionsBelow =
          countEmptySectionsBelowIfAtBorder(task.position);
      for (int direction = 0; direction < 6; ++direction) {
        if ((task.directions & (1u << direction)) == 0)
          continue;
        const LightPos to = offset(task.position, direction);
        if (!sectionAt(to))
          continue;
        if (isSource(to)) {
          enqueueIncrease(to, 15, 63, false, true);
          continue;
        }
        const int toLevel = storedLevel(to);
        if (toLevel == 0)
          continue;
        if (toLevel <= int(task.level) - 1) {
          if (!setStoredLevel(to, 0))
            continue;
          enqueueDecrease(to, static_cast<std::uint8_t>(toLevel),
                          static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))));
          propagateFromEmptySections(to, direction, toLevel, false,
                                     emptySectionsBelow);
        } else {
          enqueueIncrease(to, static_cast<std::uint8_t>(toLevel),
                          static_cast<std::uint8_t>(1u << (direction ^ 1)),
                          false, false);
        }
      }
    }

    int countEmptySectionsBelowIfAtBorder(LightPos position) const {
      if ((position.y & 15) != 0)
        return 0;
      const int localX = position.x & 15;
      const int localZ = position.z & 15;
      if (localX != 0 && localX != 15 && localZ != 0 && localZ != 15)
        return 0;
      const int sectionX = position.x >> 4;
      const int sectionZ = position.z >> 4;
      int sectionY = position.y >> 4;
      int count = 0;
      while (sectionY - count - 1 >= owner.minBlockSectionY - 1) {
        const SectionPos3 key{sectionX, sectionY - count - 1, sectionZ};
        if (storedEligible.contains(key))
          break;
        ++count;
      }
      return count;
    }

    static bool crossedSkySectionEdge(int direction, int localX,
                                      int localZ) {
      if (direction == 2)
        return localZ == 15;
      if (direction == 3)
        return localZ == 0;
      if (direction == 4)
        return localX == 15;
      if (direction == 5)
        return localX == 0;
      return false;
    }

    void propagateFromEmptySections(LightPos to, int direction, int level,
                                    bool increase, int emptySectionsBelow) {
      if (emptySectionsBelow == 0 ||
          !crossedSkySectionEdge(direction, to.x & 15, to.z & 15))
        return;
      const int sectionX = to.x >> 4;
      const int sectionZ = to.z >> 4;
      int sectionY = (to.y >> 4) - 1;
      const int bottomSectionY = sectionY - emptySectionsBelow + 1;
      while (sectionY >= bottomSectionY) {
        const SectionPos3 key{sectionX, sectionY, sectionZ};
        if (!storedEligible.contains(key)) {
          --sectionY;
          continue;
        }
        for (int localY = 15; localY >= 0; --localY) {
          const LightPos position{to.x, sectionY * 16 + localY, to.z};
          if (increase) {
            if (setStoredLevel(position, level) && level > 1)
              enqueueIncrease(
                  position, static_cast<std::uint8_t>(level),
                  static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))),
                  true, false);
          } else {
            if (storedLevel(position) == 0)
              continue;
            if (setStoredLevel(position, 0))
              enqueueDecrease(
                  position, static_cast<std::uint8_t>(level),
                  static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))));
          }
        }
        --sectionY;
      }
    }

    template <class Visitor>
    void forEachFaceCell(const LightChunkSnapshot *chunk, int direction,
                         Visitor visitor) const {
      const int minX = chunk->chunkX * 16;
      const int minZ = chunk->chunkZ * 16;
      const int minY = (owner.minBlockSectionY - 1) * 16;
      const int maxY =
          (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16;
      if (direction == 2 || direction == 3) {
        const int z = direction == 2 ? minZ : minZ + 15;
        for (int y = minY; y < maxY; ++y)
          for (int x = 0; x < 16; ++x)
            visitor(LightPos{minX + x, y, z});
      } else if (direction == 4 || direction == 5) {
        const int x = direction == 4 ? minX : minX + 15;
        for (int y = minY; y < maxY; ++y)
          for (int z = 0; z < 16; ++z)
            visitor(LightPos{x, y, minZ + z});
      }
    }

    static SectionPos3 sectionOf(LightPos position) {
      return {position.x >> 4, position.y >> 4, position.z >> 4};
    }

    void enqueueIncrease(LightPos position, std::uint8_t level,
                         std::uint8_t directions, bool fromEmpty,
                         bool source) {
      if (level == 0)
        return;
      IncreaseTask task{position, level, directions, fromEmpty, source};
      if (activeIncreaseSection &&
          sectionOf(position) == *activeIncreaseSection)
        currentIncreaseQueue.push(std::move(task));
      else
        increaseQueue.push(std::move(task));
    }

    void enqueueDecrease(LightPos position, std::uint8_t level,
                         std::uint8_t directions) {
      DecreaseTask task{position, level, directions};
      if (activeDecreaseSection &&
          sectionOf(position) == *activeDecreaseSection)
        currentDecreaseQueue.push(std::move(task));
      else
        decreaseQueue.push(std::move(task));
    }

    Impl &owner;
    std::unordered_map<std::uint64_t, WorkingChunk> &working;
    const std::unordered_set<std::uint64_t> &removed;
    std::unordered_map<std::uint64_t, SkyColumnSources> &sourceHeights;
    const std::unordered_set<std::uint64_t> &affected;
    std::unordered_set<LightPos, LightPosHash> checks;
    std::deque<LightPos> checkQueue;
    TaskQueue<DecreaseTask> decreaseQueue;
    TaskQueue<IncreaseTask> increaseQueue;
    TaskQueue<DecreaseTask> currentDecreaseQueue;
    TaskQueue<IncreaseTask> currentIncreaseQueue;
    std::optional<SectionPos3> activeDecreaseSection;
    std::optional<SectionPos3> activeIncreaseSection;
    mutable std::unordered_map<std::uint64_t, WorkingLayers> layersByChunk;
    mutable bool cellCacheValid = false;
    mutable SectionPos3 cachedCellSection{};
    mutable std::uint64_t cachedCellChunk = 0;
    mutable LightValues *cachedCellValues = nullptr;
    mutable const LightSectionSnapshot *cachedCellSectionSnapshot = nullptr;
    mutable bool cachedLayersValid = false;
    mutable std::uint64_t cachedLayersKey = 0;
    mutable WorkingLayers *cachedLayersPtr = nullptr;
    std::unordered_set<SectionPos3, SectionPos3Hash> dirtySections;
    std::unordered_set<SectionPos3, SectionPos3Hash> storedEligible;
    // stateFor is called for every cell too; cache its chunk lookup and drop
    // the cache whenever the working set or a section is replaced.
    mutable bool cachedStateValid = false;
    mutable std::uint64_t cachedStateKey = 0;
    mutable const LightChunkSnapshot *cachedStatePtr = nullptr;
    // Same idea for the per-batch storage eligibility: setStoredLevel probes it
    // for every cell it writes, but the answer only changes per section.
    mutable bool eligibleValid = false;
    mutable std::uint64_t eligibleChunkKey = 0;
    mutable int eligibleSectionY = 0;
    mutable bool eligibleResult = false;
    bool storageReady = false;
    mutable bool incomplete = false;
  };

  bool validSection(int sectionY) const {
    return sectionY >= minBlockSectionY &&
           sectionY < minBlockSectionY + blockSectionCount;
  }

  bool validInput(const LightChunkInput &input) const {
    if (input.minBlockSectionY != minBlockSectionY ||
        input.blockSectionCount != blockSectionCount)
      return false;
    if (!input.geometrySections.empty()) {
      if (input.geometrySections.size() !=
          static_cast<std::size_t>(blockSectionCount))
        return false;
      for (const GeometrySection &section : input.geometrySections) {
        if (section.uniform) {
          if (section.cells || section.uniformClass >= opticalTable->classCount())
            return false;
        } else if (!section.cells) {
          return false;
        } else {
          for (std::uint8_t classId : *section.cells)
            if (classId >= opticalTable->classCount())
              return false;
        }
      }
      return true;
    }
    if (input.sections.size() !=
        static_cast<std::size_t>(blockSectionCount + 2))
      return false;
    for (std::size_t i = 0; i < input.sections.size(); ++i)
      if (!input.sections[i] ||
          input.sections[i]->sectionY !=
              minBlockSectionY - 1 + static_cast<int>(i))
        return false;
    return true;
  }

  static std::uint32_t changeSlot(int sectionY, std::uint16_t localIndex) {
    return (static_cast<std::uint32_t>(sectionY) << 12) | localIndex;
  }

  void reportBlockCell(LightCellUpdate update) {
    if (!acceptingCells.load(std::memory_order_acquire)) {
      ++statsRejected;
      return;
    }
    GeometryInput input{nextRequestGeneration(), update};
    try {
      if (!inputQueue->push(std::move(input))) {
        ++statsDroppedGeometry;
        return;
      }
    } catch (const std::bad_alloc &) {
      // Segment growth is unbounded up to available memory. Preserve the old
      // explicit-drop reporting if a host cannot allocate the next segment.
      ++statsDroppedGeometry;
      return;
    }
    notifyWorkerSleeping();
  }

  bool ringEmpty() const { return inputQueue->empty(); }

  bool orderedInputEmptyLocked() const {
    return controlQueue->empty() && ringEmpty();
  }

  void applyOrderedControlLocked(PendingChunk control) {
    workerVersions[control.key] = {control.lifecycleEpoch, control.revision,
                                   control.unload};
    if (control.load || control.unload) {
      pending[control.key] = std::move(control);
      return;
    }
    auto inserted = pending.try_emplace(control.key, PendingChunk{});
    PendingChunk &state = inserted.first->second;
    if (control.generation <= state.inputSequence)
      return;
    state.key = control.key;
    state.generation = control.generation;
    state.lifecycleEpoch = control.lifecycleEpoch;
    state.revision = control.revision;
    for (auto &[slot, change] : control.changes) {
      const auto found = state.changes.find(slot);
      if (found == state.changes.end()) {
        state.changes.emplace(slot, std::move(change));
      } else {
        found->second.newProperties = change.newProperties;
        found->second.isEmpty = change.isEmpty;
        found->second.generation = change.generation;
      }
    }
    for (const auto &[sectionY, status] : control.sectionStatus) {
      auto &current = state.sectionStatus[sectionY];
      if (status.sequence > current.sequence)
        current = status;
    }
    for (const auto &[slot, owned] : control.ownedCells) {
      auto &current = state.ownedCells[slot];
      if (owned.sequence > current.sequence)
        current = owned;
    }
  }

  void drainCellChangesLocked() {
    const std::size_t initialGeometry = inputQueue->size();
    const std::size_t initialControls = controlQueue->size();
    const std::size_t maximum = initialGeometry + initialControls;
    for (std::size_t round = 0; round < maximum; ++round) {
      const PendingChunk *control = controlQueue->front();
      const GeometryInput *geometry = inputQueue->front();
      const std::optional<std::uint64_t> nextGeometry =
          geometry ? std::optional<std::uint64_t>{geometry->sequence}
                   : std::optional<std::uint64_t>{};
      const std::optional<std::uint64_t> nextControl =
          control ? std::optional<std::uint64_t>{control->generation}
                  : std::optional<std::uint64_t>{};
      if (!nextGeometry && !nextControl)
        break;
      if (nextControl &&
          (!nextGeometry || *nextControl < *nextGeometry)) {
        PendingChunk state;
        if (!controlQueue->pop(state))
          break;
        applyOrderedControlLocked(std::move(state));
        noteHighWaterLocked();
      } else {
        GeometryInput input;
        if (!inputQueue->pop(input))
          break;
        (void)applyOwnedCellLocked(input.sequence, input.update);
      }
    }
  }

  void notePendingLocked() {
    ++statsSubmitted;
    noteHighWaterLocked();
  }

  // The queue length only grows when a chunk first becomes pending, so the CAS
  // is skipped whenever the current length is already at or below the mark.
  std::size_t pendingChunkCountLocked() const {
    return pending.size() + controlQueue->size() + busyChunks;
  }

  void noteHighWaterLocked() {
    const auto current = pendingChunkCountLocked();
    auto previous = statsHighWater.load(std::memory_order_relaxed);
    if (current <= previous)
      return;
    while (!statsHighWater.compare_exchange_weak(previous, current,
                                                 std::memory_order_relaxed)) {
      if (current <= previous)
        return;
    }
  }

  void awaitWork(std::unique_lock<std::mutex> &lock) {
    for (;;) {
      if (closing ||
          (!paused && (!pending.empty() || !orderedInputEmptyLocked())))
        return;
      workerSleeping.store(true, std::memory_order_release);
      sleepState.notify_all();
      // Capture after advertising sleep, then re-check. A producer on either
      // side publishes the ring before advancing nextGeneration. If it runs
      // before the capture, the re-check sees its work; if it runs after, the
      // atomic wait is released by the newer sequence.
      const auto observed = wakeSequence.load(std::memory_order_acquire);
      if (closing ||
          (!paused && (!pending.empty() || !orderedInputEmptyLocked()))) {
        workerSleeping.store(false, std::memory_order_release);
        return;
      }
      lock.unlock();
      wakeSequence.wait(observed, std::memory_order_acquire);
      lock.lock();
      workerSleeping.store(false, std::memory_order_release);
    }
  }

  void run() {
    const double workerCpuStart = currentThreadCpuMs();
    std::unique_lock lock(mutex);
    for (;;) {
      awaitWork(lock);
      if (closing && pending.empty() && orderedInputEmptyLocked())
        break;
      if (paused && !closing)
        continue;
      // Coalesce before taking a batch. A batch clones every section it
      // touches for copy-on-write publication, so waiting a few milliseconds
      // for more writes turns many small batches into one larger batch and
      // clones each hot section far less often. The window closes early as
      // soon as the batch is full, and closing must never wait.
      if (!closing && !forcedChunk && options.batchWindowMs > 0 &&
          pending.size() < options.batchSize) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(options.batchWindowMs);
        while (!closing && !paused && !forcedChunk &&
               pending.size() < options.batchSize &&
               std::chrono::steady_clock::now() < deadline)
          wake.wait_until(lock, deadline);
      }
      if (paused && !closing)
        continue;
      drainCellChangesLocked();

      std::vector<PendingChunk> batch;
      if (forcedChunk) {
        const auto selected = pending.find(*forcedChunk);
        forcedChunk.reset();
        if (selected != pending.end()) {
          batch.push_back(std::move(selected->second));
          pending.erase(selected);
        }
      } else {
        const std::size_t count = std::min(
            {options.batchSize, options.maxQueuedCommands, pending.size()});
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
          auto it = pending.begin();
          batch.push_back(std::move(it->second));
          pending.erase(it);
        }
      }
      busy = true;
      busyChunks = batch.size();
      lock.unlock();

      try {
        processBatch(batch);
      } catch (const std::exception &failure) {
        lock.lock();
        error = failure.what();
        lock.unlock();
      } catch (...) {
        lock.lock();
        error = "unknown light worker failure";
        lock.unlock();
      }

      lock.lock();
      busy = false;
      busyChunks = 0;
      ++statsBatches;
      statsWorkerCpuMs.store(currentThreadCpuMs() - workerCpuStart,
                             std::memory_order_relaxed);
      if (pauseAfterBatch) {
        pauseAfterBatch = false;
        paused = true;
      }
      idle.notify_all();
      batchDone.notify_all();
      if (closing && pending.empty() && orderedInputEmptyLocked())
        break;
    }
  }

  PreparedBatch prepareBatch(std::vector<PendingChunk> &batch) {
    PreparedBatch prepared;
    // Once this owner has accepted raw production geometry, subsequent
    // ordinary cell batches continue through the independent layer path even
    // though they do not carry a full load record.
    prepared.rawGeometry = !geometry.empty();
    const auto ensurePrepared = [&](std::uint64_t key) -> WorkingChunk * {
      if (prepared.removed.contains(key))
        return nullptr;
      const auto found = prepared.chunks.find(key);
      if (found != prepared.chunks.end())
        return &found->second;
      const auto &layerStored = storedLayerSnapshot(LightLayer::Block, key);
      const auto stored =
          layerStored ? layerStored : storedLayerSnapshot(LightLayer::Sky, key);
      if (!stored)
        return nullptr;
      WorkingChunk next;
      next.key = key;
      next.lifecycleEpoch = stored->lifecycleEpoch;
      next.chunkX = stored->chunkX;
      next.chunkZ = stored->chunkZ;
      next.snapshot = std::make_shared<LightChunkSnapshot>(*stored);
      return &prepared.chunks.emplace(key, std::move(next)).first->second;
    };

    for (PendingChunk &command : batch) {
      const auto key = command.key;
      prepared.maxGeneration = std::max(prepared.maxGeneration,
                                        command.generation);
      if (command.unload) {
        prepared.removed.insert(key);
        prepared.loaded.erase(key);
        prepared.chunks.erase(key);
      }

      WorkingChunk *chunk = nullptr;
      if (command.load) {
        prepared.removed.erase(key);
        auto snapshot = std::make_shared<LightChunkSnapshot>();
        snapshot->chunkX = command.load->chunkX;
        snapshot->chunkZ = command.load->chunkZ;
        snapshot->minBlockSectionY = command.load->minBlockSectionY;
        snapshot->blockSectionCount = command.load->blockSectionCount;
        snapshot->skyLightEnabled = options.hasSkyLight;
        snapshot->generation = command.generation;
        snapshot->lifecycleEpoch = command.lifecycleEpoch;
        snapshot->revision = 0;
        if (!command.load->geometrySections.empty()) {
          prepared.rawGeometry = true;
          loadGeometry(key, *command.load);
          snapshot->sections.reserve(
              static_cast<std::size_t>(blockSectionCount + 2));
          for (int sectionY = minBlockSectionY - 1;
               sectionY <= minBlockSectionY + blockSectionCount;
               ++sectionY) {
            const GeometrySection *geometry =
                geometrySection(key, sectionY);
            const bool sectionEmpty = geometry ? geometry->empty : true;
            auto section = std::make_shared<LightSectionSnapshot>(
                sectionY, sectionEmpty,
                std::vector<BlockLightProperties>{emptyLightProperties()},
                std::vector<std::uint16_t>{});
            if (geometry)
              section->unsupportedCells = geometry->unknown;
            snapshot->sections.push_back(std::move(section));
          }
        } else {
          snapshot->sections = command.load->sections;
        }

        const auto &oldLayer = storedLayerSnapshot(LightLayer::Block, key);
        const bool replacedEpoch =
            oldLayer && oldLayer->lifecycleEpoch != command.lifecycleEpoch;
        const bool sameEpoch =
            oldLayer && oldLayer->lifecycleEpoch == command.lifecycleEpoch;
        if (replacedEpoch) {
          // A reload must never inherit an old epoch's mutable pages. Readers
          // that already hold them stay valid, but the new lifetime starts
          // with absent layers and fresh mixed allocations.
          lastReadyBlock.erase(key);
          lastReadySky.erase(key);
          protocolBaseBlock.erase(key);
          protocolBaseSky.erase(key);
          pendingProtocolDirtyBlock.erase(key);
          pendingProtocolDirtySky.erase(key);
          pendingProtocolEpochBlock.erase(key);
          pendingProtocolEpochSky.erase(key);
          internal.erase(key);
          prepared.replaced.insert(key);
        }
        if (sameEpoch) {
          prepared.replaced.insert(key);
          for (std::size_t i = 0;
               i < snapshot->sections.size() &&
               i < oldLayer->sections.size();
               ++i) {
            for (LightLayer layer : {LightLayer::Block, LightLayer::Sky}) {
              const auto index = static_cast<std::size_t>(layer);
              if (!snapshot->sections[i]->layers[index].isMissing())
                continue;
              auto section = std::make_shared<LightSectionSnapshot>(
                  *snapshot->sections[i]);
              section->layers[index] = oldLayer->sections[i]->layers[index];
              snapshot->sections[i] = section;
            }
          }
          for (int section = 0; section < command.load->blockSectionCount;
               ++section) {
            const int sectionY = command.load->minBlockSectionY + section;
            const LightSectionSnapshot *oldSection = oldLayer->section(sectionY);
            const LightSectionSnapshot *newSection = snapshot->section(sectionY);
            if (!oldSection || !newSection)
              continue;
            if (oldSection->isUniform() && newSection->isUniform() &&
                oldSection->at(0, 0, 0) == newSection->at(0, 0, 0))
              continue;
            for (int index = 0; index < kLightVolume; ++index) {
              const int x = index & 15;
              const int z = (index >> 4) & 15;
              const int y = (index >> 8) & 15;
              if (oldSection->at(x, y, z) == newSection->at(x, y, z))
                continue;
              prepared.changedPositions.push_back(
                  {snapshot->chunkX * 16 + x, sectionY * 16 + y,
                   snapshot->chunkZ * 16 + z});
            }
          }
        }

        if (!options.hasSkyLight) {
          for (std::size_t i = 0; i < snapshot->sections.size(); ++i) {
            const auto &layer = snapshot->sections[i]
                                    ->layers[static_cast<std::size_t>(
                                        LightLayer::Sky)];
            if (layer.isMissing() ||
                (layer.isUniform() && layer.uniformValue() == 0))
              continue;
            auto section = std::make_shared<LightSectionSnapshot>(
                *snapshot->sections[i]);
            section->layers[static_cast<std::size_t>(LightLayer::Sky)] =
                LightLayerSnapshot::uniform(0);
            snapshot->sections[i] = section;
          }
        }

        WorkingChunk next;
        next.key = key;
        next.lifecycleEpoch = command.lifecycleEpoch;
        next.chunkX = snapshot->chunkX;
        next.chunkZ = snapshot->chunkZ;
        next.snapshot = std::move(snapshot);
        next.dirty = true;
        chunk = &prepared.chunks.insert_or_assign(
                               key, std::move(next)).first->second;
        prepared.loaded.insert(key);
      } else if (!command.unload) {
        chunk = ensurePrepared(key);
        if (!chunk)
          continue;
      }

      if (!chunk)
        continue;
      for (const auto &[slot, change] : command.changes) {
        (void)slot;
        LightSectionSnapshot *section =
            chunk->sectionForEdit(change.sectionY);
        if (!section)
          throw std::runtime_error(
              "light change references a missing section");
        const auto paletteIt = std::find(section->palette.begin(),
                                         section->palette.end(),
                                         change.newProperties);
        std::uint16_t paletteIndex = 0;
        if (paletteIt == section->palette.end()) {
          if (section->palette.size() >= 65536)
            throw std::runtime_error("light section palette exceeds uint16");
          paletteIndex = static_cast<std::uint16_t>(section->palette.size());
          section->palette.push_back(change.newProperties);
        } else {
          paletteIndex = static_cast<std::uint16_t>(
              paletteIt - section->palette.begin());
        }
        if (section->cells.empty()) {
          section->cells.assign(kLightVolume, 0);
          if (!section->palette.front().supported)
            section->unsupportedCells = kLightVolume;
        }
        const auto oldIndex = section->cells[change.localIndex];
        if (!section->palette[oldIndex].supported)
          --section->unsupportedCells;
        section->cells[change.localIndex] = paletteIndex;
        if (!change.newProperties.supported)
          ++section->unsupportedCells;
        ++statsChanges;
        prepared.changedPositions.push_back(
            {chunk->chunkX * 16 + (change.localIndex & 15),
             change.sectionY * 16 + ((change.localIndex >> 8) & 15),
             chunk->chunkZ * 16 + ((change.localIndex >> 4) & 15)});
      }
      for (const auto &[slot, value] : command.ownedCells) {
        const int sectionY =
            static_cast<int>(std::bit_cast<std::int32_t>(slot) >> 12);
        const std::uint16_t localIndex =
            static_cast<std::uint16_t>(slot & 0xFFFu);
        LightSectionSnapshot *section = chunk->sectionForEdit(sectionY);
        if (!section)
          throw std::runtime_error("light change references a missing section");
        const auto properties = lightPropertiesOf(
            BlockState::fromId(value.state));
        const auto paletteIt = std::find(section->palette.begin(),
                                         section->palette.end(),
                                         properties);
        std::uint16_t paletteIndex = 0;
        if (paletteIt == section->palette.end()) {
          if (section->palette.size() >= 65536)
            throw std::runtime_error("light section palette exceeds uint16");
          paletteIndex = static_cast<std::uint16_t>(section->palette.size());
          section->palette.push_back(properties);
        } else {
          paletteIndex = static_cast<std::uint16_t>(
              paletteIt - section->palette.begin());
        }
        if (section->cells.empty()) {
          section->cells.assign(kLightVolume, 0);
          if (!section->palette.front().supported)
            section->unsupportedCells = kLightVolume;
        }
        const auto oldIndex = section->cells[localIndex];
        if (!section->palette[oldIndex].supported)
          --section->unsupportedCells;
        section->cells[localIndex] = paletteIndex;
        if (!section->palette[paletteIndex].supported)
          ++section->unsupportedCells;
        const RawCellResult rawChange = applyRawGeometryCell(
            key, sectionY, localIndex, value.state);
        if (const GeometrySection *geometry =
                geometrySection(key, sectionY)) {
          section->empty = geometry->empty;
        }
        const bool geometryMissing = geometrySection(key, sectionY) == nullptr;
        if (rawChange.opticalChanged || rawChange.emptyChanged ||
            geometryMissing) {
          ++statsChanges;
          prepared.changedPositions.push_back(
              {chunk->chunkX * 16 + static_cast<int>(localIndex & 15),
               sectionY * 16 + static_cast<int>((localIndex >> 8) & 15),
               chunk->chunkZ * 16 + static_cast<int>((localIndex >> 4) & 15)});
        }
      }
      for (const auto &[sectionY, status] : command.sectionStatus) {
        if (auto *section = chunk->sectionForEdit(sectionY))
          section->empty = status.empty;
      }
      chunk->dirty = true;
    }

    for (const auto &[key, chunk] : prepared.chunks) {
      (void)chunk;
      prepared.affected.insert(key);
      const int x = static_cast<int>(static_cast<std::int32_t>(key >> 32));
      const int z = static_cast<int>(static_cast<std::uint32_t>(key));
      for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
          prepared.affected.insert(chunkKey(x + dx, z + dz));
    }
    for (std::uint64_t key : prepared.removed) {
      const int x = static_cast<int>(static_cast<std::int32_t>(key >> 32));
      const int z = static_cast<int>(static_cast<std::uint32_t>(key));
      for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
          prepared.affected.insert(chunkKey(x + dx, z + dz));
    }
    return prepared;
  }

  LayerWorkspace layerWorkspace(const PreparedBatch &prepared,
                                LightLayer layer) const {
    LayerWorkspace result;
    result.removed = prepared.removed;
    for (const auto &[key, base] : prepared.chunks) {
      if (prepared.removed.contains(key))
        continue;
      WorkingChunk chunk;
      chunk.key = base.key;
      chunk.lifecycleEpoch = base.lifecycleEpoch;
      chunk.chunkX = base.chunkX;
      chunk.chunkZ = base.chunkZ;
      const auto &ownView = prepared.rawGeometry
                                ? storedLayerSnapshot(layer, key)
                                : std::shared_ptr<const LightChunkSnapshot>{};
      const LightChunkSnapshot &source =
          prepared.rawGeometry && ownView ? *ownView : *base.snapshot;
      chunk.snapshot = std::make_shared<LightChunkSnapshot>(source);
      chunk.snapshot->blockLayerSnapshot.reset();
      chunk.snapshot->skyLayerSnapshot.reset();
      if (prepared.rawGeometry) {
        const std::size_t other =
            static_cast<std::size_t>(layer == LightLayer::Block
                                         ? LightLayer::Sky
                                         : LightLayer::Block);
        for (auto &section : chunk.snapshot->sections) {
          if (!section)
            continue;
          const bool inBlockRange =
              section->sectionY >= chunk.snapshot->minBlockSectionY &&
              section->sectionY < chunk.snapshot->minBlockSectionY +
                                      chunk.snapshot->blockSectionCount;
          const GeometrySection *geometry =
              inBlockRange ? geometrySection(key, section->sectionY) : nullptr;
          const bool finalEmpty = geometry ? geometry->empty : true;
          if (!section->layers[other].isMissing() ||
              section->empty != finalEmpty) {
            auto copy = std::make_shared<LightSectionSnapshot>(*section);
            copy->layers[other] = LightLayerSnapshot::missing();
            if (copy->empty != finalEmpty) {
              copy->empty = finalEmpty;
              chunk.dirtySections.insert(section->sectionY);
            }
            section = std::move(copy);
          }
        }
      }
      chunk.dirty = base.dirty;
      if (prepared.loaded.contains(key)) {
        for (int sectionY = chunk.snapshot->minLightSectionY();
             sectionY <= chunk.snapshot->minBlockSectionY +
                             chunk.snapshot->blockSectionCount;
             ++sectionY)
          chunk.dirtySections.insert(sectionY);
      }
      result.chunks.emplace(key, std::move(chunk));
    }
    return result;
  }

  LayerOutcome executeBlockLayer(const PreparedBatch &prepared) {
    const double cpuStart = currentThreadCpuMs();
    if (options.beforeLayerForTesting)
      options.beforeLayerForTesting(LightLayer::Block);
    LayerOutcome result;
    result.layers = layerWorkspace(prepared, LightLayer::Block);
    BlockLightPropagator propagation(*this, result.layers.chunks,
                                     result.layers.removed);
    for (const LightPos &position : prepared.changedPositions)
      propagation.enqueueCheck(position);
    for (std::uint64_t key : prepared.loaded) {
      propagation.seedChunk(key);
      propagation.seedIncoming(key);
    }
    for (std::uint64_t key : prepared.removed)
      propagation.clearIncomingFrom(key);
    for (std::uint64_t key : prepared.replaced)
      if (!prepared.removed.contains(key))
        propagation.clearIncomingFrom(key);
    propagation.run();
    propagation.commitLayers();
    result.complete = propagation.isComplete();
    statsBlockWorkerCpuMs.fetch_add(
        currentThreadCpuMs() - cpuStart, std::memory_order_relaxed);
    return result;
  }

  LayerOutcome executeSkyLayer(const PreparedBatch &prepared) {
    const double cpuStart = currentThreadCpuMs();
    if (options.beforeLayerForTesting)
      options.beforeLayerForTesting(LightLayer::Sky);
    LayerOutcome result;
    result.layers = layerWorkspace(prepared, LightLayer::Sky);
    SkyLightPropagator sky(*this, result.layers.chunks,
                           result.layers.removed, skyColumnSources,
                           prepared.affected);
    for (std::uint64_t key : prepared.loaded) {
      sky.seedChunk(key);
      sky.seedIncoming(key);
    }
    for (std::uint64_t key : prepared.removed)
      sky.clearIncomingFrom(key);
    for (std::uint64_t key : prepared.replaced)
      if (!prepared.removed.contains(key))
        sky.clearIncomingFrom(key);

    std::vector<LightPos> orderedChanges = prepared.changedPositions;
    std::sort(orderedChanges.begin(), orderedChanges.end(),
              [](const LightPos &left, const LightPos &right) {
                if (left.x != right.x)
                  return left.x < right.x;
                if (left.z != right.z)
                  return left.z < right.z;
                return left.y < right.y;
              });
    struct PendingColumn {
      std::uint64_t key;
      int x;
      int z;
      int oldSource;
      int newSource;
    };
    std::vector<PendingColumn> postSeed;
    for (std::size_t begin = 0; begin < orderedChanges.size();) {
      std::size_t end = begin + 1;
      while (end < orderedChanges.size() &&
             orderedChanges[end].x == orderedChanges[begin].x &&
             orderedChanges[end].z == orderedChanges[begin].z)
        ++end;
      const LightPos &first = orderedChanges[begin];
      const std::uint64_t key =
          chunkKey(first.x >> 4, first.z >> 4);
      const int localX = first.x & 15;
      const int localZ = first.z & 15;
      const int oldSource = sky.columnSource(key, localX, localZ);
      int newSource = oldSource;
      for (std::size_t i = begin; i < end; ++i)
        newSource = sky.updateColumnSource(key, localX, localZ,
                                           orderedChanges[i].y);
      postSeed.push_back({key, localX, localZ, oldSource, newSource});
      begin = end;
    }
    // All column heights now describe the final optical batch. Only after this
    // point may seeding read neighboring heights.
    for (const PendingColumn &column : postSeed)
      sky.applyColumnSource(column.key, column.x, column.z,
                            column.oldSource, column.newSource);
    for (const LightPos &position : prepared.changedPositions)
      sky.enqueueCheck(position);
    sky.syncStorageEligibility();
    sky.run();
    sky.commitLayers();
    for (std::uint64_t key : prepared.removed)
      skyColumnSources.erase(key);
    result.complete = sky.isComplete();
    statsSkyWorkerCpuMs.fetch_add(
        currentThreadCpuMs() - cpuStart, std::memory_order_relaxed);
    return result;
  }

  void publishResult(LayerResult result) {
    while (!resultQueue->push(std::move(result))) {
      if (closing.load(std::memory_order_acquire))
        return;
      std::this_thread::yield();
    }
  }

  void publishIndependentLayer(const PreparedBatch &prepared,
                               LayerOutcome &outcome,
                               LightLayer layer) {
    if (layer == LightLayer::Sky && !options.hasSkyLight)
      return;
    auto &nextRevision =
        layer == LightLayer::Block ? nextBlockLayerRevision
                                   : nextSkyLayerRevision;
    auto &last = layer == LightLayer::Block ? lastReadyBlock : lastReadySky;
    auto &protocolBase =
        layer == LightLayer::Block ? protocolBaseBlock : protocolBaseSky;
    auto &pendingDirty =
        layer == LightLayer::Block ? pendingProtocolDirtyBlock
                                   : pendingProtocolDirtySky;
    auto &pendingEpoch =
        layer == LightLayer::Block ? pendingProtocolEpochBlock
                                   : pendingProtocolEpochSky;
    std::vector<LayerResult> ready;
    for (auto &[key, chunk] : outcome.layers.chunks) {
      if (!chunk.dirty || prepared.removed.contains(key))
        continue;
      // The layer workspace owns sections for this layer only. Publish that
      // same envelope and its section headers; no combined view is created.
      auto view = chunk.snapshot;
      view->layer = layer;
      view->blockLayerSnapshot.reset();
      view->skyLayerSnapshot.reset();
      view->generation = prepared.maxGeneration;
      view->revision = ++nextRevision;
      view->lifecycleEpoch = chunk.lifecycleEpoch;
      view->blockLightComplete =
          layer == LightLayer::Block ? outcome.complete : false;
      view->skyLightComplete =
          layer == LightLayer::Sky
              ? outcome.complete
              : (!options.hasSkyLight && layer == LightLayer::Block);
      view->propagationComplete = outcome.complete;
      view->protocolReady = outcome.complete;
      view->changedSections.clear();
      view->deltaBaseRevision = 0;
      // A mixed page is mutable and may already share its allocation with the
      // reader's previous view. Equality against that page cannot prove this
      // batch changed. Only this layer's propagator write set is authoritative.
      std::set<int> changed(chunk.dirtySections.begin(),
                            chunk.dirtySections.end());
      if (outcome.complete) {
        const auto base = protocolBase.find(key);
        if (base != protocolBase.end() && base->second &&
            base->second->lifecycleEpoch == view->lifecycleEpoch &&
            base->second->minLightSectionY() == view->minLightSectionY() &&
            base->second->lightSectionCount() == view->lightSectionCount())
          view->deltaBaseRevision = base->second->revision;
        const auto pending = pendingDirty.find(key);
        if (pending != pendingDirty.end() &&
            pendingEpoch[key] == view->lifecycleEpoch) {
          changed.insert(pending->second.begin(), pending->second.end());
          pendingDirty.erase(pending);
          pendingEpoch.erase(key);
        }
        protocolBase[key] = view;
      } else {
        auto &pending = pendingDirty[key];
        pending.insert(changed.begin(), changed.end());
        pendingEpoch[key] = view->lifecycleEpoch;
      }
      for (int sectionY : changed)
        view->changedSections.push_back(
            {sectionY, layer == LightLayer::Sky, layer == LightLayer::Block});
      if (options.collectDiagnostics) {
        const auto count = static_cast<std::uint64_t>(changed.size());
        if (layer == LightLayer::Block)
          statsBlockChanges.fetch_add(count, std::memory_order_relaxed);
        else
          statsSkyChanges.fetch_add(count, std::memory_order_relaxed);
      }
      last[key] = view;
      ready.push_back({true, layer, key, view});
    }
    for (LayerResult &result : ready)
      publishResult(std::move(result));
    statsPublished.fetch_add(ready.size(), std::memory_order_relaxed);
  }

  struct CombinedLayerResult {
    std::unordered_map<std::uint64_t, WorkingChunk> chunks;
    std::unordered_set<std::uint64_t> dirtyKeys;
    std::unordered_map<std::uint64_t,
                       std::shared_ptr<const LightChunkSnapshot>>
        blockSnapshots;
    std::unordered_map<std::uint64_t,
                       std::shared_ptr<const LightChunkSnapshot>>
        skySnapshots;
    bool blockComplete = false;
    bool skyComplete = false;
  };

  CombinedLayerResult executeLayers(const PreparedBatch &prepared) {
    LayerOutcome block = executeBlockLayer(prepared);
    if (prepared.rawGeometry)
      publishIndependentLayer(prepared, block, LightLayer::Block);
    std::optional<LayerOutcome> sky;
    if (options.hasSkyLight) {
      sky = executeSkyLayer(prepared);
      if (prepared.rawGeometry)
        publishIndependentLayer(prepared, *sky, LightLayer::Sky);
    }

    CombinedLayerResult result;
    result.blockComplete = block.complete;
    result.skyComplete = options.hasSkyLight ? sky->complete : true;
    for (const auto &[key, chunk] : block.layers.chunks) {
      result.blockSnapshots.emplace(key, chunk.snapshot);
      if (chunk.dirty)
        result.dirtyKeys.insert(key);
    }
    if (sky)
      for (const auto &[key, chunk] : sky->layers.chunks) {
        result.skySnapshots.emplace(key, chunk.snapshot);
        if (chunk.dirty)
          result.dirtyKeys.insert(key);
      }
    if (prepared.rawGeometry)
      return result;
    if (!sky) {
      result.chunks = std::move(block.layers.chunks);
      return result;
    }

    result.chunks = std::move(block.layers.chunks);
    for (auto &[key, skyChunk] : sky->layers.chunks) {
      if (prepared.removed.contains(key))
        continue;
      auto found = result.chunks.find(key);
      if (found == result.chunks.end()) {
        if (skyChunk.dirty)
          result.chunks.emplace(key, std::move(skyChunk));
        continue;
      }
      WorkingChunk &blockChunk = found->second;
      if (!blockChunk.dirty && !skyChunk.dirty)
        continue;
      auto merged =
          std::make_shared<LightChunkSnapshot>(*blockChunk.snapshot);
      const std::size_t count =
          std::min(merged->sections.size(), skyChunk.snapshot->sections.size());
      for (std::size_t index = 0; index < count; ++index) {
        const auto &blockSection = merged->sections[index];
        const auto &skySection = skyChunk.snapshot->sections[index];
        if (!blockSection)
          merged->sections[index] = skySection;
        else if (blockSection != skySection) {
          auto section =
              std::make_shared<LightSectionSnapshot>(*blockSection);
          if (skySection)
            section->layers[static_cast<std::size_t>(LightLayer::Sky)] =
                skySection->layers[static_cast<std::size_t>(LightLayer::Sky)];
          merged->sections[index] = std::move(section);
        }
      }
      blockChunk.snapshot = std::move(merged);
      blockChunk.mutableSections.clear();
      blockChunk.dirty = blockChunk.dirty || skyChunk.dirty;
    }
    return result;
  }

  std::shared_ptr<const LightChunkSnapshot> layerEnvelope(
      const std::shared_ptr<const LightChunkSnapshot> &source,
      const LightChunkSnapshot &combined, LightLayer layer) const {
    if (!source)
      return nullptr;
    auto result = std::make_shared<LightChunkSnapshot>(*source);
    result->blockLayerSnapshot.reset();
    result->skyLayerSnapshot.reset();
    result->generation = combined.generation;
    result->revision = combined.revision;
    result->lifecycleEpoch = combined.lifecycleEpoch;
    result->blockLightComplete = combined.blockLightComplete;
    result->skyLightComplete = combined.skyLightComplete;
    result->propagationComplete = combined.propagationComplete;
    result->protocolReady = combined.protocolReady;
    result->deltaBaseRevision = combined.deltaBaseRevision;
    result->changedSections.clear();
    const bool wanted = layer == LightLayer::Block;
    for (const auto &change : combined.changedSections)
      if (wanted ? change.block : change.sky)
        result->changedSections.push_back(change);
    return result;
  }

  void processBatch(std::vector<PendingChunk> &batch) {
    PreparedBatch prepared = prepareBatch(batch);
    CombinedLayerResult layers = executeLayers(prepared);
    if (prepared.rawGeometry) {
      // Block and sky were published independently at the point each worker
      // finished. There is no combined publication or reader-side merge.
      for (std::uint64_t key : prepared.removed) {
        geometry.erase(key);
        internal.erase(key);
        lastReadyBlock.erase(key);
        lastReadySky.erase(key);
        protocolBaseBlock.erase(key);
        protocolBaseSky.erase(key);
        pendingProtocolDirtyBlock.erase(key);
        pendingProtocolDirtySky.erase(key);
        pendingProtocolEpochBlock.erase(key);
        pendingProtocolEpochSky.erase(key);
      }
      return;
    }
    auto &working = layers.chunks;
    bool blockComplete = layers.blockComplete;
    bool skyComplete = layers.skyComplete;
    for (const auto &[key, chunk] : working) {
      if (prepared.removed.contains(key))
        continue;
      if (!prepared.rawGeometry) {
        chunk.snapshot->unsupportedCells = 0;
        for (const auto &section : chunk.snapshot->sections)
          chunk.snapshot->unsupportedCells += section->unsupportedCells;
      }
      if (chunk.snapshot->unsupportedCells != 0) {
        blockComplete = false;
        if (options.hasSkyLight)
          skyComplete = false;
      }
    }
    for (const auto &[key, chunk] : internal) {
      if (!prepared.removed.contains(key) && !working.contains(key) &&
          chunk->unsupportedCells != 0) {
        blockComplete = false;
        if (options.hasSkyLight)
          skyComplete = false;
      }
    }

    const std::uint64_t publicationRevision = ++nextPublicationRevision;
    std::uint64_t skySectionChanges = 0, blockSectionChanges = 0;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<LightChunkSnapshot>>>
        publication;
    for (auto &[key, chunk] : working) {
      if (prepared.removed.contains(key) || !chunk.dirty)
        continue;
      chunk.snapshot->generation = prepared.maxGeneration;
      chunk.snapshot->revision = publicationRevision;
      chunk.snapshot->lifecycleEpoch = chunk.lifecycleEpoch;
      chunk.snapshot->blockLightComplete = blockComplete;
      chunk.snapshot->skyLightComplete = skyComplete;
      chunk.snapshot->propagationComplete = blockComplete && skyComplete;
      chunk.snapshot->protocolReady = chunk.snapshot->propagationComplete;
      chunk.snapshot->changedSections.clear();
      chunk.snapshot->deltaBaseRevision = 0;
      if (chunk.snapshot->protocolReady) {
        const auto base = lastReady.find(key);
        if (base != lastReady.end() && base->second &&
            base->second->lifecycleEpoch == chunk.snapshot->lifecycleEpoch &&
            base->second->minLightSectionY() ==
                chunk.snapshot->minLightSectionY() &&
            base->second->lightSectionCount() ==
                chunk.snapshot->lightSectionCount())
          chunk.snapshot->deltaBaseRevision = base->second->revision;
        if (prepared.rawGeometry) {
          std::map<int, std::pair<bool, bool>> changes;
          const auto block = layers.blockSnapshots.find(key);
          const auto sky = layers.skySnapshots.find(key);
          const auto changedAgainst = [&](const LightChunkSnapshot *now,
                                          const LightChunkSnapshot *before,
                                          LightLayer layer) {
            if (!now)
              return;
            for (const auto &section : now->sections) {
              const LightSectionSnapshot *oldSection =
                  before ? before->section(section->sectionY) : nullptr;
              const bool changed =
                  !oldSection ||
                  oldSection->layers[static_cast<std::size_t>(layer)] !=
                      section->layers[static_cast<std::size_t>(layer)];
              if (!changed)
                continue;
              auto &flags = changes[section->sectionY];
              if (layer == LightLayer::Block)
                flags.second = true;
              else
                flags.first = true;
            }
          };
          const LightChunkSnapshot *before = base->second.get();
          changedAgainst(
              block == layers.blockSnapshots.end()
                  ? nullptr
                  : block->second.get(),
              before ? before->blockLayerSnapshot.get() : nullptr,
              LightLayer::Block);
          changedAgainst(
              sky == layers.skySnapshots.end()
                  ? nullptr
                  : sky->second.get(),
              before ? before->skyLayerSnapshot.get() : nullptr,
              LightLayer::Sky);
          for (const auto &[sectionY, flags] : changes)
            chunk.snapshot->changedSections.push_back(
                {sectionY, flags.first, flags.second});
          for (const auto &[sectionY, flags] : changes) {
            (void)sectionY;
            skySectionChanges += flags.first ? 1 : 0;
            blockSectionChanges += flags.second ? 1 : 0;
          }
        } else {
          for (const auto &now : chunk.snapshot->sections) {
            bool skyChanged = true;
            bool blockChanged = true;
            if (chunk.snapshot->deltaBaseRevision != 0) {
              const LightSectionSnapshot *before =
                  base->second->section(now->sectionY);
              if (before == now.get()) {
                skyChanged = false;
                blockChanged = false;
              } else if (before) {
                skyChanged =
                    before->layers[static_cast<std::size_t>(LightLayer::Sky)] !=
                    now->layers[static_cast<std::size_t>(LightLayer::Sky)];
                blockChanged =
                    before->layers[static_cast<std::size_t>(LightLayer::Block)] !=
                    now->layers[static_cast<std::size_t>(LightLayer::Block)];
              }
            }
            if (skyChanged || blockChanged)
              chunk.snapshot->changedSections.push_back(
                  {now->sectionY, skyChanged, blockChanged});
            skySectionChanges += skyChanged ? 1 : 0;
            blockSectionChanges += blockChanged ? 1 : 0;
          }
        }
      }
      std::shared_ptr<const LightChunkSnapshot> blockSource;
      if (const auto found = layers.blockSnapshots.find(key);
          found != layers.blockSnapshots.end())
        blockSource = found->second;
      else if (const auto found = internal.find(key);
               found != internal.end())
        blockSource = found->second;
      if (!blockSource) {
        if (const auto found = layers.skySnapshots.find(key);
            found != layers.skySnapshots.end())
          blockSource = found->second;
      }
      chunk.snapshot->blockLayerSnapshot =
          prepared.rawGeometry
              ? blockSource
              : layerEnvelope(blockSource, *chunk.snapshot,
                              LightLayer::Block);

      std::shared_ptr<const LightChunkSnapshot> skySource;
      if (options.hasSkyLight) {
        if (const auto found = layers.skySnapshots.find(key);
            found != layers.skySnapshots.end())
          skySource = found->second;
        else if (const auto found = internal.find(key);
                 found != internal.end())
          skySource = found->second;
        if (!skySource)
          skySource = blockSource;
      }
      chunk.snapshot->skyLayerSnapshot =
          skySource && !prepared.rawGeometry
              ? layerEnvelope(skySource, *chunk.snapshot, LightLayer::Sky)
              : skySource;
      if (prepared.rawGeometry)
        internal[key] = chunk.snapshot;
      else
        publication.emplace_back(key, chunk.snapshot);
    }
    if (options.collectDiagnostics) {
      statsSkyChanges.fetch_add(skySectionChanges,
                                std::memory_order_relaxed);
      statsBlockChanges.fetch_add(blockSectionChanges,
                                  std::memory_order_relaxed);
    }
    if (!publication.empty()) {
      std::sort(publication.begin(), publication.end(),
                [](const auto &left, const auto &right) {
                  return left.first < right.first;
                });
      for (auto &[key, snapshot] : publication) {
        internal[key] = snapshot;
        if (snapshot->protocolReady)
          lastReady[key] = snapshot;
        publishResult({false, LightLayer::Block, key, std::move(snapshot)});
      }
      statsPublished.fetch_add(publication.size(), std::memory_order_relaxed);
    }
    for (std::uint64_t key : prepared.removed) {
      geometry.erase(key);
      internal.erase(key);
      lastReady.erase(key);
      lastReadyBlock.erase(key);
      lastReadySky.erase(key);
    }
  }

  int minBlockSectionY;
  int blockSectionCount;
  LightWorkerOptions options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable idle;
  std::condition_variable batchDone;
  std::condition_variable sleepState;
  std::unordered_map<std::uint64_t, PendingChunk> pending;
  std::shared_ptr<const LightOpticalTable> opticalTable;
  std::unordered_map<std::uint64_t, GeometryChunk> geometry;
  std::unique_ptr<GrowableSpscRing<GeometryInput>> inputQueue;
  std::unique_ptr<SpscRing<PendingChunk>> controlQueue;
  std::unique_ptr<GrowableSpscRing<LayerResult>> resultQueue;
  struct ChunkVersion {
    std::uint64_t epoch = 0;
    std::uint64_t requestRevision = 0;
    // Merged from the former inactive set: block-change reporting probed it
    // with a second hash lookup on every write.
    bool inactive = false;
  };
  std::unordered_map<std::uint64_t, ChunkVersion> versions;
  // Worker-maintained lifecycle view consumed with the SPSC control stream.
  std::unordered_map<std::uint64_t, ChunkVersion> workerVersions;
  // Main-thread-only lifecycle view used by result draining; never touched by
  // the worker, so publication consumption cannot wait on input merging.
  std::unordered_map<std::uint64_t, ChunkVersion> mainVersions;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      latest;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      internal;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      lastReady;
  std::unordered_map<std::uint64_t, SkyColumnSources> skyColumnSources;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      latestBlock;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      latestSky;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      lastReadyBlock;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      lastReadySky;
  // Last network-visible complete result and dirty sections accumulated while
  // one layer was incomplete. Mutable pages make content comparison invalid.
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      protocolBaseBlock;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      protocolBaseSky;
  std::unordered_map<std::uint64_t, std::unordered_set<int>>
      pendingProtocolDirtyBlock;
  std::unordered_map<std::uint64_t, std::unordered_set<int>>
      pendingProtocolDirtySky;
  std::unordered_map<std::uint64_t, std::uint64_t> pendingProtocolEpochBlock;
  std::unordered_map<std::uint64_t, std::uint64_t> pendingProtocolEpochSky;
  std::uint64_t nextBlockLayerRevision = 0;
  std::uint64_t nextSkyLayerRevision = 0;
  std::function<void(std::shared_ptr<const LightChunkSnapshot>)> publishCallback;
  std::thread worker;
  bool accepting = true;
  std::atomic<bool> closing{false};
  bool busy = false;
  std::size_t busyChunks = 0;
  std::atomic<std::uint64_t> nextGeneration{0};
  std::atomic<std::uint64_t> wakeSequence{0};
  // Only the worker sets this before waiting. Producers exchange it before an
  // atomic notify, so notify-before-wait cannot strand a generation.
  std::atomic<bool> workerSleeping{false};
  std::atomic<bool> acceptingCells{true};
  std::uint64_t nextPublicationRevision = 0;
  bool paused = false;
  bool pauseAfterBatch = false;
  std::optional<std::uint64_t> forcedChunk;
  std::string error;
  mutable std::atomic<std::uint64_t> statsSubmitted{0};
  mutable std::atomic<std::uint64_t> statsRejected{0};
  mutable std::atomic<std::uint64_t> statsDroppedGeometry{0};
  mutable std::atomic<std::uint64_t> statsBatches{0};
  mutable std::atomic<std::uint64_t> statsChanges{0};
  mutable std::atomic<std::uint64_t> statsPublished{0};
  mutable std::atomic<std::uint64_t> statsStale{0};
  mutable std::atomic<std::uint64_t> statsHighWater{0};
  mutable std::atomic<std::uint64_t> statsLightQueueEntries{0};
  mutable std::atomic<std::uint64_t> statsSkyChanges{0};
  mutable std::atomic<std::uint64_t> statsBlockChanges{0};
  mutable std::atomic<std::uint64_t> statsReceivedSkySections{0};
  mutable std::atomic<std::uint64_t> statsReceivedBlockSections{0};
  mutable std::atomic<double> statsWorkerCpuMs{0};
  mutable std::atomic<double> statsBlockWorkerCpuMs{0};
  mutable std::atomic<double> statsSkyWorkerCpuMs{0};
  mutable std::atomic<std::uint64_t> statsWorkerAffinity{0};
  mutable std::atomic<std::uint32_t> statsWorkerAffinityError{0};
  mutable std::atomic<std::uint64_t> statsBlockWorkerAffinity{0};
  mutable std::atomic<std::uint32_t> statsBlockWorkerAffinityError{0};
  mutable std::atomic<std::uint64_t> statsSkyWorkerAffinity{0};
  mutable std::atomic<std::uint32_t> statsSkyWorkerAffinityError{0};
};

LightService::LightService(int minBlockSectionY, int blockSectionCount,
                           LightWorkerOptions options)
    : impl_(std::make_unique<Impl>(minBlockSectionY, blockSectionCount, options)) {}

LightService::~LightService() = default;

std::shared_ptr<const LightOpticalTable> LightService::opticalTable() const {
  return impl_->opticalTable;
}

std::uint64_t LightService::submitChunk(LightChunkInput input) {
  return impl_->submitChunk(std::move(input));
}

bool LightService::reportSectionStatus(int chunkX, int chunkZ, int sectionY,
                                       bool empty) {
  return impl_->reportSectionStatus(chunkX, chunkZ, sectionY, empty);
}

bool LightService::reportBlockChange(LightBlockChange change) {
  return impl_->reportBlockChange(change);
}

void LightService::reportBlockChanges(
    const std::vector<LightBlockChange> &changes) {
  impl_->reportBlockChanges(changes);
}

void LightService::reportBlockCell(LightCellUpdate update) {
  impl_->reportBlockCell(update);
}

bool LightService::unloadChunk(int chunkX, int chunkZ) {
  return impl_->unloadChunk(chunkX, chunkZ);
}

void LightService::drainPublished() { impl_->drainPublished(); }

std::shared_ptr<const LightChunkSnapshot> LightService::latestChunk(
    int chunkX, int chunkZ) const {
  return impl_->latestChunk(chunkX, chunkZ);
}

std::shared_ptr<const LightChunkSnapshot> LightService::latestLayerChunk(
    LightLayer layer, int chunkX, int chunkZ) const {
  return impl_->latestLayerChunk(layer, chunkX, chunkZ);
}

std::optional<std::array<int, 256>>
LightService::skyColumnSourcesForTesting(int chunkX, int chunkZ) const {
  return impl_->skyColumnSourcesForTesting(chunkX, chunkZ);
}

std::optional<BlockLightProperties>
LightService::opticalPropertiesForTesting(int x, int y, int z) const {
  return impl_->opticalPropertiesForTesting(x, y, z);
}

void LightService::setPublishCallback(
    std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback) {
  impl_->setPublishCallback(std::move(callback));
}

LightService::Stats LightService::stats() const { return impl_->stats(); }
void LightService::resetMeasurementStats() {
  impl_->resetMeasurementStats();
}
std::string LightService::workerError() const { return impl_->workerError(); }
bool LightService::waitIdleForTesting() { return impl_->waitIdleForTesting(); }
bool LightService::waitWorkerSleepingForTesting() {
  return impl_->waitWorkerSleepingForTesting();
}
void LightService::pauseForTesting() { impl_->pauseForTesting(); }
void LightService::resumeForTesting() { impl_->resumeForTesting(); }
bool LightService::processChunkBatchForTesting(int chunkX, int chunkZ) {
  return impl_->processChunkBatchForTesting(chunkX, chunkZ);
}
void LightService::close() { impl_->close(); }

} // namespace redstone
