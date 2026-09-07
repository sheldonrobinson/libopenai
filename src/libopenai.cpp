#include "libopenai/libopenai.hpp"
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <iostream>

namespace libopenai {

struct CurlGlobalInit {
    CurlGlobalInit() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobalInit() { curl_global_cleanup(); }
};

static CurlGlobalInit g_curl_init;

struct WriteContext {
    std::string* body;
    std::ofstream* file;
    Client::StreamCallback* stream_cb;
};

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* ctx = static_cast<WriteContext*>(userp);
    std::string chunk(static_cast<char*>(contents), total_size);

    if (ctx->body) {
        *(ctx->body) += chunk;
    }
    if (ctx->file && ctx->file->is_open()) {
        ctx->file->write(static_cast<char*>(contents), total_size);
    }
    if (ctx->stream_cb && *ctx->stream_cb) {
        if (!(*ctx->stream_cb)(chunk)) {
            return 0; // Abort transfer if callback returns false
        }
    }
    return total_size;
}

static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t num_bytes = size * nitems;
    std::string header(buffer, num_bytes);
    std::string* req_id = static_cast<std::string*>(userdata);
    if (header.rfind("x-request-id:", 0) == 0 || header.rfind("X-Request-Id:", 0) == 0) {
        size_t colon = header.find(':');
        if (colon != std::string::npos) {
            std::string val = header.substr(colon + 1);
            val.erase(0, val.find_first_not_of(" \t\r\n"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            *req_id = val;
        }
    }
    return num_bytes;
}

Client::Client(const std::string& api_key, const std::string& base_url)
    : api_key_(api_key), base_url_(base_url) {
    if (api_key_.empty()) {
        if (const char* env_key = std::getenv("OPENAI_API_KEY")) {
            api_key_ = env_key;
        }
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

    Response resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.status_code = -1;
        resp.body = "Failed to initialize CURL";
        return resp;
    }

    std::string url = l_base_url;
    if (!endpoint.empty()) {
        if (l_base_url.back() != '/' && endpoint.front() != '/') {
            url += "/" + endpoint;
        } else if (l_base_url.back() == '/' && endpoint.front() == '/') {
            url += endpoint.substr(1);
        } else {
            url += endpoint;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, l_timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (!l_proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, l_proxy.c_str());
    }

    struct curl_slist* headers = nullptr;
    if (!l_api_key.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + l_api_key).c_str());
    }
    if (!l_org.empty()) {
        headers = curl_slist_append(headers, ("OpenAI-Organization: " + l_org).c_str());
    }
    if (!l_proj.empty()) {
        headers = curl_slist_append(headers, ("OpenAI-Project: " + l_proj).c_str());
    }
    if (!l_req_id.empty()) {
        headers = curl_slist_append(headers, ("X-Client-Request-Id: " + l_req_id).c_str());
    }

    std::ofstream outfile;
    if (is_download) {
        outfile.open(download_path, std::ios::binary);
        if (!outfile.is_open()) {
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            resp.status_code = -1;
            resp.body = "Failed to open file for writing: " + download_path;
            return resp;
        }
    }

    WriteContext write_ctx{&resp.body, is_download ? &outfile : nullptr, l_stream_cb ? &l_stream_cb : nullptr};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.x_request_id);

    curl_mime* mime = nullptr;

    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!multipart_text.empty() || !multipart_files.empty()) {
            mime = curl_mime_init(curl);
            for (const auto& pair : multipart_text) {
                curl_mimepart* part = curl_mime_addpart(mime);
                curl_mime_name(part, pair.first.c_str());
                curl_mime_data(part, pair.second.c_str(), CURL_ZERO_TERMINATED);
            }
            for (const auto& pair : multipart_files) {
                curl_mimepart* part = curl_mime_addpart(mime);
                curl_mime_name(part, pair.first.c_str());
                curl_mime_filedata(part, pair.second.c_str());
            }
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        } else {
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
        }
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.status_code = -1;
        resp.body = std::string("CURL error: ") + curl_easy_strerror(res);
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        resp.status_code = http_code;
    }

    if (mime) {
        curl_mime_free(mime);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (outfile.is_open()) {
        outfile.close();
    }

    if (!resp.body.empty() && resp.status_code > 0) {
        try {
            resp.json_data = json::parse(resp.body);
        } catch (...) {
            // Not JSON or parse error, leave json_data empty/null
        }
    }

    return resp;
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

// --- Endpoints Implementation ---

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

json Client::CreateEmbeddings(const std::string& input, const std::string& model) {
    json body;
    body["input"] = input;
    body["model"] = model;
    Response resp = Post("/embeddings", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateEmbeddingsBatch(const std::vector<std::string>& inputs, const std::string& model) {
    json body;
    body["input"] = inputs;
    body["model"] = model;
    Response resp = Post("/embeddings", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

bool Client::CreateSpeech(const std::string& input, const std::string& voice, const std::string& model, const std::string& save_filepath, const std::string& response_format) {
    json body;
    body["model"] = model;
    body["input"] = input;
    body["voice"] = voice;
    body["response_format"] = response_format;
    Response resp = ExecuteRequest("POST", "/audio/speech", body.dump(), {}, {}, true, save_filepath);
    return !resp.is_error();
}

json Client::CreateTranscription(const std::string& filepath, const std::string& model, const std::string& prompt, const std::string& language) {
    std::map<std::string, std::string> texts = {{"model", model}};
    if (!prompt.empty()) texts["prompt"] = prompt;
    if (!language.empty()) texts["language"] = language;
    std::map<std::string, std::string> files = {{"file", filepath}};
    Response resp = PostMultipart("/audio/transcriptions", texts, files);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateTranslation(const std::string& filepath, const std::string& model, const std::string& prompt) {
    std::map<std::string, std::string> texts = {{"model", model}};
    if (!prompt.empty()) texts["prompt"] = prompt;
    std::map<std::string, std::string> files = {{"file", filepath}};
    Response resp = PostMultipart("/audio/translations", texts, files);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateImage(const std::string& prompt, int n, const std::string& size, const std::string& model) {
    json body;
    body["prompt"] = prompt;
    body["n"] = n;
    body["size"] = size;
    body["model"] = model;
    Response resp = Post("/images/generations", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateImageEdit(const std::string& image_path, const std::string& prompt, const std::string& mask_path, int n, const std::string& size) {
    std::map<std::string, std::string> texts = {{"prompt", prompt}, {"n", std::to_string(n)}, {"size", size}};
    std::map<std::string, std::string> files = {{"image", image_path}};
    if (!mask_path.empty()) files["mask"] = mask_path;
    Response resp = PostMultipart("/images/edits", texts, files);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

json Client::CreateImageVariation(const std::string& image_path, int n, const std::string& size) {
    std::map<std::string, std::string> texts = {{"n", std::to_string(n)}, {"size", size}};
    std::map<std::string, std::string> files = {{"image", image_path}};
    Response resp = PostMultipart("/images/variations", texts, files);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

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

json Client::CreateModeration(const std::string& input, const std::string& model) {
    json body;
    body["input"] = input;
    body["model"] = model;
    Response resp = Post("/moderations", body);
    return resp.json_data.is_null() ? json{{"status", resp.status_code}, {"body", resp.body}} : resp.json_data;
}

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

} // namespace libopenai
