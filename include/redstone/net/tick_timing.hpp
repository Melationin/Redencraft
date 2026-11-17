#pragma once
// Tick 耗时滑动统计窗口：聚合最近 1 秒内已完成的 tick 耗时样本，供服务端周期性向客户端上报性能指标。
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace redstone::net {
// 仅用于性能汇报统计：统计 (now - 1s, now] 时间窗内已完成的刻耗时，且至多每秒生成一次汇报，
// 避免每刻向客户端高频推送调试统计包而干扰网络带宽。
class TickTimingWindow {
public:
  using Clock = std::chrono::steady_clock;
  struct Report {
    double milliseconds;
    std::size_t ticks;
  };
  explicit TickTimingWindow(Clock::time_point start = Clock::now())
      : nextReport_(start + std::chrono::seconds(1)) {}
  std::optional<Report> record(Clock::time_point completed,
                               double milliseconds) {
    samples_.push_back({completed, milliseconds});
    sum_ += milliseconds;
    const auto cutoff = completed - std::chrono::seconds(1);
    while (!samples_.empty() && samples_.front().first <= cutoff) {
      sum_ -= samples_.front().second;
      samples_.pop_front();
    }
    if (completed < nextReport_)
      return std::nullopt;
    nextReport_ = completed + std::chrono::seconds(1);
    return Report{sum_ / samples_.size(), samples_.size()};
  }

private:
  Clock::time_point nextReport_;
  std::deque<std::pair<Clock::time_point, double>> samples_;
  double sum_ = 0;
};
} // namespace redstone::net
