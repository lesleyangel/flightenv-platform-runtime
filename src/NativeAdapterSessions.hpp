#pragma once

#include "FlightEnvPlatformRuntime/AdapterSession.hpp"
#include "FlightEnvPlatformRuntime/NativeWorkflowRunner.hpp"

#include <filesystem>
#include <memory>

namespace FlightEnvPlatformRuntime {

std::unique_ptr<IAdapterSession> createNativeAdapterSession(
    const nlohmann::json& node,
    const nlohmann::json& operator_snapshot,
    const nlohmann::json& model_binding,
    const nlohmann::json& registry_entry,
    const NativeWorkflowOptions& options,
    const std::filesystem::path& trace_dir);

bool adapterSessionPrepared(const IAdapterSession& session);

void setAdapterSessionPrepared(IAdapterSession& session, bool value);

}  // namespace FlightEnvPlatformRuntime
