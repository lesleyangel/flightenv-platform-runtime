#pragma once

/**
 * @file RuntimeNodeEvidenceBuilder.hpp
 * @brief 构造单个运行时节点执行完成后的平台证据片段。
 *
 * 大概：NativeWorkflowRunner 负责决定节点何时运行；RuntimeAdapterInvoker 负责调用
 * adapter；本组件只负责把一次节点执行整理成 input/output artifact、node snapshot、
 * uncertainty evidence 和 checkpoint evidence。它不解释对象语义，也不写文件。
 *
 * 被谁使用：NativeWorkflowRunner。
 * 使用谁：nlohmann::json 和 Runtime 统一的节点、时间、packet 摘要字段。
 */

#include <nlohmann/json.hpp>

#include <string>

namespace FlightEnvPlatformRuntime {

struct RuntimeNodeEvidenceRequest {
  std::string node_id;             ///< 平台节点 id。
  std::string operator_id;         ///< 节点绑定的 operator id。
  std::string adapter_id;          ///< 节点绑定的 adapter id。
  std::string execution_kind;      ///< 节点执行类型。
  std::string adapter_protocol;    ///< 当前 adapter session 协议。
  std::string execution_tag;       ///< 本次执行的稳定证据前缀。
  int loop_iteration_index = 0;    ///< 所属 runtime loop iteration。

  nlohmann::json time_info = nlohmann::json::object();            ///< Runtime 时间上下文。
  nlohmann::json adapter_snapshot = nlohmann::json::object();     ///< adapter snapshot 输出。
  nlohmann::json runtime_packet = nlohmann::json::object();       ///< 节点级 RuntimePacket 摘要。
  nlohmann::json runtime_port_packets = nlohmann::json::array();  ///< 端口级 RuntimePacket 摘要。
  nlohmann::json uncertainty_contract = nlohmann::json::object(); ///< 节点不确定性契约片段。
  nlohmann::json input_summary = nlohmann::json::object();        ///< 输入摘要。
  nlohmann::json output_summary = nlohmann::json::object();       ///< 输出摘要。

  std::string checkpoint_kind = "snapshot_only";                  ///< checkpoint 类型。
  std::string replay_mode = "record_replay";                      ///< replay 策略。
};

struct RuntimeNodeEvidenceResult {
  nlohmann::json input_artifact = nlohmann::json::object();       ///< 输入 artifact 摘要。
  nlohmann::json output_artifact = nlohmann::json::object();      ///< 输出 artifact 摘要。
  nlohmann::json node_snapshot = nlohmann::json::object();        ///< 节点执行快照。
  nlohmann::json uncertainty_node = nlohmann::json::object();     ///< 节点不确定性证据。
  nlohmann::json checkpoint = nlohmann::json::object();           ///< 节点 checkpoint 证据。
};

class RuntimeNodeEvidenceBuilder {
 public:
  static RuntimeNodeEvidenceResult build(const RuntimeNodeEvidenceRequest& request);

  static nlohmann::json artifactSummary(
      const std::string& node_id,
      int loop_iteration_index,
      const std::string& artifact_id,
      const std::string& kind,
      const nlohmann::json& summary);
};

}  // namespace FlightEnvPlatformRuntime
