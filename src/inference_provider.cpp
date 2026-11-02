#include "libopenai/inference_provider.hpp"

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <algorithm>
#include <sstream>

namespace libopenai {

// =========================================================================
// BackendProvider
// =========================================================================

BackendProvider::BackendProvider(ProviderInfo info, BackendPtr backend)
    : info_(std::move(info)), backend_(std::move(backend)) {}

bool BackendProvider::SupportsModality(Modality m) const {
    return info_.capabilities.supported_modalities.count(m) > 0 ||
           info_.capabilities.supported_modalities.count(Modality::Any) > 0;
}

bool BackendProvider::SupportsModel(const std::string& model) const {
    if (info_.capabilities.supported_models.empty()) {
        return true;
    }
    return info_.capabilities.supported_models.count(model) > 0;
}

std::future<Response> BackendProvider::ExecuteAsync(const Request& req) {
    return backend_->ExecuteAsync(req);
}

Response BackendProvider::Execute(const Request& req) {
    return backend_->Execute(req);
}

BackendHealth BackendProvider::Health() const {
    return backend_->Health();
}

BackendLoad BackendProvider::GetLoad() const {
    return backend_->GetLoad();
}

bool BackendProvider::IsAvailable() const {
    std::lock_guard<std::mutex> lock(availability_mutex_);
    return available_ && info_.enabled &&
           (Health() == BackendHealth::Healthy ||
            Health() == BackendHealth::Degraded);
}

// =========================================================================
// HttpInferenceProvider
// =========================================================================

HttpInferenceProvider::HttpInferenceProvider(ProviderInfo info,
                                              std::shared_ptr<ITransport> transport)
    : info_(std::move(info)),
      backend_(std::make_shared<HttpInferenceBackend>(
          info_.name,
          info_.endpoint_url,
          std::move(transport),
          "")) {
    // API key is set per-request via headers if needed
}

bool HttpInferenceProvider::SupportsModality(Modality m) const {
    return info_.capabilities.supported_modalities.count(m) > 0 ||
           info_.capabilities.supported_modalities.count(Modality::Any) > 0;
}

bool HttpInferenceProvider::SupportsModel(const std::string& model) const {
    if (info_.capabilities.supported_models.empty()) {
        return true; // accepts any model
    }
    return info_.capabilities.supported_models.count(model) > 0;
}

std::future<Response> HttpInferenceProvider::ExecuteAsync(const Request& req) {
    return backend_->ExecuteAsync(req);
}

Response HttpInferenceProvider::Execute(const Request& req) {
    return backend_->Execute(req);
}

BackendHealth HttpInferenceProvider::Health() const {
    return backend_->Health();
}

BackendLoad HttpInferenceProvider::GetLoad() const {
    return backend_->GetLoad();
}

bool HttpInferenceProvider::IsAvailable() const {
    std::lock_guard<std::mutex> lock(availability_mutex_);
    return available_ && info_.enabled &&
           (Health() == BackendHealth::Healthy ||
            Health() == BackendHealth::Degraded);
}

// =========================================================================
// OnDeviceInferenceProvider
// =========================================================================

OnDeviceInferenceProvider::OnDeviceInferenceProvider(ProviderInfo info,
                                                      std::string model_path,
                                                      int n_ctx)
    : info_(std::move(info)),
      backend_(std::make_shared<InProcessInferenceBackend>(
          info_.name, std::move(model_path), n_ctx, 0)) {}

bool OnDeviceInferenceProvider::SupportsModality(Modality m) const {
    return info_.capabilities.supported_modalities.count(m) > 0 ||
           info_.capabilities.supported_modalities.count(Modality::Any) > 0;
}

bool OnDeviceInferenceProvider::SupportsModel(const std::string& model) const {
    if (info_.capabilities.supported_models.empty()) {
        return true;
    }
    return info_.capabilities.supported_models.count(model) > 0;
}

std::future<Response> OnDeviceInferenceProvider::ExecuteAsync(const Request& req) {
    return backend_->ExecuteAsync(req);
}

Response OnDeviceInferenceProvider::Execute(const Request& req) {
    return backend_->Execute(req);
}

BackendHealth OnDeviceInferenceProvider::Health() const {
    return backend_->Health();
}

BackendLoad OnDeviceInferenceProvider::GetLoad() const {
    return backend_->GetLoad();
}

bool OnDeviceInferenceProvider::IsAvailable() const {
    std::lock_guard<std::mutex> lock(availability_mutex_);
    return available_ && info_.enabled &&
           (Health() == BackendHealth::Healthy ||
            Health() == BackendHealth::Degraded);
}

} // namespace libopenai