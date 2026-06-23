#pragma once

/**
 * @file RuntimeClock.hpp
 * @brief 平台 runtime 的统一时钟工具。
 *
 * 这个文件只提供通用时间能力：生成 UTC 证据时间戳、读取单调时钟、做秒/纳秒转换。
 * 它不理解对象语义，也不决定任何节点是否应该执行；调度策略仍由时间调度器和事件循环承担。
 */

#include <cstdint>
#include <string>

namespace FlightEnvPlatformRuntime {

/**
 * @brief 统一封装 runtime 使用的墙钟和单调时钟。
 */
class RuntimeClock {
 public:
  /**
   * @brief 返回 ISO-8601 UTC 时间戳，用于 evidence、事件和日志。
   */
  static std::string nowUtcIso();

  /**
   * @brief 返回单调时钟纳秒值，用于耗时统计，不用于业务时间线。
   */
  static std::int64_t steadyNowNs();

  /**
   * @brief 将秒转换为纳秒。
   */
  static std::int64_t secondsToNanoseconds(double seconds);

  /**
   * @brief 将纳秒转换为秒。
   */
  static double nanosecondsToSeconds(std::int64_t nanoseconds);
};

}  // namespace FlightEnvPlatformRuntime
