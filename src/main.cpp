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

