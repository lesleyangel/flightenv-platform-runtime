#include "FlightEnvPlatformRuntime/RuntimeEvidenceWriter.hpp"

#include "FlightEnvPlatformRuntime/RuntimeClock.hpp"

#include <fstream>
#include <stdexcept>

namespace FlightEnvPlatformRuntime {

RuntimeEvidenceWriter::RuntimeEvidenceWriter(std::filesystem::path run_dir)
    : run_dir_(std::move(run_dir)) {}

const std::filesystem::path& RuntimeEvidenceWriter::runDir() const {
  return run_dir_;
}

void RuntimeEvidenceWriter::writeJson(
    const std::filesystem::path& relative_path,
    const nlohmann::json& value) const {
  const std::filesystem::path out = run_dir_ / relative_path;
  std::filesystem::create_directories(out.parent_path());
  const std::filesystem::path tmp = out.string() + ".tmp";
  {
    std::ofstream file(tmp, std::ios::binary);
    if (!file) {
      throw std::runtime_error("Failed to open JSON tmp for write: " + tmp.string());
    }
    file << value.dump(2);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, out, ec);
  if (ec) {
    std::filesystem::remove(out, ec);
    ec.clear();
    std::filesystem::rename(tmp, out, ec);
  }
  if (ec) {
    throw std::runtime_error("Failed to replace JSON file: " + out.string() + " error=" + ec.message());
  }
}

nlohmann::json RuntimeEvidenceWriter::envelope(
    const std::string& schema_version,
    const std::string& run_id,
    const std::string& workflow_id,
    const std::string& object_id,
    nlohmann::json payload) const {
  if (!payload.is_object()) {
    payload = nlohmann::json::object({{"payload", payload}});
  }
  payload["schema_version"] = schema_version;
  payload["run_id"] = run_id;
  payload["workflow_id"] = workflow_id;
  payload["object_id"] = object_id;
  payload["generated_at_utc"] = RuntimeClock::nowUtcIso();
  return payload;
}

}  // namespace FlightEnvPlatformRuntime
