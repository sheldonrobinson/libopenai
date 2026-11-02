#include "libopenai/inference_backend.hpp"

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <chrono>
#include <thread>
#include <sstream>
#include <mutex>

namespace libopenai {

// =========================================================================
// HttpInferenceBackend
// =========================================================================

HttpInferenceBackend::HttpInferenceBackend(std::string name,
                                            std::string base_url,
                                            std::shared_ptr<ITransport> transport,
                                            std::string api_key)
    : name_(std::move(name)),
      base_url_(std::move(base_url)),
      api_key_(std::move(api_key)),
      transport_(std::move(transport)) {
    // Normalize base URL: strip trailing slash
    while (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
}

std::future<Response> HttpInferenceBackend::ExecuteAsync(const Request& req) {
    record_request_start();
    auto start = std::chrono::steady_clock::now();

    // Build the request with the backend's base URL and API key
    Request forwarded = req;
    forwarded.url = base_url_;
    if (!req.endpoint.empty()) {
        if (base_url_.back() != '/' && req.endpoint.front() != '/') {
            forwarded.url += "/" + req.endpoint;
        } else if (base_url_.back() == '/' && req.endpoint.front() == '/') {
            forwarded.url += req.endpoint.substr(1);
        } else {
            forwarded.url += req.endpoint;
        }
    }
    forwarded.endpoint = req.endpoint;

    if (!api_key_.empty()) {
        forwarded.headers["Authorization"] = "Bearer " + api_key_;
    }

    auto promise = std::make_shared<std::promise<Response>>();
    auto future = promise->get_future();

    auto transport_future = transport_->ExecuteAsync(forwarded);
    std::thread([promise, transport_future = std::move(transport_future),
                 start]() mutable {
        try {
            auto resp = transport_future.get();
            promise->set_value(std::move(resp));
        } catch (const std::exception& e) {
            Response resp;
            resp.status_code = -1;
            resp.body = std::string("HttpInferenceBackend error: ") + e.what();
            promise->set_value(std::move(resp));
        } catch (...) {
            Response resp;
            resp.status_code = -1;
            resp.body = "HttpInferenceBackend error: unknown exception";
            promise->set_value(std::move(resp));
        }
    }).detach();

    return future;
}

Response HttpInferenceBackend::Execute(const Request& req) {
    return ExecuteAsync(req).get();
}

BackendHealth HttpInferenceBackend::Health() const {
    perform_health_check();
    return health_;
}

BackendLoad HttpInferenceBackend::GetLoad() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    return load_;
}

bool HttpInferenceBackend::SupportsModel(const std::string& model) const {
    (void)model;
    return true;
}

void HttpInferenceBackend::record_request_start() {
    std::lock_guard<std::mutex> lock(load_mutex_);
    load_.active_requests++;
    load_.total_requests++;
}

void HttpInferenceBackend::record_request_complete(bool success,
                                                    double latency_ms) {
    std::lock_guard<std::mutex> lock(load_mutex_);
    load_.active_requests =
        (load_.active_requests > 0) ? load_.active_requests - 1 : 0;
    if (!success) {
        load_.failed_requests++;
    }
    load_.avg_latency_ms =
        (load_.avg_latency_ms * 0.9) + (latency_ms * 0.1);
    load_.current_load = static_cast<double>(load_.active_requests) / 100.0;
    if (load_.current_load > 1.0) load_.current_load = 1.0;
}

void HttpInferenceBackend::perform_health_check() const {
    auto now = std::chrono::steady_clock::now();
    if (health_ != BackendHealth::Unknown &&
        std::chrono::duration_cast<std::chrono::seconds>(now - last_health_check_)
            .count() < 5) {
        return;
    }

    Request health_req;
    health_req.method = "GET";
    health_req.url = base_url_;
    health_req.endpoint = "/health";
    health_req.timeout_seconds = 3;

    auto fut = transport_->ExecuteAsync(health_req);
    auto status = fut.wait_for(std::chrono::seconds(3));
    if (status == std::future_status::ready) {
        try {
            auto resp = fut.get();
            if (resp.status_code >= 200 && resp.status_code < 300) {
                health_ = BackendHealth::Healthy;
            } else {
                health_ = BackendHealth::Degraded;
            }
        } catch (...) {
            health_ = BackendHealth::Unhealthy;
        }
    } else {
        Request models_req;
        models_req.method = "GET";
        models_req.url = base_url_;
        models_req.endpoint = "/models";
        models_req.timeout_seconds = 3;
        auto fut2 = transport_->ExecuteAsync(models_req);
        auto status2 = fut2.wait_for(std::chrono::seconds(3));
        if (status2 == std::future_status::ready) {
            try {
                auto resp = fut2.get();
                if (resp.status_code >= 200 && resp.status_code < 300) {
                    health_ = BackendHealth::Healthy;
                } else {
                    health_ = BackendHealth::Degraded;
                }
            } catch (...) {
                health_ = BackendHealth::Unhealthy;
            }
        } else {
            health_ = BackendHealth::Unhealthy;
        }
    }
    last_health_check_ = now;
}

// =========================================================================
// InProcessInferenceBackend
// =========================================================================

InProcessInferenceBackend::InProcessInferenceBackend(std::string name,
                                                      std::string model_path,
                                                      int n_ctx,
                                                      int n_gpu_layers)
    : name_(std::move(name)),
      model_path_(std::move(model_path)),
      n_ctx_(n_ctx),
      n_gpu_layers_(n_gpu_layers) {
    health_ = model_path_.empty()
                  ? BackendHealth::Unhealthy
                  : BackendHealth::Healthy;
}

std::future<Response> InProcessInferenceBackend::ExecuteAsync(const Request& req) {
    record_request_start();
    auto start = std::chrono::steady_clock::now();

    auto promise = std::make_shared<std::promise<Response>>();
    auto future = promise->get_future();

    std::thread([promise, req, start]() mutable {
        try {
            Response resp;
            resp.status_code = 501;
            resp.body = "InProcessInferenceBackend: in-process model execution "
                        "not yet wired to a model engine (llama.cpp).";
            promise->set_value(std::move(resp));
        } catch (...) {
            Response resp;
            resp.status_code = -1;
            resp.body = "InProcessInferenceBackend: unknown exception";
            promise->set_value(std::move(resp));
        }
    }).detach();

    return future;
}

Response InProcessInferenceBackend::Execute(const Request& req) {
    return ExecuteAsync(req).get();
}

bool InProcessInferenceBackend::SupportsModel(const std::string& model) const {
    (void)model;
    return model_path_.empty();
}

void InProcessInferenceBackend::record_request_start() {
    std::lock_guard<std::mutex> lock(load_mutex_);
    load_.active_requests++;
    load_.total_requests++;
}

} // namespace libopenai