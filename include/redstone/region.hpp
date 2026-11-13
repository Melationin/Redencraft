#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace redstone::region {

enum class ReadStatus {
  Found,
  Missing,
  Corrupt,
  Unsupported,
};

struct ChunkSlot {
  int chunkX = 0;
  int chunkZ = 0;
  int regionX = 0;
  int regionZ = 0;
  int slot = 0;
  std::filesystem::path path;

  bool operator==(const ChunkSlot &) const = default;
};

struct ReadResult {
  ReadStatus status = ReadStatus::Missing;
  std::vector<std::uint8_t> nbt;
  std::string detail;

  explicit operator bool() const { return status == ReadStatus::Found; }
};

struct WriteResult {
  bool wrote = false;
  bool external = false;
  std::uint32_t sectors = 0;
  std::uint32_t sectorNumber = 0;
  int timestampSeconds = 0;
  std::filesystem::path mccPath;
  bool replacedExternal = false;
  // The target was committed, but an obsolete .mcc could not be removed.
  // This is a successful write with a cleanup warning, never a rollback signal.
  bool cleanupPending = false;
  std::string warning;
};

// Resolves a chunk into its dimension region directory. Region coordinates use
// floor division for negative chunks.
ChunkSlot locateChunk(const std::filesystem::path &regionDirectory,
                      int chunkX, int chunkZ);

// Reads only the target slot's sectors and returns its decompressed NBT bytes.
// Missing, malformed, unsupported compression, and external-file failures are
// distinct. No world/chunk parser is invoked.
ReadResult readChunkNbt(const std::filesystem::path &regionDirectory,
                        int chunkX, int chunkZ);

// Writes one existing raw NBT payload with the 26.1 default deflate version.
// Existing slots and external files for other chunks are untouched. The new
// payload is at least 256 sectors is written to .mcc; otherwise it is written
// directly into newly allocated region sectors. A directory temp file/rename
// handles external payload exchange; this is not a full-filesystem atomicity
// claim.
WriteResult writeChunkNbt(const std::filesystem::path &regionDirectory,
                          int chunkX, int chunkZ,
                          std::span<const std::uint8_t> nbt,
                          int timestampSeconds = 0);

} // namespace redstone::region
