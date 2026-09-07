#pragma once

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

namespace libopenai {

using json = nlohmann::json;

struct Message {
    std::string role;
    std::string content;
};

// Response object holding status, headers, and body / parsed json
struct Response {
    long status_code = 0;
    std::string body;
    std::string x_request_id;
    json json_data;
    bool is_error() const {
        return status_code < 200 || status_code >= 300;
    }
};

class Client {
public:
    Client(const std::string& api_key = "", const std::string& base_url = "https://api.openai.com/v1");

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

    // --- OpenAI API Endpoints ---

    // 1. Chat Completions (/chat/completions)
    json ChatCompletion(const std::vector<Message>& messages, const std::string& model = "gpt-4o-mini", int max_tokens = 512, double temperature = 1.0);
    json ChatCompletionCustom(const json& request_body);

    // 2. Completions / Legacy (/completions)
    json Completion(const std::string& prompt, const std::string& model = "gpt-3.5-turbo-instruct", int max_tokens = 16);
    json CompletionCustom(const json& request_body);

    // 3. Embeddings (/embeddings)
    json CreateEmbeddings(const std::string& input, const std::string& model = "text-embedding-3-small");
    json CreateEmbeddingsBatch(const std::vector<std::string>& inputs, const std::string& model = "text-embedding-3-small");

    // 4. Audio (/audio/speech, /audio/transcriptions, /audio/translations)
    bool CreateSpeech(const std::string& input, const std::string& voice = "alloy", const std::string& model = "tts-1", const std::string& save_filepath = "speech.mp3", const std::string& response_format = "mp3");
    json CreateTranscription(const std::string& filepath, const std::string& model = "whisper-1", const std::string& prompt = "", const std::string& language = "");
    json CreateTranslation(const std::string& filepath, const std::string& model = "whisper-1", const std::string& prompt = "");

    // 5. Images (/images/generations, /images/edits, /images/variations)
    json CreateImage(const std::string& prompt, int n = 1, const std::string& size = "1024x1024", const std::string& model = "dall-e-3");
    json CreateImageEdit(const std::string& image_path, const std::string& prompt, const std::string& mask_path = "", int n = 1, const std::string& size = "1024x1024");
    json CreateImageVariation(const std::string& image_path, int n = 1, const std::string& size = "1024x1024");

    // 6. Files (/files, /files/{file_id}, /files/{file_id}/content)
    json ListFiles();
    json UploadFile(const std::string& filepath, const std::string& purpose = "fine-tune");
    json DeleteFile(const std::string& file_id);
    json RetrieveFile(const std::string& file_id);
    bool DownloadFileContent(const std::string& file_id, const std::string& save_filepath);

    // 7. Fine-tuning (/fine_tuning/jobs, etc.)
    json CreateFineTuningJob(const std::string& training_file_id, const std::string& model = "gpt-4o-mini");
    json ListFineTuningJobs();
    json RetrieveFineTuningJob(const std::string& job_id);
    json CancelFineTuningJob(const std::string& job_id);
    json ListFineTuningEvents(const std::string& job_id);

    // 8. Models (/models, /models/{model})
    json ListModels();
    json RetrieveModel(const std::string& model_id);
    json DeleteModel(const std::string& model_id); // Fine-tuned model deletion

    // 9. Moderations (/moderations)
    json CreateModeration(const std::string& input, const std::string& model = "text-moderation-latest");

    // 10. Batches (/batches)
    json CreateBatch(const std::string& input_file_id, const std::string& endpoint = "/v1/chat/completions", const std::string& completion_window = "24h");
    json RetrieveBatch(const std::string& batch_id);
    json CancelBatch(const std::string& batch_id);
    json ListBatches();

    // 11. Assistants, Threads, Messages, Runs (Beta API)
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

private:
    mutable std::mutex mutex_;
    std::string api_key_;
    std::string base_url_;
    std::string organization_;
    std::string project_;
    std::string client_request_id_;
    long timeout_seconds_ = 60;
    std::string proxy_url_;
    StreamCallback stream_callback_;

    Response ExecuteRequest(const std::string& method, const std::string& endpoint, const std::string& payload = "", const std::map<std::string, std::string>& multipart_text = {}, const std::map<std::string, std::string>& multipart_files = {}, bool is_download = false, const std::string& download_path = "");
};

} // namespace libopenai
