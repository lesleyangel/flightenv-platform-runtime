#pragma once

/**
 * @file AdapterSession.hpp
 * @brief 封装 runtime 中一个已加载适配器会话的生命周期。
 *
 * 大概：这是 platform-runtime 内部管理 adapter 句柄、初始化状态和关闭动作的 RAII 包装。
 * 具体：它把 PDK 的 ABI 函数表变成更容易安全调用的 C++ 会话对象。
 * 被谁使用：被 NativeWorkflowRunner 和 PlatformRuntimeHost 的 adapter 执行路径使用。
 * 使用谁：使用 PDK AdapterAbi、RuntimeContext 和标准资源管理类型。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */


#include <nlohmann/json.hpp>
#include <string>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 适配器会话在运行时证据中报告的稳定契约标识。
 *
 * 该值标识由宿主拥有的生命周期协议。后端可以通过 DLL、子进程、
 * 远端工作单元或记录型适配器实现该协议；只要使用下列方法顺序，
 * 证据中就应保留此标识。
 */
inline constexpr const char* kAdapterSessionContract =
    "flightenv.platform.runtime.adapter_session.v1";

/**
 * @brief 算子适配器会话由宿主拥有的生命周期边界。
 *
 * `IAdapterSession` 将适配器实现细节封装在 JSON 请求与响应包之后，使运行时
 * 能够独立拥有调度、证据、资源引用和生命周期顺序，而无需依赖具体后端。
 * 实现可以包装进程内库、子进程适配器、远端节点、模型会话或回放后端。
 *
 * @note 生命周期：运行时通常先调用 describe/resolve、initialize、warmup，
 * 然后调用一次或多次 execute，之后调用 snapshot/flush，最后 shutdown。
 * 实现应保证在部分初始化之后调用 shutdown 仍然安全。
 * @note 线程安全：除非具体实现声明了更强保证，调用方必须串行访问同一个会话。
 */
class IAdapterSession {
 public:
  /** @brief 释放会话实现拥有的后端资源。 */
  virtual ~IAdapterSession() = default;

  /**
   * @brief 返回证据中使用的适配器协议标识。
   * @return 稳定协议字符串，例如 DLL ABI、进程模式或回放模式 id。
   */
  virtual std::string protocol() const = 0;

  /**
   * @brief 汇总当前会话状态以写入运行时证据。
   * @return 返回 JSON 对象，包含后端身份、生命周期状态以及实现特有的非敏感诊断信息。
   */
  virtual nlohmann::json summary() const = 0;

  /**
   * @brief 停止会话并释放外部句柄。
   *
   * 实现应容忍重复调用，以及生命周期步骤失败后的调用；只有阻止可靠清理报告的
   * 失败才应作为错误暴露。
   */
  virtual void shutdown() = 0;

  /**
   * @brief 在资源解析前查询适配器能力。
   * @param context 由运行时拥有的执行上下文，包含节点和 run 标识。
   * @param payload 适配器特有的请求载荷。
   * @return 用于校验和证据记录的 JSON 能力描述。
   */
  virtual nlohmann::json describe(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;

  /**
   * @brief 解析声明的资源、模型绑定或数据面引用。
   * @param context 当前节点由运行时拥有的执行上下文。
   * @param payload 解析请求，通常来自编译后的工作流元数据。
   * @return 返回 JSON 解析证据；不得暴露隐藏的跨算子状态。
   */
  virtual nlohmann::json resolve(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;

  /**
   * @brief 为运行或节点会话初始化后端状态。
   * @param context 当前节点由运行时拥有的执行上下文。
   * @param payload 初始化包，包含已解析资源和运行路径。
   * @return 返回 JSON 初始化证据。
   */
  virtual nlohmann::json initialize(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;

  /**
   * @brief 执行可选的执行前准备。
   * @param context 当前节点由运行时拥有的执行上下文。
   * @param payload 预热请求；不需要预热的适配器可以接收空对象。
   * @return 返回 JSON 预热证据。
   */
  virtual nlohmann::json warmup(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;

  /**
   * @brief 为适配器执行一次运行时派发。
   * @param context 由运行时拥有的派发上下文，包含时间和节点元数据。
   * @param payload 由上游端口和运行时引用组装的输入包。
   * @return 返回 JSON 输出包。输出应使用已声明端口和引用，而不是隐藏全局状态或
   * 直接调用其他适配器。
   */
  virtual nlohmann::json execute(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;

  /**
   * @brief 在一次派发后捕获可回放的适配器状态。
   * @param context 由运行时拥有的派发上下文。
   * @param payload 快照请求，通常为空对象。
   * @return 适合写入证据和检查点记录的 JSON 快照。
   */
  virtual nlohmann::json snapshot(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;

  /**
   * @brief 刷新已缓冲输出或伴随制品。
   * @param context 由运行时拥有的派发上下文。
   * @param payload 刷新请求，通常为空对象。
   * @return 返回 JSON flush 证据，标识已持久化制品或待完成工作。
   */
  virtual nlohmann::json flush(
      const nlohmann::json& context,
      const nlohmann::json& payload) = 0;
};

}  // 命名空间 FlightEnvPlatformRuntime
