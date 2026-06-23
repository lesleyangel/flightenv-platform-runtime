/**
 * @file RuntimeEventQueue.cpp
 * @brief 实现 runtime 事件队列。
 *
 * 大概：这是统一时钟按事件驱动推进的基础实现。
 * 具体：它负责入队、出队、清空、排序和处理相同时间事件的优先级。
 * 被谁使用：被 RuntimeTimeScheduler 和事件队列单测使用。
 * 使用谁：使用 RuntimeEventQueue.hpp、RuntimeTimeTypes 和标准优先队列。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */

#include "FlightEnvPlatformRuntime/time/RuntimeEventQueue.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace FlightEnvPlatformRuntime {
namespace {

std::string generatedEventId(const std::string& kind, std::uint64_t sequence) {
  std::ostringstream oss;
  oss << "evt.runtime." << (kind.empty() ? "event" : kind) << "."
      << std::setw(6) << std::setfill('0') << sequence;
  return oss.str();
}

void normalizeEventTime(RuntimeEvent& event) {
  if (event.event_time.nanoseconds == 0 && event.event_time_s != 0.0) {
    event.event_time = RuntimeTimePoint::fromSeconds(event.event_time_s);
  }
  event.event_time_s = event.event_time.seconds();
}

}  // namespace

bool RuntimeEventQueue::EventOrder::operator()(
    const QueuedEvent& lhs,
    const QueuedEvent& rhs) const {
  if (!(lhs.event.event_time == rhs.event.event_time)) {
    return rhs.event.event_time < lhs.event.event_time;
  }
  if (lhs.event.priority != rhs.event.priority) {
    return lhs.event.priority > rhs.event.priority;
  }
  return lhs.sequence > rhs.sequence;
}

void RuntimeEventQueue::push(RuntimeEvent event) {
  const std::uint64_t sequence = next_sequence_++;
  normalizeEventTime(event);
  if (event.event_id.empty()) {
    event.event_id = generatedEventId(event.event_kind, sequence);
  }
  queue_.push(QueuedEvent{std::move(event), sequence});
}

void RuntimeEventQueue::pushFixedPublicTicks(
    int max_iterations,
    double base_dt_s,
    double output_period_s) {
  for (int iteration = 0; iteration < max_iterations; ++iteration) {
    push(publicTickEvent(makeRuntimeLoopTick(iteration, base_dt_s, output_period_s)));
  }
}

bool RuntimeEventQueue::empty() const {
  return queue_.empty();
}

std::size_t RuntimeEventQueue::size() const {
  return queue_.size();
}

RuntimeEvent RuntimeEventQueue::pop() {
  if (queue_.empty()) {
    throw std::runtime_error("RuntimeEventQueue::pop called on an empty queue");
  }
  RuntimeEvent event = queue_.top().event;
  queue_.pop();
  return event;
}

RuntimeEvent RuntimeEventQueue::publicTickEvent(const RuntimeLoopTick& tick) {
  RuntimeEvent event;
  event.event_kind = "public_tick";
  event.event_time = tick.public_output_time;
  event.event_time_s = tick.public_output_time_s;
  event.priority = 100000;
  event.iteration_index = tick.iteration_index;
  event.target_id = "workflow";
  event.loop_tick = tick;
  event.payload = tick.toJson();

  std::ostringstream oss;
  oss << "evt.runtime.public_tick." << std::setw(6) << std::setfill('0')
      << tick.iteration_index;
  event.event_id = oss.str();
  return event;
}

RuntimeEvent RuntimeEventQueue::nodeDueEvent(
    const std::string& node_id,
    double event_time_s,
    int public_iteration_index,
    int priority,
    nlohmann::json payload) {
  RuntimeEvent event;
  event.event_kind = "node_due";
  event.event_time = RuntimeTimePoint::fromSeconds(event_time_s);
  event.event_time_s = event.event_time.seconds();
  event.priority = priority;
  event.iteration_index = public_iteration_index;
  event.target_id = node_id;
  event.payload = std::move(payload);

  std::ostringstream oss;
  oss << "evt.runtime.node_due." << node_id << "."
      << event.event_time.nanoseconds;
  event.event_id = oss.str();
  return event;
}

RuntimeEvent RuntimeEventQueue::inputArrivedEvent(
    const std::string& input_id,
    double event_time_s,
    int public_iteration_index,
    nlohmann::json payload) {
  RuntimeEvent event;
  event.event_kind = "input_arrived";
  event.event_time = RuntimeTimePoint::fromSeconds(event_time_s);
  event.event_time_s = event.event_time.seconds();
  event.priority = 0;
  event.iteration_index = public_iteration_index;
  event.target_id = input_id;
  event.payload = std::move(payload);

  std::ostringstream oss;
  oss << "evt.runtime.input_arrived." << input_id << "."
      << event.event_time.nanoseconds;
  event.event_id = oss.str();
  return event;
}

RuntimeEvent RuntimeEventQueue::checkpointDueEvent(
    const std::string& checkpoint_id,
    double event_time_s,
    int public_iteration_index,
    nlohmann::json payload) {
  RuntimeEvent event;
  event.event_kind = "checkpoint_due";
  event.event_time = RuntimeTimePoint::fromSeconds(event_time_s);
  event.event_time_s = event.event_time.seconds();
  event.priority = 90000;
  event.iteration_index = public_iteration_index;
  event.target_id = checkpoint_id;
  event.payload = std::move(payload);

  std::ostringstream oss;
  oss << "evt.runtime.checkpoint_due." << checkpoint_id << "."
      << event.event_time.nanoseconds;
  event.event_id = oss.str();
  return event;
}

RuntimeEvent RuntimeEventQueue::stopCheckDueEvent(
    double event_time_s,
    int public_iteration_index,
    nlohmann::json payload) {
  RuntimeEvent event;
  event.event_kind = "stop_check_due";
  event.event_time = RuntimeTimePoint::fromSeconds(event_time_s);
  event.event_time_s = event.event_time.seconds();
  event.priority = 110000;
  event.iteration_index = public_iteration_index;
  event.target_id = "workflow";
  event.payload = std::move(payload);

  std::ostringstream oss;
  oss << "evt.runtime.stop_check_due."
      << event.event_time.nanoseconds << "."
      << std::setw(6) << std::setfill('0') << public_iteration_index;
  event.event_id = oss.str();
  return event;
}

RuntimeEvent RuntimeEventQueue::branchTriggeredEvent(
    const std::string& branch_id,
    double event_time_s,
    int public_iteration_index,
    const std::string& cause_event_id,
    nlohmann::json payload) {
  RuntimeEvent event;
  event.event_kind = "branch_triggered";
  event.event_time = RuntimeTimePoint::fromSeconds(event_time_s);
  event.event_time_s = event.event_time.seconds();
  event.priority = 50000;
  event.iteration_index = public_iteration_index;
  event.target_id = branch_id;
  event.cause_event_id = cause_event_id;
  event.payload = std::move(payload);

  std::ostringstream oss;
  oss << "evt.runtime.branch_triggered." << branch_id << "."
      << event.event_time.nanoseconds;
  event.event_id = oss.str();
  return event;
}

nlohmann::json RuntimeEventQueue::eventEvidence(const RuntimeEvent& event) {
  return {
      {"event_id", event.event_id},
      {"event_kind", event.event_kind},
      {"target_id", event.target_id},
      {"cause_event_id", event.cause_event_id},
      {"event_time_s", event.event_time_s},
      {"event_time_ns", event.event_time.nanoseconds},
      {"priority", event.priority},
      {"loop_iteration_index", event.iteration_index},
      {"payload", event.payload.is_null() ? nlohmann::json::object() : event.payload},
  };
}

}  // namespace FlightEnvPlatformRuntime
