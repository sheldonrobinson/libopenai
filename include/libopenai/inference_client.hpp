#pragma once

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <string>
#include <memory>
#include <future>
#include <map>
#include <nlohmann/json.hpp>

#include "libopenai/libopenai.hpp"
#include "libopenai/inference_backend.hpp"
#include "libopenai/inference_provider.hpp"
#include "libopenai/provider_registry.hpp"
#include "libopenai/modality_router.hpp"
#include "libopenai/device_cloud_router.hpp"
#include "libopenai/router.hpp"

namespace libopenai {

using json = nlohmann::json;

/**
 * @brief InferenceClient - high-level inference serving frontend.
 *
 * The InferenceClient is the main entry point for applications that want to
 * send inference requests to one or more backends. It supports:
 *
 *   1. Multiple active backends via ProviderRegistry
 *   2. Multi-modality request routing (text, image, audio, video, etc.)
 *   3. Device-cloud collaborative inferencing (Konstrukt pattern)
 *   4. Automatic failover between device and cloud providers
 *   5. Backward-compatible Router/Backend API
 *
 * Usage:
 *
 *   // Simple: single HTTP backend
 *   auto transport = CreateHttplibTransport();
 *   auto provider = std::make_shared<HttpInferenceProvider>(
 *       {"local-llama", ProviderTier::Device, {}, "http://localhost:8080/v1"},
 *       transport);
 *
 *   auto registry = std::make_shared<ProviderRegistry>();
 *   registry->RegisterProvider(provider);
 *
 *   auto router = std::make_shared<DeviceCloudRouter>(registry);
 *
 *   auto client = std::make_shared<InferenceClient>(router);
 *
 *   auto future = client->ExecuteAsync(request);
 *   auto response = future.get();
 *
 *   // Multi-provider with device-cloud routing
 *   auto device = std::make_shared<OnDeviceInferenceProvider>(
 *       {"litert-device", ProviderTier::Device, caps, "", "/models/llama.tflite"});
 *   auto cloud = std::make_shared<HttpInferenceProvider>(
 *       {"vllm-cloud", ProviderTier::Cloud, caps, "https://api.example.com/v1"},
 *       transport);
 *
 *   registry->RegisterProvider(device);
 *   registry->RegisterProvider(cloud);
 *
 *   // Route with device preference, cloud fallback
 *   RoutingPolicy policy{.prefer_device = true, .allow_cloud_fallback = true};
 *   auto client2 = std::make_shared<InferenceClient>(router);
 *   client2->SetRoutingPolicy(policy);
 */
class InferenceClient {
public:
    InferenceClient();
    explicit InferenceClient(std::shared_ptr<DeviceCloudRouter> router);
    ~InferenceClient();

    // ---- Provider Management ----

    std::shared_ptr<ProviderRegistry> GetRegistry() const;
    void SetRegistry(std::shared_ptr<ProviderRegistry> registry);

    // Register a provider
    bool RegisterProvider(ProviderPtr provider);

    // Unregister a provider by name
    bool UnregisterProvider(const std::string& name);

    // Get a provider by name
    ProviderPtr GetProvider(const std::string& name) const;

    // ---- Routing ----

    std::shared_ptr<DeviceCloudRouter> GetRouter() const;
    void SetRouter(std::shared_ptr<DeviceCloudRouter> router);
    void SetRoutingPolicy(const RoutingPolicy& policy);
    RoutingPolicy GetRoutingPolicy() const;

    // Get the routing decision for a request without executing
    RoutingDecision DecideRoute(const Request& req) const;

    // ---- Request Execution ----

    // Execute a request asynchronously. The router selects the provider.
    std::future<Response> ExecuteAsync(const Request& req);

    // Execute a request synchronously (blocking)
    Response Execute(const Request& req);

    // Execute a request on a specific provider by name
    std::future<Response> ExecuteAsyncOn(const std::string& provider_name,
                                         const Request& req);

    // ---- OpenAI-compatible API methods ----

    json ChatCompletion(const std::vector<Message>& messages,
                        const std::string& model = "gpt-4o-mini",
                        int max_tokens = 512,
                        double temperature = 1.0);

    json ChatCompletionCustom(const json& request_body);

    json Completion(const std::string& prompt,
                    const std::string& model = "gpt-3.5-turbo-instruct",
                    int max_tokens = 16);

    json CreateEmbeddings(const std::string& input,
                          const std::string& model = "text-embedding-3-small");

    json ListModels();

    // Multi-modality convenience methods
    json CreateImage(const json& request_body);
    json CreateSpeech(const std::string& input,
                      const std::string& model = "tts-1",
                      const std::string& voice = "alloy",
                      const std::string& response_format = "mp3",
                      double speed = 1.0);
    json TranscribeAudio(const std::string& filepath,
                         const std::string& model = "whisper-1");

    // ---- Health and Observability ----

    BackendHealth Health() const;
    BackendLoad GetLoad() const;
    std::string ActiveProviderName() const;
    RoutingDecision LastRoutingDecision() const;

    size_t ProviderCount() const;
    size_t AvailableProviderCount() const;

private:
    std::shared_ptr<DeviceCloudRouter> router_;
    std::shared_ptr<ProviderRegistry> registry_;
    RoutingPolicy policy_;

    mutable std::string last_provider_name_;
    mutable std::mutex last_provider_mutex_;
    mutable RoutingDecision last_decision_;
    mutable std::mutex last_decision_mutex_;

    ProviderPtr SelectProvider(const Request& req) const;
};

} // namespace libopenai