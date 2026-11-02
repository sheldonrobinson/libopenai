#include "libopenai/transport.hpp"

#ifdef LIBOPENAI_USE_CURL
#include <curl/curl.h>
#include <deque>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <condition_variable>
#include <chrono>
#include <cstring>

#ifdef DeleteFile
#undef DeleteFile
#endif

namespace libopenai {

namespace {
    size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        size_t total = size * nmemb;
        userp->append(static_cast<char*>(contents), total);
        return total;
    }

    size_t HeaderCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        size_t total = size * nmemb;
        userp->append(static_cast<char*>(contents), total);
        return total;
    }
}

// ---------------------------------------------------------------------------
// EasyHandleData - user data attached to each easy handle via CURLOPT_PRIVATE.
// ---------------------------------------------------------------------------
struct EasyHandleData {
    std::string* body = nullptr;
    std::string* header = nullptr;
    struct curl_slist* headers = nullptr;

    ~EasyHandleData() {
        delete body;
        delete header;
        if (headers) {
            curl_slist_free_all(headers);
        }
    }
};

// ---------------------------------------------------------------------------
// CurlMultiContext - manages a curl_multi handle for concurrent transfers.
// ---------------------------------------------------------------------------
class CurlMultiContext {
public:
    CurlMultiContext() {
        multi_ = curl_multi_init();
    }

    ~CurlMultiContext() {
        if (multi_) {
            curl_multi_cleanup(multi_);
        }
    }

    CURLM* get() { return multi_; }

    CURLMcode add(CURL* easy) {
        return curl_multi_add_handle(multi_, easy);
    }

    CURLMcode remove(CURL* easy) {
        return curl_multi_remove_handle(multi_, easy);
    }

    // Run transfers until none are running.
    void perform_until_idle() {
        int still_running = 0;
        curl_multi_perform(multi_, &still_running);
        while (still_running > 0) {
            int numfds = 0;
            curl_multi_wait(multi_, nullptr, 0, 1000, &numfds);
            curl_multi_perform(multi_, &still_running);
        }
    }

private:
    CURLM* multi_;
};

// ---------------------------------------------------------------------------
// CurlTransport - libcurl-based transport with connection reuse and
//                 curl_multi-based concurrency.
//
// Improvements over the original implementation:
//   1. curl_share: shares DNS cache and SSL sessions across easy handles,
//      avoiding re-resolution and re-handshaking on every request.
//   2. curl_multi: multiplexes many concurrent transfers on a single worker
//      thread instead of spawning one thread per request.
//   3. CURLOPT_NOSIGNAL: thread-safe timeout handling.
//   4. CURLOPT_ERRORBUFFER: human-readable error messages.
//   5. CURLOPT_ACCEPT_ENCODING: automatic gzip/deflate/br handling.
//   6. CURLOPT_TCP_NODELAY: low-latency transfers.
//   7. CURLOPT_HTTP_VERSION: HTTP/2 with TLS fallback.
//   8. CURLOPT_MAXREDIRS, CURLOPT_LOW_SPEED_LIMIT/TIME: safety limits.
//   9. Connection reuse: easy handles are cleaned up but the underlying
//      TCP/TLS connections are retained via the share object.
// ---------------------------------------------------------------------------
class CurlTransport : public ITransport {
public:
    /**
     * @param max_concurrent Maximum number of concurrent transfers in the
     *                       curl_multi stack. Default 16.
     */
    explicit CurlTransport(int max_concurrent = 16)
        : max_concurrent_(max_concurrent == 0 ? 1 : max_concurrent) {
        curl_global_init(CURL_GLOBAL_DEFAULT);

        // Shared object: DNS cache + SSL session reuse across handles.
        share_ = curl_share_init();
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(share_, CURLSHOPT_LOCKFUNC, nullptr);
        curl_share_setopt(share_, CURLSHOPT_UNLOCKFUNC, nullptr);

        multi_ctx_ = std::make_unique<CurlMultiContext>();
    }

    ~CurlTransport() override {
        stop_worker();
        multi_ctx_.reset();
        if (share_) {
            curl_share_cleanup(share_);
        }
        curl_global_cleanup();
    }

    // ------------------------------------------------------------------
    // Blocking execute (single request)
    // ------------------------------------------------------------------
    Response Execute(const Request& req) override {
        return execute_blocking(req);
    }

    // ------------------------------------------------------------------
    // Async execute (multiple in-flight requests supported)
    //
    // Each request is queued and processed by a single worker thread using
    // curl_multi. Returns a std::future immediately.
    // ------------------------------------------------------------------
    std::future<Response> ExecuteAsync(const Request& req) override {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            pending_async_.push_back({promise, req});
            queue_cv_.notify_one();
        }

        ensure_worker();

        return future;
    }

    std::string Name() const override { return "libcurl"; }

private:
    // ------------------------------------------------------------------
    // Async worker
    // ------------------------------------------------------------------

    struct AsyncRequest {
        std::shared_ptr<std::promise<Response>> promise;
        Request req;
    };

    void ensure_worker() {
        if (!worker_running_.load(std::memory_order_acquire)) {
            bool expected = false;
            if (worker_running_.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel)) {
                worker_thread_ = std::thread(&CurlTransport::worker_loop, this);
            }
        }
    }

    void worker_loop() {
        while (true) {
            std::vector<AsyncRequest> batch;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] {
                    return !pending_async_.empty() || stop_worker_.load(std::memory_order_acquire);
                });

                if (stop_worker_.load(std::memory_order_acquire) &&
                    pending_async_.empty()) {
                    worker_running_.store(false, std::memory_order_release);
                    return;
                }

                size_t count = std::min(pending_async_.size(),
                                        static_cast<size_t>(max_concurrent_));
                batch.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    batch.push_back(std::move(pending_async_.front()));
                    pending_async_.pop_front();
                }
            }

            process_batch(batch);
        }
    }

    void process_batch(std::vector<AsyncRequest>& batch) {
        std::unordered_map<CURL*, std::shared_ptr<std::promise<Response>>> easy_to_promise;

        for (auto& areq : batch) {
            CURL* curl = curl_easy_init();
            if (!curl) {
                Response resp;
                resp.status_code = -1;
                resp.body = "Failed to initialize CURL";
                areq.promise->set_value(std::move(resp));
                continue;
            }

            CURLcode res = setup_easy_handle(curl, areq.req);
            if (res != CURLE_OK) {
                Response resp;
                resp.status_code = -1;
                resp.body = "CURL setup error: " + std::string(curl_easy_strerror(res));
                curl_easy_cleanup(curl);
                areq.promise->set_value(std::move(resp));
                continue;
            }

            CURLMcode mres = multi_ctx_->add(curl);
            if (mres != CURLM_OK) {
                Response resp;
                resp.status_code = -1;
                resp.body = "Failed to add handle to multi stack";
                // Clean up private data (CURLOPT_PRIVATE was already set).
                EasyHandleData* data = nullptr;
                curl_easy_getinfo(curl, CURLINFO_PRIVATE,
                                  reinterpret_cast<void**>(&data));
                delete data;
                curl_easy_cleanup(curl);
                areq.promise->set_value(std::move(resp));
                continue;
            }

            easy_to_promise[curl] = areq.promise;
        }

        multi_ctx_->perform_until_idle();

        CURLMsg* msg = nullptr;
        int msgs_left = 0;
        while ((msg = curl_multi_info_read(multi_ctx_->get(), &msgs_left)) != nullptr) {
            if (msg->msg == CURLMSG_DONE) {
                CURL* curl = msg->easy_handle;
                CURLcode res = msg->data.result;

                auto it = easy_to_promise.find(curl);
                if (it != easy_to_promise.end()) {
                    Response resp = collect_result(curl, res);
                    it->second->set_value(std::move(resp));
                    easy_to_promise.erase(it);
                }

                multi_ctx_->remove(curl);
                curl_easy_cleanup(curl);
            }
        }

        // Safety: any remaining promises.
        for (auto& pair : easy_to_promise) {
            Response resp;
            resp.status_code = -1;
            resp.body = "CURL transfer incomplete";
            pair.second->set_value(std::move(resp));
            multi_ctx_->remove(pair.first);
            curl_easy_cleanup(pair.first);
        }
    }

    // ------------------------------------------------------------------
    // Easy handle setup (shared by blocking and async paths)
    // ------------------------------------------------------------------

    CURLcode setup_easy_handle(CURL* curl, const Request& req) {
        std::string url = req.url;
        if (!req.endpoint.empty()) {
            if (url.back() != '/' && req.endpoint.front() != '/') {
                url += "/" + req.endpoint;
            } else if (url.back() == '/' && req.endpoint.front() == '/') {
                url += req.endpoint.substr(1);
            } else {
                url += req.endpoint;
            }
        }

        // Allocate per-handle data for body/header/headers cleanup.
        auto* data = new EasyHandleData();
        data->body = new std::string();
        data->header = new std::string();

        // Helper lambda to clean up and return error if any option fails.
        auto fail = [&](CURLcode code) {
            delete data;
            return code;
        };

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, data->body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, data->header);

        // Timeouts.
        res = curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout_seconds);
        if (res != CURLE_OK) return fail(res);
        res = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, req.timeout_seconds);
        if (res != CURLE_OK) return fail(res);

        // Redirects.
        res = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        if (res != CURLE_OK) return fail(res);
        res = curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        if (res != CURLE_OK) return fail(res);

        // TCP: keepalive + nodelay.
        res = curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        if (res != CURLE_OK) return fail(res);
        res = curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
        if (res != CURLE_OK) return fail(res);
        res = curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 10L);
        if (res != CURLE_OK) return fail(res);
        res = curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
        if (res != CURLE_OK) return fail(res);

        // HTTP/2 with TLS fallback.
        res = curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        if (res != CURLE_OK) return fail(res);

        // Do NOT treat HTTP >= 400 as a curl error; we want the status code.
        res = curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
        if (res != CURLE_OK) return fail(res);

        // Automatic content-encoding (gzip, deflate, br).
        res = curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        if (res != CURLE_OK) return fail(res);

        // Low-speed stall detection.
        res = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
        if (res != CURLE_OK) return fail(res);
        res = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        if (res != CURLE_OK) return fail(res);

        // No signals (thread-safe).
        res = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        if (res != CURLE_OK) return fail(res);

        // Headers.
        struct curl_slist* headers = nullptr;
        for (const auto& kv : req.headers) {
            std::string header_line = kv.first + ": " + kv.second;
            headers = curl_slist_append(headers, header_line.c_str());
        }
        if (headers) {
            data->headers = headers;
            res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            if (res != CURLE_OK) return fail(res);
        }

        // Proxy.
        if (!req.proxy_url.empty()) {
            res = curl_easy_setopt(curl, CURLOPT_PROXY, req.proxy_url.c_str());
            if (res != CURLE_OK) return fail(res);
        }

        // SSL verification.
        res = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        if (res != CURLE_OK) return fail(res);
        res = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        if (res != CURLE_OK) return fail(res);

        // Share DNS cache + SSL sessions across handles (connection reuse).
        res = curl_easy_setopt(curl, CURLOPT_SHARE, share_);
        if (res != CURLE_OK) return fail(res);

        // Attach URL.
        res = curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (res != CURLE_OK) return fail(res);

        // Method and body.
        if (req.method == "GET") {
            res = curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        } else if (req.method == "POST") {
            res = curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (res != CURLE_OK) return fail(res);
            if (!req.multipart_text.empty() || !req.multipart_files.empty()) {
                curl_mime* mime = curl_mime_init(curl);
                for (const auto& kv : req.multipart_text) {
                    curl_mimepart* part = curl_mime_addpart(mime);
                    curl_mime_name(part, kv.first.c_str());
                    curl_mime_data(part, kv.second.c_str(), CURL_ZERO_TERMINATED);
                }
                for (const auto& kv : req.multipart_files) {
                    curl_mimepart* part = curl_mime_addpart(mime);
                    curl_mime_name(part, kv.first.c_str());
                    curl_mime_filedata(part, kv.second.c_str());
                    std::string content_type = "application/octet-stream";
                    if (kv.second.find(".png") != std::string::npos)
                        content_type = "image/png";
                    else if (kv.second.find(".jpg") != std::string::npos ||
                             kv.second.find(".jpeg") != std::string::npos)
                        content_type = "image/jpeg";
                    curl_mime_type(part, content_type.c_str());
                }
                res = curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
            } else if (!req.payload.empty()) {
                res = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.payload.c_str());
                if (res != CURLE_OK) return fail(res);
                res = curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                                       req.payload.size());
            }
        } else if (req.method == "DELETE") {
            res = curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        }
        if (res != CURLE_OK) return fail(res);

        // Set private data LAST so all earlier failures clean up properly.
        res = curl_easy_setopt(curl, CURLOPT_PRIVATE, data);
        if (res != CURLE_OK) return fail(res);

        return CURLE_OK;
    }

    // ------------------------------------------------------------------
    // Collect result from a completed easy handle.
    // ------------------------------------------------------------------

    static Response collect_result(CURL* curl, CURLcode res_code) {
        Response resp;

        EasyHandleData* data = nullptr;
        curl_easy_getinfo(curl, CURLINFO_PRIVATE, reinterpret_cast<void**>(&data));
        if (!data) {
            resp.status_code = -1;
            resp.body = "CURL: missing private data";
            return resp;
        }

        std::string body = data->body ? *data->body : std::string();
        std::string header = data->header ? *data->header : std::string();

        if (res_code == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            resp.status_code = http_code;

            // Extract x-request-id from headers.
            size_t pos = header.find("x-request-id:");
            if (pos == std::string::npos)
                pos = header.find("X-Request-Id:");
            if (pos != std::string::npos) {
                size_t end = header.find("\r\n", pos);
                if (end != std::string::npos) {
                    resp.x_request_id =
                        header.substr(pos + 15, end - pos - 15);
                    size_t start =
                        resp.x_request_id.find_first_not_of(" \t");
                    if (start != std::string::npos)
                        resp.x_request_id =
                            resp.x_request_id.substr(start);
                }
            }

            if (!body.empty() && resp.status_code > 0) {
                try {
                    resp.json_data = json::parse(body);
                } catch (...) {
                    // Not JSON
                }
            }
            resp.body = std::move(body);
        } else {
            resp.status_code = -1;
            resp.body = "CURL Error: " +
                        std::string(curl_easy_strerror(res_code));
        }

        delete data;
        return resp;
    }

    // ------------------------------------------------------------------
    // Blocking execute (single request, own easy handle).
    // ------------------------------------------------------------------

    Response execute_blocking(const Request& req) {
        Response resp;
        CURL* curl = curl_easy_init();
        if (!curl) {
            resp.status_code = -1;
            resp.body = "Failed to initialize CURL";
            return resp;
        }

        CURLcode res = setup_easy_handle(curl, req);
        if (res != CURLE_OK) {
            resp.status_code = -1;
            resp.body = "CURL setup error: " + std::string(curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            return resp;
        }

        std::ofstream outfile;
        if (req.is_download) {
            outfile.open(req.download_path, std::ios::binary);
            if (!outfile.is_open()) {
                resp.status_code = -1;
                resp.body = "Failed to open file for writing: " + req.download_path;
                curl_easy_cleanup(curl);
                return resp;
            }
        }

        res = curl_easy_perform(curl);

        if (res == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            resp.status_code = http_code;

            EasyHandleData* data = nullptr;
            curl_easy_getinfo(curl, CURLINFO_PRIVATE, reinterpret_cast<void**>(&data));
            if (data) {
                resp.body = data->body ? *data->body : std::string();
                std::string header = data->header ? *data->header : std::string();

                size_t pos = header.find("x-request-id:");
                if (pos == std::string::npos)
                    pos = header.find("X-Request-Id:");
                if (pos != std::string::npos) {
                    size_t end = header.find("\r\n", pos);
                    if (end != std::string::npos) {
                        resp.x_request_id =
                            header.substr(pos + 15, end - pos - 15);
                        size_t start =
                            resp.x_request_id.find_first_not_of(" \t");
                        if (start != std::string::npos)
                            resp.x_request_id =
                                resp.x_request_id.substr(start);
                    }
                }

                if (req.is_download && outfile.is_open()) {
                    outfile.write(resp.body.c_str(), resp.body.size());
                    outfile.close();
                }

                if (!resp.body.empty() && resp.status_code > 0) {
                    try {
                        resp.json_data = json::parse(resp.body);
                    } catch (...) {
                        // Not JSON
                    }
                }

                delete data;
            }
        } else {
            resp.status_code = -1;
            resp.body = "CURL Error: " + std::string(curl_easy_strerror(res));

            EasyHandleData* data = nullptr;
            curl_easy_getinfo(curl, CURLINFO_PRIVATE, reinterpret_cast<void**>(&data));
            delete data;
        }

        if (req.is_download && outfile.is_open()) {
            outfile.close();
        }

        curl_easy_cleanup(curl);
        return resp;
    }

    // ------------------------------------------------------------------
    // Shutdown
    // ------------------------------------------------------------------

    void stop_worker() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_worker_.store(true, std::memory_order_release);
        }
        queue_cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    // ------------------------------------------------------------------
    // Members
    // ------------------------------------------------------------------

    int max_concurrent_;
    curl_share* share_;
    std::unique_ptr<CurlMultiContext> multi_ctx_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<AsyncRequest> pending_async_;
    std::atomic<bool> stop_worker_{false};
    std::atomic<bool> worker_running_{false};
    std::thread worker_thread_;
};

std::shared_ptr<ITransport> CreateCurlTransport() {
    return std::make_shared<CurlTransport>();
}

} // namespace libopenai

#endif // LIBOPENAI_USE_CURL
