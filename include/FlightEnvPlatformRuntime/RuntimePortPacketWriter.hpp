#pragma once

/**
 * @file RuntimePortPacketWriter.hpp
 * @brief 将一次节点执行结果写成端口级 RuntimePacket。
 *
 * 大概：节点执行完成后，runtime 需要把每个输出端口的 typed buffer / tensor / artifact 引用放进
 * ThreadSafePortStore，供后续输入对齐、公开物化、evidence 和 replay 使用。
 * 具体：该组件只读取 execute_result、time_info 和 data-plane entries，逐个输出端口生成 packet。
 * 被谁使用：NativeWorkflowRunner。
 * 使用谁：PDK 的 ThreadSafePortStore / RuntimePacket 和平台通用 JSON data-plane 条目。
 *
 * 该组件不理解对象语义，也不复制大块载荷；它只记录端口契约、引用、时间和版本索引。
 */

#include <nlohmann/json.hpp>

#include <string>

namespace flightenv::platform {
class ThreadSafePortStore;
}

namespace FlightEnvPlatformRuntime {

struct RuntimePortPacketWriteRequest {
  std::string run_id;        ///< 当前 run id。
  std::string object_id;     ///< 当前对象 id；runtime 不解释其语义。
  std::string branch_id;     ///< 平台分支 id；用于 PortStore scoped 索引。
  std::string timeline_id;   ///< 平台时间线 id；用于 PortStore scoped 索引。
  std::string node_id;       ///< 生产输出的节点 id。
  nlohmann::json execute_result = nlohmann::json::object();      ///< adapter 执行结果。
  nlohmann::json time_info = nlohmann::json::object();           ///< runtime 时间上下文。
  nlohmann::json data_plane_entries = nlohmann::json::array();   ///< 本节点物化出的 data-plane 条目。
};

struct RuntimePortPacketWriteResult {
  nlohmann::json packets = nlohmann::json::array();          ///< 已写入 packet 的 JSON 摘要列表。
  nlohmann::json packet_by_port = nlohmann::json::object();  ///< 逻辑端口 id 到 packet 摘要的索引。
};

class RuntimePortPacketWriter {
 public:
  static RuntimePortPacketWriteResult writeOutputPortPackets(
      flightenv::platform::ThreadSafePortStore& port_store,
      const RuntimePortPacketWriteRequest& request);
};

}  // namespace FlightEnvPlatformRuntime
