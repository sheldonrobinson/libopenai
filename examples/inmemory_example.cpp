#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <memory>

#include <nlohmann/json.hpp>

#include "libopenai/libopenai.hpp"
#include "libopenai/inmemory_stream.hpp"
#include "libopenai/inmemory_server.hpp"
#include "libopenai/transport.hpp"

using namespace libopenai;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Simple JSON-framed server: reads 4-byte length-prefixed JSON messages from
// the client->server direction and sends JSON responses back on the
// server->client direction. Mirrors the named-pipe transport framing so
// InMemoryTransport can interoperate.
// ---------------------------------------------------------------------------
static void json_echo_server(std::shared_ptr<InMemoryStream> stream,
                             std::atomic<bool>& running) {
    while (running) {
        // Read 4-byte length header from client->server queue
        std::string header_str;
        if (!stream->recv_from_client(header_str)) break;
        if (header_str.size() < 4) break;

        uint32_t msg_len = static_cast<uint8_t>(header_str[0])
                         | (static_cast<uint8_t>(header_str[1]) << 8)
                         | (static_cast<uint8_t>(header_str[2]) << 16)
                         | (static_cast<uint8_t>(header_str[3]) << 24);

        if (msg_len == 0 || msg_len > 10 * 1024 * 1024) break;

        // Read JSON body from client->server queue
        std::string body(msg_len, '\0');
        size_t total = 0;
        while (total < msg_len) {
            std::string chunk;
            if (!stream->recv_from_client(chunk)) break;
            size_t to_copy = (msg_len - total < chunk.size()) ? (msg_len - total) : chunk.size();
            memcpy(&body[total], chunk.data(), to_copy);
            total += to_copy;
        }
        if (total < msg_len) break;

        // Parse the incoming JSON request
        json jreq;
        try {
            jreq = json::parse(body);
        } catch (...) {
            continue; // skip unparseable messages
        }

        // Build a mock response (echo back the method + endpoint).
        // Include the request ID so the client transport can match this
        // response to the correct pending request (concurrency support).
        json jresp;
        jresp["id"] = jreq.value("id", static_cast<json::number_unsigned_t>(0));
        jresp["status_code"] = 200;
        jresp["body"] = "{\"message\":\"pong from in-memory server\"}";
        jresp["json_data"] = {
            {"method", jreq.value("method", "")},
            {"endpoint", jreq.value("endpoint", "")},
            {"echo", true}
        };

        std::string response = jresp.dump();

        // Frame: 4-byte little-endian length + JSON payload
        uint32_t resp_len = static_cast<uint32_t>(response.size());
        uint8_t resp_header[4];
        resp_header[0] = static_cast<uint8_t>(resp_len & 0xFF);
        resp_header[1] = static_cast<uint8_t>((resp_len >> 8) & 0xFF);
        resp_header[2] = static_cast<uint8_t>((resp_len >> 16) & 0xFF);
        resp_header[3] = static_cast<uint8_t>((resp_len >> 24) & 0xFF);

        // Write response to server->client queue
        std::string resp_header_str(reinterpret_cast<const char*>(resp_header), 4);
        stream->send_to_client(resp_header_str);
        stream->send_to_client(response);
    }
}

int main() {
    std::cout << "=== libopenai In-Memory Socketless Transport Example ==="
              << std::endl;

    // 1. Create the shared in-memory stream (client<->server in same process)
    //    Pure C++17 - no libuv, no cpp-httplib dependency.
    auto stream = std::make_shared<InMemoryStream>(100);

    // 2. Start a server thread that reads JSON requests and responds
    std::atomic<bool> running{true};
    std::thread server_thread(json_echo_server, stream, std::ref(running));

    // 3. Create the InMemoryTransport from the shared stream
    auto transport = CreateInMemoryTransport(stream, 5000);

    // 4. Create a Client using the in-memory transport
    Client client("dummy-key", "http://localhost", transport);
    std::cout << "Active transport: " << client.TransportName() << std::endl;

    // 5. Send a ListModels request over the in-memory transport (blocking)
    std::cout << "\nCalling ListModels() over in-memory transport..."
              << std::endl;
    auto models = client.ListModels();
    std::cout << "Result: "
              << (models.is_null() ? "error/null" : models.dump()) << std::endl;

    // 6. Send a ChatCompletion request (blocking)
    std::cout << "\nCalling ChatCompletion() over in-memory transport..."
              << std::endl;
    std::vector<Message> messages = {
        {"system", "You are a helpful assistant."},
        {"user", "Say hello!"}
    };
    auto chat = client.ChatCompletion(messages, "gpt-4o-mini", 64, 0.7);
    std::cout << "Result: "
              << (chat.is_null() ? "error/null" : chat.dump()) << std::endl;

    // 7. CONCURRENCY TEST: Issue multiple concurrent ExecuteAsync calls
    //    on the same transport instance. Each returns a future immediately;
    //    responses arrive out of order and are matched by request ID.
    std::cout << "\n=== Concurrency Test: 5 concurrent ExecuteAsync calls ==="
              << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    std::vector<std::future<Response>> futures;
    for (int i = 0; i < 5; i++) {
        Request req;
        req.method = "POST";
        req.url = "http://localhost";
        req.endpoint = "/chat/completions";
        req.headers["Content-Type"] = "application/json";
        json body;
        body["model"] = "gpt-4o-mini";
        body["messages"] = json::array({
            {{"role", "user"}, {"content", "Concurrent request #" + std::to_string(i + 1)}}
        });
        body["max_tokens"] = 32;
        req.payload = body.dump();
        req.timeout_seconds = 10;

        // ExecuteAsync returns immediately; request runs on the shared
        // connection with the response demultiplexed by request ID.
        futures.push_back(transport->ExecuteAsync(req));
    }

    // Collect responses as they arrive (may be out of order)
    for (size_t i = 0; i < futures.size(); i++) {
        auto resp = futures[i].get();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        std::cout << "  Response " << (i + 1) << ": status=" << resp.status_code
                  << " body=" << resp.body
                  << " (received at +" << elapsed.count() << "ms)" << std::endl;
    }

    auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);
    std::cout << "  All 5 concurrent requests completed in " << total_elapsed.count()
              << "ms" << std::endl;

    // 7. Clean up
    running = false;
    stream->close();
    if (server_thread.joinable()) server_thread.join();

    // 8. Print metrics
    std::cout << "\nServer metrics:" << std::endl;
    std::cout << "  Messages received: "
              << stream->get_metrics().total_messages_received.load()
              << std::endl;
    std::cout << "  Messages sent:     "
              << stream->get_metrics().total_messages_sent.load()
              << std::endl;
    std::cout << "  Errors:            "
              << stream->get_metrics().total_errors.load()
              << std::endl;

    std::cout << "\n=== Example complete ===" << std::endl;
    return 0;
}
