#pragma once

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <mutex>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>

#include "libopenai/transport.hpp"
#include "libopenai/inference_backend.hpp"

namespace libopenai {

using json = nlohmann::json;

/**
 * @brief Modality type for multi-modality request routing.
 *
 * Defines the kind of inference task a request represents. Providers
 * advertise which modalities they support, and the router selects
 * providers accordingly.
 */
enum class Modality {
    Text,          ///< Text generation (chat/completions)
    Chat,          ///< Chat completion (subset of text, for routing clarity)
    Image,         ///< Image generation or vision understanding
    Audio,         ///< Audio processing (speech, transcription, TTS)
    Video,         ///< Video understanding or generation
    Embedding,     ///< Text/image embeddings
    SpeechToText,  ///< Speech-to-text transcription
    TextToSpeech,  ///< Text-to-speech synthesis
    Any            ///< Matches any modality
};

/**
 * @brief Provider tier - where the inference actually executes.
 *
 * Device-tier providers run on local hardware (edge, mobile, embedded).
 * Cloud-tier providers run on remote servers. Hybrid providers can
 * do both depending on configuration.
 */
enum class ProviderTier {
    Device,   ///< On-device inference (litert, ncnn, cactus, executorch, onnxruntime)
    Cloud,    ///< Remote inference (rtp-llm, tensorrt-llm, vllm, sglang)
    Hybrid    ///< Can execute on either device or cloud
};

/**
 * @brief Provider capability flags.
 *
 * Describes what kinds of models and features a provider supports.
 */
struct ProviderCapabilities {
    std::set<Modality> supported_modalities;   // Modalities this provider handles
    std::set<std::string> supported_models;    // Model IDs/names (empty = any)
    bool supports_streaming = false;           // Token-by-token streaming
    bool supports_function_calling = false;    // Tool/function calls
    bool supports_json_mode = false;           // Structured JSON output
    int max_context_length = 4096;             // Max tokens in context
    double cost_per_1k_tokens = 0.0;           // Estimated cost (0 = free/local)
    double avg_latency_ms = 0.0;               // Typical response latency
    int max_concurrent_requests = 10;          // Max in-flight requests
};

/**
 * @brief ProviderInfo - static metadata about an inference provider.
 */
struct ProviderInfo {
    std::string name;                          // Unique provider name
    ProviderTier tier = ProviderTier::Cloud;   // Device/Cloud/Hybrid
    ProviderCapabilities capabilities;         // What it can do
    std::string endpoint_url;                  // For HTTP-based providers
    std::string api_key_env;                   // Env var for API key
    int priority = 100;                        // Lower = higher priority
    bool enabled = true;                       // Whether provider is active
};

/**
 * @brief InferenceProvider - abstract base for a named inference backend
 *        with associated metadata.
 *
 * A provider wraps an InferenceBackend and adds metadata about what
 * models, modalities, and features it supports. Multiple providers
 * can be active simultaneously, and the router selects among them.
 */
class InferenceProvider {
public:
    virtual ~InferenceProvider() = default;

    // ---- Identification ----
    virtual std::string Name() const = 0;
    virtual ProviderTier Tier() const = 0;
    virtual const ProviderInfo& Info() const = 0;

    // ---- Capability queries ----
    virtual bool SupportsModality(Modality m) const = 0;
    virtual bool SupportsModel(const std::string& model) const = 0;
    virtual bool SupportsStreaming() const = 0;
    virtual const ProviderCapabilities& Capabilities() const = 0;

    // ---- Request execution (delegates to wrapped backend) ----
    virtual std::future<Response> ExecuteAsync(const Request& req) = 0;
    virtual Response Execute(const Request& req) {
        return ExecuteAsync(req).get();
    }

    // ---- Health and load ----
    virtual BackendHealth Health() const = 0;
    virtual BackendLoad GetLoad() const = 0;

    // ---- Whether this provider should be considered for routing ----
    virtual bool IsAvailable() const = 0;

    // ---- Enable/disable ----
    virtual void SetEnabled(bool enabled) = 0;
    virtual bool IsEnabled() const = 0;
};

using ProviderPtr = std::shared_ptr<InferenceProvider>;

/**
 * @brief BackendProvider - adapter that wraps an InferenceBackend
 *        as an InferenceProvider.
 *
 * Useful when you have an InferenceBackend (e.g. EmbeddedBackend wrapping
 * llama.cpp) and want to register it in a ProviderRegistry or use it with
 * an InferenceClient.
 */
class BackendProvider : public InferenceProvider {
public:
    BackendProvider(ProviderInfo info, BackendPtr backend);

    ~BackendProvider() override = default;

    std::string Name() const override { return info_.name; }
    ProviderTier Tier() const override { return info_.tier; }
    const ProviderInfo& Info() const override { return info_; }

    bool SupportsModality(Modality m) const override;
    bool SupportsModel(const std::string& model) const override;
    bool SupportsStreaming() const override { return info_.capabilities.supports_streaming; }
    const ProviderCapabilities& Capabilities() const override { return info_.capabilities; }

    std::future<Response> ExecuteAsync(const Request& req) override;
    Response Execute(const Request& req) override;

    BackendHealth Health() const override;
    BackendLoad GetLoad() const override;
    bool IsAvailable() const override;

    void SetEnabled(bool enabled) override { info_.enabled = enabled; }
    bool IsEnabled() const override { return info_.enabled; }

    BackendPtr backend() const { return backend_; }

private:
    ProviderInfo info_;
    BackendPtr backend_;
    mutable std::mutex availability_mutex_;
    bool available_ = true;
};

/**
 * @brief HttpInferenceProvider - an HTTP-based inference provider.
 *
 * Wraps an HttpInferenceBackend with full metadata. Suitable for
 * cloud backends (rtp-llm, tensorrt-llm, vllm, sglang, etc.) and
 * localhost servers.
 */
class HttpInferenceProvider : public InferenceProvider {
public:
    HttpInferenceProvider(ProviderInfo info,
                          std::shared_ptr<ITransport> transport);

    ~HttpInferenceProvider() override = default;

    std::string Name() const override { return info_.name; }
    ProviderTier Tier() const override { return info_.tier; }
    const ProviderInfo& Info() const override { return info_; }

    bool SupportsModality(Modality m) const override;
    bool SupportsModel(const std::string& model) const override;
    bool SupportsStreaming() const override { return info_.capabilities.supports_streaming; }
    const ProviderCapabilities& Capabilities() const override { return info_.capabilities; }

    std::future<Response> ExecuteAsync(const Request& req) override;
    Response Execute(const Request& req) override;

    BackendHealth Health() const override;
    BackendLoad GetLoad() const override;
    bool IsAvailable() const override;

    const std::string& BaseUrl() const { return backend_->BaseUrl(); }

    void SetEnabled(bool enabled) { info_.enabled = enabled; }
    bool IsEnabled() const { return info_.enabled; }

private:
    ProviderInfo info_;
    std::shared_ptr<HttpInferenceBackend> backend_;
    mutable std::mutex availability_mutex_;
    bool available_ = true;
};

/**
 * @brief OnDeviceInferenceProvider - an on-device inference provider.
 *
 * Wraps an InProcessInferenceBackend with metadata for edge/device
 * execution (litert, ncnn, cactus-engine, executorch, onnxruntime).
 * These providers run models locally without network round-trips.
 */
class OnDeviceInferenceProvider : public InferenceProvider {
public:
    OnDeviceInferenceProvider(ProviderInfo info,
                               std::string model_path = "",
                               int n_ctx = 4096);

    ~OnDeviceInferenceProvider() override = default;

    std::string Name() const override { return info_.name; }
    ProviderTier Tier() const override { return info_.tier; }
    const ProviderInfo& Info() const override { return info_; }

    bool SupportsModality(Modality m) const override;
    bool SupportsModel(const std::string& model) const override;
    bool SupportsStreaming() const override { return info_.capabilities.supports_streaming; }
    const ProviderCapabilities& Capabilities() const override { return info_.capabilities; }

    std::future<Response> ExecuteAsync(const Request& req) override;
    Response Execute(const Request& req) override;

    BackendHealth Health() const override;
    BackendLoad GetLoad() const override;
    bool IsAvailable() const override;

    const std::string& ModelPath() const { return backend_->ModelPath(); }

    void SetEnabled(bool enabled) { info_.enabled = enabled; }
    bool IsEnabled() const { return info_.enabled; }

private:
    ProviderInfo info_;
    std::shared_ptr<InProcessInferenceBackend> backend_;
    mutable std::mutex availability_mutex_;
    bool available_ = true;
};

} // namespace libopenai