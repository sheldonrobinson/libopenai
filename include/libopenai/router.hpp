#pragma once

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <functional>
#include <nlohmann/json.hpp>

#include "libopenai/transport.hpp"
#include "libopenai/inference_backend.hpp"

namespace libopenai {

using json = nlohmann::json;

/**
 * @brief RoutingRule - a single rule for selecting a backend.
 *
 * A rule matches requests based on model name patterns, endpoint patterns,
 * or custom predicates. The first matching rule wins.
 */
struct RoutingRule {
    std::string name;                         // Human-readable rule name
    std::optional<std::string> model_pattern; // Substring match on model name
    std::optional<std::string> endpoint_pattern; // Substring match on endpoint
    std::function<bool(const Request&)> custom_predicate; // Custom match logic
    BackendPtr backend;                       // Backend to route to if matched

    bool Matches(const Request& req) const {
        if (model_pattern.has_value() && !req.payload.empty()) {
            // Try to extract model from JSON payload
            try {
                auto j = json::parse(req.payload);
                if (j.contains("model")) {
                    std::string model = j["model"].get<std::string>();
                    if (model.find(*model_pattern) == std::string::npos) {
                        return false;
                    }
                }
            } catch (...) {
                // If we can't parse the payload, don't match on model pattern
                return false;
            }
        }
        if (endpoint_pattern.has_value()) {
            if (req.endpoint.find(*endpoint_pattern) == std::string::npos) {
                return false;
            }
        }
        if (custom_predicate) {
            return custom_predicate(req);
        }
        return true;
    }
};

/**
 * @brief Router - abstract base for backend selection.
 *
 * Given a Request, the Router selects the appropriate InferenceBackend.
 * Implementations can be simple (first match) or complex (weighted,
 * latency-aware, cost-aware, etc.).
 */
class Router {
public:
    virtual ~Router() = default;

    /**
     * @brief Select a backend for the given request.
     * @param req The request to route.
     * @return Selected backend, or nullptr if no backend matches.
     */
    virtual BackendPtr Route(const Request& req) = 0;

    /**
     * @brief Add a routing rule. Rules are evaluated in order added.
     */
    virtual void AddRule(RoutingRule rule) = 0;

    /**
     * @brief Set the default backend (used when no rule matches).
     */
    virtual void SetDefaultBackend(BackendPtr backend) = 0;
};

/**
 * @brief FirstMatchRouter - selects the first backend whose rule matches.
 *
 * Rules are evaluated in the order they were added. If no rule matches,
 * the default backend is returned (if set).
 */
class FirstMatchRouter : public Router {
public:
    FirstMatchRouter() = default;

    BackendPtr Route(const Request& req) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& rule : rules_) {
            if (rule.Matches(req)) {
                return rule.backend;
            }
        }
        return default_backend_;
    }

    void AddRule(RoutingRule rule) override {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_.push_back(std::move(rule));
    }

    void SetDefaultBackend(BackendPtr backend) override {
        std::lock_guard<std::mutex> lock(mutex_);
        default_backend_ = std::move(backend);
    }

private:
    std::mutex mutex_;
    std::vector<RoutingRule> rules_;
    BackendPtr default_backend_;
};

/**
 * @brief ModelRouter - routes based on model name prefixes/patterns.
 *
 * Useful when different models are served by different backends
 * (e.g. "llama-" models go to local cluster, "gpt-" models go to OpenAI).
 */
class ModelRouter : public Router {
public:
    ModelRouter() = default;

    // Add a model prefix -> backend mapping.
    // Requests whose model starts with prefix are routed to backend.
    void AddModelRoute(const std::string& prefix, BackendPtr backend) {
        std::lock_guard<std::mutex> lock(mutex_);
        model_routes_[prefix] = std::move(backend);
    }

    void SetDefaultBackend(BackendPtr backend) override {
        std::lock_guard<std::mutex> lock(mutex_);
        default_backend_ = std::move(backend);
    }

    void AddRule(RoutingRule rule) override {
        // ModelRouter doesn't use generic rules; convert to model route.
        std::lock_guard<std::mutex> lock(mutex_);
        if (rule.model_pattern.has_value()) {
            model_routes_[*rule.model_pattern] = rule.backend;
        } else {
            generic_rules_.push_back(std::move(rule));
        }
    }

    BackendPtr Route(const Request& req) override {
        std::lock_guard<std::mutex> lock(mutex_);

        // Extract model from payload
        std::string model;
        try {
            if (!req.payload.empty()) {
                auto j = json::parse(req.payload);
                if (j.contains("model")) {
                    model = j["model"].get<std::string>();
                }
            }
        } catch (...) {
            // Ignore parse errors
        }

        // Find longest matching prefix
        std::string best_prefix;
        BackendPtr best_backend;
        for (const auto& [prefix, backend] : model_routes_) {
            if (!model.empty() && model.find(prefix) == 0) {
                if (prefix.size() > best_prefix.size()) {
                    best_prefix = prefix;
                    best_backend = backend;
                }
            }
        }

        if (best_backend) return best_backend;

        // Fall through to generic rules
        for (const auto& rule : generic_rules_) {
            if (rule.Matches(req)) {
                return rule.backend;
            }
        }

        return default_backend_;
    }

private:
    std::mutex mutex_;
    std::map<std::string, BackendPtr> model_routes_;   // prefix -> backend
    std::vector<RoutingRule> generic_rules_;
    BackendPtr default_backend_;
};

} // namespace libopenai