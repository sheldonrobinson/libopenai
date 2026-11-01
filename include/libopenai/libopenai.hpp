#pragma once

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <memory>
#include <functional>
#include <optional>
#include <mutex>
#include <nlohmann/json.hpp>

#include "libopenai/transport.hpp"

namespace libopenai {

class Client {
public:
    /**
     * @brief Construct a new OpenAI Client.
     * @param api_key OpenAI API key (if empty, reads from OPENAI_API_KEY env var).
     * @param base_url Base URL for the API (default: https://api.openai.com/v1).
     * @param transport Optional shared transport. If null, a default transport
     *                  is created based on compile-time configuration.
     */
    Client(const std::string& api_key = "",
           const std::string& base_url = "https://api.openai.com/v1",
           std::shared_ptr<ITransport> transport = nullptr);

    // Configuration (thread-safe with mutex)
    void SetApiKey(const std::string& api_key) { std::lock_guard<std::mutex> lock(mutex_); api_key_ = api_key; }
    void SetBaseUrl(const std::string& base_url) { std::lock_guard<std::mutex> lock(mutex_); base_url_ = base_url; }
    void SetOrganization(const std::string& organization) { std::lock_guard<std::mutex> lock(mutex_); organization_ = organization; }
    void SetProject(const std::string& project) { std::lock_guard<std::mutex> lock(mutex_); project_ = project; }
    void SetClientRequestId(const std::string& client_request_id) { std::lock_guard<std::mutex> lock(mutex_); client_request_id_ = client_request_id; }
    void SetTimeout(long timeout_seconds) { std::lock_guard<std::mutex> lock(mutex_); timeout_seconds_ = timeout_seconds; }
    void SetProxy(const std::string& proxy_url) { std::lock_guard<std::mutex> lock(mutex_); proxy_url_ = proxy_url; }

    // Streaming callback type: bool callback(const std::string& data)
    using StreamCallback = std::function<bool(const std::string&)>;
    void SetStreamCallback(StreamCallback cb) { std::lock_guard<std::mutex> lock(mutex_); stream_callback_ = cb; }

    // --- Core HTTP Methods ---
    Response Get(const std::string& endpoint, const std::map<std::string, std::string>& query_params = {});
    Response Post(const std::string& endpoint, const json& body);
    Response PostMultipart(const std::string& endpoint, const std::map<std::string, std::string>& text_fields, const std::map<std::string, std::string>& file_fields);
    Response Delete(const std::string& endpoint);
    bool Download(const std::string& endpoint, const std::string& save_filepath);

    // --- Decisions ---
    // POST /decisions - Classify or score input with predicate/choice/score questions
    json CreateDecision(const json& request_body);

    // --- Chat Completions (/chat/completions) ---
    json ChatCompletion(const std::vector<Message>& messages,
                        const std::string& model = "gpt-4o-mini",
                        int max_tokens = 512,
                        double temperature = 1.0);
    json ChatCompletionCustom(const json& request_body);

    // --- Completions / Legacy (/completions) ---
    json Completion(const std::string& prompt,
                    const std::string& model = "gpt-3.5-turbo-instruct",
                    int max_tokens = 16);
    json CompletionCustom(const json& request_body);

    // --- Embeddings (/embeddings) ---
    json CreateEmbeddings(const std::string& input,
                          const std::string& model = "text-embedding-3-small",
                          const std::optional<int>& dimensions = std::nullopt,
                          const std::optional<std::string>& encoding_format = std::nullopt,
                          const std::optional<std::string>& user = std::nullopt);
    json CreateEmbeddingsBatch(const std::vector<std::string>& inputs,
                               const std::string& model = "text-embedding-3-small",
                               const std::optional<int>& dimensions = std::nullopt,
                               const std::optional<std::string>& encoding_format = std::nullopt,
                               const std::optional<std::string>& user = std::nullopt);

    // --- Audio (/audio/speech, /audio/transcriptions, /audio/translations) ---
    bool CreateSpeech(const std::string& input,
                      const std::string& model = "gpt-4o-mini-tts",
                      const std::optional<json>& voice = std::nullopt,
                      const std::optional<std::string>& instructions = std::nullopt,
                      const std::string& response_format = "mp3",
                      double speed = 1.0,
                      const std::string& save_filepath = "speech.mp3",
                      const std::optional<std::string>& stream_format = std::nullopt);
    json CreateTranscription(const std::string& filepath,
                             const std::string& model = "gpt-4o-transcribe",
                             const std::optional<std::string>& prompt = std::nullopt,
                             const std::optional<std::string>& language = std::nullopt,
                             const std::optional<std::string>& response_format = std::nullopt,
                             const std::optional<json>& chunking_strategy = std::nullopt,
                             const std::optional<json>& include = std::nullopt,
                             const std::optional<json>& keywords = std::nullopt,
                             const std::optional<json>& known_speaker_names = std::nullopt,
                             const std::optional<json>& known_speaker_references = std::nullopt,
                             const std::optional<json>& languages = std::nullopt,
                             const std::optional<bool>& stream = std::nullopt,
                             double temperature = 0.0,
                             const std::optional<json>& timestamp_granularities = std::nullopt);
    json CreateTranslation(const std::string& filepath,
                           const std::string& model = "whisper-1",
                           const std::optional<std::string>& prompt = std::nullopt,
                           const std::optional<std::string>& response_format = std::nullopt,
                           double temperature = 0.0);

    // --- Images (/images/generations, /images/edits, /images/variations) ---
    json CreateImage(const json& request_body);
    json CreateImageEdit(const json& request_body);
    json CreateImageVariation(const std::string& image_path,
                              const std::optional<std::string>& model = std::nullopt,
                              int n = 1,
                              const std::optional<std::string>& response_format = std::nullopt,
                              const std::optional<std::string>& size = std::nullopt,
                              const std::optional<std::string>& user = std::nullopt);

    // --- Files (/files, /files/{file_id}, /files/{file_id}/content) ---
    json ListFiles();
    json UploadFile(const std::string& filepath, const std::string& purpose = "fine-tune");
    json DeleteFile(const std::string& file_id);
    json RetrieveFile(const std::string& file_id);
    bool DownloadFileContent(const std::string& file_id, const std::string& save_filepath);

    // --- Fine-tuning (/fine_tuning/jobs, etc.) ---
    json CreateFineTuningJob(const std::string& training_file_id, const std::string& model = "gpt-4o-mini");
    json ListFineTuningJobs();
    json RetrieveFineTuningJob(const std::string& job_id);
    json CancelFineTuningJob(const std::string& job_id);
    json ListFineTuningEvents(const std::string& job_id);

    // --- Models (/models, /models/{model}) ---
    json ListModels();
    json RetrieveModel(const std::string& model_id);
    json DeleteModel(const std::string& model_id);

    // --- Moderations (/moderations) ---
    json CreateModeration(const std::string& input,
                          const std::optional<std::string>& model = std::nullopt);
    json CreateModeration(const json& request_body);

    // --- Batches (/batches) ---
    json CreateBatch(const std::string& input_file_id,
                     const std::string& endpoint = "/v1/chat/completions",
                     const std::string& completion_window = "24h");
    json RetrieveBatch(const std::string& batch_id);
    json CancelBatch(const std::string& batch_id);
    json ListBatches();

    // --- Assistants, Threads, Messages, Runs (Beta API) ---
    json CreateAssistant(const std::string& model, const std::string& name = "", const std::string& instructions = "");
    json RetrieveAssistant(const std::string& assistant_id);
    json ModifyAssistant(const std::string& assistant_id, const json& body);
    json DeleteAssistant(const std::string& assistant_id);
    json ListAssistants();

    json CreateThread(const std::vector<Message>& messages = {});
    json RetrieveThread(const std::string& thread_id);
    json ModifyThread(const std::string& thread_id, const json& body);
    json DeleteThread(const std::string& thread_id);

    json CreateMessage(const std::string& thread_id, const std::string& role, const std::string& content);
    json ListMessages(const std::string& thread_id);
    json RetrieveMessage(const std::string& thread_id, const std::string& message_id);

    json CreateRun(const std::string& thread_id, const std::string& assistant_id);
    json RetrieveRun(const std::string& thread_id, const std::string& run_id);
    json CancelRun(const std::string& thread_id, const std::string& run_id);
    json ListRuns(const std::string& thread_id);

    // --- Realtime Translations ---
    // POST /realtime/translations/client_secrets - Create a client secret for Realtime Translation
    json CreateRealtimeTranslationClientSecret(const json& request_body);

    // Helper to build the Realtime Translations WebSocket URL
    std::string RealtimeTranslationWebSocketUrl(const std::string& model) const;

    // =====================================================================
    // Generic Custom-Endpoint Methods
    // These allow calling OpenAI-compatible endpoints on any base URL
    // (e.g. Typesafe System One, Azure OpenAI, custom proxies).
    // Shared endpoints: GET /v1/models, POST /v1/chat/completions, POST /v1/embeddings
    // =====================================================================

    // GET /v1/models - List models on a custom base URL
    json ListModelsCustom(const std::string& base_url, const std::string& api_key);

    // POST /v1/chat/completions - Create a chat completion on a custom base URL
    json CreateChatCompletionCustom(const std::string& base_url,
                                    const std::string& api_key,
                                    const json& request_body);

    // POST /v1/embeddings - Create embeddings on a custom base URL
    json CreateEmbeddingsCustom(const std::string& base_url,
                                const std::string& api_key,
                                const json& request_body);

    // =====================================================================
    // System One API (custom base URL / API key)
    // =====================================================================

    // POST /v1/chat/completions - System One chat completion
    json CreateSystemOneChatCompletion(const json& request_body);
    // GET /v1/models - List System One models
    json ListSystemOneModels();
    // POST /v1/embeddings - System One embeddings
    json CreateSystemOneEmbeddings(const json& request_body);

    // =====================================================================
    // Anthropic Messages API
    // Base URL: https://api.anthropic.com (set via SetAnthropicBaseUrl)
    // Auth: x-api-key header (set via SetAnthropicApiKey)
    // Required header: anthropic-version: 2023-06-01
    // Note: Different request/response format from OpenAI - kept provider-specific.
    // =====================================================================

    void SetAnthropicApiKey(const std::string& api_key) { std::lock_guard<std::mutex> lock(mutex_); anthropic_api_key_ = api_key; }
    void SetAnthropicBaseUrl(const std::string& base_url) { std::lock_guard<std::mutex> lock(mutex_); anthropic_base_url_ = base_url; }
    void SetAnthropicVersion(const std::string& version) { std::lock_guard<std::mutex> lock(mutex_); anthropic_version_ = version; }

    // POST /v1/messages - Create a message
    json CreateAnthropicMessage(const json& request_body);
    // GET /v1/messages/{message_id} - Retrieve a message
    json GetAnthropicMessage(const std::string& message_id);

    // =====================================================================
    // Anthropic Messages Batch API
    // =====================================================================

    // POST /v1/batches - Create a batch
    json CreateAnthropicBatch(const json& request_body);
    // GET /v1/batches/{batch_id} - Retrieve a batch
    json GetAnthropicBatch(const std::string& batch_id);
    // DELETE /v1/batches/{batch_id} - Cancel a batch
    json CancelAnthropicBatch(const std::string& batch_id);
    // GET /v1/batches - List batches
    json ListAnthropicBatches(const std::map<std::string, std::string>& query_params = {});

    /**
     * @brief Returns the name of the currently active transport backend.
     */
    std::string TransportName() const;

private:
    std::shared_ptr<ITransport> transport_;
    mutable std::mutex mutex_;
    std::string api_key_;
    std::string base_url_;
    std::string organization_;
    std::string project_;
    std::string client_request_id_;
    long timeout_seconds_ = 60;
    std::string proxy_url_;
    StreamCallback stream_callback_;

    // System One-specific configuration
    std::string system_one_api_key_;
    std::string system_one_base_url_;

    // Anthropic-specific configuration
    std::string anthropic_api_key_;
    std::string anthropic_base_url_ = "https://api.anthropic.com";
    std::string anthropic_version_ = "2023-06-01";

    Response ExecuteRequest(const std::string& method, const std::string& endpoint, const std::string& payload = "",
                            const std::map<std::string, std::string>& multipart_text = {},
                            const std::map<std::string, std::string>& multipart_files = {},
                            bool is_download = false, const std::string& download_path = "");

    // Execute with custom base URL and headers
    Response ExecuteRequestCustom(const std::string& base_url,
                                  const std::string& method,
                                  const std::string& endpoint,
                                  const std::map<std::string, std::string>& extra_headers,
                                  const std::string& payload = "",
                                  const std::map<std::string, std::string>& multipart_text = {},
                                  const std::map<std::string, std::string>& multipart_files = {},
                                  bool is_download = false, const std::string& download_path = "");
};

} // namespace libopenai
