#pragma once

/**
 * @file RuntimeEvidenceWriter.hpp
 * @brief 平台 runtime evidence 文件写入工具。
 *
 * RuntimeEvidenceWriter 只关心“把 JSON evidence 稳定写到 run package 的哪个文件”。
 * 业务含义由调用方提供；写入、目录创建、临时文件替换和时间戳由这里统一维护。
 */

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

/**
 * @brief run package 范围内的 evidence writer。
 */
class RuntimeEvidenceWriter {
 public:
  explicit RuntimeEvidenceWriter(std::filesystem::path run_dir);

  const std::filesystem::path& runDir() const;

  void writeJson(const std::filesystem::path& relative_path, const nlohmann::json& value) const;

  nlohmann::json envelope(
      const std::string& schema_version,
      const std::string& run_id,
      const std::string& workflow_id,
      const std::string& object_id,
      nlohmann::json payload = nlohmann::json::object()) const;

 private:
  std::filesystem::path run_dir_;
};

}  // namespace FlightEnvPlatformRuntime
