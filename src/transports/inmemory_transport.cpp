#include "libopenai/transport.hpp"
#include "libopenai/inmemory_stream.hpp"

#ifdef LIBOPENAI_USE_IN_MEMORY

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#ifndef ssize_t
#define ssize_t intptr_t
#endif
#endif

namespace libopenai {

/**
 * @brief Transport that sends OpenAI API requests over an in-memory socketless
 *        stream (InMemoryStream).
 *
 * Pure C++17 implementation. Zero dependencies on libuv, cpp-httplib, or any
 * third-party library beyond nlohmann/json.
 *
 * CONCURRENCY MODEL:
 *   - Single reader thread continuously reads framed messages from the stream
 *     and demultiplexes responses to pending promises by request ID.
 *   - Multiple writer threads can call ExecuteAsync() concurrently; writes are
 *     serialized by write_mutex_ (shared connection).
 *   - Each request is tagged with a unique 64-bit request ID included in the
 *     JSON payload. The server echoes the ID back, allowing the reader thread
 *     to match responses to the correct pending request even when they arrive
 *     out of order.
 *
 * Message framing: each message is prefixed with a 4-byte little-endian
 * length header followed by a JSON payload containing:
 *   { "id": <request_id>, "method": "...", "url": "...", "endpoint": "...",
 *     "headers": {...}, "payload": "...", "timeout_seconds": N }
 */
class InMemoryTransport : public ITransport {
public:
    /**
     * @param stream Shared InMemoryStream connected to an InMemoryServer.
     * @param timeout_ms Maximum time to wait for a response.
     */
    InMemoryTransport(std::shared_ptr<InMemoryStream> stream, int timeout_ms = 5000)
        : stream_(std::move(stream)), timeout_ms_(timeout_ms) {
        // Start the single reader thread that demultiplexes all responses.
        reader_thread_ = std::thread(&InMemoryTransport::reader_loop, this);
    }

    ~InMemoryTransport() override {
        stop_reader_ = true;
        stream_->close();
        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }
    }

    // ------------------------------------------------------------------
    // Blocking execute (single request)
    // ------------------------------------------------------------------
    Response Execute(const Request& req) override {
        auto fut = ExecuteAsync(req);
        return fut.get();
    }

    // ------------------------------------------------------------------
    // Async execute (multiple in-flight requests supported)
    //
    // Registers the promise, writes the framed request to the shared stream,
    // and returns the future immediately. The reader thread will fulfill the
    // promise when the matching response arrives.
    // ------------------------------------------------------------------
    std::future<Response> ExecuteAsync(const Request& req) override {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        uint64_t req_id = next_req_id_.fetch_add(1, std::memory_order_relaxed);

        // Build the JSON request message (includes request ID for correlation)
        json jreq;
        jreq["id"] = static_cast<json::number_unsigned_t>(req_id);
        jreq["method"] = req.method;
        jreq["url"] = req.url;
        jreq["endpoint"] = req.endpoint;
        jreq["headers"] = req.headers;
        jreq["payload"] = req.payload;
        jreq["timeout_seconds"] = req.timeout_seconds;

        std::string message = jreq.dump();

        // Frame: 4-byte little-endian length header + JSON payload
        uint32_t len = static_cast<uint32_t>(message.size());
        uint8_t header[4];
        header[0] = static_cast<uint8_t>(len & 0xFF);
        header[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
        header[2] = static_cast<uint8_t>((len >> 16) & 0xFF);
        header[3] = static_cast<uint8_t>((len >> 24) & 0xFF);

        // Register the pending request BEFORE sending, so no response can
        // be missed between registration and the send.
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_[req_id] = promise;
        }

        // Send on the shared stream. Writes are serialized so framed messages
        // from different requests don't interleave on the wire.
        {
            std::lock_guard<std::mutex> write_lock(write_mutex_);
            std::string header_str(reinterpret_cast<const char*>(header), 4);
            if (!stream_->send_to_server(header_str)) {
                std::lock_guard<std::mutex> lock2(pending_mutex_);
                pending_.erase(req_id);
                fulfill_error(promise, -1, "Failed to write length header");
                return future;
            }

            if (!stream_->send_to_server(message)) {
                std::lock_guard<std::mutex> lock2(pending_mutex_);
                pending_.erase(req_id);
                fulfill_error(promise, -1, "Failed to write message body");
                return future;
            }
        }

        return future;
    }

    std::string Name() const override { return "in-memory"; }

private:
    // ------------------------------------------------------------------
    // Reader loop (single thread): reads framed messages and demultiplexes
    // ------------------------------------------------------------------

    void reader_loop() {
        while (!stop_reader_.load(std::memory_order_relaxed)) {
            // Read 4-byte length header
            uint8_t header[4];
            ssize_t n = read_exact(header, 4);
            if (n != 4) {
                // Stream closed or error - wake all pending requests
                break;
            }

            uint32_t resp_len = static_cast<uint32_t>(header[0])
                              | (static_cast<uint32_t>(header[1]) << 8)
                              | (static_cast<uint32_t>(header[2]) << 16)
                              | (static_cast<uint32_t>(header[3]) << 24);

            if (resp_len == 0 || resp_len > 10 * 1024 * 1024) {
                continue; // skip invalid frames
            }

            // Read response body
            std::string resp_body(resp_len, '\0');
            n = read_exact(reinterpret_cast<uint8_t*>(&resp_body[0]), resp_len);
            if (n != static_cast<ssize_t>(resp_len)) {
                break;
            }

            // Parse response JSON and extract request ID for correlation
            try {
                json jresp = json::parse(resp_body);
                uint64_t resp_id = jresp.value("id", static_cast<uint64_t>(0));

                Response resp;
                resp.status_code = jresp.value("status_code", 0L);
                resp.body = jresp.value("body", std::string());
                resp.x_request_id = jresp.value("x_request_id", std::string());
                resp.json_data = jresp.value("json_data", json{});

                // Look up the pending promise by the response's request ID.
                // This is the key to correct demultiplexing: the ID in the
                // response tells us which pending request it belongs to.
                std::shared_ptr<std::promise<Response>> promise;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    auto it = pending_.find(resp_id);
                    if (it != pending_.end()) {
                        promise = it->second;
                        pending_.erase(it);
                    }
                }

                if (promise) {
                    promise->set_value(std::move(resp));
                }
            } catch (...) {
                // Unparseable response - skip it
            }
        }

        // Reader loop exited: fulfill all remaining pending requests with error
        std::unordered_map<uint64_t, std::shared_ptr<std::promise<Response>>> pending_copy;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_copy = pending_;
            pending_.clear();
        }
        for (auto& pair : pending_copy) {
            fulfill_error(pair.second, -1, "Transport closed");
        }
    }

    /**
     * @brief Read exactly `size` bytes from the stream, blocking until
     *        data is available or the stream is closed / stop_reader_ is set.
     *        Only called by the reader thread, so no read-side locking needed.
     */
    ssize_t read_exact(uint8_t* buf, size_t size) {
        size_t total = 0;
        while (total < size && !stop_reader_.load(std::memory_order_relaxed)) {
            std::string chunk;
            if (!stream_->recv_from_server(chunk)) {
                break;
            }
            size_t to_copy = (size - total < chunk.size()) ? (size - total) : chunk.size();
            memcpy(buf + total, chunk.data(), to_copy);
            total += to_copy;
        }
        return static_cast<ssize_t>(total);
    }

    // ------------------------------------------------------------------
    // Promise fulfillment helpers
    // ------------------------------------------------------------------

    void fulfill_value(std::shared_ptr<std::promise<Response>> promise, Response resp) {
        promise->set_value(std::move(resp));
    }

    void fulfill_error(std::shared_ptr<std::promise<Response>> promise,
                       long status_code, const std::string& body) {
        Response resp;
        resp.status_code = status_code;
        resp.body = body;
        promise->set_value(std::move(resp));
    }

    // ------------------------------------------------------------------
    // Members
    // ------------------------------------------------------------------

    std::shared_ptr<InMemoryStream> stream_;
    int timeout_ms_;

    // Concurrency support: pending requests keyed by request ID
    std::atomic<uint64_t> next_req_id_{1};
    std::mutex pending_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<std::promise<Response>>> pending_;

    // Serializes writes on the shared stream so framed messages don't interleave
    std::mutex write_mutex_;

    // Single reader thread that demultiplexes all incoming responses
    std::thread reader_thread_;
    std::atomic<bool> stop_reader_{false};
};

std::shared_ptr<ITransport> CreateInMemoryTransport(
    std::shared_ptr<InMemoryStream> stream, int timeout_ms) {
    return std::make_shared<InMemoryTransport>(std::move(stream), timeout_ms);
}

} // namespace libopenai

#endif // LIBOPENAI_USE_IN_MEMORY
