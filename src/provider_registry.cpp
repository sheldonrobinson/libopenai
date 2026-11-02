#include "libopenai/provider_registry.hpp"

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <algorithm>

namespace libopenai {

bool ProviderRegistry::RegisterProvider(ProviderPtr provider) {
    if (!provider) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (providers_.find(provider->Name()) != providers_.end()) {
        return false; // already exists
    }
    providers_[provider->Name()] = provider;
    return true;
}

bool ProviderRegistry::UnregisterProvider(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(name);
    if (it == providers_.end()) return false;
    providers_.erase(it);
    return true;
}

ProviderPtr ProviderRegistry::GetProvider(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(name);
    return (it != providers_.end()) ? it->second : nullptr;
}

std::vector<ProviderPtr> ProviderRegistry::GetAllProviders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProviderPtr> result;
    result.reserve(providers_.size());
    for (const auto& [name, provider] : providers_) {
        result.push_back(provider);
    }
    return result;
}

std::vector<ProviderPtr> ProviderRegistry::GetAvailableProviders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProviderPtr> result;
    for (const auto& [name, provider] : providers_) {
        if (provider->IsAvailable()) {
            result.push_back(provider);
        }
    }
    return result;
}

std::vector<ProviderPtr> ProviderRegistry::GetProvidersForModality(
    Modality m) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProviderPtr> result;
    for (const auto& [name, provider] : providers_) {
        if (provider->IsAvailable() && provider->SupportsModality(m)) {
            result.push_back(provider);
        }
    }
    return result;
}

std::vector<ProviderPtr> ProviderRegistry::GetProvidersForModel(
    const std::string& model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProviderPtr> result;
    for (const auto& [name, provider] : providers_) {
        if (provider->IsAvailable() && provider->SupportsModel(model)) {
            result.push_back(provider);
        }
    }
    return result;
}

std::vector<ProviderPtr> ProviderRegistry::GetProvidersByTier(
    ProviderTier tier) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProviderPtr> result;
    for (const auto& [name, provider] : providers_) {
        if (provider->IsAvailable() && provider->Tier() == tier) {
            result.push_back(provider);
        }
    }
    return result;
}

std::vector<ProviderPtr> ProviderRegistry::GetProvidersByPriority() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProviderPtr> result;
    result.reserve(providers_.size());
    for (const auto& [name, provider] : providers_) {
        if (provider->IsAvailable()) {
            result.push_back(provider);
        }
    }
    std::sort(result.begin(), result.end(),
        [](const ProviderPtr& a, const ProviderPtr& b) {
            return a->Info().priority < b->Info().priority;
        });
    return result;
}

void ProviderRegistry::EnableProvider(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(name);
    if (it != providers_.end()) {
        it->second->SetEnabled(true);
    }
}

void ProviderRegistry::DisableProvider(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(name);
    if (it != providers_.end()) {
        it->second->SetEnabled(false);
    }
}

size_t ProviderRegistry::ProviderCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.size();
}

size_t ProviderRegistry::AvailableProviderCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [name, provider] : providers_) {
        if (provider->IsAvailable()) count++;
    }
    return count;
}

} // namespace libopenai