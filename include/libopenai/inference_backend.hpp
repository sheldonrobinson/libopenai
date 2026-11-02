#pragma once

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <string>
#include <memory>
#include <future>
#include <map>
#include <optional>
#include <chrono>
#include <atomic>
#include <nlohmann/json.hpp>

#include "libopenai/transport.hpp"

namespace libopenai {

using json = nlohmann::json;

/**
 * @brief Backend health status reported by periodic health checks.
 */
enum class BackendHealth {
    Healthy,    ///< Backend is accepting and processing requests
    Degraded,   ///< Backend is responding but slow or error-prone
    Unhealthy,  ///< Backend is not responding or consistently failing
    Unknown     ///< Health has not been checked yet
};

/**
 * @brief Backend load information for load balancing decisions.
 */
struct BackendLoad {
    double current_load = 0.0;     // 0.0 = idle, 1.0 = saturated
    size_t active_requests = 0;    // Number of in-flight requests
    size_t total_requests = 0;     // Cumulative request count
    size_t failed_requests = 0;    // Cumulative failure count
    double avg_latency_ms = 0.0;   // Average request latency in milliseconds
};

/**
 * @brief InferenceBackend - abstract base for any inference backend provider.
 *
 * A backend represents a destination for inference requests. It can be:
 *   - In-process: a model loaded directly in this process (e.g. llama.cpp)
 *   - Localhost: an inference server on the same machine
 *   - Cluster: a set of backend nodes behind a load balancer
 *   - Remote: an external inference API over the network
 *
 * All backends support concurrent ExecuteAsync() calls. Each call returns
 * a std::future<Response> immediately; the request runs asynchronously and
 * the future is fulfilled when the response arrives.
 *
 * Implementations must be thread-safe: ExecuteAsync() may be called
 * concurrently from multiple threads.
 */
class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    /**
     * @brief Execute an inference request asynchronously.
     * @param req The OpenAI-compatible request to execute.
     * @return Future that will be fulfilled with the response.
     */
    virtual std::future<Response> ExecuteAsync(const Request& req) = 0;

    /**
     * @brief Execute an inference request synchronously (blocking).
     * Default implementation calls ExecuteAsync().get().
     */
    virtual Response Execute(const Request& req) {
        return ExecuteAsync(req).get();
    }

    /**
     * @brief Returns a human-readable name for this backend (logging/debugging).
     */
    virtual std::string Name() const = 0;

    /**
     * @brief Returns the type of backend (in-process, http, cluster).
     */
    virtual std::string Type() const = 0;

    /**
     * @brief Check backend health. Returns current health status.
     * Implementations may perform an actual probe (e.g. GET /health) or
     * return cached status from a background health checker.
     */
    virtual BackendHealth Health() const = 0;

    /**
     * @brief Returns current load information for load balancing.
     */
    virtual BackendLoad GetLoad() const = 0;

    /**
     * @brief Whether this backend can handle the given model.
     * Default returns true (backend accepts any model).
     */
    virtual bool SupportsModel(const std::string& model) const {
        (void)model;
        return true;
    }

    /**
     * @brief Whether this backend can handle the given endpoint.
     * Default returns true (backend accepts any endpoint).
     */
    virtual bool SupportsEndpoint(const std::string& endpoint) const {
        (void)endpoint;
        return true;
    }
};

using BackendPtr = std::shared_ptr<InferenceBackend>;

/**
 * @brief HttpInferenceBackend - an HTTP-based inference backend.
 *
 * Wraps an ITransport instance and a base URL. Suitable for localhost,
 * remote, or any HTTP-reachable inference server. Health checks are
 * performed by sending GET /health (or GET /models if /health fails).
 */
class HttpInferenceBackend : public InferenceBackend {
public:
    HttpInferenceBackend(std::string name,
                         std::string base_url,
                         std::shared_ptr<ITransport> transport,
                         std::string api_key = "");

    ~HttpInferenceBackend() override = default;

    std::future<Response> ExecuteAsync(const Request& req) override;
    Response Execute(const Request& req) override;
    std::string Name() const override { return name_; }
    std::string Type() const override { return "http"; }

    BackendHealth Health() const override;
    BackendLoad GetLoad() const override;
    bool SupportsModel(const std::string& model) const override;

    const std::string& BaseUrl() const { return base_url_; }

private:
    std::string name_;
    std::string base_url_;
    std::string api_key_;
    std::shared_ptr<ITransport> transport_;

    mutable std::mutex load_mutex_;
    BackendLoad load_;
    mutable BackendHealth health_ = BackendHealth::Unknown;
    mutable std::chrono::steady_clock::time_point last_health_check_;

    void record_request_start();
    void record_request_complete(bool success, double latency_ms);
    void perform_health_check() const;
};

/**
 * @brief InProcessInferenceBackend - an in-process inference backend.
 *
 * Represents a model loaded directly in this process (e.g. llama.cpp model
 * running in-process). Zero network overhead. Currently a placeholder that
 * returns an error indicating in-process execution is not yet wired to a
 * concrete model engine. Future work: integrate with llama.cpp directly.
 */
class InProcessInferenceBackend : public InferenceBackend {
public:
    InProcessInferenceBackend(std::string name,
                              std::string model_path = "",
                              int n_ctx = 4096,
                              int n_gpu_layers = 0);

    ~InProcessInferenceBackend() override = default;

    std::future<Response> ExecuteAsync(const Request& req) override;
    Response Execute(const Request& req) override;
    std::string Name() const override { return name_; }
    std::string Type() const override { return "in-process"; }

    BackendHealth Health() const override { return health_; }
    BackendLoad GetLoad() const override { return load_; }
    bool SupportsModel(const std::string& model) const override;

    const std::string& ModelPath() const { return model_path_; }

private:
    void record_request_start();

    std::string name_;
    std::string model_path_;
    int n_ctx_;
    int n_gpu_layers_;
    mutable BackendHealth health_ = BackendHealth::Unknown;
    mutable BackendLoad load_;
    mutable std::mutex load_mutex_;
};

} // namespace libopenai