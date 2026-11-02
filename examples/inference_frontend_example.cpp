#include "libopenai/libopenai.hpp"
#include "libopenai/inference_backend.hpp"
#include "libopenai/inference_provider.hpp"
#include "libopenai/provider_registry.hpp"
#include "libopenai/modality_router.hpp"
#include "libopenai/device_cloud_router.hpp"
#include "libopenai/inference_client.hpp"

#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <chrono>

using namespace libopenai;

int main() {
    std::cout << "=== libopenai Inference Serving Frontend (Provider-Based) ===\n\n";

    // ------------------------------------------------------------------
    // 1. Create a transport
    // ------------------------------------------------------------------
    auto transport = CreateHttplibTransport();

    // ------------------------------------------------------------------
    // 2. Create a registry and register providers
    // ------------------------------------------------------------------
    auto registry = std::make_shared<ProviderRegistry>();

    // Device provider (e.g. LiteRT on-device)
    ProviderInfo device_info;
    device_info.name = "litert-device";
    device_info.tier = ProviderTier::Device;
    device_info.capabilities.supported_modalities = {Modality::Text, Modality::Embedding};
    device_info.capabilities.supported_models = {"llama-3.1-8b", "text-embedding-3-small"};
    device_info.capabilities.supports_streaming = true;
    device_info.capabilities.max_context_length = 8192;
    device_info.capabilities.cost_per_1k_tokens = 0.0;   // local = free
    device_info.capabilities.avg_latency_ms = 50.0;
    device_info.capabilities.max_concurrent_requests = 4;
    device_info.endpoint_url = "";
    device_info.priority = 10;  // high priority (low number)
    device_info.enabled = true;

    auto device_provider = std::make_shared<OnDeviceInferenceProvider>(
        device_info, "/models/llama-3.1-8b.tflite", 8192);
    registry->RegisterProvider(device_provider);

    // Cloud provider (e.g. vLLM)
    ProviderInfo cloud_info;
    cloud_info.name = "vllm-cloud";
    cloud_info.tier = ProviderTier::Cloud;
    cloud_info.capabilities.supported_modalities = {Modality::Text, Modality::Image, Modality::Audio};
    cloud_info.capabilities.supported_models = {};  // accepts any model
    cloud_info.capabilities.supports_streaming = true;
    cloud_info.capabilities.max_context_length = 32768;
    cloud_info.capabilities.cost_per_1k_tokens = 0.0015;
    cloud_info.capabilities.avg_latency_ms = 300.0;
    cloud_info.capabilities.max_concurrent_requests = 100;
    cloud_info.endpoint_url = "https://api.example.com/v1";
    cloud_info.api_key_env = "OPENAI_API_KEY";
    cloud_info.priority = 50;
    cloud_info.enabled = true;

    auto cloud_provider = std::make_shared<HttpInferenceProvider>(
        cloud_info, transport);
    registry->RegisterProvider(cloud_provider);

    // ------------------------------------------------------------------
    // 3. Create a DeviceCloudRouter with collaborative policy
    // ------------------------------------------------------------------
    RoutingPolicy policy;
    policy.prefer_device = true;
    policy.allow_cloud_fallback = true;
    policy.allow_device_offload = true;
    policy.max_device_load = 0.7;
    policy.max_cloud_latency_ms = 5000.0;
    policy.privacy_mode = false;
    policy.battery_saver = false;
    policy.offline_mode = false;
    policy.require_quality = false;

    auto router = std::make_shared<DeviceCloudRouter>(registry, policy);

    // ------------------------------------------------------------------
    // 4. Create the InferenceClient
    // ------------------------------------------------------------------
    auto client = std::make_shared<InferenceClient>(router);
    client->SetRoutingPolicy(policy);

    // ------------------------------------------------------------------
    // 5. Execute a chat completion (will be routed by device-cloud policy)
    // ------------------------------------------------------------------
    std::cout << "--- Chat Completion (device-cloud collaborative routing) ---\n";

    Request req1;
    req1.method = "POST";
    req1.endpoint = "/chat/completions";
    req1.headers["Content-Type"] = "application/json";
    req1.payload = R"({"model":"llama-3.1-8b","messages":[{"role":"user","content":"Hello!"}],"max_tokens":50})";

    auto decision1 = client->DecideRoute(req1);
    std::cout << "Routing decision: " << decision1.reason << "\n";
    std::cout << "Provider: " << (decision1.provider ? decision1.provider->Name() : "none") << "\n";
    std::cout << "Tier: " << (decision1.tier == ProviderTier::Device ? "device" : "cloud") << "\n";
    std::cout << "Confidence: " << decision1.confidence << "\n\n";

    auto fut1 = client->ExecuteAsync(req1);
    auto resp1 = fut1.get();
    std::cout << "Status: " << resp1.status_code << "\n";
    std::cout << "Body: " << resp1.body.substr(0, 200) << "\n";
    std::cout << "Active provider: " << client->ActiveProviderName() << "\n\n";

    // ------------------------------------------------------------------
    // 6. Concurrent requests - multiple ExecuteAsync calls in flight
    // ------------------------------------------------------------------
    std::cout << "--- Concurrent Requests (5 parallel) ---\n";

    std::vector<std::future<Response>> futures;
    for (int i = 0; i < 5; i++) {
        Request req;
        req.method = "POST";
        req.endpoint = "/chat/completions";
        req.headers["Content-Type"] = "application/json";
        req.payload = R"({"model":"llama-3.1-8b","messages":[{"role":"user","content":"Request )" + std::to_string(i) + R"("}],"max_tokens":50})";
        futures.push_back(client->ExecuteAsync(req));
    }

    for (size_t i = 0; i < futures.size(); i++) {
        auto resp = futures[i].get();
        std::cout << "  Response " << (i + 1) << ": status=" << resp.status_code
                  << " provider=" << client->ActiveProviderName() << "\n";
    }
    std::cout << "\n";

    // ------------------------------------------------------------------
    // 7. Multi-modality routing: image request
    // ------------------------------------------------------------------
    std::cout << "--- Multi-Modality Routing (Image) ---\n";

    Request img_req;
    img_req.method = "POST";
    img_req.endpoint = "/images/generations";
    img_req.headers["Content-Type"] = "application/json";
    img_req.payload = R"({"model":"dall-e-3","prompt":"A futuristic city","n":1,"size":"1024x1024"})";

    auto img_decision = client->DecideRoute(img_req);
    std::cout << "Image routing: " << img_decision.reason << "\n";
    std::cout << "Provider: " << (img_decision.provider ? img_decision.provider->Name() : "none") << "\n";
    std::cout << "Tier: " << (img_decision.tier == ProviderTier::Device ? "device" : "cloud") << "\n\n";

    // ------------------------------------------------------------------
    // 8. Observability
    // ------------------------------------------------------------------
    std::cout << "--- Observability ---\n";
    std::cout << "Provider count: " << client->ProviderCount() << "\n";
    std::cout << "Available providers: " << client->AvailableProviderCount() << "\n";
    std::cout << "Client health: " << (client->Health() == BackendHealth::Healthy ? "healthy" : "not healthy") << "\n";
    auto load = client->GetLoad();
    std::cout << "Active requests: " << load.active_requests << "\n";
    std::cout << "Total requests: " << load.total_requests << "\n";
    std::cout << "Failed requests: " << load.failed_requests << "\n";
    std::cout << "Avg latency: " << load.avg_latency_ms << " ms\n";
    std::cout << "Device decisions: " << router->DeviceDecisions() << "\n";
    std::cout << "Cloud decisions: " << router->CloudDecisions() << "\n";
    std::cout << "Fallback decisions: " << router->FallbackDecisions() << "\n\n";

    std::cout << "=== Example complete ===\n";
    return 0;
}