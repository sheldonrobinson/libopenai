#include "libopenai/transport.hpp"

#ifdef LIBOPENAI_USE_WORKFLOW
#include <workflow/WFFacilities.h>
#include <workflow/WFHttpClient.h>
#include <workflow/Workflow.h>
#include <workflow/WFTaskFactory.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <future>
#include <memory>
#include <thread>

#ifdef DeleteFile
#undef DeleteFile
#endif

namespace libopenai {

class WorkflowTransport : public ITransport {
public:
    WorkflowTransport() = default;
    ~WorkflowTransport() = default;

    Response Execute(const Request& req) override {
        return execute_blocking(req);
    }

    std::future<Response> ExecuteAsync(const Request& req) override {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        // Each request creates its own WFHttpTask and runs on a background
        // thread, so multiple ExecuteAsync calls are concurrent.
        std::thread([promise, req]() mutable {
            try {
                promise->set_value(execute_blocking(req));
            } catch (const std::exception& e) {
                Response resp;
                resp.status_code = -1;
                resp.body = std::string("Exception: ") + e.what();
                promise->set_value(std::move(resp));
            } catch (...) {
                Response resp;
                resp.status_code = -1;
                resp.body = "Unknown exception";
                promise->set_value(std::move(resp));
            }
        }).detach();

        return future;
    }

    std::string Name() const override { return "c++-workflow"; }

private:
    static Response execute_blocking(const Request& req) {
        Response resp;

        // Build URL
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

        // Synchronization primitives for async callback
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
        long status_code = 0;
        std::string response_body;
        std::string error_msg;
        bool has_error = false;

        // Create HTTP task
        auto* task = WFTaskFactory::create_http_task(
            url,
            static_cast<int>(req.timeout_seconds),
            2,
            [&](WFHttpTask* http_task) {
                protocol::HttpRequest* http_req = http_task->get_req();
                protocol::HttpResponse* http_resp = http_task->get_resp();

                int state = http_task->get_state();
                int error = http_task->get_error();

                if (state != WFTaskState::WTF_SUCCESS || error != 0) {
                    std::lock_guard<std::mutex> lock(mtx);
                    has_error = true;
                    error_msg = "Workflow error: state=" + std::to_string(state) +
                               ", error=" + std::to_string(error);
                    done = true;
                    cv.notify_all();
                    return;
                }

                status_code = http_resp->get_status_code();

                const void* body_data;
                size_t body_size;
                http_resp->get_body(&body_data, &body_size);
                response_body.assign(static_cast<const char*>(body_data), body_size);

                const char* req_id = http_resp->get_header("x-request-id");
                if (!req_id) req_id = http_resp->get_header("X-Request-Id");
                if (req_id) resp.x_request_id = req_id;

                std::lock_guard<std::mutex> lock(mtx);
                done = true;
                cv.notify_all();
            }
        );

        protocol::HttpRequest* http_req = task->get_req();
        if (req.method == "POST") {
            http_req->set_method("POST");
        } else if (req.method == "DELETE") {
            http_req->set_method("DELETE");
        } else {
            http_req->set_method("GET");
        }

        for (const auto& kv : req.headers) {
            http_req->add_header_pair(kv.first.c_str(), kv.second.c_str());
        }

        if (!req.payload.empty()) {
            http_req->set_body(req.payload.data(), req.payload.size());
            http_req->set_header_pair("Content-Type", "application/json");
        }

        if (!req.multipart_text.empty() || !req.multipart_files.empty()) {
            std::string boundary = "----WorkflowBoundary----";
            std::string body;
            for (const auto& kv : req.multipart_text) {
                body += "--" + boundary + "\r\n";
                body += "Content-Disposition: form-data; name=\"" + kv.first + "\"\r\n\r\n";
                body += kv.second + "\r\n";
            }
            for (const auto& kv : req.multipart_files) {
                std::ifstream ifs(kv.second, std::ios::binary);
                if (ifs.is_open()) {
                    std::stringstream buffer;
                    buffer << ifs.rdbuf();
                    std::string content = buffer.str();
                    std::string filename = kv.second;
                    size_t last_slash = filename.find_last_of("/\\");
                    if (last_slash != std::string::npos) filename = filename.substr(last_slash + 1);
                    body += "--" + boundary + "\r\n";
                    body += "Content-Disposition: form-data; name=\"" + kv.first + "\"; filename=\"" + filename + "\"\r\n";
                    body += "Content-Type: application/octet-stream\r\n\r\n";
                    body += content + "\r\n";
                }
            }
            body += "--" + boundary + "--\r\n";
            http_req->set_body(body.data(), body.size());
            http_req->set_header_pair("Content-Type", "multipart/form-data; boundary=" + boundary);
        }

        task->start();

        std::unique_lock<std::mutex> lock(mtx);
        if (cv.wait_for(lock, std::chrono::seconds(req.timeout_seconds + 5), [&] { return done; })) {
            // Completed
        } else {
            has_error = true;
            error_msg = "Workflow request timeout";
        }

        if (has_error) {
            resp.status_code = -1;
            resp.body = error_msg;
            return resp;
        }

        resp.status_code = status_code;
        resp.body = response_body;

        if (req.stream_callback) {
            req.stream_callback(resp.body);
        }

        if (!resp.body.empty() && resp.status_code > 0) {
            try {
                resp.json_data = json::parse(resp.body);
            } catch (...) {
                // Not JSON
            }
        }

        return resp;
    }
};

std::shared_ptr<ITransport> CreateWorkflowTransport() {
    return std::make_shared<WorkflowTransport>();
}

} // namespace libopenai

#endif // LIBOPENAI_USE_WORKFLOW
