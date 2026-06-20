/**
 * @file main.cpp
 * @brief 实现 platform-runtime 命令行程序入口。
 *
 * 大概：这是把平台运行宿主包装成可从命令行启动的 exe。
 * 具体：它解析参数、加载对象包/profile/workflow，调用 PlatformRuntimeHost 或 NativeWorkflowRunner 执行。
 * 被谁使用：被开发者本地调试、脚本、CI smoke 和端到端联调使用。
 * 使用谁：使用 PlatformRuntimeHost、NativeWorkflowRunner、文件系统和 JSON 配置。
 * 拆分判断：当前职责能一句话说清，暂不需要拆；如果继续加入新的运行行为，应另建文件承接。
 */

#include "FlightEnvPlatformRuntime/PlatformRuntimeHost.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  return FlightEnvPlatformRuntime::RunPlatformRuntimeHostCli(argc, argv);
}

