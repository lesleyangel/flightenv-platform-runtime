#include "FlightEnvPlatformRuntime/estimation/SampleSetStore.hpp"

namespace FlightEnvPlatformRuntime::estimation {

namespace {

nlohmann::json vectorJson(const std::vector<double>& values) {
  nlohmann::json out = nlohmann::json::array();
  for (double value : values) {
    out.push_back(value);
  }
  return out;
}

}  // namespace

void SampleSetStore::append(PosteriorFrame frame) {
  frames_.push_back(std::move(frame));
}

const std::vector<PosteriorFrame>& SampleSetStore::frames() const {
  return frames_;
}

const PosteriorFrame* SampleSetStore::latest() const {
  if (frames_.empty()) {
    return nullptr;
  }
  return &frames_.back();
}

nlohmann::json SampleSetStore::toCheckpointJson(
    const std::string& run_id,
    const std::string& workflow_id,
    const std::string& object_id) const {
  nlohmann::json checkpoints = nlohmann::json::array();
  for (const PosteriorFrame& frame : frames_) {
    checkpoints.push_back({
        {"checkpoint_id", frame.checkpoint_id},
        {"commit_id", frame.commit_id},
        {"committed", frame.committed},
        {"commit_barrier", frame.committed ? "posterior_commit" : "uncommitted"},
        {"frame_index", frame.frame_index},
        {"sample_time_s", frame.sample_time_s},
        {"state_mean", vectorJson(frame.state_mean)},
        {"covariance_diag", vectorJson(frame.covariance_diag)},
        {"diagnostics", frame.diagnostics},
    });
  }
  return {
      {"schema_version", "flightenv.platform.state_checkpoint.v1"},
      {"run_id", run_id},
      {"workflow_id", workflow_id},
      {"object_id", object_id},
      {"checkpoints", checkpoints},
  };
}

}  // namespace FlightEnvPlatformRuntime::estimation
