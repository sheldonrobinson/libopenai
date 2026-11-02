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
#include <nlohmann/json.hpp>

#include "libopenai/transport.hpp"
#include "libopenai/inference_provider.hpp"
#include "libopenai/provider_registry.hpp"

namespace libopenai {

using json = nlohmann::json;

/**
 * @brief ModalityRouter - routes requests based on modality.
 *
 * Examines the request payload to determine the modality (text, image,
 * audio, etc.) and selects providers that support that modality.
 * Falls back to text if the modality cannot be determined.
 *
 * Supports both exact matching (provider must support the exact modality)
 * and fallback chains (try preferred modality providers, then any).
 */
class ModalityRouter {
public:
    explicit ModalityRouter(std::shared_ptr<ProviderRegistry> registry)
        : registry_(std::move(registry)) {}

    virtual ~ModalityRouter() = default;

    // ---- Routing ----

    // Select the best provider for the given request based on modality.
    // Returns nullptr if no provider supports the request's modality.
    virtual ProviderPtr Route(const Request& req) const;

    // Select all providers that can handle the request, sorted by priority.
    std::vector<ProviderPtr> RouteAll(const Request& req) const;

    // ---- Modality detection ----

    // Detect the modality of a request from its payload/endpoint.
    static Modality DetectModality(const Request& req);

    // Extract the model name from a request payload.
    static std::optional<std::string> ExtractModel(const Request& req);

protected:
    std::shared_ptr<ProviderRegistry> registry_;
};

/**
 * @brief WeightedModalityRouter - extends ModalityRouter with priority
 *        weighting and fallback chains.
 *
 * When multiple providers support the same modality, selects based on:
 * 1. Provider priority (lower = higher priority)
 * 2. Current load (least loaded preferred)
 * 3. Health status (healthy preferred over degraded)
 *
 * Falls back through provider tiers: device -> hybrid -> cloud,
 * respecting the collaborative inference pattern.
 */
class WeightedModalityRouter : public ModalityRouter {
public:
    using ModalityRouter::ModalityRouter;

    ProviderPtr Route(const Request& req) const override;

    // Route with explicit fallback: try device first, then cloud.
    // Implements the Konstrukt device-cloud collaborative pattern.
    ProviderPtr RouteWithFallback(const Request& req,
                                   bool prefer_device = true) const;
};

} // namespace libopenai