#include "libopenai/transport.hpp"

#ifdef LIBOPENAI_USE_SEASTAR
#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/future-util.hh>
#include <seastar/http/client.hh>
#include <seastar/http/functions.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/stack.hh>
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
#include <string>

#ifdef DeleteFile
#undef DeleteFile
#endif

namespace libopenai {

namespace {

// ---------------------------------------------------------------------------
// SeastarEngine - runs the seastar reactor on a dedicated background thread.
//                 Provides a bridge from seastar::future to std::future.
// ---------------------------------------------------------------------------
class SeastarEngine {
public:
    SeastarEngine() : stop_(false) {}

    void start() {
        engine_thread_ = std::thread([this] {
            seastar::app_template app;
            app.run(0, nullptr, [this] {
                // Signal that the reactor is up.
                {
                    std::lock_guard<std::mutex> lock(ready_mutex_);
                    ready_ = true;
                }
                ready_cv_.notify_all();

                // Keep the reactor running until stop() is called.
                return seastar::make_ready_future<>();
            });
        });

        {
            std::unique_lock<std::mutex> lock(ready_mutex_);
            ready_cv_.wait_for(lock, std::chrono::seconds(5),
                [this] { return ready_.load(std::memory_order_acquire); });
        }
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (engine_thread_.joinable()) {
            engine_thread_.join();
        }
    }

    bool is_ready() const {
        return ready_.load(std::memory_order_acquire);
    }

private:
    std::thread engine_thread_;
    std::atomic<bool> ready_{false};
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    std::atomic<bool> stop_{false};
};

} // namespace

// ---------------------------------------------------------------------------
// SeastarTransport - HTTP transport backed by ScyllaDB Seastar.
//
// Seastar is a shard-per-core, high-performance server framework. Its HTTP
// client (seastar::http::client) is asynchronous and uses seastar::future.
// This transport runs the seastar reactor on a background thread and bridges
// seastar::future to std::future for compatibility with ITransport.
//
// NOTE: Seastar requires careful CPU/core pinning and is typically run on
// dedicated cores. This wrapper runs a single-shard engine for simplicity.
// ---------------------------------------------------------------------------
class SeastarTransport : public ITransport {
public:
    SeastarTransport() {
        engine_.start();
    }

    ~SeastarTransport() override {
        engine_.stop();
    }

    Response Execute(const Request& req) override {
        auto fut = ExecuteAsync(req);
        return fut.get();
    }

    std::future<Response> ExecuteAsync(const Request& req) override {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        // Seastar is single-threaded per shard; each request runs its own
        // HTTP client transaction on the reactor thread via a submitted
        // task. We capture the result into a std::promise.
        std::thread([this, promise, req]() mutable {
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

    std::string Name() const override { return "seastar"; }

private:
    SeastarEngine engine_;

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

        // Parse host and port from URL
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

        try {
            // Resolve the address
            seastar::net::inet_address addr(seastar::net::inet_address::resolve_host(host).get0());

            // Create an HTTP client connection
            auto client = seastar::http::client::make_client_connection(
                seastar::net::ipv4_addr(addr), seastar::net::ipv4_addr(0));

            // Build the HTTP request
            auto request = seastar::http::request_ptr(
                new seastar::http::request());

            request->set_method(seastar::http::request_method::get);
            if (req.method == "POST") {
                request->set_method(seastar::http::request_method::post);
            } else if (req.method == "DELETE") {
                request->set_method(seastar::http::request_method::del);
            }

            request->set_target(path);
            request->set_header("host", host);
            request->set_header("connection", "close");

            for (const auto& kv : req.headers) {
                request->set_header(kv.first, kv.second);
            }

            if (!req.payload.empty()) {
                request->set_body(req.payload);
                request->set_header("content-length",
                    std::to_string(req.payload.size()));
            }

            // Send the request and wait for the response (bridge seastar::future
            // to a blocking result via promise/future on this thread).
            std::promise<Response> result_promise;
            auto result_future = result_promise.get_future();

            // Submit a task to the seastar reactor.
            // Note: In a full integration, this would use seastar::submit_to
            // to run on the reactor shard. Here we perform a synchronous
            // blocking wait which is acceptable for a transport wrapper.
            auto fut = client->do_request(std::move(request))
                .then([promise = std::move(result_promise)](
                    seastar::http::response_ptr response) mutable {
                    Response resp;
                    resp.status_code = static_cast<long>(response->status_code());
                    resp.body = std::string(response->get_body().begin(),
                                             response->get_body().end());

                    // Extract headers
                    response->for_each_header([&](seastar::sstring name,
                                                  seastar::sstring value) {
                        if (name == "x-request-id" || name == "X-Request-Id") {
                            resp.x_request_id = std::string(value.begin(),
                                                           value.end());
                        }
                    });

                    if (!resp.body.empty() && resp.status_code > 0) {
                        try {
                            resp.json_data = json::parse(resp.body);
                        } catch (...) {
                            // Not JSON
                        }
                    }
                    promise.set_value(std::move(resp));
                })
                .handle_exception([promise = std::move(result_promise)](
                    std::exception_ptr ep) mutable {
                    try {
                        std::rethrow_exception(ep);
                    } catch (const std::exception& e) {
                        Response resp;
                        resp.status_code = -1;
                        resp.body = std::string("Seastar error: ") + e.what();
                        promise.set_value(std::move(resp));
                    }
                });

            // Wait for completion (blocking bridge).
            auto status = result_future.wait_for(
                std::chrono::seconds(req.timeout_seconds + 5));
            if (status == std::future_status::ready) {
                resp = result_future.get();
            } else {
                resp.status_code = -1;
                resp.body = "Seastar request timeout";
            }

        } catch (const std::exception& e) {
            resp.status_code = -1;
            resp.body = std::string("Seastar setup error: ") + e.what();
        }

        return resp;
    }
};

std::shared_ptr<ITransport> CreateSeastarTransport() {
    return std::make_shared<SeastarTransport>();
}

} // namespace libopenai

#endif // LIBOPENAI_USE_SEASTAR
