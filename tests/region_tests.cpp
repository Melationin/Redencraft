#include "redstone/region.hpp"
#include "redstone/nbt.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <zlib.h>
#include <lz4.h>
#include <xxhash.h>
#include <stdexcept>
#include <string>
#include <vector>

using namespace redstone::region;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::vector<std::uint8_t> nbtFixture(std::size_t size, std::uint8_t seed) {
  std::vector<std::uint8_t> result(size);
  std::uint32_t state = 0x9e3779b9u ^ seed;
  for (std::size_t i = 0; i < size; ++i) {
    state = state * 1664525u + 1013904223u;
    result[i] = static_cast<std::uint8_t>(state >> 24);
  }
  // NBT compound marker and end marker are enough for raw byte round-trip;
  // this layer deliberately does not parse chunk structures.
  if (!result.empty()) {
    result.front() = 10;
    result.back() = 0;
  }
  return result;
}
std::uint32_t be32(const std::uint8_t *p) {
  return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 |
         std::uint32_t(p[2]) << 8 | p[3];
}
void putBe32(std::vector<std::uint8_t> &bytes, std::size_t offset,
             std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 24);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
  bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
  bytes[offset + 3] = static_cast<std::uint8_t>(value);
}
std::vector<std::uint8_t> disk(const std::filesystem::path &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  require(bool(f), "fixture open");
  const auto n = f.tellg();
  require(n >= 0, "fixture size");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
  f.seekg(0);
  if (!bytes.empty())
    require(bool(f.read(reinterpret_cast<char *>(bytes.data()), n)),
            "fixture read");
  return bytes;
}
void writeDisk(const std::filesystem::path &path,
               const std::vector<std::uint8_t> &bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  require(bool(f), "fixture create");
  f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  require(bool(f), "fixture write");
}
std::vector<std::uint8_t> namedIntStringNbt(int number, std::string_view text) {
  std::vector<std::uint8_t> out{10, 0, 0, 3, 0, 5};
  out.insert(out.end(), {'p', 'r', 'o', 'b', 'e'});
  out.push_back(static_cast<std::uint8_t>(number >> 24));
  out.push_back(static_cast<std::uint8_t>(number >> 16));
  out.push_back(static_cast<std::uint8_t>(number >> 8));
  out.push_back(static_cast<std::uint8_t>(number));
  out.insert(out.end(), {8, 0, 4, 'n', 'a', 'm', 'e', 0,
                         static_cast<std::uint8_t>(text.size())});
  out.insert(out.end(), text.begin(), text.end());
  out.push_back(0);
  return out;
}

std::vector<std::uint8_t> deflate(const std::vector<std::uint8_t> &input) {
  z_stream z{};
  require(deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                       Z_DEFAULT_STRATEGY) == Z_OK,
          "deflate init");
  std::vector<std::uint8_t> out;
  std::array<std::uint8_t, 4096> buffer{};
  z.next_in = const_cast<Bytef *>(input.data());
  z.avail_in = static_cast<uInt>(input.size());
  for (;;) {
    z.next_out = buffer.data();
    z.avail_out = buffer.size();
    const int rc = deflate(&z, Z_FINISH);
    require(rc == Z_OK || rc == Z_STREAM_END, "deflate");
    out.insert(out.end(), buffer.begin(), buffer.begin() + buffer.size() -
                                                 z.avail_out);
    if (rc == Z_STREAM_END)
      break;
  }
  deflateEnd(&z);
  return out;
}
void installSlot(std::vector<std::uint8_t> &region, int slot,
                 std::uint32_t sector, std::uint32_t sectors,
                 std::uint32_t length, std::uint8_t type,
                 const std::vector<std::uint8_t> &payload) {
  putBe32(region, slot * 4, (sector << 8) | sectors);
  putBe32(region, 4096 + slot * 4, 123);
  const std::size_t offset = std::size_t(sector) * 4096;
  putBe32(region, offset, length);
  region[offset + 4] = type;
  std::copy(payload.begin(), payload.end(), region.begin() + offset + 5);
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2 || argc == 3, "region_tests needs scratch directory");
    const std::filesystem::path scratch(argv[1]);
    std::filesystem::remove_all(scratch);
    const auto root = scratch / "region";
    std::filesystem::create_directories(root);

    const auto negative = locateChunk(root, -1, -1);
    require(negative.regionX == -1 && negative.regionZ == -1 &&
                negative.slot == 31 * 32 + 31,
            "negative chunk maps to floorDiv32 region and positive local slot");
    require(locateChunk(root, -33, 33).regionX == -2 &&
                locateChunk(root, -33, 33).regionZ == 1,
            "chunk region boundaries use floor division");

    const auto a = nbtFixture(200, 3);
    const auto emptyRoot = scratch / "empty-region";
    std::filesystem::create_directories(emptyRoot);
    writeDisk(emptyRoot / "r.0.0.mca", {});
    require(readChunkNbt(emptyRoot, 0, 0).status == ReadStatus::Missing,
            "zero-byte region is an empty Java region");
    for (const std::size_t shortSize :
         {std::size_t{1}, std::size_t{4095}, std::size_t{4096},
          std::size_t{8191}}) {
      writeDisk(emptyRoot / "r.0.0.mca",
                std::vector<std::uint8_t>(shortSize, 0));
      require(readChunkNbt(emptyRoot, 0, 0).status == ReadStatus::Corrupt,
              "nonempty region shorter than the double header stays corrupt");
    }
    writeDisk(emptyRoot / "r.0.0.mca", {});
    const auto emptyWrite = writeChunkNbt(emptyRoot, 0, 0, a, 100);
    require(emptyWrite.wrote &&
                readChunkNbt(emptyRoot, 0, 0).status == ReadStatus::Found,
            "first write initializes a zero-byte region header");

    const auto b = nbtFixture(5000, 7);
    const auto first = writeChunkNbt(root, -1, -1, a, 101);
    require(first.wrote && !first.external && first.sectors == 1,
            "small payload uses internal sectors");
    const auto second = writeChunkNbt(root, -1, 0, b, 102);
    require(second.wrote && !second.external && second.sectors > first.sectors,
            "larger payload allocates a larger sector run");
    const auto readA = readChunkNbt(root, -1, -1);
    const auto readB = readChunkNbt(root, -1, 0);
    require(readA.status == ReadStatus::Found && readA.nbt == a &&
                readB.status == ReadStatus::Found && readB.nbt == b,
            "two target slots round-trip independent raw NBT");

    const auto regionFile = root / "r.-1.-1.mca";
    const auto before = disk(regionFile);
    const auto neighborRecord = before.begin() + 2 * 4096;
    const auto neighborBytes = std::vector<std::uint8_t>(
        neighborRecord, neighborRecord + 4096);
    const auto overwritten = writeChunkNbt(root, -1, -1, a, 103);
    require(overwritten.wrote && !overwritten.external,
            "replacement uses a new valid target slot");
    const auto after = disk(regionFile);
    const auto target = 31 * 32 + 31;
    require(be32(after.data() + target * 4) != 0,
            "replacement keeps the target allocated");
    require(std::equal(neighborBytes.begin(), neighborBytes.end(),
                       after.begin() + 2 * 4096),
            "neighbor payload sector is byte-identical");
    const int neighborSlot = 32 * 32 + 31;
    require(be32(before.data() + neighborSlot * 4) ==
                be32(after.data() + neighborSlot * 4) &&
                be32(before.data() + 4096 + neighborSlot * 4) ==
                    be32(after.data() + 4096 + neighborSlot * 4),
            "neighbor location/timestamp fields are untouched");
    require(be32(before.data() + 4096 + target * 4) !=
                    be32(after.data() + 4096 + target * 4),
            "target timestamp follows replacement");
    const auto readNeighbor = readChunkNbt(root, -1, 0);
    require(readNeighbor.status == ReadStatus::Found && readNeighbor.nbt == b,
            "neighbor compressed payload remains readable");

    const auto missing = readChunkNbt(root, 4, 4);
    require(missing.status == ReadStatus::Missing,
            "unallocated slot is explicitly missing");

    const auto large = nbtFixture(2 * 1024 * 1024 + 300, 13);
    const auto ext = writeChunkNbt(root, -1, 1, large, 104);
    require(ext.wrote && ext.external && ext.sectors == 1,
            "oversized NBT uses one-sector external stub");
    const auto extRead = readChunkNbt(root, -1, 1);
    require(extRead.status == ReadStatus::Found && extRead.nbt == large,
            "external .mcc payload round-trips");
    std::filesystem::remove(root / "c.-1.1.mcc");
    const auto beforeMissingExternal =
        disk(root / "r.-1.0.mca");
    require(readChunkNbt(root, -1, 1).status == ReadStatus::Corrupt,
            "allocated external stream missing is corruption");
    bool refusedOverwrite = false;
    try {
      (void)writeChunkNbt(root, -1, 1, a, 105);
    } catch (const std::runtime_error &) {
      refusedOverwrite = true;
    }
    require(refusedOverwrite &&
                disk(root / "r.-1.0.mca") == beforeMissingExternal,
            "missing external payload is not overwritten");
    std::cout << "region external_missing=corrupt overwrite=refused\n";
    writeDisk(root / "c.-1.1.mcc", deflate(large));
    const auto extAgain = writeChunkNbt(root, -1, 1, a, 105);
    const auto extReadAgain = readChunkNbt(root, -1, 1);
    require(extAgain.wrote && !extAgain.external &&
                extReadAgain.status == ReadStatus::Found &&
                extReadAgain.nbt == a,
            "a new payload remains readable after the internal switch");
    require(!std::filesystem::exists(root / "c.-1.1.mcc"),
            "large-to-small replacement removes stale external payload");

    const auto compressedA = deflate(a);
    // A legacy non-sector-aligned tail is valid when the declared record fits:
    // RegionFile reads the exact record, not an assumed full allocation.
    const auto tailRoot = scratch / "tail";
    std::filesystem::create_directories(tailRoot);
    auto tailRegion = std::vector<std::uint8_t>(8192 + 5 + compressedA.size());
    installSlot(tailRegion, 0, 2, 1, compressedA.size() + 1, 2,
                compressedA);
    tailRegion.resize(8192 + 5 + compressedA.size());
    writeDisk(tailRoot / "r.0.0.mca", tailRegion);
    require(readChunkNbt(tailRoot, 0, 0).status == ReadStatus::Found,
            "complete record in a short final sector remains readable");

    auto shortHeader = tailRegion;
    shortHeader.resize(4096);
    writeDisk(tailRoot / "r.1.0.mca", shortHeader);
    require(readChunkNbt(tailRoot, 32, 0).status == ReadStatus::Corrupt,
            "less than both 4096-byte header tables is corruption");
    auto zeroShortHeader = std::vector<std::uint8_t>(4096, 0);
    writeDisk(tailRoot / "r.2.0.mca", zeroShortHeader);
    require(readChunkNbt(tailRoot, 64, 0).status == ReadStatus::Corrupt,
            "short all-zero header is corruption rather than Missing");

    const auto failure = scratch / "failed-external";
    std::filesystem::create_directories(failure);
    const auto oldInput = nbtFixture(1800, 23);
    (void)writeChunkNbt(failure, -1, -1, oldInput, 301);
    (void)writeChunkNbt(failure, -1, 0, a, 302);
    const auto oldMjca = disk(failure / "r.-1.-1.mca");
    const int failureNeighborSlot = 32 * 32 + 31;
    std::filesystem::create_directory(failure / "c.-1.-1.mcc");
    bool failed = false;
    try {
      (void)writeChunkNbt(failure, -1, -1, large, 303);
    } catch (const std::runtime_error &) {
      failed = true;
    }
    require(failed, "non-file external target fails before slot commit");
    require(std::filesystem::is_directory(failure / "c.-1.-1.mcc"),
            "predictable rename failure is isolated");
    const auto preserved = readChunkNbt(failure, -1, -1);
    const auto preservedNeighbor = readChunkNbt(failure, -1, 0);
    const auto afterFailure = disk(failure / "r.-1.-1.mca");
    const auto targetSlot = 31 * 32 + 31;
    require(preserved.status == ReadStatus::Found &&
                preserved.nbt == oldInput &&
                preservedNeighbor.status == ReadStatus::Found &&
                preservedNeighbor.nbt == a &&
                be32(afterFailure.data() + targetSlot * 4) ==
                    be32(oldMjca.data() + targetSlot * 4) &&
                be32(afterFailure.data() + failureNeighborSlot * 4) ==
                    be32(oldMjca.data() + failureNeighborSlot * 4),
            "failed external conversion preserves target and neighbor slots");

    // A covered target's old sectors are reserved until commit, so the next
    // write must reuse an existing hole somewhere in this region rather than
    // clobber the old target extent.
    const auto reuseRoot = scratch / "reuse";
    std::filesystem::create_directories(reuseRoot);
    (void)writeChunkNbt(reuseRoot, 0, 0, b, 401);
    (void)writeChunkNbt(reuseRoot, 1, 0, b, 402);
    (void)writeChunkNbt(reuseRoot, 2, 0, b, 403);
    const auto reuseA = readChunkNbt(reuseRoot, 0, 0);
    const auto reuseB = readChunkNbt(reuseRoot, 1, 0);
    const auto reuseC = readChunkNbt(reuseRoot, 2, 0);
    require(reuseA.status == ReadStatus::Found && reuseA.nbt == b &&
                reuseB.status == ReadStatus::Found && reuseB.nbt == b &&
                reuseC.status == ReadStatus::Found && reuseC.nbt == b,
            "first-fit allocation reuses free slack without corrupting slots");
    // The target's old extent is protected through commit, but after a
    // replacement it is free slack for a subsequent write.
    (void)writeChunkNbt(reuseRoot, 0, 0, a, 404);
    const auto beforeSecond = std::filesystem::file_size(
        reuseRoot / "r.0.0.mca");
    (void)writeChunkNbt(reuseRoot, 1, 0, a, 405);
    const auto reused = readChunkNbt(reuseRoot, 1, 0);
    require(reused.status == ReadStatus::Found && reused.nbt == a &&
                std::filesystem::file_size(reuseRoot / "r.0.0.mca") ==
                    beforeSecond,
            "first-fit reuses a freed target extent");

    const auto overlapRoot = scratch / "overlap";
    std::filesystem::create_directories(overlapRoot);
    auto overlap = std::vector<std::uint8_t>(8192 + 4096 * 6);
    installSlot(overlap, 0, 2, 2, compressedA.size() + 1, 2, compressedA);
    installSlot(overlap, 1, 3, 2, compressedA.size() + 1, 2, compressedA);
    writeDisk(overlapRoot / "r.0.0.mca", overlap);
    bool overlapRejected = false;
    try {
      (void)writeChunkNbt(overlapRoot, 0, 0, b, 405);
    } catch (const std::runtime_error &) {
      overlapRejected = true;
    }
    require(overlapRejected,
            "write rejects overlapping existing slot sector extents");

    const auto failRoot = scratch / "unsupported";
    std::filesystem::create_directories(failRoot);
    auto fake = std::vector<std::uint8_t>(12288);
    const auto tiny = std::vector<std::uint8_t>{1, 2, 3};
    installSlot(fake, 0, 2, 1, 4, 99, tiny);
    writeDisk(failRoot / "r.0.0.mca", fake);
    require(readChunkNbt(failRoot, 0, 0).status == ReadStatus::Unsupported,
            "unknown compression is explicitly unsupported");

    auto type2 = std::vector<std::uint8_t>(12288 + 4096);
    installSlot(type2, 1, 2, 2, compressedA.size() + 1, 2, compressedA);
    writeDisk(failRoot / "r.0.0.mca", type2);
    const auto compressedRead = readChunkNbt(failRoot, 1, 0);
    require(compressedRead.status == ReadStatus::Found &&
                compressedRead.nbt == a,
            "deflate slot round-trips raw bytes");

    auto corrupt = type2;
    putBe32(corrupt, 2 * 4096, 65535);
    writeDisk(failRoot / "r.0.0.mca", corrupt);
    require(readChunkNbt(failRoot, 1, 0).status == ReadStatus::Corrupt,
            "invalid target length is explicit corruption");
    if (argc == 3) {
      const auto cppForJava = scratch / "cpp-for-java";
      std::filesystem::create_directories(cppForJava);
      (void)writeChunkNbt(cppForJava, -1, -1,
                          namedIntStringNbt(-11, "cpp-w2a"), 200);
      (void)writeChunkNbt(cppForJava, -1, 0,
                          namedIntStringNbt(-10, "cpp-neighbor"), 201);
      const auto javaDir = std::filesystem::path(argv[2]);
      const auto javaA = readChunkNbt(javaDir, -1, -1);
      require(javaA.status == ReadStatus::Found,
              "Java 26.1 RegionFile slot is readable by C++");
      const auto parsed = redstone::nbt::parse(javaA.nbt);
      require(parsed.get("probe") && parsed.get("probe")->type == 3 &&
                  static_cast<int>(parsed.get("probe")->number) == -11 &&
                  parsed.get("name") && parsed.get("name")->type == 8 &&
                  parsed.get("name")->text == "java-w2a",
              "Java 26.1 NBT fields round-trip through C++");
    }
    std::cout << "region tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "region test failure: " << error.what() << '\n';
    return 1;
  }
}
