#include "libopenai/transport.hpp"

#ifdef LIBOPENAI_USE_LIBHTTP
#include <libhttp.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#ifdef DeleteFile
#undef DeleteFile
#endif

namespace libopenai {

namespace {
    // libhttp is a fork of mongoose with a similar event-driven API.
    // Context carries per-request state across the callback loop.
    struct LibHttpContext {
        lh_mgr mgr;                 // libhttp manager (analogous to mg_mgr)
        lh_connection* conn = nullptr;
        std::string response_body;
        std::string response_headers;
        long status_code = 0;
        bool completed = false;
        bool error = false;
        std::string error_msg;
        std::mutex mutex;
        std::condition_variable cv;
    };

    // libhttp event handler (analogous to mongoose's fn callback).
    static void lh_fn(struct lh_connection* c, int ev, void* ev_data, void* fn_data) {
        auto* ctx = static_cast<LibHttpContext*>(fn_data);
        if (ev == LH_EVT_CONNECT) {
            int* res = static_cast<int*>(ev_data);
            if (*res != 0) {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->error = true;
                ctx->error_msg = "Connection failed";
                ctx->completed = true;
                ctx->cv.notify_all();
            }
        } else if (ev == LH_EVT_HTTP_MSG) {
            auto* hm = static_cast<struct lh_http_message*>(ev_data);
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                ctx->status_code = hm->status_code;
                ctx->response_headers.assign(hm->headers.buf, hm->headers.len);
                ctx->response_body.assign(hm->body.buf, hm->body.len);
                ctx->completed = true;
                ctx->cv.notify_all();
            }
            c->is_closing = 1;
        } else if (ev == LH_EVT_ERROR || ev == LH_EVT_CLOSE) {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            if (!ctx->completed) {
                ctx->error = true;
                ctx->error_msg = "Connection closed or error";
                ctx->completed = true;
            }
            ctx->cv.notify_all();
        }
    }
}

class LibHttpTransport : public ITransport {
public:
    LibHttpTransport() {
        lh_mgr_init(&mgr_);
    }

    ~LibHttpTransport() override {
        lh_mgr_free(&mgr_);
    }

    Response Execute(const Request& req) override {
        return execute_blocking(req);
    }

    std::future<Response> ExecuteAsync(const Request& req) override {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        // Each request creates its own lh_mgr and connection, runs on a
        // background thread, so multiple ExecuteAsync calls are concurrent.
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

    std::string Name() const override { return "libhttp"; }

private:
    lh_mgr mgr_;  // kept for API compatibility; not used by ExecuteAsync

    static Response execute_blocking(const Request& req) {
        Response resp;
        LibHttpContext ctx;
        lh_mgr_init(&ctx.mgr);

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

        // Parse host and port from URL
        std::string host, port, path;
        size_t proto_pos = url.find("://");
        if (proto_pos != std::string::npos) {
            std::string rest = url.substr(proto_pos + 3);
            size_t slash_pos = rest.find("/");
            if (slash_pos != std::string::npos) {
                host = rest.substr(0, slash_pos);
                path = rest.substr(slash_pos);
            } else {
                host = rest;
                path = "/";
            }
            size_t colon_pos = host.find(":");
            if (colon_pos != std::string::npos) {
                port = host.substr(colon_pos + 1);
                host = host.substr(0, colon_pos);
            } else {
                port = (url.find("https://") == 0) ? "443" : "80";
            }
        } else {
            host = url;
            port = "80";
            path = "/";
        }

        if (path.empty()) path = "/";

        // Connect
        ctx.conn = lh_connect(&ctx.mgr, (host + ":" + port).c_str(), lh_fn, &ctx);
        if (!ctx.conn) {
            resp.status_code = -1;
            resp.body = "libhttp: failed to create connection";
            lh_mgr_free(&ctx.mgr);
            return resp;
        }

        // Build HTTP request
        std::string request;
        request += req.method + " " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Connection: close\r\n";

        for (const auto& kv : req.headers) {
            request += kv.first + ": " + kv.second + "\r\n";
        }

        if (!req.payload.empty()) {
            request += "Content-Length: " + std::to_string(req.payload.size()) + "\r\n";
        }

        request += "\r\n";
        if (!req.payload.empty()) {
            request += req.payload;
        }

        // Send request
        lh_send(ctx.conn, request.data(), request.size());

        // Poll event loop until response or timeout
        auto start = std::chrono::steady_clock::now();
        int timeout_ms = static_cast<int>(req.timeout_seconds * 1000);

        while (!ctx.completed) {
            lh_mgr_poll(&ctx.mgr, 100);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed.count() >= timeout_ms) {
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(ctx.mutex);
            if (ctx.error) {
                resp.status_code = -1;
                resp.body = ctx.error_msg.empty() ? "libhttp error" : ctx.error_msg;
            } else {
                resp.status_code = ctx.status_code;
                resp.body = ctx.response_body;

                size_t pos = ctx.response_headers.find("x-request-id:");
                if (pos == std::string::npos) pos = ctx.response_headers.find("X-Request-Id:");
                if (pos != std::string::npos) {
                    size_t end = ctx.response_headers.find("\r\n", pos);
                    if (end != std::string::npos) {
                        resp.x_request_id = ctx.response_headers.substr(pos + 15, end - pos - 15);
                        size_t start = resp.x_request_id.find_first_not_of(" \t");
                        if (start != std::string::npos) resp.x_request_id = resp.x_request_id.substr(start);
                    }
                }

                if (req.stream_callback) {
                    req.stream_callback(resp.body);
                }
            }
        }

        lh_mgr_free(&ctx.mgr);

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

std::shared_ptr<ITransport> CreateLibHttpTransport() {
    return std::make_shared<LibHttpTransport>();
}

} // namespace libopenai

#endif // LIBOPENAI_USE_LIBHTTP
