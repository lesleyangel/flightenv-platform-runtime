#include "FlightEnvPlatformRuntime/RuntimeFailurePolicy.hpp"

#include <algorithm>
#include <cctype>

namespace FlightEnvPlatformRuntime {

namespace {

std::string normalizePolicy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value.empty()) {
    return "fail_fast";
  }
  return value;
}

RuntimeFailurePolicyDecision makeDecision(
    const std::string& action,
    bool terminal,
    const std::string& reason) {
  RuntimeFailurePolicyDecision decision;
  decision.action = action;
  decision.terminal = terminal;
  decision.reason = reason;
  return decision;
}

}  // namespace

RuntimeFailurePolicyDecision decideRuntimeFailurePolicy(const RuntimeFailurePolicyRequest& request) {
  const std::string policy = normalizePolicy(request.policy);
  const std::string event_kind = normalizePolicy(request.event_kind);

  if (request.cancel_requested || event_kind == "cancel_requested") {
    RuntimeFailurePolicyDecision decision = makeDecision("cancel", true, "cancel_requested");
    decision.cancelled = true;
    decision.next_attempt_index = request.attempt_index;
    return decision;
  }

  if (request.backpressure_saturated || event_kind == "backpressure_saturated") {
    RuntimeFailurePolicyDecision decision =
        makeDecision("wait_backpressure", false, "backpressure_saturated");
    decision.backpressure_wait = true;
    decision.next_attempt_index = request.attempt_index;
    return decision;
  }

  if (policy == "retry" && request.attempt_index < request.max_retries) {
    RuntimeFailurePolicyDecision decision = makeDecision("retry", false, "retry_budget_available");
    decision.retry = true;
    decision.next_attempt_index = request.attempt_index + 1;
    return decision;
  }

  if ((policy == "degrade" || policy == "degrade_on_failure") && request.can_degrade) {
    RuntimeFailurePolicyDecision decision = makeDecision("degrade", false, "degrade_policy_selected");
    decision.degraded = true;
    decision.next_attempt_index = request.attempt_index;
    return decision;
  }

  RuntimeFailurePolicyDecision decision = makeDecision("fail", true, "fail_fast");
  decision.next_attempt_index = request.attempt_index;
  return decision;
}

nlohmann::json runtimeFailurePolicyDecisionToJson(const RuntimeFailurePolicyDecision& decision) {
  return {
      {"action", decision.action},
      {"terminal", decision.terminal},
      {"retry", decision.retry},
      {"degraded", decision.degraded},
      {"cancelled", decision.cancelled},
      {"backpressure_wait", decision.backpressure_wait},
      {"next_attempt_index", decision.next_attempt_index},
      {"reason", decision.reason},
  };
}

}  // namespace FlightEnvPlatformRuntime
