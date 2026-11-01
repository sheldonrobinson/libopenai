#include "libopenai/transport.hpp"

#ifdef LIBOPENAI_USE_NAMED_PIPE

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <uv.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <queue>

#ifdef DeleteFile
#undef DeleteFile
#endif

namespace libopenai {

/**
 * @brief Transport that sends OpenAI API requests over a libuv named pipe.
 *
 * Enables socketless IPC between processes. A client process uses this
 * transport to send requests to an embedded server process listening on
 * a named pipe (uv_pipe_t).
 *
 * Message framing: each message is prefixed with a 4-byte little-endian
 * length header followed by the JSON payload.
 */
class NamedPipeTransport : public ITransport {
public:
    NamedPipeTransport(const std::string& pipe_name, int connect_timeout_ms = 5000)
        : pipe_name_(pipe_name), connect_timeout_ms_(connect_timeout_ms) {
        loop_ = uv_loop_new();
        uv_pipe_init(loop_, &pipe_, 1);
        pipe_.data = this;
        read_promise_ = nullptr;
    }

    ~NamedPipeTransport() {
        disconnect();
        uv_loop_close(loop_);
        uv_loop_delete(loop_);
    }

    Response Execute(const Request& req) override {
        Response resp;

        if (!ensure_connected()) {
            resp.status_code = -1;
            resp.body = "Failed to connect to named pipe: " + pipe_name_;
            return resp;
        }

        // Build JSON request from the Request struct
        json jreq;
        jreq["method"] = req.method;
        jreq["url"] = req.url;
        jreq["endpoint"] = req.endpoint;
        jreq["headers"] = req.headers;
        jreq["payload"] = req.payload;
        jreq["timeout_seconds"] = req.timeout_seconds;

        std::string message = jreq.dump();

        // Send with length prefix (4 bytes, little-endian)
        uint32_t len = static_cast<uint32_t>(message.size());
        uint8_t header[4];
        header[0] = static_cast<uint8_t>(len & 0xFF);
        header[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
        header[2] = static_cast<uint8_t>((len >> 16) & 0xFF);
        header[3] = static_cast<uint8_t>((len >> 24) & 0xFF);

        std::lock_guard<std::mutex> lock(write_mutex_);

        // Write length header
        uv_buf_t header_buf = uv_buf_init(reinterpret_cast<char*>(header), 4);
        if (uv_write(&write_req_, reinterpret_cast<uv_stream_t*>(&pipe_), &header_buf, 1, on_write) != 0) {
            resp.status_code = -1;
            resp.body = "Failed to write length header";
            return resp;
        }

        // Write message body
        uv_buf_t body_buf = uv_buf_init(const_cast<char*>(message.data()), static_cast<unsigned int>(message.size()));
        if (uv_write(&write_req_, reinterpret_cast<uv_stream_t*>(&pipe_), &body_buf, 1, on_write) != 0) {
            resp.status_code = -1;
            resp.body = "Failed to write message body";
            return resp;
        }

        // Read response length header (4 bytes)
        uint8_t resp_header[4];
        if (read_exact(resp_header, 4) != 4) {
            resp.status_code = -1;
            resp.body = "Failed to read response length";
            return resp;
        }

        uint32_t resp_len = static_cast<uint32_t>(resp_header[0])
                          | (static_cast<uint32_t>(resp_header[1]) << 8)
                          | (static_cast<uint32_t>(resp_header[2]) << 16)
                          | (static_cast<uint32_t>(resp_header[3]) << 24);

        if (resp_len == 0 || resp_len > 10 * 1024 * 1024) {
            resp.status_code = -1;
            resp.body = "Invalid response length: " + std::to_string(resp_len);
            return resp;
        }

        // Read response body
        std::string resp_body(resp_len, '\0');
        if (read_exact(reinterpret_cast<uint8_t*>(&resp_body[0]), resp_len) != static_cast<ssize_t>(resp_len)) {
            resp.status_code = -1;
            resp.body = "Failed to read response body";
            return resp;
        }

        // Parse response JSON
        try {
            json jresp = json::parse(resp_body);
            resp.status_code = jresp.value("status_code", 0L);
            resp.body = jresp.value("body", std::string());
            resp.x_request_id = jresp.value("x_request_id", std::string());
            resp.json_data = jresp.value("json_data", json{});
        } catch (...) {
            resp.status_code = -1;
            resp.body = resp_body;
        }

        return resp;
    }

    std::string Name() const override { return "named-pipe"; }

private:
    // Callback for uv_write completion
    static void on_write(uv_write_t* req, int status) {
        // Write completion - we don't need to do anything special here
        // since we use synchronous uv_write calls that complete immediately
        // for named pipes (data is buffered)
    }

    // Callback for uv_read completion - delivers data to the waiting promise
    static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        auto* self = static_cast<NamedPipeTransport*>(stream->data);

        if (nread < 0) {
            if (nread != UV_EOF) {
                std::cerr << "Read error: " << uv_strerror(nread) << std::endl;
            }
            self->connected_ = false;
            delete[] buf->base;
            // Unblock any waiting reader with empty result
            if (self->read_promise_) {
                auto p = std::move(self->read_promise_);
                self->read_promise_ = nullptr;
                p->set_value(std::string());
            }
            return;
        }

        if (nread == 0) {
            delete[] buf->base;
            return;
        }

        std::string data(buf->base, static_cast<size_t>(nread));
        delete[] buf->base;

        // Deliver data to waiting promise
        std::lock_guard<std::mutex> lock(self->read_mutex_);
        if (self->read_promise_) {
            auto p = std::move(self->read_promise_);
            self->read_promise_ = nullptr;
            p->set_value(data);
        } else {
            // No reader waiting - buffer the data
            self->read_buffer_.push(data);
        }
    }

    // Callback for uv_connect completion
    static void on_connect(uv_connect_t* req, int status) {
        auto* self = static_cast<NamedPipeTransport*>(req->data);
        if (status == 0) {
            self->connected_ = true;
            uv_read_start(reinterpret_cast<uv_stream_t*>(&self->pipe_),
                          [](uv_handle_t*, size_t suggested_size, uv_buf_t* buf) {
                              buf->base = new char[suggested_size];
                              buf->len = static_cast<unsigned int>(suggested_size);
                          },
                          on_read);
        }
        delete req;
    }

    bool ensure_connected() {
        if (connected_) return true;

#ifdef _WIN32
        std::string full_name = "\\\\.\\pipe\\" + pipe_name_;
#else
        std::string full_name = pipe_name_;
#endif

        uv_connect_t* conn_req = new uv_connect_t();
        conn_req->data = this;

        uv_pipe_connect(conn_req, &pipe_, full_name.c_str(), on_connect);
        // Connection result is delivered via on_connect callback which sets connected_

        // Run the event loop to process the connect
        auto start = std::chrono::steady_clock::now();
        while (!connected_) {
            uv_run(loop_, UV_RUN_NOWAIT);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed.count() >= connect_timeout_ms_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return connected_;
    }

    void disconnect() {
        if (connected_) {
            uv_read_stop(reinterpret_cast<uv_stream_t*>(&pipe_));
            uv_close(reinterpret_cast<uv_handle_t*>(&pipe_), nullptr);
            connected_ = false;
        }
    }

    /**
     * @brief Read exactly `size` bytes from the pipe using promise/future.
     *
     * Uses uv_read_start with a promise/future mechanism to properly
     * handle async reads. If data is already buffered (from a previous
     * read that delivered more than needed), it's served from the buffer.
     */
    ssize_t read_exact(uint8_t* buf, size_t size) {
        size_t total = 0;

        while (total < size) {
            // First check if we have buffered data (release the mutex before
            // registering the read promise - otherwise we self-deadlock).
            bool consumed = false;
            {
                std::lock_guard<std::mutex> lock(read_mutex_);
                if (!read_buffer_.empty()) {
                    std::string& buffered = read_buffer_.front();
                    size_t to_copy = std::min(size - total, buffered.size());
                    memcpy(buf + total, buffered.data(), to_copy);
                    total += to_copy;
                    buffered.erase(0, to_copy);
                    if (buffered.empty()) {
                        read_buffer_.pop();
                    }
                    consumed = true;
                }
            }
            if (consumed) continue;

            // No buffered data, need to wait for async read
            auto promise = std::make_shared<std::promise<std::string>>();
            auto future = promise->get_future();

            {
                std::lock_guard<std::mutex> lock(read_mutex_);
                read_promise_ = promise;
            }

            // Run the event loop until data arrives or timeout
            auto start = std::chrono::steady_clock::now();
            while (true) {
                uv_run(loop_, UV_RUN_NOWAIT);

                // Check if future is ready
                if (future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
                    break;
                }

                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                if (elapsed.count() >= connect_timeout_ms_) {
                    // Timeout - clear the promise
                    std::lock_guard<std::mutex> lock(read_mutex_);
                    if (read_promise_ && read_promise_.get() == promise.get()) {
                        read_promise_ = nullptr;
                    }
                    return static_cast<ssize_t>(total);
                }
            }

            // Get the data
            std::string data;
            try {
                data = future.get();
            } catch (...) {
                return static_cast<ssize_t>(total);
            }

            if (data.empty()) {
                // EOF or error
                return static_cast<ssize_t>(total);
            }

            // Copy data into buffer
            size_t to_copy = std::min(size - total, data.size());
            memcpy(buf + total, data.data(), to_copy);
            total += to_copy;

            // Buffer any remaining data
            if (to_copy < data.size()) {
                std::lock_guard<std::mutex> lock(read_mutex_);
                read_buffer_.push(data.substr(to_copy));
            }
        }

        return static_cast<ssize_t>(total);
    }

    uv_loop_t* loop_;
    uv_pipe_t pipe_;
    uv_write_t write_req_;
    std::string pipe_name_;
    int connect_timeout_ms_;
    std::atomic<bool> connected_{false};
    std::mutex write_mutex_;

    // Async read support
    std::mutex read_mutex_;
    std::shared_ptr<std::promise<std::string>> read_promise_;
    std::queue<std::string> read_buffer_;
};

std::shared_ptr<ITransport> CreateNamedPipeTransport(const std::string& pipe_name) {
    return std::make_shared<NamedPipeTransport>(pipe_name);
}

} // namespace libopenai

#endif // LIBOPENAI_USE_NAMED_PIPE
