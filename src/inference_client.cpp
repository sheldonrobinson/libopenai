#include "libopenai/inference_client.hpp"

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <sstream>

namespace libopenai {

InferenceClient::InferenceClient()
    : registry_(std::make_shared<ProviderRegistry>()),
      router_(std::make_shared<DeviceCloudRouter>(registry_, policy_)) {}

InferenceClient::InferenceClient(std::shared_ptr<DeviceCloudRouter> router)
    : router_(std::move(router)),
      registry_(router_->GetRegistry()) {}

InferenceClient::~InferenceClient() = default;

std::shared_ptr<ProviderRegistry> InferenceClient::GetRegistry() const {
    return registry_;
}

void InferenceClient::SetRegistry(std::shared_ptr<ProviderRegistry> registry) {
    registry_ = std::move(registry);
    if (router_) {
        router_->SetRegistry(registry_);
    }
}

bool InferenceClient::RegisterProvider(ProviderPtr provider) {
    return registry_->RegisterProvider(provider);
}

bool InferenceClient::UnregisterProvider(const std::string& name) {
    return registry_->UnregisterProvider(name);
}

ProviderPtr InferenceClient::GetProvider(const std::string& name) const {
    return registry_->GetProvider(name);
}

std::shared_ptr<DeviceCloudRouter> InferenceClient::GetRouter() const {
    return router_;
}

void InferenceClient::SetRouter(std::shared_ptr<DeviceCloudRouter> router) {
    router_ = std::move(router);
    registry_ = router_->GetRegistry();
}

void InferenceClient::SetRoutingPolicy(const RoutingPolicy& policy) {
    policy_ = policy;
    if (router_) {
        router_->SetPolicy(policy);
    }
}

RoutingPolicy InferenceClient::GetRoutingPolicy() const {
    return policy_;
}

RoutingDecision InferenceClient::DecideRoute(const Request& req) const {
    if (!router_) {
        RoutingDecision d;
        d.reason = "No router configured";
        d.confidence = 0.0;
        return d;
    }
    auto decision = router_->Decide(req);
    {
        std::lock_guard<std::mutex> lock(last_decision_mutex_);
        last_decision_ = decision;
    }
    return decision;
}

ProviderPtr InferenceClient::SelectProvider(const Request& req) const {
    auto decision = DecideRoute(req);
    return decision.provider;
}

std::future<Response> InferenceClient::ExecuteAsync(const Request& req) {
    if (!router_) {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();
        Response resp;
        resp.status_code = -1;
        resp.body = "InferenceClient: no router configured";
        promise->set_value(std::move(resp));
        return future;
    }

    auto decision = DecideRoute(req);
    auto provider = decision.provider;

    if (!provider) {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();
        Response resp;
        resp.status_code = -1;
        resp.body = "InferenceClient: " + decision.reason;
        promise->set_value(std::move(resp));
        return future;
    }

    {
        std::lock_guard<std::mutex> lock(last_provider_mutex_);
        last_provider_name_ = provider->Name();
    }

    return router_->ExecuteAsync(req);
}

Response InferenceClient::Execute(const Request& req) {
    return ExecuteAsync(req).get();
}

std::future<Response> InferenceClient::ExecuteAsyncOn(
    const std::string& provider_name, const Request& req) {
    ProviderPtr provider = registry_->GetProvider(provider_name);
    if (!provider) {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();
        Response resp;
        resp.status_code = -1;
        resp.body = "InferenceClient: provider '" + provider_name + "' not found";
        promise->set_value(std::move(resp));
        return future;
    }

    {
        std::lock_guard<std::mutex> lock(last_provider_mutex_);
        last_provider_name_ = provider_name;
    }

    return provider->ExecuteAsync(req);
}

json InferenceClient::ChatCompletion(const std::vector<Message>& messages,
                                       const std::string& model,
                                       int max_tokens,
                                       double temperature) {
    json body;
    body["model"] = model;
    body["max_tokens"] = max_tokens;
    body["temperature"] = temperature;
    json j_msgs = json::array();
    for (const auto& m : messages) {
        j_msgs.push_back({{"role", m.role}, {"content", m.content}});
    }
    body["messages"] = j_msgs;

    Request req;
    req.method = "POST";
    req.endpoint = "/chat/completions";
    req.headers["Content-Type"] = "application/json";
    req.payload = body.dump();

    auto fut = ExecuteAsync(req);
    auto resp = fut.get();
    return resp.json_data.is_null()
               ? json{{"status", resp.status_code}, {"body", resp.body}}
               : resp.json_data;
}

json InferenceClient::ChatCompletionCustom(const json& request_body) {
    Request req;
    req.method = "POST";
    req.endpoint = "/chat/completions";
    req.headers["Content-Type"] = "application/json";
    req.payload = request_body.dump();

    auto fut = ExecuteAsync(req);
    auto resp = fut.get();
    return resp.json_data.is_null()
               ? json{{"status", resp.status_code}, {"body", resp.body}}
               : resp.json_data;
}

json InferenceClient::Completion(const std::string& prompt,
                                   const std::string& model,
                                   int max_tokens) {
    json body;
    body["model"] = model;
    body["prompt"] = prompt;
    body["max_tokens"] = max_tokens;

    Request req;
    req.method = "POST";
    req.endpoint = "/completions";
    req.headers["Content-Type"] = "application/json";
    req.payload = body.dump();

    auto fut = ExecuteAsync(req);
    auto resp = fut.get();
    return resp.json_data.is_null()
               ? json{{"status", resp.status_code}, {"body", resp.body}}
               : resp.json_data;
}

json InferenceClient::CreateEmbeddings(const std::string& input,
                                         const std::string& model) {
    json body;
    body["input"] = input;
    body["model"] = model;

    Request req;
    req.method = "POST";
    req.endpoint = "/embeddings";
    req.headers["Content-Type"] = "application/json";
    req.payload = body.dump();

    auto fut = ExecuteAsync(req);
    auto resp = fut.get();
    return resp.json_data.is_null()
               ? json{{"status", resp.status_code}, {"body", resp.body}}
               : resp.json_data;
}

json InferenceClient::ListModels() {
    Request req;
    req.method = "GET";
    req.endpoint = "/models";

    auto fut = ExecuteAsync(req);
    auto resp = fut.get();
    return resp.json_data.is_null()
               ? json{{"status", resp.status_code}, {"body", resp.body}}
               : resp.json_data;
}

json InferenceClient::CreateImage(const json& request_body) {
    Request req;
    req.method = "POST";
    req.endpoint = "/images/generations";
    req.headers["Content-Type"] = "application/json";
    req.payload = request_body.dump();

    auto fut = ExecuteAsync(req);
    auto resp = fut.get();
    return resp.json_data.is_null()
               ? json{{"status", resp.status_code}, {"body", resp.body}}
               : resp.json_data;
}

json InferenceClient::CreateSpeech(const std::string& input,
                                     const std::string& model,
                                     const std::string& voice,
                                     const std::string& response_format,
                                     double speed) {
    json body;
    body["model"] = model;
    body["input"] = input;
    body["voice"] = voice;
    body["response_format"] = response_format;
    body["speed"] = speed;

    Request req;
    req.method = "POST";
    req.endpoint = "/audio/speech";
    req.headers["Content-Type"] = "application/json";
    req.payload = body.dump();

    auto fut = ExecuteAsync(req);
    auto resp = fut.get();
    return resp.json_data.is_null()
               ? json{{"status", resp.status_code}, {"body", resp.body}}
               : resp.json_data;
}

json InferenceClient::TranscribeAudio(const std::string& filepath,
                                        const std::string& model) {
    json body;
    body["model"] = model;
    body["file"] = filepath;

    Request req;
    req.method = "POST";
    req.endpoint = "/audio/transcriptions";
    req.headers["Content-Type"] = "application/json";
    req.payload = body.dump();

    auto fut = ExecuteAsync(req);
    auto resp = fut.get();
    return resp.json_data.is_null()
               ? json{{"status", resp.status_code}, {"body", resp.body}}
               : resp.json_data;
}

BackendHealth InferenceClient::Health() const {
    if (!registry_) return BackendHealth::Unknown;
    BackendHealth overall = BackendHealth::Unknown;
    for (auto& p : registry_->GetAllProviders()) {
        auto h = p->Health();
        if (h == BackendHealth::Healthy) {
            overall = BackendHealth::Healthy;
        } else if (h == BackendHealth::Degraded &&
                   overall != BackendHealth::Healthy) {
            overall = BackendHealth::Degraded;
        } else if (h == BackendHealth::Unhealthy &&
                   overall == BackendHealth::Unknown) {
            overall = BackendHealth::Unhealthy;
        }
    }
    return overall;
}

BackendLoad InferenceClient::GetLoad() const {
    BackendLoad total;
    if (!registry_) return total;
    size_t count = 0;
    for (auto& p : registry_->GetAllProviders()) {
        auto load = p->GetLoad();
        total.active_requests += load.active_requests;
        total.total_requests += load.total_requests;
        total.failed_requests += load.failed_requests;
        total.avg_latency_ms += load.avg_latency_ms;
        count++;
    }
    if (count > 0) {
        total.avg_latency_ms /= static_cast<double>(count);
    }
    return total;
}

std::string InferenceClient::ActiveProviderName() const {
    std::lock_guard<std::mutex> lock(last_provider_mutex_);
    return last_provider_name_;
}

RoutingDecision InferenceClient::LastRoutingDecision() const {
    std::lock_guard<std::mutex> lock(last_decision_mutex_);
    return last_decision_;
}

size_t InferenceClient::ProviderCount() const {
    return registry_ ? registry_->ProviderCount() : 0;
}

size_t InferenceClient::AvailableProviderCount() const {
    return registry_ ? registry_->AvailableProviderCount() : 0;
}

} // namespace libopenai


