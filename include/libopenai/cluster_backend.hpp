#pragma once

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <random>

#include "libopenai/transport.hpp"
#include "libopenai/inference_backend.hpp"

namespace libopenai {

using json = nlohmann::json;

/**
 * @brief LoadBalancingStrategy - how to select a node in a cluster.
 */
enum class LoadBalancingStrategy {
    RoundRobin,        ///< Cycle through nodes in order
    LeastConnections,  ///< Send to node with fewest active requests
    Random,            ///< Select a random healthy node
    LowestLatency      ///< Send to node with lowest average latency
};

/**
 * @brief ClusterInferenceBackend - a load-balanced cluster of HTTP backends.
 *
 * Manages a pool of HttpInferenceBackend nodes. Each ExecuteAsync() call is
 * routed to a single node based on the load balancing strategy. Failed
 * requests are retried on a different healthy node (failover).
 *
 * Background health checking monitors all nodes and marks unhealthy ones
 * as temporarily excluded from the pool.
 */
class ClusterInferenceBackend : public InferenceBackend {
public:
    ClusterInferenceBackend(std::string name,
                            LoadBalancingStrategy strategy =
                                LoadBalancingStrategy::LeastConnections,
                            size_t max_retries = 2,
                            double retry_backoff_ms = 100.0);

    ~ClusterInferenceBackend() override;

    // Add a node to the cluster
    void AddNode(std::string base_url,
                 std::shared_ptr<ITransport> transport,
                 std::string api_key = "");

    // Remove a node by base URL
    void RemoveNode(const std::string& base_url);

    std::future<Response> ExecuteAsync(const Request& req) override;
    Response Execute(const Request& req) override;

    std::string Name() const override { return name_; }
    std::string Type() const override { return "cluster"; }

    BackendHealth Health() const override;
    BackendLoad GetLoad() const override;

    bool SupportsModel(const std::string& model) const override {
        return true; // cluster forwards to whatever node is healthy
    }

    size_t NodeCount() const;
    size_t HealthyNodeCount() const;

    LoadBalancingStrategy Strategy() const { return strategy_; }

public:
    struct ClusterNode {
        BackendPtr backend;
        std::string base_url;
        bool healthy{true};
        size_t consecutive_failures{0};
    };

private:
    BackendPtr SelectNode(const Request& req);
    size_t SelectNodeRoundRobinIndex();
    size_t SelectNodeLeastConnectionsIndex();
    size_t SelectNodeRandomIndex();
    size_t SelectNodeLowestLatencyIndex();
    static size_t FindNodeIndex(const std::vector<ClusterNode>& nodes,
                                 const BackendPtr& backend);
    void HealthCheckLoop();
    void PerformHealthCheck(const ClusterNode& node) const;

    std::string name_;
    LoadBalancingStrategy strategy_;
    size_t max_retries_;
    double retry_backoff_ms_;

    mutable std::mutex nodes_mutex_;
    std::vector<ClusterNode> nodes_;
    std::atomic<size_t> round_robin_index_{0};
    std::mt19937 rng_{std::random_device{}()};

    std::thread health_thread_;
    std::atomic<bool> stop_health_{false};
};

} // namespace libopenai