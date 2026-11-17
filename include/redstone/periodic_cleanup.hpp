#pragma once
// 周期性内存清理协调器：统一调度百刻级内存收缩（计划刻、光照队列等），
// 保证同刻及跨线程互不并发，并在高 MSPT 负载下自动延后清理。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace redstone {

// 清理执行互斥门控：防止同步主线程清理与异步工作线程清理（或重入调用）同时发生。
class CleanupExecutionGate
    : public std::enable_shared_from_this<CleanupExecutionGate> {
public:
  struct AdoptLockTag {
    explicit AdoptLockTag() = default;
  };
  static inline constexpr AdoptLockTag kAdoptLock{};

  // RAII 持有门控租约，析构或移动赋值时自动归还清理执行权。
  class Lease {
  public:
    Lease() noexcept = default;
    Lease(std::shared_ptr<CleanupExecutionGate> gate, AdoptLockTag) noexcept
        : gate_(std::move(gate)) {}
    ~Lease() { release(); }

    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;

    Lease(Lease &&other) noexcept : gate_(std::move(other.gate_)) {}
    Lease &operator=(Lease &&other) noexcept {
      if (this != &other) {
        release();
        gate_ = std::move(other.gate_);
      }
      return *this;
    }

    explicit operator bool() const noexcept { return static_cast<bool>(gate_); }

    void release() noexcept {
      if (gate_) {
        gate_->release();
        gate_.reset();
      }
    }

  private:
    std::shared_ptr<CleanupExecutionGate> gate_;
  };

  // 尝试占用唯一清理槽位；若已有清理在进行则返回 `false` 并记录一次并发拦截。
  bool tryAcquire() noexcept;
  // 尝试获取 RAII 清理租约；失败时返回空租约。
  Lease tryAcquireLease();
  // 归还清理执行权（通常由 `Lease` 自动调用）。
  void release() noexcept;

  bool isBusy() const noexcept {
    return active_.load(std::memory_order_acquire);
  }
  std::uint64_t concurrentRejectionCount() const noexcept {
    return concurrentRejections_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<bool> active_{false};
  std::atomic<std::uint64_t> concurrentRejections_{0};
};

// 周期性清理协调器的可调参数。
struct PeriodicCleanupConfig {
  std::uint64_t defaultIntervalTicks = 100;
  // 当最近一帧耗时或滑动平均 MSPT 达到该阈值（默认 40 ms，留出 50 ms 满刻裕量）时暂停清理。
  double maxAllowedMsptMs = 40.0;
  std::size_t msptWindowTicks = 100;
};

// 运行期清理统计快照，便于可观测性与回归验证。
struct PeriodicCleanupStats {
  std::uint64_t totalTicks = 0;
  std::uint64_t executedCount = 0;
  std::uint64_t deferredHighMsptCount = 0;
  std::uint64_t deferredBusyCount = 0;
  std::uint64_t concurrentRejectionCount = 0;
  double lastTickMs = 0.0;
  double averageMsptMs = 0.0;
  double maxAllowedMsptMs = 40.0;
  bool gateBusy = false;
  std::size_t registeredTasks = 0;
};

// 统一管理“每 N 刻检查回缩”的调度类：
// 1. 按刻错峰且每刻最多派发 1 项清理，防止多子系统集中清理引发卡顿；
// 2. 通过 `CleanupExecutionGate` 与异步后台收缩共享互斥租约，杜绝跨线程同时清理；
// 3. 当瞬时或窗口平均 MSPT 过高时自动推迟清理，待负载回落后再顺延消化。
class PeriodicCleanupManager {
public:
  using TaskId = std::uint64_t;
  // 清理回调：若决定将工作交给后台线程异步完成，应通过 `std::move(lease)` 接管租约；
  // 若为同步清理或发现无需收缩，则直接返回（未移交的 `lease` 会在返回后自动释放）。
  // 返回值表示本轮是否实际触发了同步/异步清理工作。
  using TaskCallback = std::function<bool(CleanupExecutionGate::Lease &lease)>;

  explicit PeriodicCleanupManager(
      PeriodicCleanupConfig config = {},
      std::shared_ptr<CleanupExecutionGate> gate = nullptr);

  TaskId registerTask(std::string name, TaskCallback callback,
                      std::uint64_t intervalTicks = 0,
                      std::optional<std::uint64_t> initialDelayTicks = std::nullopt);
  bool unregisterTask(TaskId id);

  // 推进 1 个模拟刻并记入本刻耗时（毫秒），在满足 MSPT 与互斥门控时至多派发 1 项到期任务。
  // 返回本刻是否实际触发了某项清理。
  bool advanceTick(double tickMs);

  // 可选记录服务端全刻耗时（如包含网络收发与实体广播）；后续 `advanceTick` 会合并采样。
  void recordExternalTickTimeMs(double tickMs);

  void setMaxAllowedMsptMs(double maxAllowedMsptMs);
  void setDefaultIntervalTicks(std::uint64_t intervalTicks);

  bool isHighMspt() const;
  PeriodicCleanupStats stats() const;
  const std::shared_ptr<CleanupExecutionGate> &gate() const noexcept {
    return gate_;
  }

private:
  struct MsptTracker {
    explicit MsptTracker(std::size_t windowSize);
    void record(double tickMs);
    double average() const noexcept;
    double last() const noexcept { return lastMs; }
    std::size_t count() const noexcept { return sampleCount; }

    std::vector<double> samples;
    std::size_t cursor = 0;
    std::size_t sampleCount = 0;
    double sumMs = 0.0;
    double lastMs = 0.0;
  };

  struct TaskEntry {
    TaskId id = 0;
    std::string name;
    std::uint64_t intervalTicks = 100;
    std::uint64_t nextDueTick = 100;
    TaskCallback callback;
  };

  bool isHighMsptLocked() const noexcept;
  bool hasDueTaskLocked() const noexcept;

  PeriodicCleanupConfig config_;
  std::shared_ptr<CleanupExecutionGate> gate_;
  mutable std::mutex mutex_;
  MsptTracker internalMspt_;
  MsptTracker externalMspt_;
  std::uint64_t currentTick_ = 0;
  std::uint64_t nextTaskId_ = 1;
  std::uint64_t nextStaggerOffset_ = 0;
  std::uint64_t executedCount_ = 0;
  std::uint64_t deferredHighMsptCount_ = 0;
  std::uint64_t deferredBusyCount_ = 0;
  std::vector<TaskEntry> tasks_;
};

} // namespace redstone
