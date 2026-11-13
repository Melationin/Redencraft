#include "redstone/region.hpp"

#include "redstone/nbt.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <lz4.h>
#include <zlib.h>
#include <memory>
#include <set>
#include <xxhash.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace redstone::region {
namespace {

constexpr std::uint32_t kSectorBytes = 4096;
constexpr std::uint32_t kHeaderBytes = 2 * kSectorBytes;
constexpr std::uint32_t kHeaderSectors = 2;
constexpr std::size_t kMaxRegionChunks = 1024;
constexpr std::size_t kMaxNbtBytes = 128u << 20;
constexpr std::uint32_t kExternalFlag = 128;
constexpr std::uint32_t kExternalChunkThreshold = 256;
constexpr int kDeflateVersion = 2;

std::uint32_t be32(const std::uint8_t *p) {
  return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 |
         std::uint32_t(p[2]) << 8 | p[3];
}
void putBe32(std::uint8_t *p, std::uint32_t value) {
  p[0] = static_cast<std::uint8_t>(value >> 24);
  p[1] = static_cast<std::uint8_t>(value >> 16);
  p[2] = static_cast<std::uint8_t>(value >> 8);
  p[3] = static_cast<std::uint8_t>(value);
}
int floorDiv32(int value) { return value >= 0 ? value / 32 : -((-value + 31) / 32); }
int floorMod32(int value) {
  const int region = floorDiv32(value);
  return value - region * 32;
}
std::uint32_t sectorsFor(std::size_t bytes) {
  return static_cast<std::uint32_t>((bytes + kSectorBytes - 1) / kSectorBytes);
}
std::filesystem::path regionPath(const std::filesystem::path &directory,
                                 const ChunkSlot &slot) {
  return directory / ("r." + std::to_string(slot.regionX) + "." +
                      std::to_string(slot.regionZ) + ".mca");
}
std::filesystem::path externalPath(const std::filesystem::path &directory,
                                   const ChunkSlot &slot) {
  return directory / ("c." + std::to_string(slot.chunkX) + "." +
                      std::to_string(slot.chunkZ) + ".mcc");
}
std::vector<std::uint8_t> readFile(const std::filesystem::path &path,
                                   std::size_t limit, bool &exists) {
  std::error_code ec;
  exists = std::filesystem::is_regular_file(path, ec);
  if (!exists || ec)
    return {};
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    throw std::runtime_error("cannot open region payload: " + path.string());
  const auto length = file.tellg();
  if (length < 0 || static_cast<std::uint64_t>(length) > limit)
    throw std::runtime_error("region payload exceeds limit: " + path.string());
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
  file.seekg(0);
  if (!bytes.empty() &&
      !file.read(reinterpret_cast<char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("cannot read region payload: " + path.string());
  return bytes;
}
std::vector<std::uint8_t> readAt(const std::filesystem::path &path,
                                 std::size_t offset, std::size_t length) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open region file: " + path.string());
  file.seekg(static_cast<std::streamoff>(offset));
  std::vector<std::uint8_t> bytes(length);
  if (!bytes.empty() &&
      !file.read(reinterpret_cast<char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("short region read: " + path.string());
  return bytes;
}
std::uint64_t fileSize(const std::filesystem::path &path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec)
    throw std::runtime_error("cannot stat region file: " + path.string());
  return size;
}
std::vector<std::uint8_t> inflate(std::span<const std::uint8_t> input,
                                  int windowBits) {
  return nbt::decompress(input, windowBits);
}
std::vector<std::uint8_t> lz4BlockStream(std::span<const std::uint8_t> input) {
  constexpr std::size_t limit = 128u << 20;
  std::vector<std::uint8_t> output;
  std::size_t position = 0;
  auto le32 = [](const std::uint8_t *p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
           std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
  };
  for (;;) {
    if (input.size() - position < 21 ||
        std::memcmp(input.data() + position, "LZ4Block", 8) != 0)
      throw std::runtime_error("invalid LZ4 block header");
    const auto *header = input.data() + position;
    const std::uint8_t token = header[8];
    const std::uint8_t method = static_cast<std::uint8_t>(token & 0xf0);
    const std::uint32_t compressed = le32(header + 9);
    const std::uint32_t original = le32(header + 13);
    const std::uint32_t checksum = le32(header + 17);
    position += 21;
    if ((method != 0x10 && method != 0x20) ||
        original > (1u << (10 + (token & 15))) ||
        compressed > input.size() - position ||
        (original == 0) != (compressed == 0) ||
        (method == 0x10 && original != compressed))
      throw std::runtime_error("invalid LZ4 block length");
    if (!original) {
      if (checksum || position != input.size())
        throw std::runtime_error("invalid LZ4 terminator");
      return output;
    }
    if (original > limit - output.size() ||
        compressed > static_cast<std::uint32_t>(INT_MAX))
      throw std::runtime_error("LZ4 payload exceeds limit");
    const std::size_t start = output.size();
    output.resize(start + original);
    if (method == 0x10) {
      std::copy_n(input.data() + position, original, output.data() + start);
    } else if (LZ4_decompress_safe(
                   reinterpret_cast<const char *>(input.data() + position),
                   reinterpret_cast<char *>(output.data() + start),
                   static_cast<int>(compressed),
                   static_cast<int>(original)) != static_cast<int>(original)) {
      throw std::runtime_error("invalid LZ4 compressed block");
    }
    if ((XXH32(output.data() + start, original, 0x9747b28cU) &
         0x0fffffffU) != checksum)
      throw std::runtime_error("LZ4 block checksum mismatch");
    position += compressed;
  }
}
std::vector<std::uint8_t> deflateNbt(std::span<const std::uint8_t> nbtBytes) {
  // zlib's deflateInit2(windowBits=15) produces the RegionFileVersion 2 stream.
  z_stream stream{};
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK)
    throw std::runtime_error("deflate initialization failed");
  struct Guard {
    z_stream *stream;
    ~Guard() { deflateEnd(stream); }
  } guard{&stream};
  if (nbtBytes.size() > std::numeric_limits<uInt>::max())
    throw std::runtime_error("raw NBT too large to compress");
  stream.next_in = const_cast<Bytef *>(nbtBytes.data());
  stream.avail_in = static_cast<uInt>(nbtBytes.size());
  std::vector<std::uint8_t> output;
  std::array<std::uint8_t, 16384> buffer{};
  for (;;) {
    stream.next_out = buffer.data();
    stream.avail_out = static_cast<uInt>(buffer.size());
    const int result = deflate(&stream, Z_FINISH);
    if (result != Z_OK && result != Z_STREAM_END)
      throw std::runtime_error("deflate failed");
    output.insert(output.end(), buffer.begin(),
                  buffer.begin() + (buffer.size() - stream.avail_out));
    if (result == Z_STREAM_END)
      break;
  }
  return output;
}

class SectorSet {
public:
  void seed(std::uint32_t first, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i)
      used_.insert(first + i);
  }
  std::uint32_t allocate(std::uint32_t count) const {
    std::uint32_t first = kHeaderSectors;
    for (;;) {
      while (used_.contains(first))
        ++first;
      std::uint32_t end = first;
      while (end - first < count && !used_.contains(end))
        ++end;
      if (end - first >= count)
        return first;
      first = end;
    }
  }
  std::uint64_t highWater() const {
    return used_.empty() ? kHeaderSectors : *used_.rbegin() + 1;
  }

private:
  std::set<std::uint32_t> used_;
};


std::uint64_t fileSectorSize(const std::filesystem::path &path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec)
    throw std::runtime_error("cannot stat region file");
  return (size + kSectorBytes - 1) / kSectorBytes;
}

struct RegionState {
  std::vector<std::uint8_t> header = std::vector<std::uint8_t>(kHeaderBytes);
  SectorSet sectors;
  std::uint64_t fileSectors = kHeaderSectors;
};

RegionState loadState(const std::filesystem::path &path,
                      std::size_t targetSlot) {
  RegionState state;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    state.fileSectors = kHeaderSectors;
    return state;
  }
  state.fileSectors = fileSectorSize(path);
  if (state.fileSectors == 0) {
    state.fileSectors = kHeaderSectors;
    return state;
  }
  const auto header = readAt(path, 0, kHeaderBytes);
  state.header = header;
  std::set<std::uint32_t> occupied{kHeaderSectors - 2, kHeaderSectors - 1};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> extents;
  for (std::size_t slot = 0; slot < kMaxRegionChunks; ++slot) {
    const std::uint32_t location = be32(state.header.data() + slot * 4);
    if (!location)
      continue;
    const std::uint32_t first = location >> 8;
    const std::uint32_t count = location & 255;
    if (first < kHeaderSectors || count == 0 ||
        std::uint64_t(first) + count > state.fileSectors)
      throw std::runtime_error("invalid region sector location");
    for (std::uint32_t i = 0; i < count; ++i)
      if (!occupied.insert(first + i).second)
        throw std::runtime_error("overlapping region slot extents");
    extents.emplace_back(first, count);
  }
  // Every referenced extent, including the target's old copy, remains reserved
  // until a new header commits. Unreferenced slack from prior rewrites is
  // available to the first-fit allocator.
  (void)targetSlot;
  for (const auto &[first, count] : extents)
    state.sectors.seed(first, count);
  return state;
}
void writeAt(const std::filesystem::path &path, std::uint64_t offset,
             std::span<const std::uint8_t> bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file)
    throw std::runtime_error("cannot open region for write: " + path.string());
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!file)
    throw std::runtime_error("short region write: " + path.string());
  file.flush();
  if (!file)
    throw std::runtime_error("region flush failed: " + path.string());
  file.close();
  if (!file)
    throw std::runtime_error("region close failed: " + path.string());
}
void createRegionFile(const std::filesystem::path &path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file)
    throw std::runtime_error("cannot create region file: " + path.string());
  std::array<std::uint8_t, kHeaderBytes> zero{};
  file.write(reinterpret_cast<const char *>(zero.data()), zero.size());
  file.flush();
  if (!file)
    throw std::runtime_error("cannot initialize region file: " + path.string());
  file.close();
  if (!file)
    throw std::runtime_error("cannot close new region file: " + path.string());
}
std::uint64_t nowSeconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
struct TempFileGuard {
  std::filesystem::path path;
  bool active = true;
  TempFileGuard() = default;
  explicit TempFileGuard(std::filesystem::path value) : path(std::move(value)) {}
  TempFileGuard(const TempFileGuard &) = delete;
  TempFileGuard &operator=(const TempFileGuard &) = delete;
  ~TempFileGuard() {
    if (active && !path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
  void release() { active = false; }
};

std::filesystem::path tempPathFor(const std::filesystem::path &directory) {
  std::filesystem::create_directories(directory);
  static std::atomic<std::uint64_t> counter{0};
  for (;;) {
    const auto id = counter.fetch_add(1, std::memory_order_relaxed);
    auto path = directory / ("tmp-region-" + std::to_string(::getpid()) +
                             "-" + std::to_string(id));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
      return path;
  }
}

} // namespace

ChunkSlot locateChunk(const std::filesystem::path &directory, int chunkX,
                      int chunkZ) {
  ChunkSlot result;
  result.chunkX = chunkX;
  result.chunkZ = chunkZ;
  result.regionX = floorDiv32(chunkX);
  result.regionZ = floorDiv32(chunkZ);
  result.slot = floorMod32(chunkX) + floorMod32(chunkZ) * 32;
  result.path = regionPath(directory, result);
  return result;
}

ReadResult readChunkNbt(const std::filesystem::path &directory, int chunkX,
                        int chunkZ) {
  const ChunkSlot slot = locateChunk(directory, chunkX, chunkZ);
  try {
    if (!std::filesystem::is_regular_file(slot.path))
      return {ReadStatus::Missing, {}, "region file missing"};
    const std::uint64_t size = fileSize(slot.path);
    if (size == 0)
      return {ReadStatus::Missing, {}, "empty region file"};
    if (size < kHeaderBytes)
      return {ReadStatus::Corrupt, {}, "truncated region header"};
    const auto header = readAt(slot.path, slot.slot * 4, 4);
    const std::uint32_t location = be32(header.data());
    if (!location)
      return {ReadStatus::Missing, {}, "chunk slot empty"};
    const std::uint32_t sector = location >> 8;
    const std::uint32_t sectors = location & 255;
    if (sector < 2 || sectors == 0 ||
        std::uint64_t(sector) * kSectorBytes >= size)
      return {ReadStatus::Corrupt, {}, "invalid sector location"};
    const std::uint64_t recordStart =
        std::uint64_t(sector) * kSectorBytes;
    const std::uint64_t allocationEnd =
        recordStart + std::uint64_t(sectors) * kSectorBytes;
    if (size < recordStart + 5)
      return {ReadStatus::Corrupt, {}, "truncated chunk header"};
    auto record = readAt(slot.path, recordStart, 5);
    const std::uint32_t length = be32(record.data());
    const std::uint8_t version = record[4];
    if (length == 0)
      return {ReadStatus::Corrupt, {}, "allocated slot has zero length"};
    const bool external = (version & kExternalFlag) != 0;
    const std::uint8_t compression = version & ~kExternalFlag;
    const auto externalPathForSlot = externalPath(directory, slot);
    std::vector<std::uint8_t> compressed;
    if (external) {
      if (length != 1)
        return {ReadStatus::Corrupt, {}, "external slot length is not one"};
      bool exists = false;
      compressed = readFile(externalPathForSlot, kMaxNbtBytes, exists);
      if (!exists)
        return {ReadStatus::Corrupt, {}, "allocated external chunk file missing"};
    } else {
      if (length < 1)
        return {ReadStatus::Corrupt, {}, "invalid chunk length"};
      const std::uint64_t payloadEnd = recordStart + 4 + length;
      if (payloadEnd > allocationEnd || payloadEnd > size)
        return {ReadStatus::Corrupt, {}, "chunk payload exceeds allocation"};
      compressed = readAt(slot.path, recordStart + 5,
                          static_cast<std::size_t>(length - 1));
    }
    std::vector<std::uint8_t> nbtBytes;
    if (compression == 1)
      nbtBytes = inflate(compressed, 31);
    else if (compression == 2)
      nbtBytes = inflate(compressed, 15);
    else if (compression == 3)
      nbtBytes = std::move(compressed);
    else if (compression == 4)
      nbtBytes = lz4BlockStream(compressed);
    else
      return {ReadStatus::Unsupported, {}, "unsupported compression type"};
    return {ReadStatus::Found, std::move(nbtBytes), {}};
  } catch (const std::exception &error) {
    return {ReadStatus::Corrupt, {}, error.what()};
  }
}

WriteResult writeChunkNbt(const std::filesystem::path &directory, int chunkX,
                          int chunkZ, std::span<const std::uint8_t> nbtBytes,
                          int timestampSeconds) {
  if (nbtBytes.empty() || nbtBytes.size() > kMaxNbtBytes ||
      timestampSeconds < 0)
    throw std::invalid_argument("raw NBT or timestamp is invalid");
  const ChunkSlot slot = locateChunk(directory, chunkX, chunkZ);
  std::filesystem::create_directories(directory);
  const auto compressed = deflateNbt(nbtBytes);
  const std::size_t inlinePayload = compressed.size() + 1;
  const std::uint32_t internalSectors = sectorsFor(compressed.size() + 5);
  const bool external = internalSectors >= kExternalChunkThreshold;
  const std::uint32_t neededSectors = external ? 1 : internalSectors;
  RegionState state = loadState(slot.path, static_cast<std::size_t>(slot.slot));

  std::error_code ec;
  const auto externalFile = externalPath(directory, slot);
  const bool oldExternal =
      std::filesystem::is_regular_file(externalFile, ec) && !ec;
  const std::uint32_t oldLocation =
      be32(state.header.data() + slot.slot * 4);
  const std::uint32_t oldSector = oldLocation >> 8;
  const std::uint32_t oldSectors = oldLocation & 255;
  if (oldLocation != 0) {
    const std::uint64_t oldOffset =
        std::uint64_t(oldSector) * kSectorBytes;
    if (oldSector < kHeaderSectors || oldSectors == 0 ||
        oldOffset + 5 > fileSize(slot.path))
      throw std::runtime_error("invalid existing region slot");
    const auto oldRecord = readAt(slot.path, oldOffset + 4, 1);
    const bool oldIsExternal =
        (oldRecord[0] & kExternalFlag) != 0;
    std::error_code externalError;
    if (oldIsExternal &&
        !std::filesystem::is_regular_file(externalFile, externalError))
      throw std::runtime_error(
          "refuse to overwrite allocated external slot with missing payload");
  }
  const std::uint32_t newSector = state.sectors.allocate(neededSectors);
  const std::uint64_t requiredSectors = std::max<std::uint64_t>(
      state.fileSectors,
      static_cast<std::uint64_t>(newSector) + neededSectors);
  const std::uint64_t requiredBytes = requiredSectors * kSectorBytes;

  if (!std::filesystem::exists(slot.path, ec))
    createRegionFile(slot.path);
  std::error_code currentError;
  const auto currentBytes = std::filesystem::exists(slot.path, currentError)
                                ? std::filesystem::file_size(slot.path,
                                                             currentError)
                                : 0;
  if (currentError)
    throw std::runtime_error("cannot stat region file");
  if (currentBytes < requiredBytes) {
    std::filesystem::resize_file(slot.path, requiredBytes, currentError);
    if (currentError)
      throw std::runtime_error("cannot extend region file");
  }

  std::filesystem::path temp;
  std::vector<std::uint8_t> payload;
  const bool externalTargetExists =
      std::filesystem::exists(externalFile, ec);
  if (external && externalTargetExists &&
      !std::filesystem::is_regular_file(externalFile, ec))
    throw std::runtime_error(
        "external chunk path is not a regular file");
  if (external) {
    payload.resize(kSectorBytes, 0);
    putBe32(payload.data(), 1);
    payload[4] = static_cast<std::uint8_t>(kDeflateVersion | kExternalFlag);
    writeAt(slot.path, std::uint64_t(newSector) * kSectorBytes, payload);
    temp = tempPathFor(directory);
    TempFileGuard tempGuard(temp);
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out)
        throw std::runtime_error("cannot create external temporary");
      out.write(reinterpret_cast<const char *>(compressed.data()),
                static_cast<std::streamsize>(compressed.size()));
      out.flush();
      if (!out)
        throw std::runtime_error("cannot write external temporary");
      out.close();
      if (!out)
        throw std::runtime_error("cannot close external temporary");
    }

    const auto oldHeader = state.header;
    const int timestamp = timestampSeconds != 0
                              ? timestampSeconds
                              : static_cast<int>(nowSeconds());
    putBe32(state.header.data() + slot.slot * 4,
            (newSector << 8) | neededSectors);
    putBe32(state.header.data() + 4096 + slot.slot * 4,
            static_cast<std::uint32_t>(timestamp));
    writeAt(slot.path, 0, state.header);
    if (std::rename(temp.string().c_str(), externalFile.string().c_str()) != 0) {
      try {
        writeAt(slot.path, 0, oldHeader);
      } catch (const std::exception &restoreError) {
        throw std::runtime_error(
            std::string("external commit and header restore failed: ") +
            restoreError.what());
      }
      throw std::runtime_error(
          "cannot commit external chunk file; previous header restored");
    }
    tempGuard.release();
    return {true, true, neededSectors, newSector, timestamp, externalFile,
            oldExternal, false, {}};
  }

  payload.resize(neededSectors * kSectorBytes, 0);
  putBe32(payload.data(), static_cast<std::uint32_t>(inlinePayload));
  payload[4] = static_cast<std::uint8_t>(kDeflateVersion);
  std::copy(compressed.begin(), compressed.end(), payload.begin() + 5);
  writeAt(slot.path, std::uint64_t(newSector) * kSectorBytes, payload);

  const int timestamp = timestampSeconds != 0
                            ? timestampSeconds
                            : static_cast<int>(nowSeconds());
  putBe32(state.header.data() + slot.slot * 4,
          (newSector << 8) | neededSectors);
  putBe32(state.header.data() + 4096 + slot.slot * 4,
          static_cast<std::uint32_t>(timestamp));
  writeAt(slot.path, 0, state.header);
  bool cleanupPending = false;
  std::string warning;
  if (oldSector && oldExternal) {
    std::error_code cleanupError;
    std::filesystem::remove(externalFile, cleanupError);
    if (cleanupError) {
      cleanupPending = true;
      warning = "region slot committed; stale external chunk cleanup failed";
    }
  }
  // The region header already points at the new payload. Old in-region sectors
  // are unreachable slack; a later single-slot write can reuse them.
  (void)oldSectors;
  return {true, false, neededSectors, newSector, timestamp, {}, oldExternal,
          cleanupPending, warning};
}

} // namespace redstone::region
