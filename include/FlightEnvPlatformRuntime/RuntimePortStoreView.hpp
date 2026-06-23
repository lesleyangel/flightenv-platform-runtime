#pragma once

/**
 * @file RuntimePortStoreView.hpp
 * @brief 声明端口存储的只读 evidence 视图。
 *
 * 这个组件把 PDK 的 ThreadSafePortStore / RuntimePacket 转成平台运行时可读的
 * 端口级引用摘要。它被 RuntimePublicFrameBuilder 使用，用于解释 public_tick
 * 中 held/carry-forward 端口到底沿用了哪一个 packet/ref。它不解释端口值的对象语义。
 */

#include <nlohmann/json.hpp>

#include <string>

namespace flightenv::platform {
class ThreadSafePortStore;
}

namespace FlightEnvPlatformRuntime {

class RuntimePortStoreView {
 public:
  static nlohmann::json nodeOutputRefs(
      const flightenv::platform::ThreadSafePortStore& port_store,
      const std::string& node_id,
      const nlohmann::json& data_plane_info);
};

}  // namespace FlightEnvPlatformRuntime
