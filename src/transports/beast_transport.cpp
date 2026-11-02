#include "libopenai/transport.hpp"

#ifdef LIBOPENAI_USE_BEAST
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <future>
#include <thread>

#ifdef DeleteFile
#undef DeleteFile
#endif

namespace libopenai {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class BeastTransport : public ITransport {
public:
    BeastTransport() = default;
    ~BeastTransport() = default;

    Response Execute(const Request& req) override {
        return execute_blocking(req);
    }

    std::future<Response> ExecuteAsync(const Request& req) override {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        // Each request gets its own io_context and stream, runs on a
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

    std::string Name() const override { return "boost-beast"; }

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

        try {
            net::io_context io_context;
            tcp::resolver resolver(io_context);
            ssl::context ssl_ctx(ssl::context::tlsv12_client);
            ssl_ctx.set_default_verify_paths();

            // Resolve the host
            auto const results = resolver.resolve(host, port);

            bool ssl = use_ssl;

            std::string response_body;
            std::string response_headers;
            long status_code = 0;

            if (ssl) {
                auto stream = std::make_shared<beast::ssl_stream<beast::tcp_stream>>(io_context, ssl_ctx);
                stream->set_verify_mode(ssl::verify_peer);
                stream->set_verify_callback(ssl::rfc2818_verification(host));

                beast::get_lowest_layer(*stream).connect(results);
                stream->handshake(ssl::stream_base::client);

                http::request<http::string_body> req_http{http::verb::get, path, 11};
                if (req.method == "POST") req_http.method(http::verb::post);
                else if (req.method == "DELETE") req_http.method(http::verb::del);
                else if (req.method == "GET") req_http.method(http::verb::get);

                req_http.target(path);
                req_http.set(http::field::host, host);
                req_http.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
                req_http.set(http::field::connection, "close");

                for (const auto& kv : req.headers) {
                    req_http.set(kv.first, kv.second);
                }

                if (!req.payload.empty()) {
                    req_http.body() = req.payload;
                    req_http.set(http::field::content_length, req.payload.size());
                    req_http.set(http::field::content_type, "application/json");
                }

                req_http.prepare_payload();

                http::write(*stream, req_http);

                beast::flat_buffer buffer;
                http::response<http::dynamic_body> res;
                http::read(*stream, buffer, res);

                status_code = res.result_int();
                response_headers = res.base().to_string();
                response_body = beast::buffers_to_string(res.body().data());

                beast::error_code close_ec;
                stream->shutdown(close_ec);
            } else {
                auto stream = std::make_shared<beast::tcp_stream>(io_context);
                stream->connect(results);

                http::request<http::string_body> req_http{http::verb::get, path, 11};
                if (req.method == "POST") req_http.method(http::verb::post);
                else if (req.method == "DELETE") req_http.method(http::verb::del);
                else if (req.method == "GET") req_http.method(http::verb::get);

                req_http.target(path);
                req_http.set(http::field::host, host);
                req_http.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
                req_http.set(http::field::connection, "close");

                for (const auto& kv : req.headers) {
                    req_http.set(kv.first, kv.second);
                }

                if (!req.payload.empty()) {
                    req_http.body() = req.payload;
                    req_http.set(http::field::content_length, req.payload.size());
                    req_http.set(http::field::content_type, "application/json");
                }

                req_http.prepare_payload();

                http::write(*stream, req_http);

                beast::flat_buffer buffer;
                http::response<http::dynamic_body> res;
                http::read(*stream, buffer, res);

                status_code = res.result_int();
                response_headers = res.base().to_string();
                response_body = beast::buffers_to_string(res.body().data());

                beast::error_code close_ec;
                stream->socket().shutdown(tcp::socket::shutdown_both, close_ec);
            }

            resp.status_code = status_code;
            resp.body = response_body;

            size_t pos = response_headers.find("x-request-id:");
            if (pos == std::string::npos) pos = response_headers.find("X-Request-Id:");
            if (pos != std::string::npos) {
                size_t end = response_headers.find("\r\n", pos);
                if (end != std::string::npos) {
                    resp.x_request_id = response_headers.substr(pos + 15, end - pos - 15);
                    size_t start = resp.x_request_id.find_first_not_of(" \t");
                    if (start != std::string::npos) resp.x_request_id = resp.x_request_id.substr(start);
                }
            }

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

        } catch (const std::exception& e) {
            resp.status_code = -1;
            resp.body = "Boost.Beast Error: " + std::string(e.what());
            return resp;
        }

        return resp;
    }
};

std::shared_ptr<ITransport> CreateBeastTransport() {
    return std::make_shared<BeastTransport>();
}

} // namespace libopenai

#endif // LIBOPENAI_USE_BEAST
