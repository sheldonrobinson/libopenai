#include "libopenai/transport.hpp"

#ifdef LIBOPENAI_USE_CPPSERVER
#include <cppserver/cppserver.h>
#include <iostream>
#include <fstream>
#include <sstream>
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

class CppServerTransport : public ITransport {
public:
    CppServerTransport() = default;
    ~CppServerTransport() = default;

    Response Execute(const Request& req) override {
        return execute_blocking(req);
    }

    std::future<Response> ExecuteAsync(const Request& req) override {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        // Each request gets its own IOContext and HttpClient, runs on a
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

    std::string Name() const override { return "cppserver"; }

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

        // Parse URL
        std::string host, port, path;
        bool use_ssl = false;
        size_t proto_pos = url.find("://");
        if (proto_pos != std::string::npos) {
            use_ssl = (url.substr(0, proto_pos) == "https");
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
                port = use_ssl ? "443" : "80";
            }
        } else {
            host = url;
            port = "80";
            path = "/";
        }

        if (path.empty()) path = "/";

        // Synchronization for async CppServer callbacks
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> done{false};
        std::atomic<long> status_code{0};
        std::string response_body;
        std::string response_headers;
        std::string error_msg;
        std::atomic<bool> has_error{false};

        try {
            auto io = std::make_shared<cppserver::IOContext>();
            io->start(1);

            if (use_ssl) {
                auto ssl_ctx = std::make_shared<cppserver::SSLContext>();
                ssl_ctx->set_default_verify_paths();

                auto client = std::make_shared<cppserver::HttpClient>(io, ssl_ctx);

                client->set_connect_callback([&](cppserver::HttpClient::ptr client) {
                    auto req_ptr = std::make_shared<cppserver::HttpRequest>();
                    req_ptr->set_method(req.method == "POST" ? cppserver::HttpMethod::POST :
                                       req.method == "DELETE" ? cppserver::HttpMethod::DELETE :
                                       cppserver::HttpMethod::GET);
                    req_ptr->set_url(path);
                    req_ptr->set_header("Host", host);
                    req_ptr->set_header("Connection", "close");
                    req_ptr->set_header("User-Agent", "libopenai-cppserver/1.0");

                    for (const auto& kv : req.headers) {
                        req_ptr->set_header(kv.first, kv.second);
                    }

                    if (!req.payload.empty()) {
                        req_ptr->set_body(req.payload);
                        req_ptr->set_header("Content-Type", "application/json");
                        req_ptr->set_header("Content-Length", std::to_string(req.payload.size()));
                    }

                    client->send(req_ptr);
                });

                client->set_response_callback([&](cppserver::HttpClient::ptr client,
                                                  cppserver::HttpResponse::ptr response) {
                    std::lock_guard<std::mutex> lock(mtx);
                    status_code = response->get_status_code();
                    response_body = response->get_body();
                    response_headers = response->get_headers_string();

                    auto req_id = response->get_header("x-request-id");
                    if (!req_id.has_value()) req_id = response->get_header("X-Request-Id");
                    if (req_id.has_value()) resp.x_request_id = *req_id;

                    done = true;
                    cv.notify_all();
                });

                client->set_error_callback([&](cppserver::HttpClient::ptr client,
                                               const std::error_code& ec) {
                    std::lock_guard<std::mutex> lock(mtx);
                    has_error = true;
                    error_msg = "CppServer SSL error: " + ec.message();
                    done = true;
                    cv.notify_all();
                });

                client->connect(host, port);
            } else {
                auto client = std::make_shared<cppserver::HttpClient>(io);

                client->set_connect_callback([&](cppserver::HttpClient::ptr client) {
                    auto req_ptr = std::make_shared<cppserver::HttpRequest>();
                    req_ptr->set_method(req.method == "POST" ? cppserver::HttpMethod::POST :
                                       req.method == "DELETE" ? cppserver::HttpMethod::DELETE :
                                       cppserver::HttpMethod::GET);
                    req_ptr->set_url(path);
                    req_ptr->set_header("Host", host);
                    req_ptr->set_header("Connection", "close");
                    req_ptr->set_header("User-Agent", "libopenai-cppserver/1.0");

                    for (const auto& kv : req.headers) {
                        req_ptr->set_header(kv.first, kv.second);
                    }

                    if (!req.payload.empty()) {
                        req_ptr->set_body(req.payload);
                        req_ptr->set_header("Content-Type", "application/json");
                        req_ptr->set_header("Content-Length", std::to_string(req.payload.size()));
                    }

                    client->send(req_ptr);
                });

                client->set_response_callback([&](cppserver::HttpClient::ptr client,
                                                  cppserver::HttpResponse::ptr response) {
                    std::lock_guard<std::mutex> lock(mtx);
                    status_code = response->get_status_code();
                    response_body = response->get_body();
                    response_headers = response->get_headers_string();

                    auto req_id = response->get_header("x-request-id");
                    if (!req_id.has_value()) req_id = response->get_header("X-Request-Id");
                    if (req_id.has_value()) resp.x_request_id = *req_id;

                    done = true;
                    cv.notify_all();
                });

                client->set_error_callback([&](cppserver::HttpClient::ptr client,
                                               const std::error_code& ec) {
                    std::lock_guard<std::mutex> lock(mtx);
                    has_error = true;
                    error_msg = "CppServer error: " + ec.message();
                    done = true;
                    cv.notify_all();
                });

                client->connect(host, port);
            }

            // Wait for response
            std::unique_lock<std::mutex> lock(mtx);
            if (cv.wait_for(lock, std::chrono::seconds(req.timeout_seconds + 5), [&] { return done; })) {
                // Completed
            } else {
                has_error = true;
                error_msg = "CppServer request timeout";
            }

            io->stop();

        } catch (const std::exception& e) {
            resp.status_code = -1;
            resp.body = "CppServer Error: " + std::string(e.what());
            return resp;
        }

        if (has_error) {
            resp.status_code = -1;
            resp.body = error_msg;
            return resp;
        }

        resp.status_code = status_code.load();
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

std::shared_ptr<ITransport> CreateCppServerTransport() {
    return std::make_shared<CppServerTransport>();
}

} // namespace libopenai

#endif // LIBOPENAI_USE_CPPSERVER
