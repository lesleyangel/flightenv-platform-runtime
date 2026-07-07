#include "FlightEnvPlatformRuntime/estimation/RuntimeObservationInbox.hpp"

#include <algorithm>
#include <cmath>

namespace FlightEnvPlatformRuntime::estimation {
namespace {

double jsonNumber(const nlohmann::json& value, const std::string& key, double fallback) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
    return fallback;
  }
  return value.at(key).get<double>();
}

int jsonInt(const nlohmann::json& value, const std::string& key, int fallback) {
  if (!value.is_object() || !value.contains(key)) {
    return fallback;
  }
  const auto& item = value.at(key);
  if (item.is_number_integer()) {
    return item.get<int>();
  }
  if (item.is_number()) {
    return static_cast<int>(item.get<double>());
  }
  return fallback;
}

bool jsonBool(const nlohmann::json& value, const std::string& key, bool fallback) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_boolean()) {
    return fallback;
  }
  return value.at(key).get<bool>();
}

bool hasNumber(const nlohmann::json& value, const std::string& key) {
  return value.is_object() && value.contains(key) && value.at(key).is_number();
}

std::string jsonString(const nlohmann::json& value, const std::string& key, const std::string& fallback) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return fallback;
  }
  return value.at(key).get<std::string>();
}

nlohmann::json framePayload(const nlohmann::json& item) {
  if (item.is_object() && item.contains("frame") && item.at("frame").is_object()) {
    return item.at("frame");
  }
  return item;
}

void collectNumbers(
    const nlohmann::json& value,
    const std::string& prefix,
    std::vector<std::string>& labels,
    std::vector<double>& numbers,
    std::size_t limit) {
  if (numbers.size() >= limit) {
    return;
  }
  if (value.is_number()) {
    labels.push_back(prefix.empty() ? "value_" + std::to_string(numbers.size()) : prefix);
    numbers.push_back(value.get<double>());
    return;
  }
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end() && numbers.size() < limit; ++it) {
      collectNumbers(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key(), labels, numbers, limit);
    }
    return;
  }
  if (value.is_array()) {
    for (std::size_t i = 0; i < value.size() && numbers.size() < limit; ++i) {
      collectNumbers(value.at(i), prefix + "[" + std::to_string(i) + "]", labels, numbers, limit);
    }
  }
}

void extractObservationVector(
    const nlohmann::json& payload,
    std::vector<std::string>& labels,
    std::vector<double>& values) {
  labels.clear();
  values.clear();
  if (payload.is_object() && payload.contains("selected_state") && payload.at("selected_state").is_object()) {
    collectNumbers(payload.at("selected_state"), "selected_state", labels, values, 32);
  }
  if (values.empty() && payload.is_object() && payload.contains("values")) {
    collectNumbers(payload.at("values"), "values", labels, values, 32);
  }
  if (values.empty()) {
    collectNumbers(payload, "", labels, values, 32);
  }
  for (double& value : values) {
    if (!std::isfinite(value)) {
      value = 0.0;
    }
  }
}

}  // namespace

RuntimeObservationInbox RuntimeObservationInbox::fromJsonStream(const nlohmann::json& stream, int max_frames) {
  RuntimeObservationInbox inbox;
  nlohmann::json frames = nlohmann::json::array();
  if (stream.is_object() && stream.contains("frames") && stream.at("frames").is_array()) {
    frames = stream.at("frames");
  } else if (stream.is_array()) {
    frames = stream;
  }
  const int limit = max_frames > 0
                        ? std::min<int>(max_frames, static_cast<int>(frames.size()))
                        : static_cast<int>(frames.size());
  inbox.frames_.reserve(static_cast<std::size_t>(std::max(0, limit)));
  for (int i = 0; i < limit; ++i) {
    const nlohmann::json raw = frames.at(static_cast<std::size_t>(i));
    const nlohmann::json payload = framePayload(raw);
    RuntimeObservationFrame frame;
    frame.frame_index = jsonInt(payload, "frame_index", i);
    frame.sample_time_s = jsonNumber(payload, "sample_time_s", jsonNumber(payload, "time_s", static_cast<double>(i)));
    frame.has_arrival_time = hasNumber(payload, "arrival_time_s");
    frame.arrival_time_s = frame.has_arrival_time
                               ? jsonNumber(payload, "arrival_time_s", frame.sample_time_s)
                               : frame.sample_time_s;
    frame.observation_status = jsonString(payload, "observation_status", "available");
    frame.missing_observation = jsonBool(payload, "missing_observation", false) ||
                                frame.observation_status == "missing" ||
                                frame.observation_status == "unavailable";
    frame.explicit_late_observation = jsonBool(payload, "late_observation", false) ||
                                      frame.observation_status == "late";
    frame.payload = payload;
    frame.source_summary = {
        {"source", payload.value("source", std::string("external_observation_stream"))},
        {"sensor_count", jsonInt(payload, "sensor_count", 0)},
        {"has_selected_state", payload.is_object() && payload.contains("selected_state")},
        {"observation_status", frame.observation_status},
        {"missing_observation", frame.missing_observation},
        {"late_observation_declared", frame.explicit_late_observation},
        {"has_arrival_time", frame.has_arrival_time},
        {"arrival_time_s", frame.arrival_time_s},
    };
    extractObservationVector(payload, frame.value_labels, frame.values);
    if (!frame.values.empty() || frame.missing_observation) {
      inbox.frames_.push_back(std::move(frame));
    }
  }
  return inbox;
}

const std::vector<RuntimeObservationFrame>& RuntimeObservationInbox::frames() const {
  return frames_;
}

bool RuntimeObservationInbox::empty() const {
  return frames_.empty();
}

std::size_t RuntimeObservationInbox::size() const {
  return frames_.size();
}

}  // namespace FlightEnvPlatformRuntime::estimation
