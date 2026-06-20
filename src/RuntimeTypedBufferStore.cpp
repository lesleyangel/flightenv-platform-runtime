/**
 * @file RuntimeTypedBufferStore.cpp
 * @brief 实现 runtime-owned typed buffer 分配器。
 */

#include "FlightEnvPlatformRuntime/RuntimeTypedBufferStore.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace FlightEnvPlatformRuntime {
namespace {

std::uint64_t fnv64(const std::string& text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string hex64(std::uint64_t value) {
  std::ostringstream oss;
  oss << std::hex << std::setw(16) << std::setfill('0') << value;
  return oss.str();
}

std::string nowUtcIso() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const std::time_t time = clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

std::string pathString(const fs::path& path) {
  return path.lexically_normal().string();
}

const char* dtypeName(flightenv::platform::AdapterScalarDType dtype) {
  using flightenv::platform::AdapterScalarDType;
  switch (dtype) {
    case AdapterScalarDType::Float64:
      return "float64";
    case AdapterScalarDType::Float32:
      return "float32";
    case AdapterScalarDType::Int64:
      return "int64";
    case AdapterScalarDType::Int32:
      return "int32";
    case AdapterScalarDType::UInt8:
      return "uint8";
    case AdapterScalarDType::Unknown:
    default:
      return "unknown";
  }
}

std::uint64_t dtypeSizeBytes(flightenv::platform::AdapterScalarDType dtype) {
  using flightenv::platform::AdapterScalarDType;
  switch (dtype) {
    case AdapterScalarDType::Float64:
    case AdapterScalarDType::Int64:
      return 8;
    case AdapterScalarDType::Float32:
    case AdapterScalarDType::Int32:
      return 4;
    case AdapterScalarDType::UInt8:
      return 1;
    case AdapterScalarDType::Unknown:
    default:
      return 0;
  }
}

nlohmann::json shapeJson(const std::array<std::uint64_t, 8>& shape, std::uint32_t rank) {
  nlohmann::json result = nlohmann::json::array();
  const std::uint32_t clamped_rank = std::min<std::uint32_t>(rank, 8u);
  for (std::uint32_t i = 0; i < clamped_rank; ++i) {
    result.push_back(shape[i]);
  }
  return result;
}

std::uint64_t elementCount(const std::array<std::uint64_t, 8>& shape, std::uint32_t rank) {
  const std::uint32_t clamped_rank = std::min<std::uint32_t>(rank, 8u);
  if (clamped_rank == 0) {
    return 0;
  }
  std::uint64_t count = 1;
  for (std::uint32_t i = 0; i < clamped_rank; ++i) {
    if (shape[i] == 0) {
      return 0;
    }
    count *= shape[i];
  }
  return count;
}

bool typedBufferTraceEnabled() {
  const char* value = std::getenv("FLIGHTENV_TYPED_BUFFER_TRACE");
  if (!value) {
    return false;
  }
  const std::string text(value);
  return !text.empty() && text != "0" && text != "false" && text != "FALSE";
}

std::string normalizedModeText(std::string value) {
  value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
                return std::isspace(c) != 0;
              }),
              value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string envString(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

void writeBinary(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  if (!path.parent_path().empty()) {
    fs::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Cannot write typed buffer shadow artifact: " + pathString(path));
  }
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
}

bool asyncShadowWriteEnabled(const RuntimeTypedBufferRequest& request) {
  const char* value = std::getenv("FLIGHTENV_TYPED_BUFFER_SHADOW_SYNC");
  if (value) {
    const std::string text(value);
    if (!text.empty() && text != "0" && text != "false" && text != "FALSE") {
      return false;
    }
  }
  return request.async_shadow_artifact;
}

void appendStoreTrace(const fs::path& run_dir, const std::string& message) {
  if (run_dir.empty() || !typedBufferTraceEnabled()) {
    return;
  }
  std::error_code ec;
  fs::create_directories(run_dir, ec);
  std::ofstream out(run_dir / "typed_buffer_store_trace.log", std::ios::app);
  if (!out) {
    return;
  }
  out << nowUtcIso() << ' ' << message << '\n';
}

bool acquireStoreLock(std::atomic_flag& lock) {
  for (int attempt = 0; attempt < 250; ++attempt) {
    if (!lock.test_and_set(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

void releaseStoreLock(std::atomic_flag& lock) {
  lock.clear(std::memory_order_release);
}

class StoreLockGuard {
 public:
  explicit StoreLockGuard(std::atomic_flag& lock) : lock_(&lock), locked_(acquireStoreLock(lock)) {}
  ~StoreLockGuard() {
    if (locked_ && lock_) {
      releaseStoreLock(*lock_);
    }
  }

  StoreLockGuard(const StoreLockGuard&) = delete;
  StoreLockGuard& operator=(const StoreLockGuard&) = delete;

  bool locked() const { return locked_; }

 private:
  std::atomic_flag* lock_ = nullptr;
  bool locked_ = false;
};

flightenv::platform::AdapterAbiStatus allocateIntoView(
    RuntimeTypedBufferStore& store,
    const fs::path& run_dir,
    RuntimeTypedBufferPersistenceMode persistence_mode,
    const flightenv::platform::AdapterTypedBufferView* request,
    flightenv::platform::AdapterTypedBufferView* allocated) {
  if (!request || !allocated || request->byte_size == 0) {
    return flightenv::platform::AdapterAbiStatus::FatalError;
  }
  RuntimeTypedBufferRequest req;
  req.run_dir = run_dir;
  req.node_id = request->node_id ? request->node_id : "adapter";
  req.port_id = request->port_id ? request->port_id : "output";
  req.schema_id = request->schema_id ? request->schema_id : "";
  req.dto_name = request->dto_name ? request->dto_name : "";
  req.layout_id = request->layout_id ? request->layout_id : "";
  req.format = request->format ? request->format : "fe_typed_dto_binary.v1";
  req.persistence_mode = persistence_mode;
  req.write_shadow_artifact = runtimeTypedBufferPersistenceWritesShadow(persistence_mode);
  req.dtype = request->dtype;
  req.rank = std::min<std::uint32_t>(request->rank, 8u);
  for (std::uint32_t i = 0; i < req.rank; ++i) {
    req.shape[i] = request->shape[i];
  }
  if (req.rank == 0) {
    req.rank = 1;
    req.shape[0] = request->byte_size;
    req.dtype = flightenv::platform::AdapterScalarDType::UInt8;
  }
  req.flags = request->flags | flightenv::platform::AdapterTypedBufferFlagRuntimeOwned;
  req.bytes.assign(static_cast<std::size_t>(request->byte_size), 0);
  const RuntimeTypedBufferAllocation allocation = store.allocate(std::move(req));
  if (!allocation.retained_in_store) {
    return flightenv::platform::AdapterAbiStatus::FatalError;
  }
  thread_local std::string allocated_buffer_id;
  allocated_buffer_id = allocation.buffer_id;
  *allocated = *request;
  allocated->buffer_id = allocated_buffer_id.c_str();
  allocated->node_id = request->node_id;
  allocated->port_id = request->port_id;
  allocated->data = allocation.bytes && !allocation.bytes->empty() ? allocation.bytes->data() : nullptr;
  allocated->byte_size = allocation.bytes ? static_cast<std::uint64_t>(allocation.bytes->size()) : 0;
  allocated->flags =
      (request->flags | flightenv::platform::AdapterTypedBufferFlagRuntimeOwned) &
      ~flightenv::platform::AdapterTypedBufferFlagAdapterOwned;
  return flightenv::platform::AdapterAbiStatus::Ok;
}

flightenv::platform::AdapterAbiStatus allocateCallback(
    void* allocator_user,
    const flightenv::platform::AdapterTypedBufferView* request,
    flightenv::platform::AdapterTypedBufferView* allocated) {
  if (!allocator_user) {
    return flightenv::platform::AdapterAbiStatus::FatalError;
  }
  auto* store = static_cast<RuntimeTypedBufferStore*>(allocator_user);
  return allocateIntoView(
      *store,
      fs::path{},
      RuntimeTypedBufferPersistenceMode::ShadowArtifact,
      request,
      allocated);
}

flightenv::platform::AdapterAbiStatus allocateCallbackWithContext(
    void* allocator_user,
    const flightenv::platform::AdapterTypedBufferView* request,
    flightenv::platform::AdapterTypedBufferView* allocated) {
  if (!allocator_user) {
    return flightenv::platform::AdapterAbiStatus::FatalError;
  }
  auto* context = static_cast<RuntimeTypedBufferAllocatorContext*>(allocator_user);
  if (!context->store) {
    return flightenv::platform::AdapterAbiStatus::FatalError;
  }
  return allocateIntoView(
      *context->store,
      context->run_dir,
      context->persistence_mode,
      request,
      allocated);
}

flightenv::platform::AdapterAbiStatus releaseCallback(void* allocator_user, const char* buffer_id) {
  if (!allocator_user || !buffer_id) {
    return flightenv::platform::AdapterAbiStatus::FatalError;
  }
  auto* store = static_cast<RuntimeTypedBufferStore*>(allocator_user);
  store->release(buffer_id);
  return flightenv::platform::AdapterAbiStatus::Ok;
}

flightenv::platform::AdapterAbiStatus releaseCallbackWithContext(void* allocator_user, const char* buffer_id) {
  if (!allocator_user || !buffer_id) {
    return flightenv::platform::AdapterAbiStatus::FatalError;
  }
  auto* context = static_cast<RuntimeTypedBufferAllocatorContext*>(allocator_user);
  if (!context->store) {
    return flightenv::platform::AdapterAbiStatus::FatalError;
  }
  context->store->release(buffer_id);
  return flightenv::platform::AdapterAbiStatus::Ok;
}

}  // namespace

RuntimeTypedBufferStore& RuntimeTypedBufferStore::instance() {
  static RuntimeTypedBufferStore store;
  return store;
}

RuntimeTypedBufferPersistenceMode parseRuntimeTypedBufferPersistence(
    const std::string& value) {
  const std::string mode = normalizedModeText(value.empty() ? "shadow_artifact" : value);
  if (mode == "shadow_artifact" || mode == "shadow" || mode == "artifact" ||
      mode == "persist" || mode == "debug" || mode == "replay") {
    return RuntimeTypedBufferPersistenceMode::ShadowArtifact;
  }
  if (mode == "memory_only" || mode == "memory" || mode == "loaned" ||
      mode == "none" || mode == "off") {
    return RuntimeTypedBufferPersistenceMode::MemoryOnly;
  }
  throw std::runtime_error(
      "Unsupported runtime typed buffer persistence: " + value +
      " (expected shadow_artifact or memory_only)");
}

std::string runtimeTypedBufferPersistenceName(
    RuntimeTypedBufferPersistenceMode mode) {
  switch (mode) {
    case RuntimeTypedBufferPersistenceMode::ShadowArtifact:
      return "shadow_artifact";
    case RuntimeTypedBufferPersistenceMode::MemoryOnly:
      return "memory_only";
  }
  return "shadow_artifact";
}

RuntimeTypedBufferPersistenceMode resolveRuntimeTypedBufferPersistence(
    const std::string& requested_mode) {
  const std::string env_mode = envString("FLIGHTENV_TYPED_BUFFER_PERSISTENCE");
  if (!env_mode.empty()) {
    return parseRuntimeTypedBufferPersistence(env_mode);
  }
  return parseRuntimeTypedBufferPersistence(
      requested_mode.empty() ? "shadow_artifact" : requested_mode);
}

bool runtimeTypedBufferPersistenceWritesShadow(
    RuntimeTypedBufferPersistenceMode mode) {
  return mode == RuntimeTypedBufferPersistenceMode::ShadowArtifact;
}

RuntimeTypedBufferAllocation RuntimeTypedBufferStore::allocate(RuntimeTypedBufferRequest request) {
  appendStoreTrace(request.run_dir,
                   "allocate_enter node=" + request.node_id + " port=" + request.port_id +
                       " bytes=" + std::to_string(request.bytes.size()));
  if (request.bytes.empty()) {
    throw std::runtime_error("typed buffer allocation requires non-empty bytes");
  }
  const std::string seed = request.node_id + "|" + request.port_id + "|" + request.schema_id + "|" +
                           request.dto_name + "|" + std::to_string(request.bytes.size()) + "|" + nowUtcIso();
  RuntimeTypedBufferAllocation allocation;
  allocation.buffer_id = "rtb_" + hex64(fnv64(seed));
  allocation.bytes = std::make_shared<std::vector<std::uint8_t>>(std::move(request.bytes));
  allocation.persistence_mode = request.persistence_mode;
  allocation.dtype = request.dtype;
  allocation.rank = std::min<std::uint32_t>(request.rank, 8u);
  allocation.shape = request.shape;
  if (allocation.rank == 0 || elementCount(allocation.shape, allocation.rank) == 0) {
    allocation.rank = 1;
    allocation.shape[0] = static_cast<std::uint64_t>(allocation.bytes->size());
    allocation.dtype = flightenv::platform::AdapterScalarDType::UInt8;
  }
  allocation.flags = request.flags | flightenv::platform::AdapterTypedBufferFlagRuntimeOwned;
  allocation.runtime_owned =
      (allocation.flags & flightenv::platform::AdapterTypedBufferFlagRuntimeOwned) != 0;
  allocation.read_only =
      (allocation.flags & flightenv::platform::AdapterTypedBufferFlagReadOnly) != 0;
  allocation.logical_ref_count = 1;
  allocation.released = false;
  const bool write_shadow =
      request.write_shadow_artifact &&
      runtimeTypedBufferPersistenceWritesShadow(request.persistence_mode);
  allocation.shadow_write_status = write_shadow ? "pending" : "not_requested";
  appendStoreTrace(request.run_dir, "bytes_owned buffer_id=" + allocation.buffer_id);

  fs::path shadow_path;
  if (write_shadow && !request.run_dir.empty()) {
    const std::string port_key = "p_" + hex64(fnv64(request.port_id));
    shadow_path = request.run_dir / "tb" / (allocation.buffer_id + "." + port_key + ".bin");
    allocation.shadow_path = shadow_path;
    allocation.shadow_write_pending = asyncShadowWriteEnabled(request);
  }

  appendStoreTrace(request.run_dir, "build_ref_begin buffer_id=" + allocation.buffer_id);
  const std::uint64_t element_count = elementCount(allocation.shape, allocation.rank);
  const std::uint64_t dtype_size = dtypeSizeBytes(allocation.dtype);
  allocation.ref = {
      {"buffer_id", allocation.buffer_id},
      {"uri", "runtime://typed-buffer/" + allocation.buffer_id},
      {"path", shadow_path.empty() ? "" : pathString(shadow_path.lexically_relative(request.run_dir))},
      {"format", request.format},
      {"schema_id", request.schema_id},
      {"dto_name", request.dto_name},
      {"layout_id", request.layout_id.empty() ? request.schema_id : request.layout_id},
      {"byte_size", allocation.bytes->size()},
      {"dtype", dtypeName(allocation.dtype)},
      {"dtype_size_bytes", dtype_size},
      {"rank", allocation.rank},
      {"shape", shapeJson(allocation.shape, allocation.rank)},
      {"element_count", element_count},
      {"zero_copy_eligible", true},
      {"persistence", runtimeTypedBufferPersistenceName(allocation.persistence_mode)},
      {"storage", "runtime_owned_memory"},
      {"shadow_artifact", !shadow_path.empty()},
      {"shadow_write_status", allocation.shadow_write_status},
      {"shadow_write_pending", allocation.shadow_write_pending},
      {"shadow_write_count", allocation.shadow_write_count},
      {"node_id", request.node_id},
      {"port_id", request.port_id},
      {"flags", allocation.flags},
      {"access", allocation.read_only ? "read_only" : "read_write"},
      {"ownership", allocation.runtime_owned ? "runtime_owned" : "external"},
      {"logical_ref_count", allocation.logical_ref_count},
      {"lifecycle_state", "retained"},
      {"released", allocation.released},
      {"created_at_utc", nowUtcIso()},
  };
  appendStoreTrace(request.run_dir, "build_ref_end buffer_id=" + allocation.buffer_id);

  {
    appendStoreTrace(request.run_dir, "store_lock_begin buffer_id=" + allocation.buffer_id);
    StoreLockGuard lock(lock_);
    if (lock.locked()) {
      appendStoreTrace(request.run_dir, "store_lock_acquired buffer_id=" + allocation.buffer_id);
      allocation.retained_in_store = true;
      allocation.ref["zero_copy_eligible"] = true;
      allocation.ref["storage"] = "runtime_owned_memory";
      buffers_[allocation.buffer_id] = allocation;
      appendStoreTrace(request.run_dir, "store_insert_end buffer_id=" + allocation.buffer_id);
    } else {
      allocation.ref["zero_copy_eligible"] = false;
      allocation.ref["storage"] = shadow_path.empty() ? "runtime_transient_memory" : "shadow_artifact_only";
      allocation.ref["store_warning"] = "runtime typed buffer store lock was unavailable; memory retention skipped";
      appendStoreTrace(request.run_dir, "store_lock_timeout buffer_id=" + allocation.buffer_id);
    }
  }
  if (write_shadow && !shadow_path.empty()) {
    if (asyncShadowWriteEnabled(request)) {
      const std::string buffer_id = allocation.buffer_id;
      const fs::path run_dir = request.run_dir;
      const std::shared_ptr<std::vector<std::uint8_t>> bytes = allocation.bytes;
      appendStoreTrace(request.run_dir, "write_shadow_async_schedule path=" + pathString(shadow_path));
      std::thread([this, buffer_id, shadow_path, bytes, run_dir]() {
        try {
          writeBinary(shadow_path, bytes ? *bytes : std::vector<std::uint8_t>{});
          updateShadowWriteStatus(buffer_id, true, "");
          appendStoreTrace(run_dir, "write_shadow_async_end path=" + pathString(shadow_path));
        } catch (const std::exception& e) {
          updateShadowWriteStatus(buffer_id, false, e.what());
          appendStoreTrace(run_dir, std::string("write_shadow_async_failed reason=") + e.what());
        }
      }).detach();
    } else {
      appendStoreTrace(request.run_dir, "write_shadow_begin path=" + pathString(shadow_path));
      try {
        writeBinary(shadow_path, *allocation.bytes);
        updateShadowWriteStatus(allocation.buffer_id, true, "");
      } catch (...) {
        updateShadowWriteStatus(allocation.buffer_id, false, "sync shadow write failed");
        throw;
      }
      appendStoreTrace(request.run_dir, "write_shadow_end path=" + pathString(shadow_path));
    }
  }
  appendStoreTrace(request.run_dir, "allocate_return buffer_id=" + allocation.buffer_id);
  return allocation;
}

bool RuntimeTypedBufferStore::release(const std::string& buffer_id) {
  StoreLockGuard lock(lock_);
  if (!lock.locked()) {
    return false;
  }
  auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return false;
  }
  RuntimeTypedBufferAllocation& allocation = it->second;
  if (allocation.logical_ref_count > 0) {
    --allocation.logical_ref_count;
  }
  allocation.released = allocation.logical_ref_count == 0;
  allocation.ref["released"] = allocation.released;
  allocation.ref["logical_ref_count"] = allocation.logical_ref_count;
  allocation.ref["lifecycle_state"] = allocation.logical_ref_count == 0 ? "released" : "retained";
  return true;
}

RuntimeTypedBufferAllocation RuntimeTypedBufferStore::find(const std::string& buffer_id) const {
  if (buffer_id.empty()) {
    return {};
  }
  StoreLockGuard lock(lock_);
  if (!lock.locked()) {
    return {};
  }
  const auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return {};
  }
  return it->second;
}

RuntimeTypedBufferAllocation RuntimeTypedBufferStore::retain(const std::string& buffer_id) {
  if (buffer_id.empty()) {
    return {};
  }
  StoreLockGuard lock(lock_);
  if (!lock.locked()) {
    return {};
  }
  auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return {};
  }
  RuntimeTypedBufferAllocation& allocation = it->second;
  ++allocation.logical_ref_count;
  allocation.ref["logical_ref_count"] = allocation.logical_ref_count;
  allocation.ref["lifecycle_state"] = "retained";
  return allocation;
}

nlohmann::json RuntimeTypedBufferStore::refForBuffer(const std::string& buffer_id) const {
  const RuntimeTypedBufferAllocation allocation = find(buffer_id);
  if (allocation.ref.is_object()) {
    return allocation.ref;
  }
  return nlohmann::json::object();
}

bool RuntimeTypedBufferStore::refreshShadowArtifact(const std::string& buffer_id) {
  RuntimeTypedBufferAllocation allocation;
  {
    StoreLockGuard lock(lock_);
    if (!lock.locked()) {
      return false;
    }
    const auto it = buffers_.find(buffer_id);
    if (it == buffers_.end()) {
      return false;
    }
    it->second.shadow_write_pending = true;
    it->second.shadow_write_status = "pending";
    it->second.ref["shadow_write_pending"] = true;
    it->second.ref["shadow_write_status"] = "pending";
    allocation = it->second;
  }
  if (allocation.shadow_path.empty() || !allocation.bytes) {
    return false;
  }
  const std::string buffer_id_copy = buffer_id;
  const fs::path shadow_path = allocation.shadow_path;
  const std::shared_ptr<std::vector<std::uint8_t>> bytes = allocation.bytes;
  std::thread([this, buffer_id_copy, shadow_path, bytes]() {
    try {
      writeBinary(shadow_path, bytes ? *bytes : std::vector<std::uint8_t>{});
      updateShadowWriteStatus(buffer_id_copy, true, "");
    } catch (const std::exception& e) {
      updateShadowWriteStatus(buffer_id_copy, false, e.what());
    }
  }).detach();
  return true;
}

void RuntimeTypedBufferStore::updateShadowWriteStatus(
    const std::string& buffer_id,
    bool ok,
    const std::string& error) {
  StoreLockGuard lock(lock_);
  if (!lock.locked()) {
    return;
  }
  auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return;
  }
  RuntimeTypedBufferAllocation& allocation = it->second;
  allocation.shadow_write_pending = false;
  allocation.shadow_write_status = ok ? "complete" : "failed";
  allocation.shadow_write_error = error;
  if (ok) {
    ++allocation.shadow_write_count;
    allocation.flags |= flightenv::platform::AdapterTypedBufferFlagHasShadowArtifact;
  }
  allocation.ref["shadow_write_pending"] = allocation.shadow_write_pending;
  allocation.ref["shadow_write_status"] = allocation.shadow_write_status;
  allocation.ref["shadow_write_count"] = allocation.shadow_write_count;
  allocation.ref["shadow_error"] = allocation.shadow_write_error;
  allocation.ref["flags"] = allocation.flags;
  allocation.ref["updated_at_utc"] = nowUtcIso();
}

nlohmann::json RuntimeTypedBufferStore::summary() const {
  StoreLockGuard lock(lock_);
  if (!lock.locked()) {
    return {
        {"schema_version", "flightenv.platform.runtime_typed_buffer_store.v1"},
        {"buffer_count", nullptr},
        {"total_bytes", nullptr},
        {"warning", "runtime typed buffer store lock was unavailable"},
    };
  }
  std::uint64_t total_bytes = 0;
  std::uint64_t released_count = 0;
  std::uint64_t runtime_owned_count = 0;
  std::uint64_t read_only_count = 0;
  std::uint64_t total_logical_ref_count = 0;
  std::uint64_t max_logical_ref_count = 0;
  std::uint64_t extra_logical_ref_count = 0;
  std::uint64_t shadow_artifact_count = 0;
  std::uint64_t memory_only_count = 0;
  std::uint64_t pending_shadow_count = 0;
  for (const auto& item : buffers_) {
    if (item.second.bytes) {
      total_bytes += static_cast<std::uint64_t>(item.second.bytes->size());
    }
    if (item.second.released) {
      ++released_count;
    }
    if (item.second.runtime_owned) {
      ++runtime_owned_count;
    }
    if (item.second.read_only) {
      ++read_only_count;
    }
    if (!item.second.shadow_path.empty()) {
      ++shadow_artifact_count;
    }
    if (item.second.persistence_mode == RuntimeTypedBufferPersistenceMode::MemoryOnly) {
      ++memory_only_count;
    }
    if (item.second.shadow_write_pending) {
      ++pending_shadow_count;
    }
    total_logical_ref_count += item.second.logical_ref_count;
    max_logical_ref_count = std::max(max_logical_ref_count, item.second.logical_ref_count);
    if (item.second.logical_ref_count > 1) {
      extra_logical_ref_count += item.second.logical_ref_count - 1;
    }
  }
  const auto result = nlohmann::json{
      {"schema_version", "flightenv.platform.runtime_typed_buffer_store.v1"},
      {"buffer_count", buffers_.size()},
      {"total_bytes", total_bytes},
      {"released_count", released_count},
      {"runtime_owned_count", runtime_owned_count},
      {"read_only_count", read_only_count},
      {"shadow_artifact_count", shadow_artifact_count},
      {"memory_only_count", memory_only_count},
      {"pending_shadow_count", pending_shadow_count},
      {"total_logical_ref_count", total_logical_ref_count},
      {"max_logical_ref_count", max_logical_ref_count},
      {"extra_logical_ref_count", extra_logical_ref_count},
  };
  return result;
}

flightenv::platform::AdapterTypedBufferAllocatorV2 makeRuntimeTypedBufferAllocator(
    RuntimeTypedBufferStore& store) {
  flightenv::platform::AdapterTypedBufferAllocatorV2 allocator;
  allocator.user = &store;
  allocator.allocate = &allocateCallback;
  allocator.release = &releaseCallback;
  return allocator;
}

flightenv::platform::AdapterTypedBufferAllocatorV2 makeRuntimeTypedBufferAllocator(
    RuntimeTypedBufferAllocatorContext& context) {
  flightenv::platform::AdapterTypedBufferAllocatorV2 allocator;
  allocator.user = &context;
  allocator.allocate = &allocateCallbackWithContext;
  allocator.release = &releaseCallbackWithContext;
  return allocator;
}

}  // namespace FlightEnvPlatformRuntime
