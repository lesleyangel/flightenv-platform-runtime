#pragma once

/**
 * @file RuntimePortBindingResolver.hpp
 * @brief 解析节点执行前的端口输入绑定。
 *
 * 大概：Runtime 调度器决定节点到期后，本组件负责把“上一轮种子、外部输入、
 * 显式 edge binding、历史 PortStore 包、兼容性隐式绑定”合成为 adapter execute
 * 看到的 upstream。它只理解平台端口、契约、branch/time 索引，不理解对象物理语义。
 *
 * 被谁使用：NativeWorkflowRunner 的 node_due 热路径。
 * 使用谁：ThreadSafePortStore、RuntimePacket 摘要、compiled workflow/data-plane 的端口声明。
 */

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace flightenv::platform {
class ThreadSafePortStore;
}

namespace FlightEnvPlatformRuntime {

struct RuntimePortBindingResolveRequest {
  nlohmann::json node = nlohmann::json::object();             ///< 当前目标节点声明。
  nlohmann::json base_upstream = nlohmann::json::object();    ///< 进入绑定前已有的 upstream。
  nlohmann::json external_seed = nlohmann::json::object();    ///< 当前时刻外部输入种子。
  nlohmann::json current_outputs = nlohmann::json::object();  ///< 当前 run 中已完成节点输出。
  nlohmann::json data_plane_info = nlohmann::json::object();  ///< 目标节点 data-plane 输入输出声明。
  const flightenv::platform::ThreadSafePortStore* port_store = nullptr; ///< 历史端口包存储。
  std::string branch_id;                                      ///< scoped port store branch。
  std::string timeline_id;                                    ///< scoped port store timeline。
  std::int64_t event_time_ns = 0;                             ///< 当前 node_due 事件时间。
  bool allow_implicit_contract_port_binding = false;          ///< 兼容旧 workflow 的隐式端口注入。
  std::function<void(const std::string&)> trace;              ///< 可选 trace 回调。
};

struct RuntimePortBindingResolveResult {
  nlohmann::json upstream = nlohmann::json::object(); ///< adapter execute 使用的最终 upstream。
  nlohmann::json evidence = nlohmann::json::object(); ///< 绑定来源、数量和兼容路径摘要。
};

class RuntimePortBindingResolver {
 public:
  static RuntimePortBindingResolveResult resolve(const RuntimePortBindingResolveRequest& request);
};

}  // namespace FlightEnvPlatformRuntime
