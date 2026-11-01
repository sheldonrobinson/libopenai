#include "libopenai/libopenai.hpp"

#ifdef DeleteFile
#undef DeleteFile
#endif

namespace libopenai {

Client::Client(const std::string& api_key, const std::string& base_url, std::shared_ptr<ITransport> transport)
    : api_key_(api_key), base_url_(base_url), transport_(transport) {
    if (api_key_.empty()) {
        if (const char* env_key = std::getenv("OPENAI_API_KEY")) {
            api_key_ = env_key;
        }
    }
    if (!transport_) {
#if defined(LIBOPENAI_USE_NAMED_PIPE)
        transport_ = CreateNamedPipeTransport("llama-openai-pipe");
#elif defined(LIBOPENAI_USE_BEAST)
        transport_ = CreateBeastTransport();
#elif defined(LIBOPENAI_USE_WORKFLOW)
        transport_ = CreateWorkflowTransport();
#elif defined(LIBOPENAI_USE_CPPSERVER)
        transport_ = CreateCppServerTransport();
#elif defined(LIBOPENAI_USE_CURL)
        transport_ = CreateCurlTransport();
#elif defined(LIBOPENAI_USE_MONGOOSE)
        transport_ = CreateMongooseTransport();
#elif defined(LIBOPENAI_USE_CIVETWEB)
        transport_ = CreateCivetwebTransport();
#else
        transport_ = CreateHttplibTransport();
#endif
    }
}

Response Client::ExecuteRequest(const std::string& method, const std::string& endpoint, const std::string& payload,
                              const std::map<std::string, std::string>& multipart_text,
                              const std::map<std::string, std::string>& multipart_files,
                              bool is_download, const std::string& download_path) {
    std::string l_base_url, l_api_key, l_org, l_proj, l_req_id, l_proxy;
    long l_timeout;
    Client::StreamCallback l_stream_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_base_url = base_url_;
        l_api_key = api_key_;
        l_org = organization_;
        l_proj = project_;
        l_req_id = client_request_id_;
        l_timeout = timeout_seconds_;
        l_proxy = proxy_url_;
        l_stream_cb = stream_callback_;
    }

    Request req;
    req.method = method;
    req.url = l_base_url;
    if (!endpoint.empty()) {
        if (l_base_url.back() != '/' && endpoint.front() != '/') {
            req.url += "/" + endpoint;
        } else if (l_base_url.back() == '/' && endpoint.front() == '/') {
            req.url += endpoint.substr(1);
        } else {
            req.url += endpoint;
        }
    }
    req.endpoint = endpoint;

    if (!l_api_key.empty()) {
        req.headers["Authorization"] = "Bearer " + l_api_key;
    }
    if (!l_org.empty()) {
        req.headers["OpenAI-Organization"] = l_org;
    }
    if (!l_proj.empty()) {
        req.headers["OpenAI-Project"] = l_proj;
    }
    if (!l_req_id.empty()) {
        req.headers["X-Client-Request-Id"] = l_req_id;
    }

    req.payload = payload;
    req.multipart_text = multipart_text;
    req.multipart_files = multipart_files;
    req.is_download = is_download;
    req.download_path = download_path;
    req.timeout_seconds = l_timeout;
    req.proxy_url = l_proxy;
    req.stream_callback = l_stream_cb;

    return transport_->Execute(req);
}

Response Client::Get(const std::string& endpoint, const std::map<std::string, std::string>& query_params) {
    std::string ep = endpoint;
    if (!query_params.empty()) {
        ep += "?";
        bool first = true;
        for (const auto& q : query_params) {
            if (!first) ep += "&";
            ep += q.first + "=" + q.second;
            first = false;
        }
    }
    return ExecuteRequest("GET", ep);
}

Response Client::Post(const std::string& endpoint, const json& body) {
    return ExecuteRequest("POST", endpoint, body.dump());
}

Response Client::PostMultipart(const std::string& endpoint, const std::map<std::string, std::string>& text_fields, const std::map<std::string, std::string>& file_fields) {
    return ExecuteRequest("POST", endpoint, "", text_fields, file_fields);
}

Response Client::Delete(const std::string& endpoint) {
    return ExecuteRequest("DELETE", endpoint);
}

bool Client::Download(const std::string& endpoint, const std::string& save_filepath) {
    Response resp = ExecuteRequest("GET", endpoint, "", {}, {}, true, save_filepath);
    return !resp.is_error();
}

std::string Client::TransportName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transport_ ? transport_->Name() : "none";
}

// =====================================================================
// Decisions
// =====================================================================

json Client::CreateDecision(const json& request_body) {
    Response resp = Post("/decisions", request_body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Chat Completions
// =====================================================================

json Client::ChatCompletion(const std::vector<Message>& messages, const std::string& model, int max_tokens, double temperature) {
    json body;
    body["model"] = model;
    body["max_tokens"] = max_tokens;
    body["temperature"] = temperature;
    json j_msgs = json::array();
    for (const auto& m : messages) {
        j_msgs.push_back({{"role", m.role}, {"content", m.content}});
    }
    body["messages"] = j_msgs;
    Response resp = Post("/chat/completions", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ChatCompletionCustom(const json& request_body) {
    Response resp = Post("/chat/completions", request_body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Completions (Legacy)
// =====================================================================

json Client::Completion(const std::string& prompt, const std::string& model, int max_tokens) {
    json body;
    body["model"] = model;
    body["prompt"] = prompt;
    body["max_tokens"] = max_tokens;
    Response resp = Post("/completions", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CompletionCustom(const json& request_body) {
    Response resp = Post("/completions", request_body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Embeddings
// =====================================================================

json Client::CreateEmbeddings(const std::string& input,
                              const std::string& model,
                              const std::optional<int>& dimensions,
                              const std::optional<std::string>& encoding_format,
                              const std::optional<std::string>& user) {
    json body;
    body["input"] = input;
    body["model"] = model;
    if (dimensions.has_value()) body["dimensions"] = *dimensions;
    if (encoding_format.has_value()) body["encoding_format"] = *encoding_format;
    if (user.has_value()) body["user"] = *user;
    Response resp = Post("/embeddings", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateEmbeddingsBatch(const std::vector<std::string>& inputs,
                                   const std::string& model,
                                   const std::optional<int>& dimensions,
                                   const std::optional<std::string>& encoding_format,
                                   const std::optional<std::string>& user) {
    json body;
    body["input"] = inputs;
    body["model"] = model;
    if (dimensions.has_value()) body["dimensions"] = *dimensions;
    if (encoding_format.has_value()) body["encoding_format"] = *encoding_format;
    if (user.has_value()) body["user"] = *user;
    Response resp = Post("/embeddings", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Audio
// =====================================================================

bool Client::CreateSpeech(const std::string& input,
                          const std::string& model,
                          const std::optional<json>& voice,
                          const std::optional<std::string>& instructions,
                          const std::string& response_format,
                          double speed,
                          const std::string& save_filepath,
                          const std::optional<std::string>& stream_format) {
    json body;
    body["input"] = input;
    body["model"] = model;
    if (voice.has_value()) body["voice"] = *voice;
    if (instructions.has_value()) body["instructions"] = *instructions;
    body["response_format"] = response_format;
    body["speed"] = speed;
    if (stream_format.has_value()) body["stream_format"] = *stream_format;

    Response resp = ExecuteRequest("POST", "/audio/speech", body.dump(), {}, {}, true, save_filepath);
    return !resp.is_error();
}

json Client::CreateTranscription(const std::string& filepath,
                                 const std::string& model,
                                 const std::optional<std::string>& prompt,
                                 const std::optional<std::string>& language,
                                 const std::optional<std::string>& response_format,
                                 const std::optional<json>& chunking_strategy,
                                 const std::optional<json>& include,
                                 const std::optional<json>& keywords,
                                 const std::optional<json>& known_speaker_names,
                                 const std::optional<json>& known_speaker_references,
                                 const std::optional<json>& languages,
                                 const std::optional<bool>& stream,
                                 double temperature,
                                 const std::optional<json>& timestamp_granularities) {
    std::map<std::string, std::string> texts = {{"model", model}};
    if (prompt.has_value()) texts["prompt"] = *prompt;
    if (language.has_value()) texts["language"] = *language;
    if (response_format.has_value()) texts["response_format"] = *response_format;
    if (chunking_strategy.has_value()) texts["chunking_strategy"] = chunking_strategy->dump();
    if (include.has_value()) texts["include"] = include->dump();
    if (keywords.has_value()) texts["keywords"] = keywords->dump();
    if (known_speaker_names.has_value()) texts["known_speaker_names"] = known_speaker_names->dump();
    if (known_speaker_references.has_value()) texts["known_speaker_references"] = known_speaker_references->dump();
    if (languages.has_value()) texts["languages"] = languages->dump();
    if (stream.has_value()) texts["stream"] = *stream ? "true" : "false";
    texts["temperature"] = std::to_string(temperature);
    if (timestamp_granularities.has_value()) texts["timestamp_granularities"] = timestamp_granularities->dump();

    std::map<std::string, std::string> files = {{"file", filepath}};
    Response resp = PostMultipart("/audio/transcriptions", texts, files);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateTranslation(const std::string& filepath,
                               const std::string& model,
                               const std::optional<std::string>& prompt,
                               const std::optional<std::string>& response_format,
                               double temperature) {
    std::map<std::string, std::string> texts = {{"model", model}};
    if (prompt.has_value()) texts["prompt"] = *prompt;
    if (response_format.has_value()) texts["response_format"] = *response_format;
    texts["temperature"] = std::to_string(temperature);
    std::map<std::string, std::string> files = {{"file", filepath}};
    Response resp = PostMultipart("/audio/translations", texts, files);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Images
// =====================================================================

json Client::CreateImage(const json& request_body) {
    Response resp = Post("/images/generations", request_body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateImageEdit(const json& request_body) {
    Response resp = Post("/images/edits", request_body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateImageVariation(const std::string& image_path,
                                  const std::optional<std::string>& model,
                                  int n,
                                  const std::optional<std::string>& response_format,
                                  const std::optional<std::string>& size,
                                  const std::optional<std::string>& user) {
    std::map<std::string, std::string> texts = {{"n", std::to_string(n)}};
    if (model.has_value()) texts["model"] = *model;
    if (response_format.has_value()) texts["response_format"] = *response_format;
    if (size.has_value()) texts["size"] = *size;
    if (user.has_value()) texts["user"] = *user;
    std::map<std::string, std::string> files = {{"image", image_path}};
    Response resp = PostMultipart("/images/variations", texts, files);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Files
// =====================================================================

json Client::ListFiles() {
    Response resp = Get("/files");
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::UploadFile(const std::string& filepath, const std::string& purpose) {
    std::map<std::string, std::string> texts = {{"purpose", purpose}};
    std::map<std::string, std::string> files = {{"file", filepath}};
    Response resp = PostMultipart("/files", texts, files);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::DeleteFile(const std::string& file_id) {
    Response resp = Delete("/files/" + file_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::RetrieveFile(const std::string& file_id) {
    Response resp = Get("/files/" + file_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

bool Client::DownloadFileContent(const std::string& file_id, const std::string& save_filepath) {
    return Download("/files/" + file_id + "/content", save_filepath);
}

// =====================================================================
// Fine-tuning
// =====================================================================

json Client::CreateFineTuningJob(const std::string& training_file_id, const std::string& model) {
    json body;
    body["training_file"] = training_file_id;
    body["model"] = model;
    Response resp = Post("/fine_tuning/jobs", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ListFineTuningJobs() {
    Response resp = Get("/fine_tuning/jobs");
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::RetrieveFineTuningJob(const std::string& job_id) {
    Response resp = Get("/fine_tuning/jobs/" + job_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CancelFineTuningJob(const std::string& job_id) {
    Response resp = Post("/fine_tuning/jobs/" + job_id + "/cancel", {});
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ListFineTuningEvents(const std::string& job_id) {
    Response resp = Get("/fine_tuning/jobs/" + job_id + "/events");
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Models
// =====================================================================

json Client::ListModels() {
    Response resp = Get("/models");
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::RetrieveModel(const std::string& model_id) {
    Response resp = Get("/models/" + model_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::DeleteModel(const std::string& model_id) {
    Response resp = Delete("/models/" + model_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Moderations
// =====================================================================

json Client::CreateModeration(const std::string& input, const std::optional<std::string>& model) {
    json body;
    body["input"] = input;
    if (model.has_value()) body["model"] = *model;
    Response resp = Post("/moderations", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateModeration(const json& request_body) {
    Response resp = Post("/moderations", request_body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Batches
// =====================================================================

json Client::CreateBatch(const std::string& input_file_id, const std::string& endpoint, const std::string& completion_window) {
    json body;
    body["input_file_id"] = input_file_id;
    body["endpoint"] = endpoint;
    body["completion_window"] = completion_window;
    Response resp = Post("/batches", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::RetrieveBatch(const std::string& batch_id) {
    Response resp = Get("/batches/" + batch_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CancelBatch(const std::string& batch_id) {
    Response resp = Post("/batches/" + batch_id + "/cancel", {});
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ListBatches() {
    Response resp = Get("/batches");
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Assistants, Threads, Messages, Runs (Beta API)
// =====================================================================

json Client::CreateAssistant(const std::string& model, const std::string& name, const std::string& instructions) {
    json body;
    body["model"] = model;
    if (!name.empty()) body["name"] = name;
    if (!instructions.empty()) body["instructions"] = instructions;
    Response resp = Post("/assistants", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::RetrieveAssistant(const std::string& assistant_id) {
    Response resp = Get("/assistants/" + assistant_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ModifyAssistant(const std::string& assistant_id, const json& body) {
    Response resp = ExecuteRequest("POST", "/assistants/" + assistant_id, body.dump());
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::DeleteAssistant(const std::string& assistant_id) {
    Response resp = Delete("/assistants/" + assistant_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ListAssistants() {
    Response resp = Get("/assistants");
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateThread(const std::vector<Message>& messages) {
    json body;
    if (!messages.empty()) {
        json j_msgs = json::array();
        for (const auto& m : messages) {
            j_msgs.push_back({{"role", m.role}, {"content", m.content}});
        }
        body["messages"] = j_msgs;
    }
    Response resp = Post("/threads", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::RetrieveThread(const std::string& thread_id) {
    Response resp = Get("/threads/" + thread_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ModifyThread(const std::string& thread_id, const json& body) {
    Response resp = ExecuteRequest("POST", "/threads/" + thread_id, body.dump());
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::DeleteThread(const std::string& thread_id) {
    Response resp = Delete("/threads/" + thread_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateMessage(const std::string& thread_id, const std::string& role, const std::string& content) {
    json body;
    body["role"] = role;
    body["content"] = content;
    Response resp = Post("/threads/" + thread_id + "/messages", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ListMessages(const std::string& thread_id) {
    Response resp = Get("/threads/" + thread_id + "/messages");
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::RetrieveMessage(const std::string& thread_id, const std::string& message_id) {
    Response resp = Get("/threads/" + thread_id + "/messages/" + message_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateRun(const std::string& thread_id, const std::string& assistant_id) {
    json body;
    body["assistant_id"] = assistant_id;
    Response resp = Post("/threads/" + thread_id + "/runs", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::RetrieveRun(const std::string& thread_id, const std::string& run_id) {
    Response resp = Get("/threads/" + thread_id + "/runs/" + run_id);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CancelRun(const std::string& thread_id, const std::string& run_id) {
    Response resp = Post("/threads/" + thread_id + "/runs/" + run_id + "/cancel", {});
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ListRuns(const std::string& thread_id) {
    Response resp = Get("/threads/" + thread_id + "/runs");
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Realtime Translations
// =====================================================================

json Client::CreateRealtimeTranslationClientSecret(const json& request_body) {
    Response resp = Post("/realtime/translations/client_secrets", request_body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

std::string Client::RealtimeTranslationWebSocketUrl(const std::string& model) const {
    std::string l_base_url = base_url_;
    std::string ws_url = l_base_url;
    if (ws_url.rfind("https://", 0) == 0) {
        ws_url = "wss://" + ws_url.substr(8);
    } else if (ws_url.rfind("http://", 0) == 0) {
        ws_url = "ws://" + ws_url.substr(7);
    }
    while (!ws_url.empty() && ws_url.back() == '/') {
        ws_url.pop_back();
    }
    ws_url += "/realtime/translations?model=" + model;
    return ws_url;
}

// =====================================================================
// ExecuteRequestCustom - Custom base URL and headers (for Anthropic / System One)
// =====================================================================

Response Client::ExecuteRequestCustom(const std::string& base_url,
                                      const std::string& method,
                                      const std::string& endpoint,
                                      const std::map<std::string, std::string>& extra_headers,
                                      const std::string& payload,
                                      const std::map<std::string, std::string>& multipart_text,
                                      const std::map<std::string, std::string>& multipart_files,
                                      bool is_download, const std::string& download_path) {
    std::string l_base_url = base_url;
    long l_timeout;
    Client::StreamCallback l_stream_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_timeout = timeout_seconds_;
        l_stream_cb = stream_callback_;
    }

    Request req;
    req.method = method;
    req.url = l_base_url;
    if (!endpoint.empty()) {
        if (l_base_url.back() != '/' && endpoint.front() != '/') {
            req.url += "/" + endpoint;
        } else if (l_base_url.back() == '/' && endpoint.front() == '/') {
            req.url += endpoint.substr(1);
        } else {
            req.url += endpoint;
        }
    }
    req.endpoint = endpoint;

    for (const auto& kv : extra_headers) {
        req.headers[kv.first] = kv.second;
    }

    req.payload = payload;
    req.multipart_text = multipart_text;
    req.multipart_files = multipart_files;
    req.is_download = is_download;
    req.download_path = download_path;
    req.timeout_seconds = l_timeout;
    req.stream_callback = l_stream_cb;

    return transport_->Execute(req);
}

// =====================================================================
// Anthropic Messages API
// =====================================================================

json Client::CreateAnthropicMessage(const json& request_body) {
    std::string l_anthropic_api_key, l_anthropic_base_url, l_anthropic_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_anthropic_api_key = anthropic_api_key_;
        l_anthropic_base_url = anthropic_base_url_;
        l_anthropic_version = anthropic_version_;
    }

    if (l_anthropic_api_key.empty()) {
        if (const char* env_key = std::getenv("ANTHROPIC_API_KEY")) {
            l_anthropic_api_key = env_key;
        }
    }

    std::map<std::string, std::string> extra_headers = {
        {"x-api-key", l_anthropic_api_key},
        {"anthropic-version", l_anthropic_version},
        {"Content-Type", "application/json"}
    };

    Response resp = ExecuteRequestCustom(l_anthropic_base_url, "POST", "/v1/messages",
                                         extra_headers, request_body.dump());
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::GetAnthropicMessage(const std::string& message_id) {
    std::string l_anthropic_api_key, l_anthropic_base_url, l_anthropic_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_anthropic_api_key = anthropic_api_key_;
        l_anthropic_base_url = anthropic_base_url_;
        l_anthropic_version = anthropic_version_;
    }

    if (l_anthropic_api_key.empty()) {
        if (const char* env_key = std::getenv("ANTHROPIC_API_KEY")) {
            l_anthropic_api_key = env_key;
        }
    }

    std::map<std::string, std::string> extra_headers = {
        {"x-api-key", l_anthropic_api_key},
        {"anthropic-version", l_anthropic_version}
    };

    Response resp = ExecuteRequestCustom(l_anthropic_base_url, "GET",
                                         "/v1/messages/" + message_id, extra_headers);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Anthropic Messages Batch API
// =====================================================================

json Client::CreateAnthropicBatch(const json& request_body) {
    std::string l_anthropic_api_key, l_anthropic_base_url, l_anthropic_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_anthropic_api_key = anthropic_api_key_;
        l_anthropic_base_url = anthropic_base_url_;
        l_anthropic_version = anthropic_version_;
    }

    if (l_anthropic_api_key.empty()) {
        if (const char* env_key = std::getenv("ANTHROPIC_API_KEY")) {
            l_anthropic_api_key = env_key;
        }
    }

    std::map<std::string, std::string> extra_headers = {
        {"x-api-key", l_anthropic_api_key},
        {"anthropic-version", l_anthropic_version},
        {"Content-Type", "application/json"}
    };

    Response resp = ExecuteRequestCustom(l_anthropic_base_url, "POST", "/v1/batches",
                                         extra_headers, request_body.dump());
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::GetAnthropicBatch(const std::string& batch_id) {
    std::string l_anthropic_api_key, l_anthropic_base_url, l_anthropic_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_anthropic_api_key = anthropic_api_key_;
        l_anthropic_base_url = anthropic_base_url_;
        l_anthropic_version = anthropic_version_;
    }

    if (l_anthropic_api_key.empty()) {
        if (const char* env_key = std::getenv("ANTHROPIC_API_KEY")) {
            l_anthropic_api_key = env_key;
        }
    }

    std::map<std::string, std::string> extra_headers = {
        {"x-api-key", l_anthropic_api_key},
        {"anthropic-version", l_anthropic_version}
    };

    Response resp = ExecuteRequestCustom(l_anthropic_base_url, "GET",
                                         "/v1/batches/" + batch_id, extra_headers);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CancelAnthropicBatch(const std::string& batch_id) {
    std::string l_anthropic_api_key, l_anthropic_base_url, l_anthropic_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_anthropic_api_key = anthropic_api_key_;
        l_anthropic_base_url = anthropic_base_url_;
        l_anthropic_version = anthropic_version_;
    }

    if (l_anthropic_api_key.empty()) {
        if (const char* env_key = std::getenv("ANTHROPIC_API_KEY")) {
            l_anthropic_api_key = env_key;
        }
    }

    std::map<std::string, std::string> extra_headers = {
        {"x-api-key", l_anthropic_api_key},
        {"anthropic-version", l_anthropic_version}
    };

    Response resp = ExecuteRequestCustom(l_anthropic_base_url, "DELETE",
                                         "/v1/batches/" + batch_id, extra_headers);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ListAnthropicBatches(const std::map<std::string, std::string>& query_params) {
    std::string l_anthropic_api_key, l_anthropic_base_url, l_anthropic_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_anthropic_api_key = anthropic_api_key_;
        l_anthropic_base_url = anthropic_base_url_;
        l_anthropic_version = anthropic_version_;
    }

    if (l_anthropic_api_key.empty()) {
        if (const char* env_key = std::getenv("ANTHROPIC_API_KEY")) {
            l_anthropic_api_key = env_key;
        }
    }

    std::string endpoint = "/v1/batches";
    if (!query_params.empty()) {
        endpoint += "?";
        bool first = true;
        for (const auto& q : query_params) {
            if (!first) endpoint += "&";
            endpoint += q.first + "=" + q.second;
            first = false;
        }
    }

    std::map<std::string, std::string> extra_headers = {
        {"x-api-key", l_anthropic_api_key},
        {"anthropic-version", l_anthropic_version}
    };

    Response resp = ExecuteRequestCustom(l_anthropic_base_url, "GET", endpoint, extra_headers);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

// =====================================================================
// Typesafe System One API
// =====================================================================

json Client::CreateSystemOneChatCompletion(const json& request_body) {
    std::string l_system_one_api_key, l_system_one_base_url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_system_one_api_key = system_one_api_key_;
        l_system_one_base_url = system_one_base_url_;
    }

    if (l_system_one_api_key.empty()) {
        if (const char* env_key = std::getenv("SYSTEM_ONE_API_KEY")) {
            l_system_one_api_key = env_key;
        }
    }

    std::map<std::string, std::string> extra_headers = {
        {"Authorization", "Bearer " + l_system_one_api_key},
        {"Content-Type", "application/json"}
    };

    Response resp = ExecuteRequestCustom(l_system_one_base_url, "POST", "/v1/chat/completions",
                                         extra_headers, request_body.dump());
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::ListSystemOneModels() {
    std::string l_system_one_api_key, l_system_one_base_url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_system_one_api_key = system_one_api_key_;
        l_system_one_base_url = system_one_base_url_;
    }

    if (l_system_one_api_key.empty()) {
        if (const char* env_key = std::getenv("SYSTEM_ONE_API_KEY")) {
            l_system_one_api_key = env_key;
        }
    }

    std::map<std::string, std::string> extra_headers = {
        {"Authorization", "Bearer " + l_system_one_api_key}
    };

    Response resp = ExecuteRequestCustom(l_system_one_base_url, "GET", "/v1/models", extra_headers);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateSystemOneEmbeddings(const json& request_body) {
    std::string l_system_one_api_key, l_system_one_base_url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        l_system_one_api_key = system_one_api_key_;
        l_system_one_base_url = system_one_base_url_;
    }

    if (l_system_one_api_key.empty()) {
        if (const char* env_key = std::getenv("SYSTEM_ONE_API_KEY")) {
            l_system_one_api_key = env_key;
        }
    }

    std::map<std::string, std::string> extra_headers = {
        {"Authorization", "Bearer " + l_system_one_api_key},
        {"Content-Type", "application/json"}
    };

    Response resp = ExecuteRequestCustom(l_system_one_base_url, "POST", "/v1/embeddings",
                                         extra_headers, request_body.dump());
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

} // namespace libopenai
