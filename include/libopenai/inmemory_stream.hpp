#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace libopenai {

// ============================================================================
// InMemoryStreamMetrics - lightweight metrics for the in-memory stream
// ============================================================================
struct InMemoryStreamMetrics {
    std::atomic<size_t> active_connections{0};
    std::atomic<size_t> total_messages_received{0};
    std::atomic<size_t> total_messages_sent{0};
    std::atomic<size_t> total_errors{0};
    std::atomic<size_t> total_connections{0};
};

// ============================================================================
// InMemoryStream - lossless blocking in-memory duplex stream
//
// Pure C++17 implementation. Zero dependencies on libuv, cpp-httplib, or any
// third-party library. Thread-safe: send_to_server() and recv_from_server()
// can be called concurrently from different threads.
//
// Two independent bounded queues provide full-duplex communication:
//   - client_to_server: written by send_to_server(), read by server side
//   - server_to_client: written by server side, read by recv_from_server()
// ============================================================================
class InMemoryStream {
public:
    explicit InMemoryStream(size_t max_queue_size = 100)
        : max_queue_size_(max_queue_size), closed_(false) {}

    ~InMemoryStream() {
        close();
    }

    // Client sends data to server (request direction). Blocks if queue is full.
    bool send_to_server(const std::string& data) {
        if (closed_) return false;

        std::unique_lock<std::mutex> lock(c2s_mutex_);
        c2s_cv_not_full_.wait(lock, [this] {
            return client_to_server_queue_.size() < max_queue_size_ || closed_;
        });

        if (closed_) return false;

        client_to_server_queue_.push(data);
        metrics_.total_messages_received++;
        c2s_cv_not_empty_.notify_one();
        return true;
    }

    // Client receives data from server (response direction).
    // Blocks until data is available or the stream is closed.
    // Returns false if the stream is closed and no data remains.
    bool recv_from_server(std::string& out) {
        std::unique_lock<std::mutex> lock(s2c_mutex_);
        s2c_cv_not_empty_.wait(lock, [this] {
            return !server_to_client_queue_.empty() || closed_;
        });

        if (server_to_client_queue_.empty()) return false;

        out = std::move(server_to_client_queue_.front());
        server_to_client_queue_.pop();
        metrics_.total_messages_sent++;
        s2c_cv_not_full_.notify_one();
        return true;
    }

    // Server receives data from client (request direction).
    // Same underlying queue as recv_from_server but exposed with a distinct name
    // for semantic clarity on the server side.
    bool recv_from_client(std::string& out) {
        std::unique_lock<std::mutex> lock(c2s_mutex_);
        c2s_cv_not_empty_.wait(lock, [this] {
            return !client_to_server_queue_.empty() || closed_;
        });

        if (client_to_server_queue_.empty()) return false;

        out = std::move(client_to_server_queue_.front());
        client_to_server_queue_.pop();
        c2s_cv_not_full_.notify_one();
        return true;
    }

    // Server sends data to client (response direction).
    // Same underlying queue as send_to_server but exposed with a distinct name
    // for semantic clarity on the server side.
    bool send_to_client(const std::string& data) {
        if (closed_) return false;

        std::unique_lock<std::mutex> lock(s2c_mutex_);
        s2c_cv_not_full_.wait(lock, [this] {
            return server_to_client_queue_.size() < max_queue_size_ || closed_;
        });

        if (closed_) return false;

        server_to_client_queue_.push(data);
        metrics_.total_messages_sent++;
        s2c_cv_not_empty_.notify_one();
        return true;
    }

    void close() {
        if (closed_.exchange(true)) return;

        c2s_cv_not_empty_.notify_all();
        c2s_cv_not_full_.notify_all();
        s2c_cv_not_empty_.notify_all();
        s2c_cv_not_full_.notify_all();
    }

    bool is_closed() const { return closed_; }

    const InMemoryStreamMetrics& get_metrics() const { return metrics_; }

private:
    std::queue<std::string> client_to_server_queue_;
    std::queue<std::string> server_to_client_queue_;

    std::mutex c2s_mutex_;
    std::condition_variable c2s_cv_not_empty_;
    std::condition_variable c2s_cv_not_full_;

    std::mutex s2c_mutex_;
    std::condition_variable s2c_cv_not_empty_;
    std::condition_variable s2c_cv_not_full_;

    size_t max_queue_size_;
    std::atomic<bool> closed_;
    InMemoryStreamMetrics metrics_;
};

} // namespace libopenai
