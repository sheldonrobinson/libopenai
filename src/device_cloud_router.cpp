#include "libopenai/device_cloud_router.hpp"

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <algorithm>
#include <sstream>
#include <thread>

namespace libopenai {

DeviceCloudRouter::DeviceCloudRouter(
    std::shared_ptr<ProviderRegistry> registry,
    RoutingPolicy policy)
    : registry_(std::move(registry)), policy_(std::move(policy)) {}

void DeviceCloudRouter::SetPolicy(const RoutingPolicy& policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_ = policy;
}

void DeviceCloudRouter::SetRegistry(std::shared_ptr<ProviderRegistry> registry) {
    std::lock_guard<std::mutex> lock(mutex_);
    registry_ = std::move(registry);
}

ProviderPtr DeviceCloudRouter::SelectProviderForModality(Modality m,
                                                           ProviderTier tier) const {
    auto providers = registry_->GetProvidersForModality(m);
    std::vector<ProviderPtr> tier_providers;
    for (auto& p : providers) {
        if (p->Tier() == tier) {
            tier_providers.push_back(p);
        }
    }
    if (tier_providers.empty()) return nullptr;

    // Sort by priority, then by load
    std::sort(tier_providers.begin(), tier_providers.end(),
        [](const ProviderPtr& a, const ProviderPtr& b) {
            if (a->Info().priority != b->Info().priority) {
                return a->Info().priority < b->Info().priority;
            }
            return a->GetLoad().active_requests < b->GetLoad().active_requests;
        });

    return tier_providers[0];
}

bool DeviceCloudRouter::IsDeviceOverloaded() const {
    auto device_providers = registry_->GetProvidersByTier(ProviderTier::Device);
    if (device_providers.empty()) return false;

    double total_load = 0.0;
    for (auto& p : device_providers) {
        total_load += p->GetLoad().current_load;
    }
    double avg_load = total_load / static_cast<double>(device_providers.size());
    return avg_load > policy_.max_device_load;
}

bool DeviceCloudRouter::IsNetworkAvailable() const {
    // Simplified: check if any cloud provider is healthy
    auto cloud_providers = registry_->GetProvidersByTier(ProviderTier::Cloud);
    for (auto& p : cloud_providers) {
        if (p->Health() == BackendHealth::Healthy ||
            p->Health() == BackendHealth::Degraded) {
            return true;
        }
    }
    return false;
}

RoutingDecision DeviceCloudRouter::Decide(const Request& req) const {
    RoutingDecision decision;
    Modality mod = ModalityRouter::DetectModality(req);
    auto model = ModalityRouter::ExtractModel(req);

    std::lock_guard<std::mutex> lock(mutex_);

    // --- Rule 1: Offline mode - device only ---
    if (policy_.offline_mode) {
        auto device = SelectProviderForModality(mod, ProviderTier::Device);
        if (device) {
            decision.provider = device;
            decision.tier = ProviderTier::Device;
            decision.reason = "Offline mode: device-only";
            decision.confidence = 0.95;
            device_decisions_++;
            return decision;
        }
        // No device provider available - fail
        decision.reason = "Offline mode: no device provider available";
        decision.confidence = 0.0;
        return decision;
    }

    // --- Rule 2: Privacy mode - device only ---
    if (policy_.privacy_mode) {
        auto device = SelectProviderForModality(mod, ProviderTier::Device);
        if (device) {
            decision.provider = device;
            decision.tier = ProviderTier::Device;
            decision.reason = "Privacy mode: device-only";
            decision.confidence = 0.95;
            device_decisions_++;
            return decision;
        }
        // Privacy required but no device provider - fail rather than leak to cloud
        decision.reason = "Privacy mode: no device provider available";
        decision.confidence = 0.0;
        return decision;
    }

    // --- Rule 3: Model availability on device ---
    bool model_on_device = false;
    if (model.has_value()) {
        auto device_providers = registry_->GetProvidersForModel(*model);
        for (auto& p : device_providers) {
            if (p->Tier() == ProviderTier::Device) {
                model_on_device = true;
                break;
            }
        }
    }

    // --- Rule 4: Prefer device if model is available and device not overloaded ---
    if (policy_.prefer_device && !IsDeviceOverloaded()) {
        if (model_on_device || !model.has_value()) {
            auto device = SelectProviderForModality(mod, ProviderTier::Device);
            if (device) {
                decision.provider = device;
                decision.tier = ProviderTier::Device;
                decision.reason = "Device preferred: model available, load acceptable";
                decision.confidence = 0.9;
                device_decisions_++;
                return decision;
            }
        }
    }

    // --- Rule 5: Cloud for quality/throughput ---
    if (policy_.require_quality || policy_.battery_saver) {
        auto cloud = SelectProviderForModality(mod, ProviderTier::Cloud);
        if (cloud && IsNetworkAvailable()) {
            decision.provider = cloud;
            decision.tier = ProviderTier::Cloud;
            decision.reason = policy_.require_quality
                ? "Cloud required for quality"
                : "Cloud to save device battery";
            decision.confidence = 0.85;
            cloud_decisions_++;
            return decision;
        }
    }

    // --- Rule 6: Hybrid - try device first, fall back to cloud ---
    auto device = SelectProviderForModality(mod, ProviderTier::Device);
    auto cloud = SelectProviderForModality(mod, ProviderTier::Cloud);

    if (device && !IsDeviceOverloaded()) {
        decision.provider = device;
        decision.tier = ProviderTier::Device;
        decision.reason = "Hybrid: device selected, cloud available as fallback";
        decision.confidence = 0.8;
        device_decisions_++;
        return decision;
    }

    if (cloud && IsNetworkAvailable()) {
        decision.provider = cloud;
        decision.tier = ProviderTier::Cloud;
        decision.reason = "Hybrid: cloud selected (device unavailable or overloaded)";
        decision.fallback = true;
        decision.confidence = 0.75;
        cloud_decisions_++;
        fallback_decisions_++;
        return decision;
    }

    // --- Rule 7: Last resort - any available provider ---
    auto all = registry_->GetAvailableProviders();
    if (!all.empty()) {
        decision.provider = all[0];
        decision.tier = decision.provider->Tier();
        decision.reason = "Fallback: any available provider";
        decision.fallback = true;
        decision.confidence = 0.5;
        fallback_decisions_++;
        return decision;
    }

    // --- No provider available ---
    decision.reason = "No provider available for request";
    decision.confidence = 0.0;
    return decision;
}

std::future<Response> DeviceCloudRouter::ExecuteAsync(const Request& req) const {
    auto decision = Decide(req);

    if (!decision.provider) {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();
        Response resp;
        resp.status_code = -1;
        resp.body = "DeviceCloudRouter: " + decision.reason;
        promise->set_value(std::move(resp));
        return future;
    }

    auto promise = std::make_shared<std::promise<Response>>();
    auto future = promise->get_future();

    auto provider = decision.provider;
    auto tier = decision.tier;
    auto is_fallback = decision.fallback;
    auto policy = policy_; // capture copy for fallback logic
    auto registry = registry_; // capture copy

    std::thread([promise, req, provider, tier, is_fallback, policy,
                 registry]() mutable {
        try {
            auto fut = provider->ExecuteAsync(req);
            auto status = fut.wait_for(std::chrono::seconds(
                static_cast<long>(req.timeout_seconds + 5)));

            if (status == std::future_status::ready) {
                auto resp = fut.get();

                // If the device provider failed and we allow cloud fallback,
                // retry on cloud
                if (resp.status_code < 200 || resp.status_code >= 300) {
                    if (tier == ProviderTier::Device &&
                        policy.allow_cloud_fallback && !is_fallback) {
                        // Try cloud fallback
                        Modality mod = ModalityRouter::DetectModality(req);
                        auto cloud_providers =
                            registry->GetProvidersForModality(mod);
                        for (auto& cp : cloud_providers) {
                            if (cp->Tier() == ProviderTier::Cloud &&
                                cp->IsAvailable()) {
                                try {
                                    auto cloud_fut = cp->ExecuteAsync(req);
                                    auto cloud_status =
                                        cloud_fut.wait_for(
                                            std::chrono::seconds(
                                                static_cast<long>(
                                                    req.timeout_seconds + 5)));
                                    if (cloud_status ==
                                        std::future_status::ready) {
                                        promise->set_value(cloud_fut.get());
                                        return;
                                    }
                                } catch (...) {
                                    // Continue trying
                                }
                            }
                        }
                    }
                }

                promise->set_value(std::move(resp));
                return;
            }

            // Timeout - try fallback if applicable
            if (tier == ProviderTier::Device &&
                policy.allow_cloud_fallback && !is_fallback) {
                Modality mod = ModalityRouter::DetectModality(req);
                auto cloud_providers =
                    registry->GetProvidersForModality(mod);
                for (auto& cp : cloud_providers) {
                    if (cp->Tier() == ProviderTier::Cloud &&
                        cp->IsAvailable()) {
                        try {
                            auto cloud_fut = cp->ExecuteAsync(req);
                            auto cloud_status =
                                cloud_fut.wait_for(
                                    std::chrono::seconds(
                                        static_cast<long>(
                                            req.timeout_seconds + 5)));
                            if (cloud_status ==
                                std::future_status::ready) {
                                promise->set_value(cloud_fut.get());
                                return;
                            }
                        } catch (...) {
                            // Continue trying
                        }
                    }
                }
            }

            Response resp;
            resp.status_code = -1;
            resp.body = "DeviceCloudRouter: request timed out or failed";
            promise->set_value(std::move(resp));
        } catch (const std::exception& e) {
            Response resp;
            resp.status_code = -1;
            resp.body = std::string("DeviceCloudRouter error: ") + e.what();
            promise->set_value(std::move(resp));
        } catch (...) {
            Response resp;
            resp.status_code = -1;
            resp.body = "DeviceCloudRouter: unknown exception";
            promise->set_value(std::move(resp));
        }
    }).detach();

    return future;
}

} // namespace libopenai