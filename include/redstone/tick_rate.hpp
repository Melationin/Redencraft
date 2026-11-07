#pragma once
#include <chrono>
#include <cstdint>
#include <optional>

namespace redstone {
// net.minecraft.world.TickRateManager plus the ServerTickRateManager half.
//
// The manager owns no network or chat state: the sprint clock lives here but the
// "commands.tick.sprint.report" line is handed to the caller as a value, so this
// class stays usable from tests and from any front end.
class TickRateManager {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr float kMinTickRate = 1.0f;
  static constexpr std::int64_t kNanosecondsPerSecond = 1000000000;

  // Bumped by every change the client has to be told about
  // (ClientboundTickingStatePacket / TickingStepPacket). The countdown inside
  // tick() is not a change: the client counts down on its own, like vanilla.
  std::uint64_t revision() const { return revision_; }

  void setTickRate(float rate) {
    tickRate_ = rate < kMinTickRate ? kMinTickRate : rate;
    nanosecondsPerTick_ = static_cast<std::int64_t>(
        static_cast<double>(kNanosecondsPerSecond) / tickRate_);
    ++revision_;
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
    ++revision_;
  }
  bool isFrozen() const { return frozen_; }

  void tick() {
    runGameElements_ = !frozen_ || frozenTicksToRun_ > 0;
    if (frozenTicksToRun_ > 0)
      --frozenTicksToRun_;
  }

  // ServerTickRateManager.
  bool isSprinting() const { return scheduledSprintTicks_ > 0; }
  int scheduledSprintTicks() const { return scheduledSprintTicks_; }
  int remainingSprintTicks() const { return remainingSprintTicks_; }

  bool stepGameIfPaused(int ticks) {
    if (!isFrozen())
      return false;
    frozenTicksToRun_ = ticks;
    ++revision_;
    return true;
  }
  bool stopStepping() {
    if (frozenTicksToRun_ <= 0)
      return false;
    frozenTicksToRun_ = 0;
    ++revision_;
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
    return interrupted;
  }

  // True when this loop iteration must tick immediately instead of waiting for
  // the next scheduled time. Only call it while isSprinting().
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
  // Set once when a sprint finishes (exhausted or cancelled).
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
  std::uint64_t revision_ = 1;
};
} // namespace redstone
