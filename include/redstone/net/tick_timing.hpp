#pragma once
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace redstone::net {
// Reporting only: completed ticks in (now - 1s, now], at most one report/sec.
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
