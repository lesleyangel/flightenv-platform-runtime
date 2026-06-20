#pragma once

/**
 * @file RuntimeZeroCopyPolicy.hpp
 * @brief 定义 Runtime 热路径零拷贝合规规则。
 *
 * 大概：这里集中回答“某个算子端口是否还允许只返回 JSON”的问题。
 * 具体：workflow、初始化参数、资源绑定和 evidence 仍然可以使用 JSON；但声明了
 * `json_operator_io_forbidden` 的算子输入/输出端口，在执行热路径上必须携带
 * artifact/tensor/typed-buffer 类引用，不能只留下 inline JSON。
 * 被谁使用：NativeWorkflowRunner、RuntimeTypedPayloadBridge 和后续 typed ABI 审计工具。
 * 使用谁：只依赖 nlohmann::json 和少量字符串工具，保持平台中性，不理解对象物理语义。
 */

#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

enum class RuntimeZeroCopyMode {
  Off,
  Prefer,
  Require,
  Auto,
};

struct RuntimeZeroCopyExecuteDecision {
  RuntimeZeroCopyMode mode = RuntimeZeroCopyMode::Auto;
  bool in_process_session = false;
  bool typed_adapter_available = false;
  bool typed_output_required = false;
  bool typed_contract_present = false;
  bool input_buffer_ref_present = false;
  bool use_typed_execute = false;
  bool fail_fast = false;
  std::string reason;
  std::string message;
};

/**
 * @brief 输出端口零拷贝检查结果。
 */
struct RuntimeZeroCopyCheck {
  bool required = false;          ///< 端口是否声明了禁止 JSON hot path。
  bool has_reference = false;     ///< 载荷是否已经带 artifact/tensor/typed buffer 等引用。
  bool compliant = true;          ///< 当前载荷是否满足热路径要求。
  std::string reason;             ///< 不满足时的稳定原因码。
  std::string message;            ///< 面向日志和异常的说明文本。
};

/**
 * @brief 判断端口声明是否禁止算子 IO 只走 JSON。
 */
bool zeroCopyRequiredForPort(const nlohmann::json& port_spec);

/**
 * @brief 判断 payload 是否携带热路径可接受的数据引用。
 */
bool hasZeroCopyPayloadRef(const nlohmann::json& payload);

RuntimeZeroCopyMode parseRuntimeZeroCopyMode(const std::string& value);

std::string runtimeZeroCopyModeName(RuntimeZeroCopyMode mode);

RuntimeZeroCopyMode resolveRuntimeZeroCopyMode(const std::string& requested_mode);

bool dataPlaneHasTypedIoContract(const nlohmann::json& data_plane_info);

bool dataPlaneRequiresTypedExecute(const nlohmann::json& data_plane_info);

RuntimeZeroCopyExecuteDecision decideRuntimeZeroCopyExecute(
    const nlohmann::json& data_plane_info,
    const nlohmann::json& payload,
    const std::string& requested_mode,
    bool in_process_session,
    bool typed_adapter_available,
    const std::string& node_id,
    const std::string& adapter_id);

nlohmann::json runtimeZeroCopyExecuteDecisionToJson(
    const RuntimeZeroCopyExecuteDecision& decision);

/**
 * @brief 是否允许把 inline JSON 临时桥接为 `json_typed_payload.v1` typed buffer。
 *
 * 默认不允许。该开关只用于迁移诊断，不作为生产合规路径。
 */
bool allowInlineJsonTypedPayloadBridge();

/**
 * @brief 对单个输出端口执行零拷贝策略检查。
 */
RuntimeZeroCopyCheck checkOutputZeroCopyPolicy(
    const nlohmann::json& port_spec,
    const nlohmann::json& payload,
    const std::string& node_id,
    const std::string& port_id);

/**
 * @brief 检查所有声明为 typed-only 的输出端口。
 *
 * @throws std::runtime_error 任一 required 输出仍是 inline-only JSON 时抛出。
 */
void enforceOutputZeroCopyPolicy(
    const nlohmann::json& data_plane_info,
    const nlohmann::json& execute_result,
    const std::string& node_id);

}  // namespace FlightEnvPlatformRuntime
