#include "engine/internal.hpp"
#include "redstone/item.hpp"

namespace redstone {
BlockState rotatePillar(BlockState state, Rotation rotation) {
  if (rotation != Rotation::Clockwise90 &&
      rotation != Rotation::Counterclockwise90)
    return state;
  const auto axis = state.getView("axis");
  if (axis == "x") return state.with("axis", "z");
  if (axis == "z") return state.with("axis", "x");
  return state;
}

Direction opposite(Direction d) {
  switch (d) {
  case Direction::Down:
    return Direction::Up;
  case Direction::Up:
    return Direction::Down;
  case Direction::North:
    return Direction::South;
  case Direction::South:
    return Direction::North;
  case Direction::West:
    return Direction::East;
  case Direction::East:
    return Direction::West;
  }
  return Direction::Down;
}
Direction directionFromString(std::string_view s) {
  if (s == "up")
    return Direction::Up;
  if (s == "north")
    return Direction::North;
  if (s == "south")
    return Direction::South;
  if (s == "west")
    return Direction::West;
  if (s == "east")
    return Direction::East;
  return Direction::Down;
}
std::string directionName(Direction d) {
  switch (d) {
  case Direction::Down:
    return "down";
  case Direction::Up:
    return "up";
  case Direction::North:
    return "north";
  case Direction::South:
    return "south";
  case Direction::West:
    return "west";
  case Direction::East:
    return "east";
  }
  return "down";
}
Pos Pos::relative(Direction d) const {
  Pos p = *this;
  switch (d) {
  case Direction::Down:
    --p.y;
    break;
  case Direction::Up:
    ++p.y;
    break;
  case Direction::North:
    --p.z;
    break;
  case Direction::South:
    ++p.z;
    break;
  case Direction::West:
    --p.x;
    break;
  case Direction::East:
    ++p.x;
    break;
  }
  return p;
}
std::uint32_t Random::next(int bits) {
  state_ = (state_ * 0x5deece66dULL + 0xbULL) & ((1ULL << 48) - 1);
  return static_cast<std::uint32_t>(state_ >> (48 - bits));
}
int Random::nextInt() { return static_cast<std::int32_t>(next(32)); }
int Random::nextInt(int bound) {
  if (bound <= 0)
    throw std::invalid_argument("bound must be positive");
  if ((bound & -bound) == bound)
    return static_cast<int>((static_cast<std::uint64_t>(bound) * next(31)) >>
                            31);
  int bits, value;
  do {
    bits = next(31);
    value = bits % bound;
  } while (bits - value + (bound - 1) < 0);
  return value;
}
std::int64_t Random::nextLong() {
  std::int64_t high = static_cast<std::int32_t>(next(32));
  std::int64_t low = static_cast<std::int32_t>(next(32));
  return (high << 32) + low;
}
float Random::nextFloat() { return next(24) / static_cast<float>(1 << 24); }
double Random::nextDouble() {
  return ((static_cast<std::uint64_t>(next(26)) << 27) + next(27)) /
         static_cast<double>(1ULL << 53);
}
Vec3 Vec3::operator+(const Vec3 &o) const {
  return {x + o.x, y + o.y, z + o.z};
}
Vec3 Vec3::operator-(const Vec3 &o) const {
  return {x - o.x, y - o.y, z - o.z};
}
Vec3 Vec3::operator*(double s) const { return {x * s, y * s, z * s}; }
Vec3 &Vec3::operator+=(const Vec3 &o) {
  x += o.x;
  y += o.y;
  z += o.z;
  return *this;
}
double Vec3::lengthSqr() const { return x * x + y * y + z * z; }
double Vec3::length() const { return std::sqrt(lengthSqr()); }
double Vec3::horizontalDistanceSqr() const { return x * x + z * z; }
double Vec3::horizontalDistance() const { return std::sqrt(horizontalDistanceSqr()); }
Vec3 Vec3::normalize() const {
  double length = this->length();
  return length < 1.0e-4 ? Vec3{} : *this * (1.0 / length);
}
Vec3 Vec3::multiply(double mx, double my, double mz) const {
  return {x * mx, y * my, z * mz};
}
Vec3 Vec3::with(double nx, double ny, double nz) const {
  return {nx, ny, nz};
}
bool Vec3::isFinite() const {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}
Aabb Aabb::ofSize(Vec3 center, double width, double height) {
  double half = width / 2.0;
  return {center.x - half, center.y, center.z - half,
          center.x + half, center.y + height, center.z + half};
}
Aabb Aabb::move(double x, double y, double z) const {
  return {minX + x, minY + y, minZ + z, maxX + x, maxY + y, maxZ + z};
}
Aabb Aabb::expandTowards(const Vec3 &v) const {
  double x0 = std::min(minX, minX + v.x), y0 = std::min(minY, minY + v.y),
         z0 = std::min(minZ, minZ + v.z);
  double x1 = std::max(maxX, maxX + v.x), y1 = std::max(maxY, maxY + v.y),
         z1 = std::max(maxZ, maxZ + v.z);
  return {x0, y0, z0, x1, y1, z1};
}
Aabb Aabb::inflate(double x, double y, double z) const {
  return {minX - x, minY - y, minZ - z, maxX + x, maxY + y, maxZ + z};
}
Aabb Aabb::inflate(double amount) const {
  return inflate(amount, amount, amount);
}
Aabb Aabb::deflate(double amount) const {
  return inflate(-amount, -amount, -amount);
}
bool Aabb::intersects(const Aabb &o) const {
  return minX < o.maxX && maxX > o.minX && minY < o.maxY && maxY > o.minY &&
         minZ < o.maxZ && maxZ > o.minZ;
}
Aabb Aabb::ofSize(Vec3 center, double dx, double dy, double dz) {
  return {center.x - dx * 0.5, center.y - dy * 0.5, center.z - dz * 0.5,
          center.x + dx * 0.5, center.y + dy * 0.5, center.z + dz * 0.5};
}
bool Aabb::intersects(Pos pos) const {
  return minX < pos.x + 1 && maxX > pos.x && minY < pos.y + 1 &&
         maxY > pos.y && minZ < pos.z + 1 && maxZ > pos.z;
}
bool Aabb::contains(Vec3 p) const {
  return p.x >= minX && p.x < maxX && p.y >= minY && p.y < maxY &&
         p.z >= minZ && p.z < maxZ;
}
Vec3 Aabb::getCenter() const {
  return {(minX + maxX) * 0.5, (minY + maxY) * 0.5, (minZ + maxZ) * 0.5};
}
Pose poseFromId(int id) {
  if (id < 0 || id > static_cast<int>(Pose::Inhaling))
    return Pose::Standing;
  return static_cast<Pose>(id);
}
const std::vector<ItemType> &itemRegistry() {
  static const std::vector<ItemType> entries = [] {
    std::vector<ItemType> result;
    const auto &items = allItems();
    result.reserve(items.size());
    for (const Item *item : items) {
      if (item && item->id() > 0)
        result.push_back({item->name(), item->maxStackSize()});
    }
    return result;
  }();
  return entries;
}
const ItemType *itemType(const std::string &name) {
  const Item *item = itemByName(name);
  if (!item || item->id() == 0)
    return nullptr;
  static const auto byName = [] {
    phmap::flat_hash_map<std::string, ItemType, AnkerlStringHash,
                         std::equal_to<>>
        map;
    for (const Item *it : allItems()) {
      if (it && it->id() > 0)
        map.emplace(it->name(), ItemType{it->name(), it->maxStackSize()});
    }
    return map;
  }();
  auto found = byName.find(item->name());
  return found == byName.end() ? nullptr : &found->second;
}
} // namespace redstone
