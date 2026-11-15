#include "redstone/block_tags.hpp"
#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <limits>
#include <zlib.h>
#include "picojson.h"

namespace redstone {
namespace {
struct TagEntry { std::string_view name; std::size_t offset; std::size_t count; };
#include "block_tags.inc"
struct SourceDefinition { std::string_view name; std::string_view entries; };
#include "block_tag_sources.inc"

struct Entry { std::string id; bool tag = false; bool required = true; };
using Definitions = std::map<std::string, std::vector<Entry>, std::less<>>;

bool identifier(std::string &id) {
  if (id.find(':') == std::string::npos) id.insert(0, "minecraft:");
  const auto colon = id.find(':');
  if (colon == 0 || colon == std::string::npos || colon + 1 == id.size() ||
      id.find(':', colon + 1) != std::string::npos) return false;
  for (std::size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    if (i == colon) continue;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '_' || c == '-' || c == '.' || (i > colon && c == '/')) continue;
    return false;
  }
  return true;
}
bool makeEntry(std::string id, bool required, Entry &entry) {
  entry.tag = !id.empty() && id.front() == '#';
  if (entry.tag) id.erase(0, 1);
  if (!identifier(id)) return false;
  entry.id = std::move(id);
  entry.required = required;
  return true;
}
Definitions vanillaDefinitions() {
  Definitions definitions;
  for (const auto &source : kVanillaTagSources) {
    auto &out = definitions[std::string(source.name)];
    std::istringstream input{std::string(source.entries)};
    std::string token;
    while (std::getline(input, token, ',')) {
      const bool required = token.empty() || token.back() != '?';
      if (!required) token.pop_back();
      Entry entry;
      if (!makeEntry(token, required, entry)) throw std::logic_error("invalid vanilla tag source");
      out.push_back(std::move(entry));
    }
  }
  return definitions;
}
bool parseTag(const std::string &source, bool &replace, std::vector<Entry> &entries,
              std::string &error) {
  picojson::value root;
  error.clear();
  auto end = picojson::parse(root, source.begin(), source.end(), &error);
  if (!error.empty()) return false;
  while (end != source.end() && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) ++end;
  if (end != source.end()) { error = "trailing JSON content"; return false; }
  if (!root.is<picojson::object>()) { error = "expected object"; return false; }
  const auto &object = root.get<picojson::object>();
  auto it = object.find("replace");
  replace = false;
  if (it != object.end()) {
    if (!it->second.is<bool>()) { error = "replace must be boolean"; return false; }
    replace = it->second.get<bool>();
  }
  it = object.find("values");
  if (it == object.end() || !it->second.is<picojson::array>()) {
    error = "values must be an array"; return false;
  }
  for (const auto &value : it->second.get<picojson::array>()) {
    std::string id;
    bool required = true;
    if (value.is<std::string>()) id = value.get<std::string>();
    else if (value.is<picojson::object>()) {
      const auto &item = value.get<picojson::object>();
      const auto idIt = item.find("id");
      if (idIt == item.end() || !idIt->second.is<std::string>()) { error = "entry id must be a string"; return false; }
      id = idIt->second.get<std::string>();
      const auto req = item.find("required");
      if (req != item.end()) {
        if (!req->second.is<bool>()) { error = "entry required must be boolean"; return false; }
        required = req->second.get<bool>();
      }
    } else { error = "entry must be a string or object"; return false; }
    Entry entry;
    if (!makeEntry(id, required, entry)) { error = "invalid entry identifier"; return false; }
    entries.push_back(std::move(entry));
  }
  return true;
}

struct TagResource { std::string id, data, origin; };
std::uint16_t u16(const std::vector<unsigned char> &data, std::size_t offset) {
  if (offset > data.size() || data.size() - offset < 2) throw std::runtime_error("truncated ZIP");
  return static_cast<std::uint16_t>(data[offset] | (std::uint16_t(data[offset + 1]) << 8));
}
std::uint32_t u32(const std::vector<unsigned char> &data, std::size_t offset) {
  return std::uint32_t(u16(data, offset)) | (std::uint32_t(u16(data, offset + 2)) << 16);
}
std::optional<std::string> resourceTagId(std::string_view path) {
  constexpr std::string_view prefix = "data/", marker = "/tags/block/", suffix = ".json";
  if (!path.starts_with(prefix) || !path.ends_with(suffix)) return std::nullopt;
  const auto split = path.find(marker, prefix.size());
  if (split == std::string_view::npos || split + marker.size() + suffix.size() >= path.size()) return std::nullopt;
  std::string id(path.substr(prefix.size(), split - prefix.size()));
  id.push_back(':');
  id.append(path.substr(split + marker.size(), path.size() - split - marker.size() - suffix.size()));
  if (!identifier(id) || id.find("..") != std::string::npos) return std::nullopt;
  return id;
}
// Only tag resources are decompressed; a ZIP is never extracted to the disk.
// ZIP64/multi-disk archives are explicitly rejected rather than guessed.
std::vector<TagResource> readZipPack(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot open pack " + path.string());
  std::vector<unsigned char> data{std::istreambuf_iterator<char>(stream), {}};
  if (stream.bad() || data.size() < 22) throw std::runtime_error("invalid ZIP pack: " + path.string());
  const std::size_t min = data.size() > 65557 ? data.size() - 65557 : 0;
  std::size_t eocd = data.size() - 22;
  while (u32(data, eocd) != 0x06054b50 || eocd + 22 + u16(data, eocd + 20) != data.size()) {
    if (eocd == min) throw std::runtime_error("missing ZIP end record: " + path.string());
    --eocd;
  }
  const auto count = u16(data, eocd + 10);
  if (u16(data, eocd + 4) != 0 || u16(data, eocd + 6) != 0 ||
      u16(data, eocd + 8) != count || count == 0xffff ||
      u32(data, eocd + 12) == 0xffffffff || u32(data, eocd + 16) == 0xffffffff)
    throw std::runtime_error("ZIP64 or multi-disk pack is unsupported: " + path.string());
  const auto directorySize = u32(data, eocd + 12);
  std::size_t cursor = u32(data, eocd + 16);
  if (cursor > eocd || directorySize > eocd - cursor) throw std::runtime_error("invalid ZIP directory");
  const auto directoryEnd = cursor + directorySize;
  std::vector<TagResource> out;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (cursor > directoryEnd || directoryEnd - cursor < 46 || u32(data, cursor) != 0x02014b50)
      throw std::runtime_error("invalid ZIP directory entry");
    const auto flags = u16(data, cursor + 8), method = u16(data, cursor + 10);
    const auto expectedCrc = u32(data, cursor + 16), compressed = u32(data, cursor + 20), size = u32(data, cursor + 24);
    const auto nameLen = u16(data, cursor + 28), extraLen = u16(data, cursor + 30), commentLen = u16(data, cursor + 32);
    const auto local = u32(data, cursor + 42);
    const std::size_t length = 46u + nameLen + extraLen + commentLen;
    if (length > directoryEnd - cursor) throw std::runtime_error("truncated ZIP directory entry");
    const std::string name(reinterpret_cast<const char *>(data.data() + cursor + 46), nameLen);
    cursor += length;
    const auto id = resourceTagId(name);
    if (!id) continue;
    if ((flags & 1) || (method != 0 && method != 8) || compressed == 0xffffffff || size == 0xffffffff || local == 0xffffffff)
      throw std::runtime_error("unsupported ZIP tag resource " + name);
    if (size > 16u * 1024u * 1024u) throw std::runtime_error("ZIP tag resource exceeds 16 MiB: " + name);
    if (local > data.size() || data.size() - local < 30 || u32(data, local) != 0x04034b50)
      throw std::runtime_error("invalid ZIP local header: " + name);
    const std::size_t start = std::size_t(local) + 30u + u16(data, local + 26) + u16(data, local + 28);
    if (start > data.size() || compressed > data.size() - start) throw std::runtime_error("truncated ZIP resource: " + name);
    std::string contents(size, '\0');
    if (method == 0) {
      if (compressed != size) throw std::runtime_error("invalid stored ZIP resource: " + name);
      std::copy_n(data.data() + start, size, contents.data());
    } else {
      z_stream z{};
      z.next_in = data.data() + start; z.avail_in = compressed;
      // Non-null output even for an empty deflate stream.
      unsigned char dummy = 0;
      z.next_out = size ? reinterpret_cast<Bytef *>(contents.data()) : &dummy;
      z.avail_out = size ? size : 1;
      if (inflateInit2(&z, -MAX_WBITS) != Z_OK) throw std::runtime_error("ZIP inflate initialization failed");
      const int status = inflate(&z, Z_FINISH);
      inflateEnd(&z);
      if (status != Z_STREAM_END || z.total_out != size || z.total_in != compressed)
        throw std::runtime_error("invalid ZIP deflate stream: " + name);
    }
    const auto crc = crc32(0, reinterpret_cast<const Bytef *>(contents.data()), static_cast<uInt>(contents.size()));
    if (crc != expectedCrc) throw std::runtime_error("ZIP CRC mismatch: " + name);
    out.push_back({*id, std::move(contents), path.string() + ":" + name});
  }
  if (cursor != directoryEnd) throw std::runtime_error("invalid ZIP directory length");
  return out;
}
}
std::optional<bool> blockHasTag(const Block &block, std::string_view tag) {
  const auto it = std::lower_bound(std::begin(kBlockTags), std::end(kBlockTags), tag,
      [](const TagEntry &entry, std::string_view name) { return entry.name < name; });
  if (it == std::end(kBlockTags) || it->name != tag) return std::nullopt;
  const auto first = std::begin(kBlockTagMembers) + it->offset;
  return std::binary_search(first, first + it->count, std::string_view(block.name()));
}
std::optional<bool> blockHasTag(const BlockState &state, std::string_view tag) {
  return blockHasTag(state.block(), tag);
}

BlockTagRegistry::BlockTagRegistry() {
  // The pinned resolved table makes construction cheap and exact.
  for (const auto &tag : kBlockTags) {
    auto &values = tags_[std::string(tag.name)];
    for (std::size_t i = 0; i < tag.count; ++i)
      values.emplace_back(kBlockTagMembers[tag.offset + i]);
  }
}
std::optional<bool> BlockTagRegistry::contains(std::string_view tag, std::string_view block) const {
  const auto found = tags_.find(tag);
  if (found == tags_.end()) return std::nullopt;
  return std::binary_search(found->second.begin(), found->second.end(), block);
}

BlockTagRegistry::ReloadResult BlockTagRegistry::reload(const std::vector<std::filesystem::path> &packs) {
  ReloadResult result;
  Definitions definitions = vanillaDefinitions();
  try {
    for (const auto &pack : packs) {
      const auto apply = [&](const TagResource &resource) {
        bool replace = false;
        std::vector<Entry> entries;
        std::string error;
        if (!parseTag(resource.data, replace, entries, error)) {
          result.diagnostics.push_back("invalid tag " + resource.id + " in " + resource.origin + ": " + error);
          return;
        }
        auto &target = definitions[resource.id];
        if (replace) target.clear();
        target.insert(target.end(), entries.begin(), entries.end());
      };
      if (std::filesystem::is_regular_file(pack)) {
        for (const auto &resource : readZipPack(pack)) apply(resource);
        continue;
      }
      if (!std::filesystem::is_directory(pack)) {
        result.diagnostics.push_back("pack is not a directory: " + pack.string());
        return result;
      }
      const auto data = pack / "data";
      if (!std::filesystem::exists(data)) continue;
      for (const auto &ns : std::filesystem::directory_iterator(data)) {
        if (!ns.is_directory()) continue;
        const auto base = ns.path() / "tags" / "block";
        if (!std::filesystem::exists(base)) continue;
        for (const auto &file : std::filesystem::recursive_directory_iterator(base)) {
          if (!file.is_regular_file() || file.path().extension() != ".json") continue;
          auto relative = std::filesystem::relative(file.path(), base);
          relative.replace_extension();
          std::string id = ns.path().filename().string() + ":" + relative.generic_string();
          if (!identifier(id)) { result.diagnostics.push_back("invalid tag path: " + file.path().string()); continue; }
          std::ifstream stream(file.path(), std::ios::binary);
          if (!stream) throw std::runtime_error("cannot read " + file.path().string());
          std::string json{std::istreambuf_iterator<char>(stream), {}};
          if (stream.bad()) throw std::runtime_error("cannot read " + file.path().string());
          apply({std::move(id), std::move(json), file.path().string()});
        }
      }
    }
    // DependencySorter adds required edges first, then optional edges, skipping
    // edges that introduce cycles. Evaluate dependencies before their users.
    std::map<std::string, std::set<std::string>, std::less<>> edges;
    std::function<bool(const std::string &, const std::string &, std::set<std::string> &)> reaches =
        [&](const auto &from, const auto &target, auto &seen) {
          if (from == target) return true;
          if (!seen.insert(from).second) return false;
          for (const auto &next : edges[from]) if (reaches(next, target, seen)) return true;
          return false;
        };
    for (bool required : {true, false})
      for (const auto &[id, entries] : definitions)
        for (const auto &entry : entries)
          if (entry.tag && entry.required == required) {
            std::set<std::string> seen;
            if (!reaches(entry.id, id, seen)) edges[id].insert(entry.id);
          }
    std::map<std::string, std::vector<std::string>, std::less<>> resolved;
    std::set<std::string> visited;
    std::function<void(const std::string &)> build = [&](const auto &id) {
      if (!visited.insert(id).second) return;
      for (const auto &dependency : edges[id]) build(dependency);
      auto definition = definitions.find(id);
      if (definition == definitions.end()) return;
      std::set<std::string> values;
      bool valid = true;
      for (const auto &entry : definition->second) {
        if (entry.tag) {
          const auto found = resolved.find(entry.id);
          if (found != resolved.end()) values.insert(found->second.begin(), found->second.end());
          else if (entry.required) { valid = false; result.diagnostics.push_back(id + ": missing required tag #" + entry.id); }
        } else {
          const bool known = std::binary_search(std::begin(kVanillaBlockNames), std::end(kVanillaBlockNames), std::string_view(entry.id));
          if (known) values.insert(entry.id);
          else if (entry.required) { valid = false; result.diagnostics.push_back(id + ": missing required block " + entry.id); }
        }
      }
      if (valid) resolved.emplace(id, std::vector<std::string>(values.begin(), values.end()));
    };
    for (const auto &[id, entries] : definitions) { (void)entries; build(id); }
    tags_ = std::move(resolved);
    result.applied = true;
  } catch (const std::exception &e) {
    result.diagnostics.push_back(e.what());
  }
  return result;
}
} // namespace redstone
