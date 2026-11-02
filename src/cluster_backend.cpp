#include "libopenai/cluster_backend.hpp"

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <chrono>
#include <thread>
#include <algorithm>
#include <sstream>

namespace libopenai {

ClusterInferenceBackend::ClusterInferenceBackend(std::string name,
                                                  LoadBalancingStrategy strategy,
                                                  size_t max_retries,
                                                  double retry_backoff_ms)
    : name_(std::move(name)),
      strategy_(strategy),
      max_retries_(max_retries),
      retry_backoff_ms_(retry_backoff_ms) {
    stop_health_ = false;
    health_thread_ = std::thread(&ClusterInferenceBackend::HealthCheckLoop, this);
}

ClusterInferenceBackend::~ClusterInferenceBackend() {
    stop_health_ = true;
    if (health_thread_.joinable()) {
        health_thread_.join();
    }
}

void ClusterInferenceBackend::AddNode(std::string base_url,
                                       std::shared_ptr<ITransport> transport,
                                       std::string api_key) {
    auto node_backend = std::make_shared<HttpInferenceBackend>(
        name_ + "-node-" + std::to_string(nodes_.size()),
        std::move(base_url),
        std::move(transport),
        std::move(api_key));

    std::lock_guard<std::mutex> lock(nodes_mutex_);
    ClusterNode node;
    node.backend = node_backend;
    node.base_url = node_backend->BaseUrl();
    node.healthy = true;
    node.consecutive_failures = 0;
    nodes_.push_back(node);
}

void ClusterInferenceBackend::RemoveNode(const std::string& base_url) {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    nodes_.erase(
        std::remove_if(nodes_.begin(), nodes_.end(),
            [&](const ClusterNode& n) { return n.base_url == base_url; }),
        nodes_.end());
}

size_t ClusterInferenceBackend::FindNodeIndex(const std::vector<ClusterNode>& nodes,
                                               const BackendPtr& backend) {
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].backend == backend) {
            return i;
        }
    }
    return nodes.size();
}

BackendPtr ClusterInferenceBackend::SelectNode(const Request& req) {
    (void)req;
    std::lock_guard<std::mutex> lock(nodes_mutex_);

    std::vector<size_t> healthy_indices;
    for (size_t i = 0; i < nodes_.size(); i++) {
        if (nodes_[i].healthy) {
            healthy_indices.push_back(i);
        }
    }

    if (healthy_indices.empty()) {
        return nullptr;
    }

    size_t chosen_idx;
    switch (strategy_) {
        case LoadBalancingStrategy::RoundRobin:
            chosen_idx = SelectNodeRoundRobinIndex();
            break;
        case LoadBalancingStrategy::LeastConnections:
            chosen_idx = SelectNodeLeastConnectionsIndex();
            break;
        case LoadBalancingStrategy::Random:
            chosen_idx = SelectNodeRandomIndex();
            break;
        case LoadBalancingStrategy::LowestLatency:
            chosen_idx = SelectNodeLowestLatencyIndex();
            break;
    }

    if (chosen_idx >= nodes_.size()) {
        return nullptr;
    }
    return nodes_[chosen_idx].backend;
}

size_t ClusterInferenceBackend::SelectNodeRoundRobinIndex() {
    size_t idx = round_robin_index_.fetch_add(1, std::memory_order_relaxed);
    size_t attempts = 0;
    while (attempts < nodes_.size()) {
        size_t candidate = idx % nodes_.size();
        if (nodes_[candidate].healthy) {
            return candidate;
        }
        idx++;
        attempts++;
    }
    for (size_t i = 0; i < nodes_.size(); i++) {
        if (nodes_[i].healthy) {
            return i;
        }
    }
    return nodes_.size();
}

size_t ClusterInferenceBackend::SelectNodeLeastConnectionsIndex() {
    size_t best_idx = nodes_.size();
    size_t best_load = SIZE_MAX;
    bool found = false;

    for (size_t i = 0; i < nodes_.size(); i++) {
        if (!nodes_[i].healthy) continue;
        auto load = nodes_[i].backend->GetLoad();
        if (load.active_requests < best_load) {
            best_load = load.active_requests;
            best_idx = i;
            found = true;
        }
    }

    return found ? best_idx : nodes_.size();
}

size_t ClusterInferenceBackend::SelectNodeRandomIndex() {
    std::vector<size_t> healthy;
    for (size_t i = 0; i < nodes_.size(); i++) {
        if (nodes_[i].healthy) {
            healthy.push_back(i);
        }
    }
    if (healthy.empty()) return nodes_.size();
    std::uniform_int_distribution<size_t> dist(0, healthy.size() - 1);
    return healthy[dist(rng_)];
}

size_t ClusterInferenceBackend::SelectNodeLowestLatencyIndex() {
    size_t best_idx = nodes_.size();
    double best_latency = 1e9;
    bool found = false;

    for (size_t i = 0; i < nodes_.size(); i++) {
        if (!nodes_[i].healthy) continue;
        auto load = nodes_[i].backend->GetLoad();
        if (load.avg_latency_ms < best_latency) {
            best_latency = load.avg_latency_ms;
            best_idx = i;
            found = true;
        }
    }

    return found ? best_idx : nodes_.size();
}

std::future<Response> ClusterInferenceBackend::ExecuteAsync(const Request& req) {
    auto promise = std::make_shared<std::promise<Response>>();
    auto future = promise->get_future();

    std::thread([this, promise, req]() mutable {
        size_t attempt = 0;
        while (attempt <= max_retries_) {
            BackendPtr node = SelectNode(req);
            if (!node) {
                Response resp;
                resp.status_code = -1;
                resp.body = "ClusterInferenceBackend: no healthy nodes available";
                promise->set_value(std::move(resp));
                return;
            }

            auto start = std::chrono::steady_clock::now();
            auto fut = node->ExecuteAsync(req);
            auto status = fut.wait_for(std::chrono::seconds(
                static_cast<long>(req.timeout_seconds + 5)));

            if (status == std::future_status::ready) {
                try {
                    auto resp = fut.get();
                    auto end = std::chrono::steady_clock::now();
                    double latency_ms =
                        std::chrono::duration<double, std::milli>(end - start).count();

                    bool success = (resp.status_code >= 200 &&
                                    resp.status_code < 300);

                    if (success) {
                        promise->set_value(std::move(resp));
                        return;
                    }

                    {
                        std::lock_guard<std::mutex> lock(nodes_mutex_);
                        size_t idx = FindNodeIndex(nodes_, node);
                        if (idx < nodes_.size()) {
                            nodes_[idx].consecutive_failures += 1;
                            if (nodes_[idx].consecutive_failures >= 3) {
                                nodes_[idx].healthy = false;
                            }
                        }
                    }

                    attempt++;
                    if (attempt <= max_retries_) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(
                                static_cast<int>(retry_backoff_ms_ * attempt)));
                    }
                } catch (...) {
                    attempt++;
                    if (attempt <= max_retries_) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(
                                static_cast<int>(retry_backoff_ms_ * attempt)));
                    }
                }
            } else {
                {
                    std::lock_guard<std::mutex> lock(nodes_mutex_);
                    size_t idx = FindNodeIndex(nodes_, node);
                    if (idx < nodes_.size()) {
                        nodes_[idx].consecutive_failures += 1;
                        if (nodes_[idx].consecutive_failures >= 3) {
                            nodes_[idx].healthy = false;
                        }
                    }
                }
                attempt++;
                if (attempt <= max_retries_) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(
                            static_cast<int>(retry_backoff_ms_ * attempt)));
                }
            }
        }

        Response resp;
        resp.status_code = -1;
        resp.body = "ClusterInferenceBackend: all retries exhausted";
        promise->set_value(std::move(resp));
    }).detach();

    return future;
}

Response ClusterInferenceBackend::Execute(const Request& req) {
    return ExecuteAsync(req).get();
}

BackendHealth ClusterInferenceBackend::Health() const {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    bool any_healthy = false;
    bool any_degraded = false;
    for (const auto& node : nodes_) {
        auto h = node.backend->Health();
        if (h == BackendHealth::Healthy) {
            any_healthy = true;
        } else if (h == BackendHealth::Degraded) {
            any_degraded = true;
        }
    }
    if (nodes_.empty()) return BackendHealth::Unknown;
    if (any_healthy) return BackendHealth::Healthy;
    if (any_degraded) return BackendHealth::Degraded;
    return BackendHealth::Unhealthy;
}

BackendLoad ClusterInferenceBackend::GetLoad() const {
    BackendLoad total;
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    size_t healthy_count = 0;
    for (const auto& node : nodes_) {
        if (!node.healthy) continue;
        auto load = node.backend->GetLoad();
        total.active_requests += load.active_requests;
        total.total_requests += load.total_requests;
        total.failed_requests += load.failed_requests;
        total.avg_latency_ms += load.avg_latency_ms;
        healthy_count++;
    }
    if (healthy_count > 0) {
        total.avg_latency_ms /= static_cast<double>(healthy_count);
    }
    total.current_load =
        (total.active_requests > 0)
            ? static_cast<double>(total.active_requests) /
                  (static_cast<double>(nodes_.size()) * 100.0)
            : 0.0;
    if (total.current_load > 1.0) total.current_load = 1.0;
    return total;
}

void ClusterInferenceBackend::HealthCheckLoop() {
    while (!stop_health_.load(std::memory_order_acquire)) {
        std::vector<ClusterNode> nodes_copy;
        {
            std::lock_guard<std::mutex> lock(nodes_mutex_);
            nodes_copy = nodes_;
        }

        for (const auto& node : nodes_copy) {
            (void)node.backend->Health();
        }

        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

size_t ClusterInferenceBackend::NodeCount() const {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    return nodes_.size();
}

size_t ClusterInferenceBackend::HealthyNodeCount() const {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    return std::count_if(nodes_.begin(), nodes_.end(),
        [](const ClusterNode& n) {
            return n.healthy;
        });
}

} // namespace libopenai