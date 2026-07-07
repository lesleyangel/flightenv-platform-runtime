#include "NativeWorkflowNodePreparation.hpp"

#include <stdexcept>

namespace FlightEnvPlatformRuntime {

NativeWorkflowNodeInputPreparationResult prepareNativeWorkflowNodeInputs(
    const NativeWorkflowNodeInputPreparationRequest& request) {
  if (request.port_store == nullptr) {
    throw std::runtime_error("NativeWorkflowNodePreparation requires a non-null port_store.");
  }
  if (request.sample_buffer == nullptr) {
    throw std::runtime_error("NativeWorkflowNodePreparation requires a non-null sample_buffer.");
  }

  NativeWorkflowNodeInputPreparationResult result;

  RuntimePortBindingResolveRequest binding_request;
  binding_request.node = request.node;
  binding_request.base_upstream = request.base_upstream;
  binding_request.external_seed = request.external_seed;
  binding_request.current_outputs = request.current_outputs;
  binding_request.data_plane_info = request.data_plane_info;
  binding_request.port_store = request.port_store;
  binding_request.branch_id = request.branch_id;
  binding_request.timeline_id = request.timeline_id;
  binding_request.event_time_ns = request.event_time_ns;
  binding_request.allow_implicit_contract_port_binding =
      request.allow_implicit_contract_port_binding;
  binding_request.trace = request.trace;
  result.port_binding = RuntimePortBindingResolver::resolve(binding_request);
  result.upstream = result.port_binding.upstream;

  result.transition_execution = RuntimeRateTransitionExecutor::executeForTarget({
      request.run_id,
      request.object_id,
      request.branch_id,
      request.timeline_id,
      request.node_id,
      request.node,
      request.time_info,
      request.iteration_index,
      request.sample_buffer,
      request.port_store,
  });

  result.input_alignment =
      RuntimeInputAlignment::alignNodeInputs(request.node, request.time_info, *request.sample_buffer);
  RuntimeInputAlignment::applyAlignedInputs(result.input_alignment, result.upstream);
  return result;
}

}  // namespace FlightEnvPlatformRuntime
