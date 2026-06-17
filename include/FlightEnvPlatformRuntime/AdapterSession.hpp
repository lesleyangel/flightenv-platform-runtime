#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

inline constexpr const char* kAdapterSessionContract =
    "flightenv.platform.runtime.adapter_session.v1";

// RuntimeHost owns adapter lifecycle through this interface. Backends may be
// in-process DLLs, process adapters, ROS2 nodes, Python workers, ONNX sessions,
// or recording adapters, but the lifecycle and evidence shape stay identical.
class IAdapterSession {
 public:
  virtual ~IAdapterSession() = default;

  virtual std::string protocol() const = 0;
  virtual nlohmann::json summary() const = 0;
  virtual void shutdown() = 0;

  virtual nlohmann::json describe(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;
  virtual nlohmann::json resolve(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;
  virtual nlohmann::json initialize(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;
  virtual nlohmann::json warmup(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;
  virtual nlohmann::json execute(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;
  virtual nlohmann::json snapshot(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;
  virtual nlohmann::json flush(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;
};

}  // namespace FlightEnvPlatformRuntime
