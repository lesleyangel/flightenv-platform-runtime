#pragma once

/**
 * @file RuntimeTypedBufferStore.hpp
 * @brief Runtime 内部 typed buffer 分配与索引。
 *
 * 大概：这是 typed DTO/typed buffer 热路径的 runtime 侧内存池，不解释对象语义。
 * 具体：它为 compact typed 输出分配进程内二进制 buffer，生成可进入 data-plane
 * manifest 的 `typed_buffer_ref`，并可写出 shadow artifact 方便回放和调试。
 * 被谁使用：被 RuntimeTypedPayloadBridge 和后续 adapter ABI v2 allocator 使用。
 * 使用谁：使用 PDK AdapterAbi 的 typed buffer 结构、标准文件系统和 nlohmann::json。
 */

#include "FlightEnvPlatform/Adapter/AdapterAbi.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace FlightEnvPlatformRuntime {

enum class RuntimeTypedBufferPersistenceMode {
  ShadowArtifact,
  MemoryOnly,
};

RuntimeTypedBufferPersistenceMode parseRuntimeTypedBufferPersistence(
    const std::string& value);

std::string runtimeTypedBufferPersistenceName(
    RuntimeTypedBufferPersistenceMode mode);

RuntimeTypedBufferPersistenceMode resolveRuntimeTypedBufferPersistence(
    const std::string& requested_mode);

bool runtimeTypedBufferPersistenceWritesShadow(
    RuntimeTypedBufferPersistenceMode mode);

struct RuntimeTypedBufferAllocation {
  std::string buffer_id;
  std::shared_ptr<std::vector<std::uint8_t>> bytes;
  nlohmann::json ref = nlohmann::json::object();
  std::filesystem::path shadow_path;
  bool retained_in_store = false;
  flightenv::platform::AdapterScalarDType dtype = flightenv::platform::AdapterScalarDType::UInt8;
  std::uint32_t rank = 1;
  std::array<std::uint64_t, 8> shape = {};
  std::uint32_t flags = flightenv::platform::AdapterTypedBufferFlagRuntimeOwned;
  RuntimeTypedBufferPersistenceMode persistence_mode =
      RuntimeTypedBufferPersistenceMode::ShadowArtifact;
  std::uint64_t logical_ref_count = 1;
  std::uint64_t shadow_write_count = 0;
  bool released = false;
  bool read_only = false;
  bool runtime_owned = true;
  bool shadow_write_pending = false;
  std::string shadow_write_status;
  std::string shadow_write_error;
};

struct RuntimeTypedBufferRequest {
  std::filesystem::path run_dir;
  std::string node_id;
  std::string port_id;
  std::string schema_id;
  std::string dto_name;
  std::string layout_id;
  std::string format = "fe_typed_dto_binary.v1";
  flightenv::platform::AdapterScalarDType dtype = flightenv::platform::AdapterScalarDType::UInt8;
  std::uint32_t rank = 1;
  std::array<std::uint64_t, 8> shape = {};
  std::uint32_t flags = flightenv::platform::AdapterTypedBufferFlagRuntimeOwned;
  RuntimeTypedBufferPersistenceMode persistence_mode =
      RuntimeTypedBufferPersistenceMode::ShadowArtifact;
  std::vector<std::uint8_t> bytes;
  bool write_shadow_artifact = true;
  bool async_shadow_artifact = true;
};

class RuntimeTypedBufferStore;

struct RuntimeTypedBufferAllocatorContext {
  RuntimeTypedBufferStore* store = nullptr;
  std::filesystem::path run_dir;
  RuntimeTypedBufferPersistenceMode persistence_mode =
      RuntimeTypedBufferPersistenceMode::ShadowArtifact;
};

class RuntimeTypedBufferStore {
 public:
  static RuntimeTypedBufferStore& instance();

  RuntimeTypedBufferAllocation allocate(RuntimeTypedBufferRequest request);

  RuntimeTypedBufferAllocation find(const std::string& buffer_id) const;

  RuntimeTypedBufferAllocation retain(const std::string& buffer_id);

  nlohmann::json refForBuffer(const std::string& buffer_id) const;

  bool refreshShadowArtifact(const std::string& buffer_id);

  bool release(const std::string& buffer_id);

  nlohmann::json summary() const;

 private:
  RuntimeTypedBufferStore() = default;

  void updateShadowWriteStatus(
      const std::string& buffer_id,
      bool ok,
      const std::string& error);

  mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  std::unordered_map<std::string, RuntimeTypedBufferAllocation> buffers_;
};

flightenv::platform::AdapterTypedBufferAllocatorV2 makeRuntimeTypedBufferAllocator(
    RuntimeTypedBufferStore& store);

flightenv::platform::AdapterTypedBufferAllocatorV2 makeRuntimeTypedBufferAllocator(
    RuntimeTypedBufferAllocatorContext& context);

}  // namespace FlightEnvPlatformRuntime
