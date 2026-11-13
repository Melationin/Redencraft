#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace redstone::nbt {
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Tag {
  uint8_t type{};
  int64_t number{};
  double real{};
  std::string text;
  std::vector<int8_t> bytes;
  std::vector<int32_t> ints;
  std::vector<int64_t> longs;
  std::vector<Tag> list;
  // Preserve the element type even when a list is empty. Insertion order is
  // retained so values representable losslessly by this DOM can re-encode
  // stably; exotic payloads such as noncanonical IEEE NaN payloads are not
  // promised. Lookup remains ordered iteration.
  uint8_t listElementType{};
  std::map<std::string, Tag> compound;
  // Original key order for untouched compounds. Duplicate entries are emitted
  // once. New keys are appended in map order by the writer; existing keys keep
  // their captured owner-defined ordering.
  std::vector<std::string> compoundOrder;
  const Tag *get(const std::string &key) const;
  Tag *get(const std::string &key);
};
struct RootTag {
  std::string name;
  Tag value;
};
RootTag parseRoot(std::span<const uint8_t> data);
Tag parse(std::span<const uint8_t> data);
std::vector<uint8_t> writeRoot(const RootTag &root);
std::vector<uint8_t> decompress(std::span<const uint8_t> data, int window_bits,
                                size_t limit = 128 * 1024 * 1024);
// zlib stream writer. bits=31 produces the gzip envelope used by level.dat
// and SavedDataStorage; bits=15 produces a zlib stream.
std::vector<uint8_t> compress(std::span<const uint8_t> data, int window_bits);
} // namespace redstone::nbt
