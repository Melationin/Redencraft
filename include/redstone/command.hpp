#pragma once
// 服务端指令系统接口：实现类似 Brigadier 的指令节点树、解析/执行/补全分发器，
// 并定义 /tick 与 /time 等内置指令依赖的上下文与权限执行源。
#include "redstone/tick_rate.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {
class World;
class WorldClocks;
enum class BuiltinWorldClock : std::uint8_t;
}
namespace redstone::command {
// 指令产生的一条聊天输出行。原版通过可翻译文本组件（translatable component）发送给客户端解析，
// 因此服务端仅需保存并传输翻译键及其参数列表。
struct Message {
  std::string key;
  std::vector<std::string> args;
  bool failure = false;
  // 对应 CommandSourceStack.sendSuccess(message, broadcast)：true 表示广播给所有有权限
  // 查看指令的玩家，false 表示仅发送给指令发起者。
  bool broadcast = false;
};
// 供 /tick 与 /time 指令访问与修改的服务端状态；由调用方在每个 tick 刷新耗时统计。
struct Context {
  TickRateManager *ticks = nullptr;
  World *world = nullptr;
  WorldClocks *worldClocks = nullptr;
  // 对应 ServerClockManager.modifyClock：仅广播发生变更的单一世界时钟。
  std::function<void(BuiltinWorldClock)> worldClockChanged;
  std::int64_t averageTickTimeNanos = 0;
  // 对应 MinecraftServer.tickTimesNanos：长度为 100 的环形计时样本数组，启动时清零。
  std::array<std::int64_t, 100> tickTimes{};
};
// 指令执行源（对应 CommandSourceStack 的权限校验与输出收集部分）。
struct Source {
  // 权限等级：0 全员，2 游戏管理员，3 管理员，4 服主（对应 Commands.LEVEL_*）。
  int permissionLevel = 0;
  std::string name;
  std::vector<Message> messages;
  void success(std::string key, std::vector<std::string> args = {},
               bool broadcast = false);
  void failure(std::string key, std::vector<std::string> args = {});
};
// 对应 CommandDispatcher.getCompletionSuggestions 的返回结果：调用方据此把替换范围和
// 候选字符串写入 ClientboundCommandSuggestions 协议包。
struct Completion {
  int start = 0;
  int length = 0;
  std::vector<std::string> texts;
};
// 指令语法树节点：通过枚举区分根节点、字面量与参数，同时承载权限过滤、自定义补全与执行回调，
// 以便服务端本地解析执行与协议侧编码 ClientboundCommandsPacket 共用同一棵树。
struct Node {
  enum class Kind { Root, Literal, Argument };
  // /tick 与 /time 使用的参数解析类型；协议编码器会将其映射到官方解析器 ID
  //（float=1, time=43, resource=46）。
  enum class ArgumentKind { Float, Time, Resource, Identifier };

  Kind kind = Kind::Root;
  std::string name;
  // 对应 CommandNode.getRequirement：仅当该条件返回 true 时，节点才对执行源可见/可达。
  std::function<bool(const Source &)> requirement;
  ArgumentKind argumentKind = ArgumentKind::Float;
  double minimum = 0, maximum = 0;
  // 对应 ResourceArgument.Info 载荷，同时限定当前指令树可解析的注册表候选集合。
  std::string resourceRegistry;
  std::map<std::string, int> resourceValues;
  // 对应 ArgumentCommandNode.getCustomSuggestions：原版为参数提供的固定示例建议
  //（例如 /tick rate -> "20"，step/sprint 时间参数 -> "1t", "1s", ...）。
  std::vector<std::string> suggestions;
  // 依赖前置已解析参数的动态补全（例如根据已选时钟筛选兼容的时间线 timeline）。
  std::function<std::vector<std::string>(
      const std::map<std::string, double> &)>
      dynamicSuggestions;
  std::function<int(const Context &, Source &,
                    const std::map<std::string, double> &)>
      executes;
  // IdentifierArgument 保留归一化后的字符串文本，供 /time set <timemarker> 等按名称查询的指令使用。
  std::function<int(const Context &, Source &,
                    const std::map<std::string, double> &,
                    const std::map<std::string, std::string> &)>
      executesWithStrings;
  std::vector<std::unique_ptr<Node>> children;

  bool executable() const {
    return static_cast<bool>(executes) ||
           static_cast<bool>(executesWithStrings);
  }
  bool restricted() const { return static_cast<bool>(requirement); }
};

// 指令树管理与解析分发器：负责命令行匹配执行及 Tab 补全建议计算。
class Dispatcher {
public:
  Dispatcher();
  Node &root();
  const Node &root() const;
  // 解析并执行不含前导斜杠的命令行；返回指令执行结果，若无节点接受该输入（未知/不完整指令
  // 或执行源未满足 requirement）则返回 -1。
  int run(std::string_view line, Source &, const Context &) const;
  // 对应 CommandDispatcher.getCompletionSuggestions：针对不含前导斜杠的部分命令行，
  // 返回最后一个词（可为空）的补全候选列表。
  Completion suggestions(std::string_view line, const Source &) const;

private:
  std::unique_ptr<Node> root_;
};

// 注册受支持的指令树。registerTimeCommand 为可选显式注册，因为旧版离线投影场景没有 WorldClocks 宿主。
void registerTickCommand(Dispatcher &);
void registerTimeCommand(Dispatcher &);
} // namespace redstone::command
