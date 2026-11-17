#include "redstone/periodic_cleanup.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace redstone {

bool CleanupExecutionGate::tryAcquire() noexcept {
  bool expected = false;
  if (active_.compare_exchange_strong(expected, true,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    return true;
  }
  concurrentRejections_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

CleanupExecutionGate::Lease CleanupExecutionGate::tryAcquireLease() {
  if (!tryAcquire())
    return {};
  return Lease(shared_from_this(), kAdoptLock);
}

void CleanupExecutionGate::release() noexcept {
  active_.store(false, std::memory_order_release);
}

PeriodicCleanupManager::MsptTracker::MsptTracker(std::size_t windowSize)
    : samples(std::max<std::size_t>(1, windowSize), 0.0) {}

void PeriodicCleanupManager::MsptTracker::record(double tickMs) {
  const double clamped = std::isfinite(tickMs) ? std::max(0.0, tickMs) : 0.0;
  lastMs = clamped;
  if (sampleCount < samples.size()) {
    sumMs += clamped;
    samples[cursor] = clamped;
    cursor = (cursor + 1) % samples.size();
    ++sampleCount;
    return;
  }
  sumMs -= samples[cursor];
  sumMs += clamped;
  samples[cursor] = clamped;
  cursor = (cursor + 1) % samples.size();
}

double PeriodicCleanupManager::MsptTracker::average() const noexcept {
  if (sampleCount == 0)
    return 0.0;
  return sumMs / static_cast<double>(sampleCount);
}

PeriodicCleanupManager::PeriodicCleanupManager(
    PeriodicCleanupConfig config, std::shared_ptr<CleanupExecutionGate> gate)
    : config_(config),
      gate_(gate ? std::move(gate) : std::make_shared<CleanupExecutionGate>()),
      internalMspt_(config.msptWindowTicks),
      externalMspt_(config.msptWindowTicks) {
  if (config_.defaultIntervalTicks == 0)
    config_.defaultIntervalTicks = 100;
  if (config_.msptWindowTicks == 0)
    config_.msptWindowTicks = 100;
}

PeriodicCleanupManager::TaskId PeriodicCleanupManager::registerTask(
    std::string name, TaskCallback callback, std::uint64_t intervalTicks,
    std::optional<std::uint64_t> initialDelayTicks) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::uint64_t interval =
      intervalTicks > 0 ? intervalTicks : config_.defaultIntervalTicks;
  const std::uint64_t stagger =
      initialDelayTicks.has_value() ? *initialDelayTicks : (nextStaggerOffset_++);
  const std::uint64_t delay = initialDelayTicks.has_value()
                                  ? std::max<std::uint64_t>(1, *initialDelayTicks)
                                  : (interval + (stagger % interval));
  const TaskId id = nextTaskId_++;
  tasks_.push_back(TaskEntry{
      .id = id,
      .name = std::move(name),
      .intervalTicks = interval,
      .nextDueTick = currentTick_ + delay,
      .callback = std::move(callback),
  });
  return id;
}

bool PeriodicCleanupManager::unregisterTask(TaskId id) {
  if (id == 0)
    return false;
  std::lock_guard<std::mutex> lock(mutex_);
  for (std::size_t i = 0; i < tasks_.size(); ++i) {
    if (tasks_[i].id == id) {
      tasks_.erase(tasks_.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
  }
  return false;
}

bool PeriodicCleanupManager::advanceTick(double tickMs) {
  TaskCallback chosenCallback;
  CleanupExecutionGate::Lease lease;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++currentTick_;
    internalMspt_.record(tickMs);

    if (!hasDueTaskLocked())
      return false;

    if (isHighMsptLocked()) {
      ++deferredHighMsptCount_;
      return false;
    }

    lease = gate_->tryAcquireLease();
    if (!lease) {
      ++deferredBusyCount_;
      return false;
    }

    // 选择过期最久（`nextDueTick` 最小）的一项到期任务执行，保证每刻至多清理一项。
    std::size_t bestIndex = tasks_.size();
    for (std::size_t i = 0; i < tasks_.size(); ++i) {
      if (tasks_[i].nextDueTick > currentTick_)
        continue;
      if (bestIndex == tasks_.size() ||
          tasks_[i].nextDueTick < tasks_[bestIndex].nextDueTick) {
        bestIndex = i;
      }
    }
    if (bestIndex == tasks_.size())
      return false;

    tasks_[bestIndex].nextDueTick =
        currentTick_ + tasks_[bestIndex].intervalTicks;
    chosenCallback = tasks_[bestIndex].callback;
  }

  // 不在持锁状态下执行清理回调，允许期间安全注销任务或交由异步线程接管租约。
  if (!chosenCallback)
    return false;
  const bool performed = chosenCallback(lease);
  if (performed) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++executedCount_;
  }
  return performed;
}

void PeriodicCleanupManager::recordExternalTickTimeMs(double tickMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  externalMspt_.record(tickMs);
}

void PeriodicCleanupManager::setMaxAllowedMsptMs(double maxAllowedMsptMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.maxAllowedMsptMs = maxAllowedMsptMs;
}

void PeriodicCleanupManager::setDefaultIntervalTicks(
    std::uint64_t intervalTicks) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.defaultIntervalTicks = std::max<std::uint64_t>(1, intervalTicks);
}

bool PeriodicCleanupManager::isHighMspt() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return isHighMsptLocked();
}

bool PeriodicCleanupManager::isHighMsptLocked() const noexcept {
  if (config_.maxAllowedMsptMs <= 0.0)
    return false;
  if (internalMspt_.count() > 0 &&
      (internalMspt_.last() >= config_.maxAllowedMsptMs ||
       internalMspt_.average() >= config_.maxAllowedMsptMs)) {
    return true;
  }
  if (externalMspt_.count() > 0 &&
      (externalMspt_.last() >= config_.maxAllowedMsptMs ||
       externalMspt_.average() >= config_.maxAllowedMsptMs)) {
    return true;
  }
  return false;
}

bool PeriodicCleanupManager::hasDueTaskLocked() const noexcept {
  for (const auto &task : tasks_) {
    if (task.nextDueTick <= currentTick_)
      return true;
  }
  return false;
}

PeriodicCleanupStats PeriodicCleanupManager::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  PeriodicCleanupStats out;
  out.totalTicks = currentTick_;
  out.executedCount = executedCount_;
  out.deferredHighMsptCount = deferredHighMsptCount_;
  out.deferredBusyCount = deferredBusyCount_;
  out.concurrentRejectionCount = gate_->concurrentRejectionCount();
  out.lastTickMs = externalMspt_.count() > 0
                       ? std::max(internalMspt_.last(), externalMspt_.last())
                       : internalMspt_.last();
  out.averageMsptMs =
      externalMspt_.count() > 0
          ? std::max(internalMspt_.average(), externalMspt_.average())
          : internalMspt_.average();
  out.maxAllowedMsptMs = config_.maxAllowedMsptMs;
  out.gateBusy = gate_->isBusy();
  out.registeredTasks = tasks_.size();
  return out;
}

} // namespace redstone
