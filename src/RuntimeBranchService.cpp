#include "FlightEnvPlatformRuntime/RuntimeBranchService.hpp"

#include "FlightEnvPlatformRuntime/RuntimeClock.hpp"
#include "FlightEnvPlatformRuntime/time/RuntimeTimeTypes.hpp"

#include <iomanip>
#include <sstream>

namespace FlightEnvPlatformRuntime {
namespace {

std::string jsonString(const nlohmann::json& value, const std::string& key, const std::string& fallback = "") {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

}  // namespace

void RuntimeBranchService::upsertRecord(nlohmann::json& records, const nlohmann::json& record) {
  if (!records.is_array()) {
    records = nlohmann::json::array();
  }
  const std::string branch_id = jsonString(record, "branch_id");
  if (!branch_id.empty()) {
    for (auto& item : records) {
      if (jsonString(item, "branch_id") == branch_id) {
        item = record;
        return;
      }
    }
  }
  records.push_back(record);
}

bool RuntimeBranchService::updateRecordStatus(
    nlohmann::json& records,
    const std::string& branch_id,
    const std::string& status,
    const nlohmann::json& summary) {
  if (!records.is_array()) {
    return false;
  }
  for (auto& item : records) {
    if (jsonString(item, "branch_id") == branch_id) {
      item["status"] = status;
      item["updated_at_utc"] = RuntimeClock::nowUtcIso();
      if (!summary.is_null()) {
        item["summary"] = summary;
      }
      return true;
    }
  }
  return false;
}

std::string RuntimeBranchService::appendEvent(
    nlohmann::json& events,
    const std::string& run_id_prefix,
    const std::string& event_kind,
    const std::string& branch_id,
    int frame_index,
    double time_s,
    const nlohmann::json& payload) {
  if (!events.is_array()) {
    events = nlohmann::json::array();
  }
  const RuntimeTimePoint event_time = RuntimeTimePoint::fromSeconds(time_s);
  std::string event_id = jsonString(payload, "event_id");
  if (event_id.empty()) {
    std::ostringstream id;
    id << "evt." << run_id_prefix << "." << event_kind;
    if (frame_index >= 0) {
      id << ".frame_" << std::setw(4) << std::setfill('0') << frame_index;
    }
    id << "." << std::setw(5) << std::setfill('0') << events.size();
    event_id = id.str();
  }
  events.push_back({
      {"event_id", event_id},
      {"event_kind", event_kind},
      {"branch_id", branch_id},
      {"frame_index", frame_index},
      {"time_s", event_time.seconds()},
      {"time_ns", event_time.nanoseconds},
      {"generated_at_utc", RuntimeClock::nowUtcIso()},
      {"payload", payload.is_null() ? nlohmann::json::object() : payload},
  });
  return event_id;
}

}  // namespace FlightEnvPlatformRuntime
