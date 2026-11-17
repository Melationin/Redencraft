#pragma once
// 游戏刻速率、冻结/单步调试与冲刺（sprint）调度状态机。
// 对齐原版 `TickRateManager` 与 `ServerTickRateManager`，以纯状态方式供主循环、指令与测试共用。
#include <chrono>
#include <cstdint>
#include <optional>

namespace redstone {
// 对应原版 `net.minecraft.world.TickRateManager` 及其服务端半部 `ServerTickRateManager`。
//
// 本管理器不持有任何网络或聊天状态：冲刺（sprint）计时器在此维护，但生成的
// `"commands.tick.sprint.report"` 数据以纯值 `SprintReport` 返回给调用方，
// 从而使该类无需依赖协议层即可被测试与任意前端复用。
class TickRateManager {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr float kMinTickRate = 1.0f;
  static constexpr std::int64_t kNanosecondsPerSecond = 1000000000;

  // 每次发生需要通知客户端的变化时递增对应版本号
  // （分别对应 `ClientboundTickingStatePacket` 与 `TickingStepPacket`）。
  // `tick()` 内部的步进倒计时不触发版本号递增：与原版一致，客户端会自行倒数剩余步数。
  std::uint64_t stateRevision() const { return stateRevision_; }
  std::uint64_t stepRevision() const { return stepRevision_; }
  std::uint64_t revision() const { return stateRevision_ + stepRevision_; }

  void setTickRate(float rate) {
    tickRate_ = rate < kMinTickRate ? kMinTickRate : rate;
    nanosecondsPerTick_ = static_cast<std::int64_t>(
        static_cast<double>(kNanosecondsPerSecond) / tickRate_);
    ++stateRevision_;
  }
  float tickrate() const { return tickRate_; }
  float millisecondsPerTick() const {
    return static_cast<float>(nanosecondsPerTick_) / 1.0e6f;
  }
  std::int64_t nanosecondsPerTick() const { return nanosecondsPerTick_; }
  bool runsNormally() const { return runGameElements_; }
  bool isSteppingForward() const { return frozenTicksToRun_ > 0; }
  int frozenTicksToRun() const { return frozenTicksToRun_; }
  void setFrozen(bool frozen) {
    if (frozen_ == frozen)
      return;
    frozen_ = frozen;
    ++stateRevision_;
  }
  bool isFrozen() const { return frozen_; }

  void tick() {
    runGameElements_ = !frozen_ || frozenTicksToRun_ > 0;
    if (frozenTicksToRun_ > 0)
      --frozenTicksToRun_;
  }

  // 对应 `ServerTickRateManager` 的步进与冲刺控制接口。
  bool isSprinting() const { return scheduledSprintTicks_ > 0; }
  int scheduledSprintTicks() const { return scheduledSprintTicks_; }
  int remainingSprintTicks() const { return remainingSprintTicks_; }

  // 仅在处于冻结状态时允许设定向前步进的 tick 数，供 `/tick step` 使用。
  bool stepGameIfPaused(int ticks) {
    if (!isFrozen())
      return false;
    frozenTicksToRun_ = ticks;
    ++stepRevision_;
    return true;
  }
  bool stopStepping() {
    if (frozenTicksToRun_ <= 0)
      return false;
    frozenTicksToRun_ = 0;
    ++stepRevision_;
    return true;
  }
  bool stopSprinting() {
    if (remainingSprintTicks_ <= 0)
      return false;
    finishSprint();
    return true;
  }
  bool requestGameToSprint(int ticks) {
    const bool interrupted = remainingSprintTicks_ > 0;
    sprintTimeSpend_ = Clock::duration::zero();
    scheduledSprintTicks_ = ticks;
    remainingSprintTicks_ = ticks;
    previousIsFrozen_ = isFrozen();
    setFrozen(false);
    // 冲刺开始时立即解除游戏元素冻结标志，防止第一次冲刺迭代读到上一次冻结 tick
    // 遗留的 `runGameElements_ == false` 而空转或消耗额外未计入预算的 tick。
    runGameElements_ = true;
    return interrupted;
  }

  // 若返回 true，表示主循环本次迭代应跳过等待、立即执行下一游戏刻。仅在 `isSprinting()` 时调用。
  bool checkShouldSprintThisTick(Clock::time_point now = Clock::now()) {
    if (!runGameElements_)
      return false;
    if (remainingSprintTicks_ <= 0) {
      finishSprint();
      return false;
    }
    sprintTickStart_ = now;
    --remainingSprintTicks_;
    return true;
  }
  void endTickWork(Clock::time_point now = Clock::now()) {
    sprintTimeSpend_ += now - sprintTickStart_;
  }

  struct SprintReport {
    int ticksPerSecond = 0;
    double millisecondsPerTick = 0;
  };
  // 冲刺结束（配额耗尽或被取消）后置位一次，取出后清空。
  std::optional<SprintReport> takeSprintReport() {
    const auto report = sprintReport_;
    sprintReport_.reset();
    return report;
  }

private:
  void finishSprint() {
    const int completed = scheduledSprintTicks_ - remainingSprintTicks_;
    const double milliseconds = static_cast<double>(std::max<std::int64_t>(
        std::int64_t(1), std::chrono::duration_cast<std::chrono::nanoseconds>(
                             sprintTimeSpend_)
                             .count())) /
                                1.0e6;
    SprintReport report;
    report.ticksPerSecond = completed == 0
                                ? 0
                                : static_cast<int>(1000.0 * completed /
                                                   milliseconds);
    report.millisecondsPerTick =
        completed == 0 ? double(millisecondsPerTick()) : milliseconds / completed;
    scheduledSprintTicks_ = 0;
    sprintTimeSpend_ = Clock::duration::zero();
    remainingSprintTicks_ = 0;
    setFrozen(previousIsFrozen_);
    sprintReport_ = report;
  }

  float tickRate_ = 20.0f;
  std::int64_t nanosecondsPerTick_ = kNanosecondsPerSecond / 20;
  int frozenTicksToRun_ = 0;
  bool runGameElements_ = true;
  bool frozen_ = false;
  int scheduledSprintTicks_ = 0;
  int remainingSprintTicks_ = 0;
  Clock::time_point sprintTickStart_{};
  Clock::duration sprintTimeSpend_{};
  bool previousIsFrozen_ = false;
  std::optional<SprintReport> sprintReport_;
  std::uint64_t stateRevision_ = 1;
  std::uint64_t stepRevision_ = 1;
};
} // namespace redstone
