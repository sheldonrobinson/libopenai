#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <string>
#include <mutex>
#include <future>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "libopenai/libopenai.hpp"
#include "libopenai/transport.hpp"
#include <nlohmann/json.hpp>

using namespace libopenai;

static constexpr char TEST_PIPE[] = "libopenai-integration-test-pipe";

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// ------------------------------------------------------------------
// Platform-native server: listens on a named pipe / Unix domain socket,
// reads framed JSON requests, sends framed JSON responses echoing the
// request ID. No libuv dependency.
// ------------------------------------------------------------------
static void run_server(std::atomic<bool>& ready, std::atomic<bool>& done) {
#ifdef _WIN32
    std::string full_name = "\\\\.\\pipe\\" + std::string(TEST_PIPE);

    HANDLE hPipe = CreateNamedPipeA(
        full_name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        std::cerr << "[server] CreateNamedPipe failed: " << GetLastError() << std::endl;
        done.store(true);
        return;
    }

    // Signal ready immediately after pipe creation (before blocking on ConnectNamedPipe)
    ready.store(true);

    if (!ConnectNamedPipe(hPipe, nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            std::cerr << "[server] ConnectNamedPipe failed: " << err << std::endl;
            CloseHandle(hPipe);
            done.store(true);
            return;
        }
    }

    // Read 4-byte length header
    uint8_t header[4];
    DWORD read = 0;
    if (!ReadFile(hPipe, header, 4, &read, nullptr) || read != 4) {
        std::cerr << "[server] Failed to read header" << std::endl;
        CloseHandle(hPipe);
        done.store(true);
        return;
    }

    uint32_t msg_len = le32(header);
    if (msg_len == 0 || msg_len > 10 * 1024 * 1024) {
        std::cerr << "[server] Invalid message length: " << msg_len << std::endl;
        CloseHandle(hPipe);
        done.store(true);
        return;
    }

    // Read JSON body
    std::string body(msg_len, '\0');
    if (!ReadFile(hPipe, &body[0], msg_len, &read, nullptr) || read != msg_len) {
        std::cerr << "[server] Failed to read body" << std::endl;
        CloseHandle(hPipe);
        done.store(true);
        return;
    }

    // Parse request to extract the request ID
    uint64_t req_id = 0;
    try {
        auto jreq = json::parse(body);
        req_id = jreq.value("id", static_cast<uint64_t>(0));
    } catch (...) {
        // Use default req_id = 0
    }

    std::cout << "[server] Received request id=" << req_id << std::endl;
    std::cout.flush();

    // Build response echoing the request ID
    json jresp;
    jresp["id"] = req_id;
    jresp["status_code"] = 200;
    jresp["body"] = "{\"data\":[]}";
    jresp["json_data"] = {{"object", "list"}, {"data", json::array()}};

    std::string response = jresp.dump();
    uint32_t resp_len = static_cast<uint32_t>(response.size());
    uint8_t resp_header[4];
    resp_header[0] = static_cast<uint8_t>(resp_len & 0xFF);
    resp_header[1] = static_cast<uint8_t>((resp_len >> 8) & 0xFF);
    resp_header[2] = static_cast<uint8_t>((resp_len >> 16) & 0xFF);
    resp_header[3] = static_cast<uint8_t>((resp_len >> 24) & 0xFF);

    // Write response header + body
    DWORD written = 0;
    WriteFile(hPipe, resp_header, 4, &written, nullptr);
    WriteFile(hPipe, response.data(), resp_len, &written, nullptr);

    FlushFileBuffers(hPipe);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);

#else
    // Unix domain socket
    std::string full_name = TEST_PIPE;
    unlink(full_name.c_str());

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[server] socket() failed: " << strerror(errno) << std::endl;
        done.store(true);
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, full_name.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[server] bind() failed: " << strerror(errno) << std::endl;
        close(server_fd);
        done.store(true);
        return;
    }

    if (listen(server_fd, 16) < 0) {
        std::cerr << "[server] listen() failed: " << strerror(errno) << std::endl;
        close(server_fd);
        done.store(true);
        return;
    }

    ready.store(true);

    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
        std::cerr << "[server] accept() failed: " << strerror(errno) << std::endl;
        close(server_fd);
        done.store(true);
        return;
    }

    // Read 4-byte length header
    uint8_t header[4];
    size_t total = 0;
    while (total < 4) {
        ssize_t n = recv(client_fd, reinterpret_cast<char*>(header + total), 4 - total, 0);
        if (n <= 0) {
            std::cerr << "[server] Failed to read header" << std::endl;
            close(client_fd);
            close(server_fd);
            done.store(true);
            return;
        }
        total += static_cast<size_t>(n);
    }

    uint32_t msg_len = le32(header);
    if (msg_len == 0 || msg_len > 10 * 1024 * 1024) {
        std::cerr << "[server] Invalid message length: " << msg_len << std::endl;
        close(client_fd);
        close(server_fd);
        done.store(true);
        return;
    }

    // Read JSON body
    std::string body(msg_len, '\0');
    total = 0;
    while (total < msg_len) {
        ssize_t n = recv(client_fd, &body[total], msg_len - total, 0);
        if (n <= 0) {
            std::cerr << "[server] Failed to read body" << std::endl;
            close(client_fd);
            close(server_fd);
            done.store(true);
            return;
        }
        total += static_cast<size_t>(n);
    }

    // Parse request to extract the request ID
    uint64_t req_id = 0;
    try {
        auto jreq = json::parse(body);
        req_id = jreq.value("id", static_cast<uint64_t>(0));
    } catch (...) {
        // Use default req_id = 0
    }

    std::cout << "[server] Received request id=" << req_id << std::endl;

    // Build response echoing the request ID
    json jresp;
    jresp["id"] = req_id;
    jresp["status_code"] = 200;
    jresp["body"] = "{\"data\":[]}";
    jresp["json_data"] = {{"object", "list"}, {"data", json::array()}};

    std::string response = jresp.dump();
    uint32_t resp_len = static_cast<uint32_t>(response.size());
    uint8_t resp_header[4];
    resp_header[0] = static_cast<uint8_t>(resp_len & 0xFF);
    resp_header[1] = static_cast<uint8_t>((resp_len >> 8) & 0xFF);
    resp_header[2] = static_cast<uint8_t>((resp_len >> 16) & 0xFF);
    resp_header[3] = static_cast<uint8_t>((resp_len >> 24) & 0xFF);

    // Write response header + body
    total = 0;
    while (total < 4) {
        ssize_t n = send(client_fd, reinterpret_cast<const char*>(resp_header + total), 4 - total, 0);
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }
    total = 0;
    while (total < resp_len) {
        ssize_t n = send(client_fd, response.data() + total, resp_len - total, 0);
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }

    close(client_fd);
    close(server_fd);
    unlink(full_name.c_str());
#endif

    done.store(true);
}

int main() {
    std::cout << "=== NamedPipeTransport end-to-end integration test ==="
              << std::endl;

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_done{false};

    std::thread server(run_server, std::ref(server_ready), std::ref(server_done));

    // Wait for server to start listening
    for (int i = 0; i < 500 && !server_ready.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!server_ready.load()) {
        std::cerr << "[client] Server failed to start" << std::endl;
        return 1;
    }

    bool test_passed = false;
    try {
        auto transport = CreateNamedPipeTransport(TEST_PIPE);
        // Give the reader thread time to establish the connection
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Client client("dummy-key", "http://localhost", transport);

        std::cout << "[client] transport: " << client.TransportName() << std::endl;

        // Issue a ListModels request over the named pipe.
        std::cout << "[client] Calling ListModels()..." << std::endl;
        json result = client.ListModels();
        std::cout << "[client] ListModels returned." << std::endl;

        if (!result.is_null() && result.contains("object") && result["object"] == "list") {
            test_passed = true;
            std::cout << "[client] ListModels OK: " << result.dump() << std::endl;
        } else {
            std::cout << "[client] ListModels returned: " << result.dump() << std::endl;
        }
    } catch (const std::future_error& fe) {
        std::cerr << "[client] future_error code=" << fe.code()
                  << " what=" << fe.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[client] exception: " << e.what() << std::endl;
    }

    server.join();

    std::cout << (test_passed ? "\n[PASS] end-to-end integration test passed"
                               : "\n[FAIL] end-to-end integration test failed")
              << std::endl;
    return test_passed ? 0 : 1;
}
