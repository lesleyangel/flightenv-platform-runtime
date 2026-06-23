#include "FlightEnvPlatformRuntime/RuntimeEventLoop.hpp"

#include "FlightEnvPlatformRuntime/RuntimeClock.hpp"

namespace FlightEnvPlatformRuntime {

RuntimeEventQueue& RuntimeEventLoop::queue() {
  return queue_;
}

const RuntimeEventQueue& RuntimeEventLoop::queue() const {
  return queue_;
}

bool RuntimeEventLoop::empty() const {
  return queue_.empty();
}

std::size_t RuntimeEventLoop::pendingCount() const {
  return queue_.size();
}

void RuntimeEventLoop::push(RuntimeEvent event) {
  queue_.push(std::move(event));
}

RuntimeEvent RuntimeEventLoop::next() {
  RuntimeEvent event = queue_.pop();
  ++dispatched_count_;
  ++dispatched_by_kind_[event.event_kind];
  return event;
}

nlohmann::json RuntimeEventLoop::schedulerEvent(
    const RuntimeEvent& event,
    const std::string& event_name,
    nlohmann::json extra) {
  nlohmann::json record = RuntimeEventQueue::eventEvidence(event);
  record["timestamp_utc"] = RuntimeClock::nowUtcIso();
  record["event"] = event_name.empty() ? event.event_kind : event_name;
  record["runtime_event_id"] = event.event_id;
  record["runtime_event_kind"] = event.event_kind;
  record["runtime_event_time_s"] = event.event_time_s;
  record["runtime_event_time_ns"] = event.event_time.nanoseconds;

  // Keep the legacy scheduler-event field names while exposing the normalized
  // eventEvidence fields above. Downstream evidence readers can migrate at
  // their own pace without losing the stable runtime_event_* keys.
  record["loop_iteration_index"] = event.iteration_index;
  record["event_time_s"] = event.event_time_s;
  record["event_time_ns"] = event.event_time.nanoseconds;

  if (extra.is_object()) {
    for (auto it = extra.begin(); it != extra.end(); ++it) {
      record[it.key()] = it.value();
    }
  }
  return record;
}

nlohmann::json RuntimeEventLoop::summary() const {
  nlohmann::json by_kind = nlohmann::json::object();
  for (const auto& item : dispatched_by_kind_) {
    by_kind[item.first] = item.second;
  }
  return {
      {"type", "FlightEnvPlatformRuntime::RuntimeEventLoop"},
      {"dispatched_count", dispatched_count_},
      {"pending_count", pendingCount()},
      {"dispatched_by_kind", by_kind},
  };
}

}  // namespace FlightEnvPlatformRuntime
