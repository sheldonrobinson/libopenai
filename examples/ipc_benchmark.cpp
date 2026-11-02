/**
 * IPC Benchmark: Named Pipe (platform-native) round-trip throughput
 *
 * Measures request/response round-trip throughput using the platform-native
 * NamedPipeTransport over a named pipe / Unix domain socket.
 *
 * No libuv dependency.
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "libopenai/libopenai.hpp"
#include "libopenai/transport.hpp"

using namespace libopenai;

constexpr size_t NUM_MESSAGES = 5000;
constexpr char PIPE_NAME[] = "ipc-benchmark-pipe";

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static std::string make_frame(const std::string& json_payload) {
    std::string frame;
    uint32_t len = (uint32_t)json_payload.size();
    frame.push_back((char)(len & 0xFF));
    frame.push_back((char)((len >> 8) & 0xFF));
    frame.push_back((char)((len >> 16) & 0xFF));
    frame.push_back((char)((len >> 24) & 0xFF));
    frame.append(json_payload);
    return frame;
}

// ------------------------------------------------------------------
// Platform-native echo server: accepts multiple sequential connections,
// reads framed JSON requests on each, echoes back framed JSON responses
// with the same request ID.
// ------------------------------------------------------------------
static void run_server(std::atomic<bool>& ready, std::atomic<bool>& stop) {
#ifdef _WIN32
    std::string full_name = "\\\\.\\pipe\\" + std::string(PIPE_NAME);

    HANDLE hPipe = CreateNamedPipeA(
        full_name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        std::cerr << "[server] CreateNamedPipe failed: " << GetLastError() << std::endl;
        ready.store(true);
        return;
    }

    // Signal ready immediately after pipe creation
    ready.store(true);

    while (!stop.load()) {
        // Wait for a client to connect
        if (!ConnectNamedPipe(hPipe, nullptr)) {
            DWORD err = GetLastError();
            if (err != ERROR_PIPE_CONNECTED) {
                break;
            }
        }

        // Handle requests on this connection until client disconnects
        while (!stop.load()) {
            uint8_t header[4];
            DWORD read = 0;
            if (!ReadFile(hPipe, header, 4, &read, nullptr) || read != 4) break;

            uint32_t msg_len = le32(header);
            if (msg_len == 0 || msg_len > 10 * 1024 * 1024) break;

            std::string body(msg_len, '\0');
            if (!ReadFile(hPipe, &body[0], msg_len, &read, nullptr) || read != msg_len) break;

            // Parse request ID
            uint64_t req_id = 0;
            try {
                auto jreq = json::parse(body);
                req_id = jreq.value("id", static_cast<uint64_t>(0));
            } catch (...) {}

            // Build echo response
            json jresp;
            jresp["id"] = req_id;
            jresp["status_code"] = 200;
            jresp["body"] = body;
            jresp["json_data"] = {{"echo", true}};

            std::string response = jresp.dump();
            uint32_t resp_len = (uint32_t)response.size();
            uint8_t resp_header[4];
            resp_header[0] = (uint8_t)(resp_len & 0xFF);
            resp_header[1] = (uint8_t)((resp_len >> 8) & 0xFF);
            resp_header[2] = (uint8_t)((resp_len >> 16) & 0xFF);
            resp_header[3] = (uint8_t)((resp_len >> 24) & 0xFF);

            DWORD written = 0;
            WriteFile(hPipe, resp_header, 4, &written, nullptr);
            WriteFile(hPipe, response.data(), resp_len, &written, nullptr);
            FlushFileBuffers(hPipe);
        }

        // Client disconnected; prepare for next connection
        DisconnectNamedPipe(hPipe);
    }

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
#else
    std::string full_name = PIPE_NAME;
    unlink(full_name.c_str());

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[server] socket() failed: " << strerror(errno) << std::endl;
        ready.store(true);
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, full_name.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[server] bind() failed: " << strerror(errno) << std::endl;
        close(server_fd);
        ready.store(true);
        return;
    }

    if (listen(server_fd, 16) < 0) {
        std::cerr << "[server] listen() failed: " << strerror(errno) << std::endl;
        close(server_fd);
        ready.store(true);
        return;
    }

    ready.store(true);

    while (!stop.load()) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) break;

        // Handle requests on this connection until client disconnects
        while (!stop.load()) {
            uint8_t header[4];
            size_t total = 0;
            while (total < 4) {
                ssize_t n = recv(client_fd, reinterpret_cast<char*>(header + total), 4 - total, 0);
                if (n <= 0) goto client_done;
                total += static_cast<size_t>(n);
            }

            uint32_t msg_len = le32(header);
            if (msg_len == 0 || msg_len > 10 * 1024 * 1024) break;

            std::string body(msg_len, '\0');
            total = 0;
            while (total < msg_len) {
                ssize_t n = recv(client_fd, &body[total], msg_len - total, 0);
                if (n <= 0) goto client_done;
                total += static_cast<size_t>(n);
            }

            // Parse request ID
            uint64_t req_id = 0;
            try {
                auto jreq = json::parse(body);
                req_id = jreq.value("id", static_cast<uint64_t>(0));
            } catch (...) {}

            // Build echo response
            json jresp;
            jresp["id"] = req_id;
            jresp["status_code"] = 200;
            jresp["body"] = body;
            jresp["json_data"] = {{"echo", true}};

            std::string response = jresp.dump();
            uint32_t resp_len = (uint32_t)response.size();
            uint8_t resp_header[4];
            resp_header[0] = (uint8_t)(resp_len & 0xFF);
            resp_header[1] = (uint8_t)((resp_len >> 8) & 0xFF);
            resp_header[2] = (uint8_t)((resp_len >> 16) & 0xFF);
            resp_header[3] = (uint8_t)((resp_len >> 24) & 0xFF);

            total = 0;
            while (total < 4) {
                ssize_t n = send(client_fd, reinterpret_cast<const char*>(resp_header + total), 4 - total, 0);
                if (n <= 0) goto client_done;
                total += static_cast<size_t>(n);
            }
            total = 0;
            while (total < resp_len) {
                ssize_t n = send(client_fd, response.data() + total, resp_len - total, 0);
                if (n <= 0) goto client_done;
                total += static_cast<size_t>(n);
            }
        }

client_done:
        close(client_fd);
    }

    close(server_fd);
    unlink(full_name.c_str());
#endif
}

int main() {
    std::cout << "IPC Benchmark: Named Pipe round-trip (platform-native)" << std::endl;
    std::cout << "=====================================================" << std::endl;

    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};

    std::thread server_thread(run_server, std::ref(ready), std::ref(stop));

    // Wait for server
    while (!ready.load() && !stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    try {
        auto transport = CreateNamedPipeTransport(PIPE_NAME);

        // Warm-up request
        {
            Request req;
            req.method = "POST";
            req.url = "http://localhost";
            req.endpoint = "/bench";
            req.headers["Content-Type"] = "application/json";
            req.payload = "{\"seq\":0}";
            req.timeout_seconds = 10;
            auto resp = transport->Execute(req);
            std::cout << "[warm-up] status=" << resp.status_code << std::endl;
        }

        // Sequential benchmark
        std::string payload(256, 'X');
        auto start = std::chrono::high_resolution_clock::now();

        size_t successes = 0;
        for (size_t i = 0; i < NUM_MESSAGES; i++) {
            Request req;
            req.method = "POST";
            req.url = "http://localhost";
            req.endpoint = "/bench";
            req.headers["Content-Type"] = "application/json";
            json body;
            body["seq"] = (int)i;
            body["data"] = payload;
            req.payload = body.dump();
            req.timeout_seconds = 10;

            auto resp = transport->Execute(req);
            if (resp.status_code == 200) successes++;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_sec = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000000.0;

        std::cout << "\n=== Results ===" << std::endl;
        std::cout << "Messages: " << NUM_MESSAGES << std::endl;
        std::cout << "Successes: " << successes << std::endl;
        std::cout << "Elapsed: " << elapsed_sec << " seconds" << std::endl;
        std::cout << "Round-trips/sec: " << (NUM_MESSAGES / elapsed_sec) << std::endl;
        std::cout << "Avg latency: " << (elapsed_sec * 1000000.0 / NUM_MESSAGES) << " microseconds" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[client] exception: " << e.what() << std::endl;
        stop.store(true);
        server_thread.join();
        return 1;
    }

    stop.store(true);
    server_thread.join();

    std::cout << "\nBenchmark complete." << std::endl;
    return 0;
}
