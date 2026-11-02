#include "libopenai/transport.hpp"

#ifdef LIBOPENAI_USE_CIVETWEB
#include <civetweb.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <cstring>
#include <future>
#include <memory>
#include <thread>

#ifdef DeleteFile
#undef DeleteFile
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace libopenai {

namespace {
#ifdef _WIN32
    bool g_wsa_initialized = false;

    void ensure_wsa() {
        if (!g_wsa_initialized) {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
                g_wsa_initialized = true;
            }
        }
    }
#endif

    int create_socket(const char* host, const char* port, int timeout_sec) {
#ifdef _WIN32
        ensure_wsa();
#endif
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        int rc = getaddrinfo(host, port, &hints, &res);
        if (rc != 0) return -1;

        int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) {
            freeaddrinfo(res);
            return -1;
        }

#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
#else
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

        if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
#else
            int err = errno;
            if (err != EINPROGRESS) {
#endif
                closesocket(sock);
                freeaddrinfo(res);
                return -1;
            }
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        rc = select(sock + 1, nullptr, &fds, nullptr, &tv);
        if (rc <= 0) {
            closesocket(sock);
            freeaddrinfo(res);
            return -1;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error != 0) {
            closesocket(sock);
            freeaddrinfo(res);
            return -1;
        }

#ifdef _WIN32
        mode = 0;
        ioctlsocket(sock, FIONBIO, &mode);
#else
        fcntl(sock, F_SETFL, flags);
#endif

#ifdef _WIN32
        int tm = timeout_sec * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tm, sizeof(tm));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tm, sizeof(tm));
#else
        struct timeval to;
        to.tv_sec = timeout_sec;
        to.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
#endif

        freeaddrinfo(res);
        return sock;
    }

    void closesocket_func(int sock) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }

    ssize_t sock_send(int sock, const char* buf, size_t len) {
        size_t sent = 0;
        while (sent < len) {
#ifdef _WIN32
            int n = send(sock, buf + sent, (int)(len - sent), 0);
#else
            ssize_t n = send(sock, buf + sent, len - sent, 0);
#endif
            if (n <= 0) return static_cast<ssize_t>(sent);
            sent += static_cast<size_t>(n);
        }
        return static_cast<ssize_t>(sent);
    }

    ssize_t sock_recv(int sock, char* buf, size_t len) {
#ifdef _WIN32
        return recv(sock, buf, (int)len, 0);
#else
        return recv(sock, buf, len, 0);
#endif
    }
}

class CivetwebTransport : public ITransport {
public:
    CivetwebTransport() = default;
    ~CivetwebTransport() = default;

    Response Execute(const Request& req) override {
        return execute_blocking(req);
    }

    std::future<Response> ExecuteAsync(const Request& req) override {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        // Each request gets its own socket and runs on a background thread,
        // so multiple ExecuteAsync calls are fully concurrent.
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

    std::string Name() const override { return "civetweb"; }

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

        // Create socket
        int sock = create_socket(host.c_str(), port.c_str(), static_cast<int>(req.timeout_seconds));
        if (sock < 0) {
            resp.status_code = -1;
            resp.body = "Civetweb: failed to connect to " + host + ":" + port;
            return resp;
        }

        // Build HTTP request
        std::string request;
        request += req.method + " " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Connection: close\r\n";
        request += "User-Agent: libopenai-civetweb/1.0\r\n";

        for (const auto& kv : req.headers) {
            request += kv.first + ": " + kv.second + "\r\n";
        }

        if (!req.payload.empty()) {
            request += "Content-Length: " + std::to_string(req.payload.size()) + "\r\n";
            request += "Content-Type: application/json\r\n";
        }

        request += "\r\n";
        if (!req.payload.empty()) {
            request += req.payload;
        }

        // Send request
        if (sock_send(sock, request.data(), request.size()) < 0) {
            resp.status_code = -1;
            resp.body = "Civetweb: failed to send request";
            closesocket_func(sock);
            return resp;
        }

        // Receive response
        std::string response;
        char buf[4096];
        while (true) {
            ssize_t n = sock_recv(sock, buf, sizeof(buf));
            if (n <= 0) break;
            response.append(buf, static_cast<size_t>(n));
        }

        closesocket_func(sock);

        // Parse response: split headers and body
        size_t header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            resp.status_code = -1;
            resp.body = "Civetweb: invalid HTTP response";
            return resp;
        }

        std::string header_section = response.substr(0, header_end);
        std::string body = response.substr(header_end + 4);

        // Parse status line
        size_t first_line_end = header_section.find("\r\n");
        if (first_line_end != std::string::npos) {
            std::string status_line = header_section.substr(0, first_line_end);
            size_t code_start = status_line.find_first_of("0123456789");
            if (code_start != std::string::npos) {
                std::string code_str = status_line.substr(code_start, 3);
                resp.status_code = std::stol(code_str);
            }
        }

        // Parse headers
        size_t hpos = 0;
        while ((hpos = header_section.find("\r\n", hpos)) != std::string::npos) {
            hpos += 2;
            if (hpos >= header_section.size()) break;
            size_t colon = header_section.find(":", hpos);
            if (colon != std::string::npos) {
                size_t line_end = header_section.find("\r\n", hpos);
                if (line_end == std::string::npos) line_end = header_section.size();
                std::string key = header_section.substr(hpos, colon - hpos);
                std::string value = header_section.substr(colon + 1, line_end - colon - 1);
                size_t vstart = value.find_first_not_of(" \t");
                if (vstart != std::string::npos) value = value.substr(vstart);
                if (key == "x-request-id" || key == "X-Request-Id") {
                    resp.x_request_id = value;
                }
            }
        }

        resp.body = body;

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

std::shared_ptr<ITransport> CreateCivetwebTransport() {
    return std::make_shared<CivetwebTransport>();
}

} // namespace libopenai

#endif // LIBOPENAI_USE_CIVETWEB
