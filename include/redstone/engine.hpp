#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace redstone {
enum class Direction { Down, Up, North, South, West, East };
Direction opposite(Direction d);
Direction directionFromString(const std::string &s);
std::string directionName(Direction d);
struct Pos {
  int x = 0, y = 0, z = 0;
  auto operator<=>(const Pos &) const = default;
  Pos relative(Direction d) const;
};
struct BlockState {
  std::string name = "minecraft:air";
  std::map<std::string, std::string> properties;
  BlockState() = default;
  BlockState(std::string n) : name(std::move(n)) {}
  BlockState(std::string n, std::map<std::string, std::string> p)
      : name(std::move(n)), properties(std::move(p)) {}
  std::string get(const std::string &key,
                  const std::string &fallback = "") const;
  BlockState with(const std::string &key, const std::string &value) const;
  bool operator==(const BlockState &) const = default;
};
BlockState defaultState(const std::string &name);
bool isSupportedBlock(const std::string &name);
bool isWoolCarpet(const BlockState &state);
bool isDeadCoralFan(const BlockState &state);
bool isDeadCoralWallFan(const BlockState &state);
struct EntityType {
  std::string name;
  bool simulated = false;
};
const std::vector<EntityType> &entityRegistry();
struct BlockMetadata {
  float hardness;
  bool conductor;
  std::string pushReaction;
  bool supported = false;
};
struct BlockRegistration {
  std::string name;
  BlockMetadata metadata;
};
const std::vector<BlockRegistration> &blockRegistry();
BlockMetadata blockMetadata(const BlockState &state);
struct ScheduledTick {
  Pos pos;
  std::string block;
  std::int64_t trigger;
  int priority;
  std::int64_t order;
};
struct BlockEvent {
  Pos pos;
  std::string block;
  int type = 0, data = 0;
  bool operator==(const BlockEvent &) const = default;
};
struct LogEntry {
  std::string kind;
  Pos pos;
  std::string detail;
};
struct PistonMovingBlockEntity;
struct PistonTickerSlot {
  Pos pos;
  std::weak_ptr<PistonMovingBlockEntity> entity;
};
class World {
public:
  explicit World(int minY = -64, int maxY = 320,
                 int maxNeighborUpdates = 1000000);
  void loadChunk(int x, int z);
  void unloadChunk(int x, int z);
  bool isChunkLoaded(int x, int z) const;
  bool isLoaded(Pos pos) const;
  BlockState getBlock(Pos pos) const;
  bool setBlock(Pos pos, BlockState state, int flags = 3, int limit = 512);
  void rawSetBlock(Pos pos, BlockState state);
  std::map<Pos, BlockState> allBlocks() const;
  int minY() const { return minY_; }
  int maxY() const { return maxY_; }
  std::optional<BlockState> coralPlacementState(Pos pos,
                                                const BlockState &selected,
                                                Direction clickedFace);
  BlockState updateFromNeighbourShapes(Pos p, const BlockState &state);
  void updateOrDestroy(Pos p, const BlockState &oldState,
                       const BlockState &newState, int flags, int limit);
  void neighborShapeChanged(Pos p, Direction d, Pos neighbor,
                            const BlockState &state, int flags, int limit);
  void updateNeighbourShapes(Pos p, const BlockState &state, int flags,
                             int limit);
  void updateIndirectNeighbourShapes(Pos p, const BlockState &state, int flags,
                                     int limit);
  void affectNeighborsAfterRemoval(Pos p, const BlockState &state, bool moved);
  void neighborChanged(Pos pos, const std::string &source);
  void neighborChanged(const BlockState &state, Pos pos,
                       const std::string &source, bool movedByPiston = false);
  void updateNeighborsAt(Pos pos, const std::string &source,
                         std::optional<Direction> skip = {});
  void scheduleTick(Pos pos, const std::string &block, int delay,
                    int priority = 0);
  bool hasScheduledTick(Pos pos, const std::string &block) const;
  void blockEvent(Pos pos, const std::string &block, int type, int data);
  int getSignal(Pos pos, Direction direction) const;
  bool hasNeighborSignal(Pos pos) const;
  void tick();
  std::int64_t gameTime() const { return time_; }
  void setGameTime(std::int64_t value) { time_ = value; }
  bool isHandlingTick() const { return handlingTick_; }
  std::vector<LogEntry> log;
  std::function<void(World &)> blockEntityTicker;

  void setPistonEntity(Pos pos,
                       std::shared_ptr<PistonMovingBlockEntity> entity);
  void removePistonEntity(Pos pos);
  std::map<Pos, std::shared_ptr<PistonMovingBlockEntity>> pistonEntities;
  std::map<Pos, std::shared_ptr<PistonTickerSlot>> pistonTickerSlots;
  std::vector<std::shared_ptr<PistonTickerSlot>> pistonEntityOrder;
  // Optional instrumentation hook, invoked after a moving entity is removed.
  std::function<void(World &, Pos)> pistonCompletionObserver;
  std::vector<ScheduledTick> scheduledTicks;
  std::vector<BlockEvent> blockEvents;
  bool tntExplodes = true;
  bool traceUpdates = false;

private:
  struct Update {
    enum class Kind { Simple, Full, Multi, Shape } kind;
    Pos pos;
    std::string source;
    std::optional<Direction> skip;
    int index = 0;
    Direction direction = Direction::Down;
    BlockState neighbor;
    BlockState capturedState;
    bool movedByPiston = false;
    Pos neighborPos;
    int flags = 0, limit = 0;
  };
  void enqueue(Update update);
  void runUpdates();
  bool runNext(Update &update);
  void executeNeighbor(Pos pos, const BlockState &state,
                       const std::string &source, bool movedByPiston);
  BlockState updateShapeState(Pos pos, const BlockState &state,
                              Direction direction, Pos neighborPos,
                              const BlockState &neighbor);
  void observerNotify(Pos pos, const BlockState &state);
  void shapeUpdate(Pos pos, Direction direction, Pos neighborPos,
                   const BlockState &neighbor, int flags, int limit);
  void runScheduled(const ScheduledTick &tick);
  void runEvents();
  struct Section {
    std::map<int, BlockState> states;
  };
  struct Chunk {
    bool loaded = true;
    std::map<int, Section> sections;
  };
  std::map<std::pair<int, int>, Chunk> chunks_;
  int minY_, maxY_, maxUpdates_, updateCount_ = 0;
  bool handlingTick_ = false;
  std::int64_t time_ = 0;
  std::int64_t order_ = 0;
  std::vector<Update> stack_, added_;
};
} // namespace redstone
