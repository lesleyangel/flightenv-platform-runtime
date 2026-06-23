#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace FlightEnvPlatformRuntime {

struct RuntimeFailurePolicyRequest {
  std::string event_kind;
  std::string policy = "fail_fast";
  int attempt_index = 0;
  int max_retries = 0;
  bool can_degrade = false;
  bool cancel_requested = false;
  bool backpressure_saturated = false;
};

struct RuntimeFailurePolicyDecision {
  std::string action = "fail";
  bool terminal = true;
  bool retry = false;
  bool degraded = false;
  bool cancelled = false;
  bool backpressure_wait = false;
  int next_attempt_index = 0;
  std::string reason;
};

RuntimeFailurePolicyDecision decideRuntimeFailurePolicy(const RuntimeFailurePolicyRequest& request);

nlohmann::json runtimeFailurePolicyDecisionToJson(const RuntimeFailurePolicyDecision& decision);

}  // namespace FlightEnvPlatformRuntime
