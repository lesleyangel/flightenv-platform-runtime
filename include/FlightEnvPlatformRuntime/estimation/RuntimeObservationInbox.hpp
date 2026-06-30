#pragma once

/**
 * @file RuntimeObservationInbox.hpp
 * @brief 系统级估计服务的观测输入缓冲。
 *
 * 大概：把外部观测流统一整理成 RuntimeEstimationService 可以消费的帧序列。
 * 具体：只抽取时间、帧号、原始 payload 和通用数值向量，不理解任何对象业务语义。
 * 被谁使用：RuntimeEstimationService。
 * 使用谁：nlohmann::json 和标准容器。
 */

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace FlightEnvPlatformRuntime::estimation {

struct RuntimeObservationFrame {
  int frame_index = 0;
  double sample_time_s = 0.0;
  nlohmann::json payload = nlohmann::json::object();
  nlohmann::json source_summary = nlohmann::json::object();
  std::vector<std::string> value_labels;
  std::vector<double> values;
};

class RuntimeObservationInbox {
 public:
  static RuntimeObservationInbox fromJsonStream(const nlohmann::json& stream, int max_frames);

  const std::vector<RuntimeObservationFrame>& frames() const;
  bool empty() const;
  std::size_t size() const;

 private:
  std::vector<RuntimeObservationFrame> frames_;
};

}  // namespace FlightEnvPlatformRuntime::estimation
