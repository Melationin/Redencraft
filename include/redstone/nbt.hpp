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
  std::map<std::string, Tag> compound;
  const Tag *get(const std::string &key) const;
};
Tag parse(std::span<const uint8_t> data);
std::vector<uint8_t> decompress(std::span<const uint8_t> data, int window_bits,
                                size_t limit = 128 * 1024 * 1024);
} // namespace redstone::nbt
