#include "libopenai/transport.hpp"
#include "httplib.h"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <fstream>
#include <sstream>
#include <iostream>

#ifdef DeleteFile
#undef DeleteFile
#endif

namespace libopenai {

class HttplibTransport : public ITransport {
public:
    Response Execute(const Request& req) override {
        return execute_blocking(req);
    }

    std::future<Response> ExecuteAsync(const Request& req) override {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        // Each request gets its own httplib::Client and runs on a background
        // thread, so multiple ExecuteAsync calls are fully concurrent.
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

    std::string Name() const override { return "cpp-httplib"; }

private:
    static Response execute_blocking(const Request& req) {
        Response resp;

        // Parse host and path from URL
        std::string host = req.url;
        std::string path = req.endpoint.empty() ? "/" : req.endpoint;

        size_t protocol_pos = host.find("://");
        if (protocol_pos != std::string::npos) {
            size_t path_pos = host.find("/", protocol_pos + 3);
            if (path_pos != std::string::npos) {
                path = host.substr(path_pos) + (req.endpoint.empty() ? "" : req.endpoint);
                host = host.substr(0, path_pos);
            }
        }

        httplib::Client cli(host);
        cli.set_connection_timeout(req.timeout_seconds);
        cli.set_read_timeout(req.timeout_seconds);

        httplib::Headers headers;
        for (const auto& kv : req.headers) {
            headers.insert({kv.first, kv.second});
        }

        auto execute = [&]() -> httplib::Result {
            if (req.method == "GET") {
                return cli.Get(path, headers);
            } else if (req.method == "POST") {
                if (!req.multipart_text.empty() || !req.multipart_files.empty()) {
                    httplib::MultipartFormDataItems items;
                    for (const auto& kv : req.multipart_text) {
                        items.push_back({kv.first, kv.second, "", ""});
                    }
                    for (const auto& kv : req.multipart_files) {
                        std::ifstream ifs(kv.second, std::ios::binary);
                        if (ifs.is_open()) {
                            std::stringstream buffer;
                            buffer << ifs.rdbuf();
                            std::string content_type = "application/octet-stream";
                            if (kv.second.find(".png") != std::string::npos) content_type = "image/png";
                            else if (kv.second.find(".jpg") != std::string::npos || kv.second.find(".jpeg") != std::string::npos) content_type = "image/jpeg";
                            items.push_back({kv.first, buffer.str(), kv.second, content_type});
                        }
                    }
                    return cli.Post(path, headers, items);
                } else {
                    return cli.Post(path, headers, req.payload, "application/json");
                }
            } else if (req.method == "DELETE") {
                return cli.Delete(path, headers);
            }
            return httplib::Result(nullptr, httplib::Error::Unknown);
        };

        httplib::Result res = execute();

        std::ofstream outfile;
        if (req.is_download) {
            outfile.open(req.download_path, std::ios::binary);
            if (!outfile.is_open()) {
                resp.status_code = -1;
                resp.body = "Failed to open file for writing: " + req.download_path;
                return resp;
            }
        }

        if (res) {
            resp.status_code = res->status;
            resp.body = res->body;

            if (req.is_download && outfile.is_open()) {
                outfile.write(resp.body.c_str(), resp.body.size());
                outfile.close();
            }
            if (req.stream_callback) {
                req.stream_callback(resp.body);
            }

            if (res->has_header("x-request-id")) {
                resp.x_request_id = res->get_header_value("x-request-id");
            } else if (res->has_header("X-Request-Id")) {
                resp.x_request_id = res->get_header_value("X-Request-Id");
            }
        } else {
            resp.status_code = -1;
            resp.body = "HTTP Client Error: " + httplib::to_string(res.error());
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

std::shared_ptr<ITransport> CreateHttplibTransport() {
    return std::make_shared<HttplibTransport>();
}

} // namespace libopenai
