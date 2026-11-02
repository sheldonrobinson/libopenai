#pragma once

#include "libopenai/inmemory_stream.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace libopenai {

// ============================================================================
// InMemoryServer - in-memory socketless IPC server
//
// Pure C++17 implementation. Zero dependencies on libuv, cpp-httplib, or any
// third-party library. Accepts InMemoryStream connections and dispatches each
// connection to a handler thread for request/response processing.
//
// Designed for embedded scenarios where a client and server coexist in the
// same process and communicate over in-memory queues instead of network
// sockets.
// ============================================================================
class InMemoryServer {
public:
    using StreamPtr = std::shared_ptr<InMemoryStream>;
    using Handler = std::function<void(StreamPtr)>;

    InMemoryServer(size_t max_queue_size = 100)
        : max_queue_size_(max_queue_size), running_(false) {}

    ~InMemoryServer() {
        stop();
    }

    // Start accepting connections. Spawns a dispatcher thread.
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return;
        running_ = true;
        dispatcher_thread_ = std::thread(&InMemoryServer::dispatch_loop, this);
    }

    // Stop the server and close all connections.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            running_ = false;
        }
        cv_.notify_all();

        if (dispatcher_thread_.joinable()) {
            dispatcher_thread_.join();
        }

        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (auto& conn : connections_) {
            conn->close();
        }
        connections_.clear();
    }

    // Create a new in-memory connection and register it with the server.
    // Returns a stream that can be used by a client to connect to the server.
    StreamPtr create_connection() {
        auto stream = std::make_shared<InMemoryStream>(max_queue_size_);
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            connections_.insert(stream);
            metrics_.active_connections++;
            metrics_.total_connections++;
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            conn_queue_.push_back(stream);
        }
        cv_.notify_one();
        return stream;
    }

    // Set the request handler. Called once per connection in a dedicated thread.
    void set_handler(Handler h) {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler_ = std::move(h);
    }

    const InMemoryStreamMetrics& get_metrics() const { return metrics_; }

    bool is_running() const { return running_; }

private:
    void dispatch_loop() {
        while (true) {
            std::vector<StreamPtr> batch;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this] {
                    return !conn_queue_.empty() || !running_;
                });

                if (!running_ && conn_queue_.empty()) {
                    return;
                }

                // Drain all pending connections
                while (!conn_queue_.empty()) {
                    batch.push_back(std::move(conn_queue_.front()));
                    conn_queue_.erase(conn_queue_.begin());
                }
            }

            for (auto& stream : batch) {
                Handler h;
                {
                    std::lock_guard<std::mutex> lock(handler_mutex_);
                    h = handler_;
                }
                if (h) {
                    std::thread([this, h, stream]() {
                        try {
                            h(stream);
                        } catch (const std::exception&) {
                            metrics_.total_errors++;
                        } catch (...) {
                            metrics_.total_errors++;
                        }
                        remove_connection(stream);
                    }).detach();
                } else {
                    remove_connection(stream);
                }
            }
        }
    }

    void remove_connection(const StreamPtr& stream) {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (!stream->is_closed()) {
            stream->close();
            metrics_.active_connections--;
        }
        connections_.erase(stream);
    }

    size_t max_queue_size_;
    std::atomic<bool> running_;

    std::mutex mutex_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::thread dispatcher_thread_;

    std::vector<StreamPtr> conn_queue_;
    std::mutex conn_mutex_;
    std::unordered_set<StreamPtr> connections_;

    Handler handler_;
    std::mutex handler_mutex_;

    InMemoryStreamMetrics metrics_;
};

} // namespace libopenai
