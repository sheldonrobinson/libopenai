#pragma once

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <optional>
#include <chrono>
#include <atomic>

#include "libopenai/transport.hpp"
#include "libopenai/inference_provider.hpp"
#include "libopenai/provider_registry.hpp"
#include "libopenai/modality_router.hpp"

namespace libopenai {

/**
 * @brief RoutingPolicy - policy for device-cloud collaborative routing.
 *
 * Defines how the router decides between device and cloud execution.
 * See Konstrukt: Device-Cloud Collaborative ML Platform.
 */
struct RoutingPolicy {
    bool prefer_device = true;           // Prefer on-device when possible
    bool allow_cloud_fallback = true;    // Fall back to cloud if device fails
    bool allow_device_offload = true;    // Offload to cloud if device overloaded
    double max_device_load = 0.7;        // Max device load before offloading
    double max_cloud_latency_ms = 5000;  // Max acceptable cloud latency
    bool privacy_mode = false;           // Always use device if available
    bool battery_saver = false;          // Prefer cloud to save device battery
    bool offline_mode = false;           // Only use device providers
    bool require_quality = false;        // Prefer cloud for higher quality
};

/**
 * @brief RoutingDecision - the result of a routing decision.
 */
struct RoutingDecision {
    ProviderPtr provider;                // Selected provider
    ProviderTier tier;                   // Device or cloud
    std::string reason;                  // Human-readable reason for the decision
    bool fallback = false;               // Whether this was a fallback choice
    double confidence = 1.0;             // Confidence in the decision (0-1)
};

/**
 * @brief DeviceCloudRouter - implements device-cloud collaborative inferencing.
 *
 * This is the core of the Konstrukt pattern. It intelligently routes
 * inference requests between on-device providers (litert, ncnn,
 * cactus-engine, executorch, onnxruntime) and cloud providers
 * (rtp-llm, tensorrt-llm, vllm, sglang) based on:
 *
 *   - Privacy requirements (always device if privacy_mode)
 *   - Network availability (offline_mode = device only)
 *   - Device load (offload to cloud if overloaded)
 *   - Latency requirements (device for low latency, cloud for throughput)
 *   - Battery/power constraints (cloud to save battery)
 *   - Model availability (cloud for models not on device)
 *   - Cost considerations (device is free, cloud costs money)
 *
 * Supports cascading: try device first, fall back to cloud on failure.
 * Supports splitting: different modalities can go to different tiers.
 */
class DeviceCloudRouter {
public:
    explicit DeviceCloudRouter(std::shared_ptr<ProviderRegistry> registry,
                                RoutingPolicy policy = {});

    ~DeviceCloudRouter() = default;

    // ---- Routing ----

    // Make a routing decision for the given request.
    RoutingDecision Decide(const Request& req) const;

    // Execute the request using the selected provider.
    // Falls back to an alternate provider if the first fails.
    std::future<Response> ExecuteAsync(const Request& req) const;

    // Synchronous execution
    Response Execute(const Request& req) const {
        return ExecuteAsync(req).get();
    }

    // ---- Policy management ----

    void SetPolicy(const RoutingPolicy& policy);
    RoutingPolicy GetPolicy() const { return policy_; }

    // ---- Provider management ----

    void SetRegistry(std::shared_ptr<ProviderRegistry> registry);
    std::shared_ptr<ProviderRegistry> GetRegistry() const { return registry_; }

    // ---- Statistics ----

    size_t DeviceDecisions() const { return device_decisions_; }
    size_t CloudDecisions() const { return cloud_decisions_; }
    size_t FallbackDecisions() const { return fallback_decisions_; }

private:
    // Select a provider for a specific modality within a tier
    ProviderPtr SelectProviderForModality(Modality m,
                                           ProviderTier tier) const;

    // Check if device is overloaded based on load metrics
    bool IsDeviceOverloaded() const;

    // Check if network is available (simplified heuristic)
    bool IsNetworkAvailable() const;

    mutable std::mutex mutex_;
    std::shared_ptr<ProviderRegistry> registry_;
    RoutingPolicy policy_;

    // Decision counters
    mutable std::atomic<size_t> device_decisions_{0};
    mutable std::atomic<size_t> cloud_decisions_{0};
    mutable std::atomic<size_t> fallback_decisions_{0};
};

} // namespace libopenai