#include "redstone/worldgen/climate.hpp"

#include "redstone/worldgen/noise_router.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

namespace redstone::worldgen {
namespace {

constexpr int kChildrenPerNode = 6;
constexpr int kDimensions = 7;

std::int64_t square64(std::int64_t v) noexcept { return v * v; }

std::atomic<std::uint64_t> gNextTreeId{1};

struct RadialSearchResult {
  Pos location{0, 0, 0};
  std::int64_t fitness = 0;
};

RadialSearchResult
evaluateSpawnCandidate(const std::vector<ClimateParameterPoint> &targetClimates,
                       const ClimateSampler &sampler, int blockX, int blockZ) {
  const ClimateTargetPoint sampled = sampler.sample(blockX >> 2, 0, blockZ >> 2);
  const ClimateTargetPoint zeroDepth{
      .temperature = sampled.temperature,
      .humidity = sampled.humidity,
      .continentalness = sampled.continentalness,
      .erosion = sampled.erosion,
      .depth = 0,
      .weirdness = sampled.weirdness,
  };
  std::int64_t minFitness = std::numeric_limits<std::int64_t>::max();
  for (const auto &pt : targetClimates) {
    minFitness = std::min(minFitness, pt.fitness(zeroDepth));
  }
  const std::int64_t distanceBias =
      square64(static_cast<std::int64_t>(blockX)) +
      square64(static_cast<std::int64_t>(blockZ));
  const std::int64_t totalFitness =
      minFitness * square64(2048LL) + distanceBias;
  return RadialSearchResult{Pos{blockX, 0, blockZ}, totalFitness};
}

} // namespace

ClimateParameter ClimateParameter::span(float minVal, float maxVal) {
  if (minVal > maxVal) {
    throw std::invalid_argument("ClimateParameter min > max");
  }
  return ClimateParameter{quantizeClimateCoord(minVal),
                          quantizeClimateCoord(maxVal)};
}

ClimateParameter ClimateParameter::span(const ClimateParameter &minParam,
                                        const ClimateParameter &maxParam) {
  if (minParam.min > maxParam.max) {
    throw std::invalid_argument("ClimateParameter min > max");
  }
  return ClimateParameter{minParam.min, maxParam.max};
}

std::int64_t
ClimateParameterPoint::fitness(const ClimateTargetPoint &target) const noexcept {
  return square64(temperature.distance(target.temperature)) +
         square64(humidity.distance(target.humidity)) +
         square64(continentalness.distance(target.continentalness)) +
         square64(erosion.distance(target.erosion)) +
         square64(depth.distance(target.depth)) +
         square64(weirdness.distance(target.weirdness)) + square64(offset);
}

std::array<ClimateParameter, 7>
ClimateParameterPoint::parameterSpace() const noexcept {
  return {temperature, humidity,  continentalness,
          erosion,     depth,     weirdness,
          ClimateParameter{offset, offset}};
}

namespace detail {

namespace {

struct BuildNode {
  std::array<ClimateParameter, kDimensions> parameterSpace{};
  std::size_t valueIndex = 0;
  std::vector<std::unique_ptr<BuildNode>> children;
};

inline std::int64_t
flatNodeDistance(const RawClimateRTree::FlatNode &n,
                 const std::int64_t *target) noexcept {
  std::int64_t dist = 0;
  for (int i = 0; i < kDimensions; ++i) {
    const std::int64_t lo = n.min[i] - target[i];
    const std::int64_t hi = target[i] - n.max[i];
    const std::int64_t d = lo > 0 ? lo : (hi > 0 ? hi : 0);
    dist += d * d;
  }
  return dist;
}

void searchFlatRec(const RawClimateRTree::FlatNode *nodes,
                   std::uint32_t nodeIdx, const std::int64_t *target,
                   std::int64_t &bestDist, std::uint32_t &bestLeafIdx) noexcept {
  const RawClimateRTree::FlatNode &node = nodes[nodeIdx];
  const std::uint32_t childBase = node.firstChildOrValue;
  const std::uint32_t count = node.childCount;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t childIdx = childBase + i;
    const RawClimateRTree::FlatNode &child = nodes[childIdx];
    const std::int64_t childDist = flatNodeDistance(child, target);
    if (bestDist > childDist) {
      if (child.childCount == 0) {
        bestDist = childDist;
        bestLeafIdx = childIdx;
      } else {
        searchFlatRec(nodes, childIdx, target, bestDist, bestLeafIdx);
      }
      if (bestDist == 0)
        return;
    }
  }
}

std::array<ClimateParameter, kDimensions>
computeBounds(const std::vector<std::unique_ptr<BuildNode>> &children) {
  std::array<ClimateParameter, kDimensions> bounds =
      children[0]->parameterSpace;
  for (std::size_t c = 1; c < children.size(); ++c) {
    for (int d = 0; d < kDimensions; ++d) {
      bounds[static_cast<std::size_t>(d)] =
          bounds[static_cast<std::size_t>(d)].spanWith(
              children[c]->parameterSpace[static_cast<std::size_t>(d)]);
    }
  }
  return bounds;
}

std::unique_ptr<BuildNode>
makeSubTreeNode(std::vector<std::unique_ptr<BuildNode>> children) {
  auto n = std::make_unique<BuildNode>();
  n->parameterSpace = computeBounds(children);
  n->children = std::move(children);
  return n;
}

std::int64_t spaceCost(const std::array<ClimateParameter, kDimensions> &space) {
  std::int64_t total = 0;
  for (int d = 0; d < kDimensions; ++d) {
    total += std::abs(space[static_cast<std::size_t>(d)].max -
                      space[static_cast<std::size_t>(d)].min);
  }
  return total;
}

std::int64_t axisKey(const ClimateParameter &p, bool absolute) noexcept {
  const std::int64_t center = (p.min + p.max) / 2;
  return absolute ? std::abs(center) : center;
}

template <class Ptr>
void sortNodes(std::vector<Ptr> &nodes, int startDim, bool absolute) {
  std::stable_sort(nodes.begin(), nodes.end(), [&](const Ptr &a, const Ptr &b) {
    for (int offset = 0; offset < kDimensions; ++offset) {
      const std::size_t d =
          static_cast<std::size_t>((startDim + offset) % kDimensions);
      const std::int64_t ka = axisKey(a->parameterSpace[d], absolute);
      const std::int64_t kb = axisKey(b->parameterSpace[d], absolute);
      if (ka != kb)
        return ka < kb;
    }
    return false;
  });
}

struct Bucket {
  std::array<ClimateParameter, kDimensions> parameterSpace{};
  std::vector<std::unique_ptr<BuildNode>> children;

  explicit Bucket(std::vector<std::unique_ptr<BuildNode>> ch)
      : children(std::move(ch)) {
    parameterSpace = computeBounds(children);
  }
};

std::unique_ptr<BuildNode>
buildTree(std::vector<std::unique_ptr<BuildNode>> children) {
  if (children.empty())
    throw std::logic_error("Need at least one child to build RTree node");
  if (children.size() == 1)
    return std::move(children[0]);
  if (children.size() <= static_cast<std::size_t>(kChildrenPerNode)) {
    std::stable_sort(
        children.begin(), children.end(),
        [](const std::unique_ptr<BuildNode> &a,
           const std::unique_ptr<BuildNode> &b) {
          std::int64_t magA = 0;
          std::int64_t magB = 0;
          for (int d = 0; d < kDimensions; ++d) {
            const auto idx = static_cast<std::size_t>(d);
            magA += std::abs(
                (a->parameterSpace[idx].min + a->parameterSpace[idx].max) / 2);
            magB += std::abs(
                (b->parameterSpace[idx].min + b->parameterSpace[idx].max) / 2);
          }
          return magA < magB;
        });
    return makeSubTreeNode(std::move(children));
  }

  const int expectedChildrenCount = static_cast<int>(std::pow(
      6.0, std::floor(std::log(static_cast<double>(children.size()) - 0.01) /
                      std::log(6.0))));

  std::int64_t minCost = std::numeric_limits<std::int64_t>::max();
  int minDimension = -1;
  for (int d = 0; d < kDimensions; ++d) {
    sortNodes(children, d, false);
    std::int64_t totalCost = 0;
    for (std::size_t i = 0; i < children.size();
         i += static_cast<std::size_t>(expectedChildrenCount)) {
      const std::size_t end =
          std::min(children.size(),
                   i + static_cast<std::size_t>(expectedChildrenCount));
      std::array<ClimateParameter, kDimensions> bounds =
          children[i]->parameterSpace;
      for (std::size_t j = i + 1; j < end; ++j) {
        for (int ax = 0; ax < kDimensions; ++ax) {
          bounds[static_cast<std::size_t>(ax)] =
              bounds[static_cast<std::size_t>(ax)].spanWith(
                  children[j]->parameterSpace[static_cast<std::size_t>(ax)]);
        }
      }
      totalCost += spaceCost(bounds);
    }
    if (minCost > totalCost) {
      minCost = totalCost;
      minDimension = d;
    }
  }

  sortNodes(children, minDimension, false);
  std::vector<std::unique_ptr<Bucket>> buckets;
  for (std::size_t i = 0; i < children.size();) {
    const std::size_t end = std::min(
        children.size(), i + static_cast<std::size_t>(expectedChildrenCount));
    std::vector<std::unique_ptr<BuildNode>> slice;
    slice.reserve(end - i);
    for (std::size_t j = i; j < end; ++j)
      slice.push_back(std::move(children[j]));
    buckets.push_back(std::make_unique<Bucket>(std::move(slice)));
    i = end;
  }

  sortNodes(buckets, minDimension, true);

  std::vector<std::unique_ptr<BuildNode>> subNodes;
  subNodes.reserve(buckets.size());
  for (auto &bucket : buckets) {
    subNodes.push_back(buildTree(std::move(bucket->children)));
  }
  return makeSubTreeNode(std::move(subNodes));
}

void flattenBuildTree(const BuildNode *root,
                      std::vector<RawClimateRTree::FlatNode> &out) {
  auto writeBounds = [](RawClimateRTree::FlatNode &dst, const BuildNode *src) {
    for (int d = 0; d < kDimensions; ++d) {
      dst.min[d] = src->parameterSpace[static_cast<std::size_t>(d)].min;
      dst.max[d] = src->parameterSpace[static_cast<std::size_t>(d)].max;
    }
  };
  out.clear();
  out.emplace_back();
  writeBounds(out[0], root);

  std::vector<std::pair<const BuildNode *, std::uint32_t>> queue;
  queue.emplace_back(root, 0u);
  for (std::size_t q = 0; q < queue.size(); ++q) {
    const BuildNode *bn = queue[q].first;
    const std::uint32_t dstIdx = queue[q].second;
    if (bn->children.empty()) {
      out[dstIdx].childCount = 0;
      out[dstIdx].firstChildOrValue = static_cast<std::uint32_t>(bn->valueIndex);
      continue;
    }
    const std::uint32_t firstChild = static_cast<std::uint32_t>(out.size());
    out[dstIdx].firstChildOrValue = firstChild;
    out[dstIdx].childCount = static_cast<std::uint8_t>(bn->children.size());
    out.resize(out.size() + bn->children.size());
    for (std::size_t c = 0; c < bn->children.size(); ++c) {
      const std::uint32_t childOutIdx = firstChild + static_cast<std::uint32_t>(c);
      const BuildNode *childBn = bn->children[c].get();
      writeBounds(out[childOutIdx], childBn);
      queue.emplace_back(childBn, childOutIdx);
    }
  }
}

struct LastLeafSlot {
  std::uint64_t treeId = 0;
  std::uint32_t nodeIdx = 0;
};

} // namespace

RawClimateRTree::RawClimateRTree() = default;
RawClimateRTree::~RawClimateRTree() = default;
RawClimateRTree::RawClimateRTree(RawClimateRTree &&) noexcept = default;
RawClimateRTree &
RawClimateRTree::operator=(RawClimateRTree &&) noexcept = default;

RawClimateRTree
RawClimateRTree::build(const std::vector<ClimateParameterPoint> &points) {
  if (points.empty())
    throw std::invalid_argument("Need at least one point to build RTree");
  RawClimateRTree tree;
  tree.treeId_ = gNextTreeId.fetch_add(1, std::memory_order_relaxed);
  std::vector<std::unique_ptr<BuildNode>> leaves;
  leaves.reserve(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    auto leaf = std::make_unique<BuildNode>();
    leaf->parameterSpace = points[i].parameterSpace();
    leaf->valueIndex = i;
    leaves.push_back(std::move(leaf));
  }
  auto root = buildTree(std::move(leaves));
  flattenBuildTree(root.get(), tree.nodes_);
  return tree;
}

std::size_t
RawClimateRTree::search(const ClimateTargetPoint &target) const noexcept {
  if (nodes_.empty())
    return 0;
  const RawClimateRTree::FlatNode *nodes = nodes_.data();
  if (nodes[0].childCount == 0)
    return nodes[0].firstChildOrValue;

  const std::array<std::int64_t, kDimensions> arr = target.toParameterArray();
  thread_local std::array<LastLeafSlot, 16> tlsLastLeaf{};
  LastLeafSlot &slot = tlsLastLeaf[treeId_ & 15u];

  std::int64_t bestDist = std::numeric_limits<std::int64_t>::max();
  std::uint32_t bestLeafIdx = UINT32_MAX;
  if (slot.treeId == treeId_ && slot.nodeIdx < nodes_.size()) {
    bestDist = flatNodeDistance(nodes[slot.nodeIdx], arr.data());
    if (bestDist == 0)
      return nodes[slot.nodeIdx].firstChildOrValue;
    bestLeafIdx = slot.nodeIdx;
  }

  searchFlatRec(nodes, 0u, arr.data(), bestDist, bestLeafIdx);
  slot.treeId = treeId_;
  slot.nodeIdx = bestLeafIdx;
  return nodes[bestLeafIdx].firstChildOrValue;
}

} // namespace detail

Pos ClimateSpawnFinder::findSpawnPosition(
    const std::vector<ClimateParameterPoint> &targetClimates,
    const ClimateSampler &sampler) {
  if (targetClimates.empty())
    return Pos{0, 0, 0};

  RadialSearchResult best = evaluateSpawnCandidate(targetClimates, sampler, 0, 0);
  auto radialSearch = [&](float maxRadius, float radiusIncrement) {
    float angle = 0.0f;
    float radius = radiusIncrement;
    const Pos searchOrigin = best.location;
    while (radius <= maxRadius) {
      const int x = searchOrigin.x +
                    static_cast<int>(std::sin(static_cast<double>(angle)) *
                                     static_cast<double>(radius));
      const int z = searchOrigin.z +
                    static_cast<int>(std::cos(static_cast<double>(angle)) *
                                     static_cast<double>(radius));
      const RadialSearchResult cand =
          evaluateSpawnCandidate(targetClimates, sampler, x, z);
      if (cand.fitness < best.fitness) {
        best = cand;
      }
      angle += radiusIncrement / radius;
      if (static_cast<double>(angle) > std::numbers::pi * 2.0) {
        angle = 0.0f;
        radius += radiusIncrement;
      }
    }
  };

  radialSearch(2048.0f, 512.0f);
  radialSearch(512.0f, 32.0f);
  return best.location;
}

} // namespace redstone::worldgen
