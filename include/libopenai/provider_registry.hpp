#pragma once

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <algorithm>

#include "libopenai/inference_provider.hpp"

namespace libopenai {

/**
 * @brief ProviderRegistry - central registry of all inference providers.
 *
 * Maintains a catalog of available providers with their metadata.
 * Providers can be registered dynamically at runtime. The registry
 * supports lookups by name, modality, model, tier, and capability.
 *
 * Thread-safe: all public methods are safe for concurrent use.
 */
class ProviderRegistry {
public:
    ProviderRegistry() = default;
    ~ProviderRegistry() = default;

    // ---- Registration ----

    // Register a provider. Returns false if a provider with the same
    // name already exists.
    bool RegisterProvider(ProviderPtr provider);

    // Unregister a provider by name. Returns false if not found.
    bool UnregisterProvider(const std::string& name);

    // ---- Lookup ----

    // Get a provider by name
    ProviderPtr GetProvider(const std::string& name) const;

    // Get all registered providers
    std::vector<ProviderPtr> GetAllProviders() const;

    // Get all available (enabled + healthy) providers
    std::vector<ProviderPtr> GetAvailableProviders() const;

    // Get providers that support a given modality
    std::vector<ProviderPtr> GetProvidersForModality(Modality m) const;

    // Get providers that support a given model
    std::vector<ProviderPtr> GetProvidersForModel(const std::string& model) const;

    // Get providers of a given tier (device/cloud/hybrid)
    std::vector<ProviderPtr> GetProvidersByTier(ProviderTier tier) const;

    // Get providers sorted by priority (lowest first)
    std::vector<ProviderPtr> GetProvidersByPriority() const;

    // ---- Provider management ----

    void EnableProvider(const std::string& name);
    void DisableProvider(const std::string& name);

    size_t ProviderCount() const;
    size_t AvailableProviderCount() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, ProviderPtr> providers_;
};

} // namespace libopenai