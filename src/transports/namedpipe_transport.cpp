#include "libopenai/transport.hpp"

#ifdef LIBOPENAI_USE_NAMED_PIPE

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(LIBOPENAI_USE_IO_URING) && defined(__linux__)
#include <liburing.h>
#endif

#if defined(_WIN32)
#ifndef ssize_t
#define ssize_t intptr_t
#endif
#endif

#ifdef DeleteFile
#undef DeleteFile
#endif

namespace libopenai {

/**
 * @brief Transport that sends OpenAI API requests over a platform-native
 *        named pipe / Unix domain socket.
 *
 * Pure C++17 implementation. Zero dependencies on libuv or any third-party
 * library beyond nlohmann/json (and optionally liburing on Linux).
 * Uses platform-native IPC:
 *   - Windows: Named pipes via CreateFile / ReadFile / WriteFile
 *   - Unix:    Unix domain sockets via socket(AF_UNIX) / connect / read / write
 *
 * CONCURRENCY MODEL (Connection Pool):
 *   - The transport maintains a pool of N pre-connected pipe/socket handles.
 *   - Each ExecuteAsync() call checks out ONE connection from the pool,
 *     performs write+read on that connection, then returns it to the pool.
 *     A connection is never used by two threads simultaneously, which avoids
 *     concurrent ReadFile/WriteFile on the same handle (which can deadlock
 *     on Windows named pipes).
 *   - Multiple ExecuteAsync() calls run concurrently, each on its own
 *     pooled connection. Each request is tagged with a unique 64-bit request
 *     ID included in the JSON payload for response correlation.
 *   - If the pool is exhausted, callers block waiting for a connection to
 *     be returned.
 *
 * LINUX io_URING PATH:
 *   - When LIBOPENAI_USE_IO_URING is defined on Linux, socket I/O uses
 *     io_uring (IORING_OP_SEND / IORING_OP_RECV) instead of blocking
 *     send/recv. A single io_uring instance services all pooled connections.
 *   - A mutex protects submit+wait on the shared ring so concurrent threads
 *     don't consume each other's completions.
 *   - io_uring_submit_and_wait batches the submission and wait into a single
 *     syscall, reducing context switches versus blocking send/recv.
 *
 * Message framing: each message is prefixed with a 4-byte little-endian
 * length header followed by a JSON payload containing:
 *   { "id": <request_id>, "method": "...", "url": "...", "endpoint": "...",
 *     "headers": {...}, "payload": "...", "timeout_seconds": N }
 */
class NamedPipeTransport : public ITransport {
public:
    /**
     * @param pipe_name The named pipe name (e.g. "myapp-pipe" or
     *                  "/tmp/myapp.sock" on Unix).
     * @param pool_size Maximum number of pooled connections. Default 4.
     * @param connect_timeout_ms Maximum time to wait for connection.
     * @param timeout_ms Maximum time to wait for read/write operations.
     */
    NamedPipeTransport(const std::string& pipe_name,
                       size_t pool_size = 4,
                       int connect_timeout_ms = 5000,
                       int timeout_ms = 5000)
        : pipe_name_(pipe_name),
          pool_size_(pool_size == 0 ? 1 : pool_size),
          connect_timeout_ms_(connect_timeout_ms),
          timeout_ms_(timeout_ms) {
#if defined(LIBOPENAI_USE_IO_URING) && defined(__linux__)
        // Initialize a single io_uring instance that services all pooled
        // connections. Queue entries = 2 * pool_size (send + recv per conn).
        int ring_entries = static_cast<int>(pool_size_ * 2 + 8);
        int ret = io_uring_queue_init(ring_entries, &ring_, 0);
        if (ret < 0) {
            std::cerr << "[io_uring] io_uring_queue_init failed: "
                      << strerror(-ret) << std::endl;
            ring_ = {};
            ring_init_ok_ = false;
        } else {
            ring_init_ok_ = true;
        }
#endif

        // Pre-connect pool_size connections so they're ready for use.
        for (size_t i = 0; i < pool_size_; ++i) {
            auto conn = platform_connect();
            if (conn.valid) {
                pool_.push_back(std::move(conn));
            }
        }
    }

    ~NamedPipeTransport() override {
#if defined(LIBOPENAI_USE_IO_URING) && defined(__linux__)
        if (ring_init_ok_) {
            io_uring_queue_exit(&ring_);
        }
#endif

        // Close all pooled connections.
        std::lock_guard<std::mutex> lock(pool_mutex_);
        for (auto& conn : pool_) {
            platform_disconnect(conn);
        }
        pool_.clear();
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
    // Each call checks out a pooled connection, performs write+read,
    // then returns the connection to the pool. Returns a future
    // immediately; the request runs on a background thread.
    // Multiple concurrent ExecuteAsync() calls are fully supported.
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

        // Each request runs on its own background thread with a pooled
        // connection. This avoids concurrent ReadFile/WriteFile on a shared
        // handle (which can deadlock on Windows named pipes).
        std::thread([this, req_id, header, message, promise]() mutable {
            try {
                // Check out a connection from the pool (blocks if pool empty).
                Connection conn = check_out();
                if (!conn.valid) {
                    fulfill_error(promise, req_id, -1,
                                  "Failed to obtain pooled connection");
                    return;
                }

                bool ok = true;

                // Write length header
                if (!io_write_all(conn, reinterpret_cast<const char*>(header), 4)) {
                    fulfill_error(promise, req_id, -1, "Failed to write length header");
                    ok = false;
                }

                // Write message body
                if (ok && !io_write_all(conn, message.data(), message.size())) {
                    fulfill_error(promise, req_id, -1, "Failed to write message body");
                    ok = false;
                }

#if defined(_WIN32)
                if (ok) {
                    FlushFileBuffers(conn.handle);
                }
#endif

                // Read response length header (4 bytes)
                uint8_t resp_header[4] = {0, 0, 0, 0};
                if (ok && io_read_exact(conn, resp_header, 4) != 4) {
                    fulfill_error(promise, req_id, -1, "Failed to read response length");
                    ok = false;
                }

                uint32_t resp_len = 0;
                if (ok) {
                    resp_len = static_cast<uint32_t>(resp_header[0])
                             | (static_cast<uint32_t>(resp_header[1]) << 8)
                             | (static_cast<uint32_t>(resp_header[2]) << 16)
                             | (static_cast<uint32_t>(resp_header[3]) << 24);

                    if (resp_len == 0 || resp_len > 10 * 1024 * 1024) {
                        fulfill_error(promise, req_id, -1,
                                      "Invalid response length: " + std::to_string(resp_len));
                        ok = false;
                    }
                }

                // Read response body
                std::string resp_body;
                if (ok) {
                    resp_body.resize(resp_len);
                    if (io_read_exact(conn,
                                       reinterpret_cast<uint8_t*>(&resp_body[0]), resp_len) !=
                        static_cast<ssize_t>(resp_len)) {
                        fulfill_error(promise, req_id, -1, "Failed to read response body");
                        ok = false;
                    }
                }

                // Parse response JSON and correlate by request ID
                if (ok) {
                    try {
                        json jresp = json::parse(resp_body);
                        uint64_t resp_id = jresp.value("id", static_cast<uint64_t>(0));

                        Response resp;
                        resp.status_code = jresp.value("status_code", 0L);
                        resp.body = jresp.value("body", std::string());
                        resp.x_request_id = jresp.value("x_request_id", std::string());
                        resp.json_data = jresp.value("json_data", json{});

                        // Verify the response ID matches our request ID
                        if (resp_id != req_id) {
                            fulfill_error(promise, req_id, -1,
                                          "Response ID mismatch: expected " +
                                              std::to_string(req_id) + ", got " +
                                              std::to_string(resp_id));
                        } else {
                            fulfill_value(promise, req_id, std::move(resp));
                        }
                    } catch (...) {
                        fulfill_error(promise, req_id, -1, resp_body);
                    }
                }

                // Return the connection to the pool.
                check_in(std::move(conn));
            } catch (const std::exception& e) {
                fulfill_error(promise, req_id, -1,
                              std::string("Exception: ") + e.what());
            } catch (...) {
                fulfill_error(promise, req_id, -1, "Unknown exception");
            }
        }).detach();

        return future;
    }

    std::string Name() const override { return "named-pipe"; }

    /**
     * @brief Returns the number of connections currently available in the pool.
     */
    size_t pool_available() const {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return pool_.size();
    }

private:
    // Connection handle wrapper
    struct Connection {
#ifdef _WIN32
        HANDLE handle;
#else
        int handle;
#endif
        bool valid;
    };

#if defined(LIBOPENAI_USE_IO_URING) && defined(__linux__)
    // io_uring ring instance (single, shared across all pooled connections).
    // Guarded by ring_mutex_ so concurrent threads don't consume each other's
    // completions via io_uring_wait_cqe.
    io_uring ring_;
    bool ring_init_ok_ = false;
    std::mutex ring_mutex_;
#endif

    std::string pipe_name_;
    size_t pool_size_;
    int connect_timeout_ms_;
    int timeout_ms_;

    // Concurrency support: unique request IDs
    std::atomic<uint64_t> next_req_id_{1};

    // ------------------------------------------------------------------
    // Connection pool
    // ------------------------------------------------------------------

    mutable std::mutex pool_mutex_;
    std::condition_variable pool_cv_;
    std::vector<Connection> pool_;

    // ------------------------------------------------------------------
    // Pool checkout/checkin
    // ------------------------------------------------------------------

    /**
     * @brief Check out a connection from the pool. Blocks until one is
     *        available or a new one can be created.
     */
    Connection check_out() {
        std::unique_lock<std::mutex> lock(pool_mutex_);

        // Wait until a connection is available in the pool.
        pool_cv_.wait(lock, [this] { return !pool_.empty(); });

        Connection conn = pool_.back();
        pool_.pop_back();
        return conn;
    }

    /**
     * @brief Return a connection to the pool. If the connection is no longer
     *        valid, it is discarded and a replacement is created (lazily).
     */
    void check_in(Connection conn) {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        if (conn.valid) {
            pool_.push_back(std::move(conn));
        } else {
            // Connection died; create a replacement to maintain pool size.
            auto replacement = platform_connect();
            if (replacement.valid) {
                pool_.push_back(std::move(replacement));
            }
        }

        pool_cv_.notify_one();
    }

    // ------------------------------------------------------------------
    // Platform connection (one per pooled slot)
    // ------------------------------------------------------------------

    Connection platform_connect() {
#ifdef _WIN32
        std::string full_name = "\\\\.\\pipe\\" + pipe_name_;

        // Wait for the named pipe to become available
        ULONGLONG start = GetTickCount64();
        while (true) {
            if (WaitNamedPipeA(full_name.c_str(), 0)) {
                break;
            }
            ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= static_cast<ULONGLONG>(connect_timeout_ms_)) {
                return {INVALID_HANDLE_VALUE, false};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        HANDLE h = CreateFileA(
            full_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (h == INVALID_HANDLE_VALUE) {
            return {INVALID_HANDLE_VALUE, false};
        }

        return {h, true};

#else
        // Unix domain socket
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return {-1, false};
        }

        // Set non-blocking for connect with timeout
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (pipe_name_.size() >= sizeof(addr.sun_path)) {
            close(fd);
            return {-1, false};
        }
        strncpy(addr.sun_path, pipe_name_.c_str(), sizeof(addr.sun_path) - 1);

        auto start = std::chrono::steady_clock::now();
        int ret = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        if (ret < 0 && errno != EINPROGRESS) {
            close(fd);
            return {-1, false};
        }

        if (ret < 0) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);

            timeval tv;
            tv.tv_sec = connect_timeout_ms_ / 1000;
            tv.tv_usec = (connect_timeout_ms_ % 1000) * 1000;

            ret = select(fd + 1, nullptr, &wfds, nullptr, &tv);
            if (ret <= 0) {
                close(fd);
                return {-1, false};
            }

            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error != 0) {
                close(fd);
                return {-1, false};
            }
        }

        fcntl(fd, F_SETFL, flags);

        timeval timeout;
        timeout.tv_sec = timeout_ms_ / 1000;
        timeout.tv_usec = (timeout_ms_ % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        return {fd, true};
#endif
    }

    void platform_disconnect(Connection conn) {
        if (!conn.valid) return;
#ifdef _WIN32
        CloseHandle(conn.handle);
#else
        close(conn.handle);
#endif
    }

    // ------------------------------------------------------------------
    // Platform read/write (io_uring on Linux, blocking otherwise)
    // ------------------------------------------------------------------

    bool io_write_all(Connection conn, const char* data, size_t size) {
#if defined(LIBOPENAI_USE_IO_URING) && defined(__linux__)
        if (!ring_init_ok_) {
            return write_all_blocking(conn, data, size);
        }
        if (!conn.valid) return false;

        size_t total = 0;
        while (total < size) {
            size_t chunk = size - total;

            std::lock_guard<std::mutex> lock(ring_mutex_);

            // Copy data into an owned buffer so it stays valid until completion.
            auto owned = std::make_shared<std::vector<uint8_t>>(data + total,
                                                                 data + total + chunk);

            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                return false;
            }

            struct iovec iov;
            iov.iov_base = owned->data();
            iov.iov_len = chunk;

            struct msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;

            io_uring_prep_sendmsg(sqe, conn.handle, &msg, 0);
            io_uring_sqe_set_data(sqe, owned.get());

            // Submit and wait for completion in a single syscall.
            int ret = io_uring_submit_and_wait(&ring_, 1);
            if (ret < 0) {
                return false;
            }

            // Collect the completion (non-blocking: submit_and_wait already
            // guaranteed at least one CQE is available).
            io_uring_cqe* cqe = nullptr;
            ret = io_uring_peek_cqe(&ring_, &cqe);
            if (ret < 0 || !cqe) {
                return false;
            }

            bool success = (cqe->res >= 0);
            ssize_t n = success ? static_cast<ssize_t>(cqe->res) : 0;
            io_uring_cqe_seen(&ring_, cqe);

            total += static_cast<size_t>(n);
            if (!success) {
                return false;
            }
        }
        return true;
#else
        return write_all_blocking(conn, data, size);
#endif
    }

    ssize_t io_read_exact(Connection conn, uint8_t* buf, size_t size) {
#if defined(LIBOPENAI_USE_IO_URING) && defined(__linux__)
        if (!ring_init_ok_) {
            return read_exact_blocking(conn, buf, size);
        }
        if (!conn.valid) return -1;

        size_t total = 0;
        while (total < size) {
            size_t chunk = size - total;

            std::lock_guard<std::mutex> lock(ring_mutex_);

            // Use a temporary buffer for the recv; copy into caller's buf after.
            auto tmp = std::make_shared<std::vector<uint8_t>>(chunk);

            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                return static_cast<ssize_t>(total);
            }

            struct iovec iov;
            iov.iov_base = tmp->data();
            iov.iov_len = chunk;

            struct msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;

            io_uring_prep_recvmsg(sqe, conn.handle, &msg, 0);
            io_uring_sqe_set_data(sqe, tmp.get());

            // Submit and wait for completion in a single syscall.
            int ret = io_uring_submit_and_wait(&ring_, 1);
            if (ret < 0) {
                return static_cast<ssize_t>(total);
            }

            // Collect the completion (non-blocking: submit_and_wait already
            // guaranteed at least one CQE is available).
            io_uring_cqe* cqe = nullptr;
            ret = io_uring_peek_cqe(&ring_, &cqe);
            if (ret < 0 || !cqe) {
                return static_cast<ssize_t>(total);
            }

            bool success = (cqe->res > 0);
            ssize_t n = success ? static_cast<ssize_t>(cqe->res) : 0;
            io_uring_cqe_seen(&ring_, cqe);

            if (n > 0) {
                memcpy(buf + total, tmp->data(), static_cast<size_t>(n));
                total += static_cast<size_t>(n);
            }
            if (!success || n == 0) {
                return static_cast<ssize_t>(total);
            }
        }
        return static_cast<ssize_t>(total);
#else
        return read_exact_blocking(conn, buf, size);
#endif
    }

    // Blocking fallback implementations
    bool write_all_blocking(Connection conn, const char* data, size_t size) {
        if (!conn.valid) return false;
        size_t total = 0;
        while (total < size) {
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(conn.handle, data + total,
                           static_cast<DWORD>(size - total), &written, nullptr)) {
                return false;
            }
            total += written;
#else
            ssize_t n = send(conn.handle, data + total, size - total, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            total += static_cast<size_t>(n);
#endif
        }
        return true;
    }

    ssize_t read_exact_blocking(Connection conn, uint8_t* buf, size_t size) {
        if (!conn.valid) return -1;
        size_t total = 0;
        while (total < size) {
#ifdef _WIN32
            DWORD read = 0;
            if (!ReadFile(conn.handle, buf + total,
                          static_cast<DWORD>(size - total), &read, nullptr)) {
                return static_cast<ssize_t>(total);
            }
            if (read == 0) {
                return static_cast<ssize_t>(total);
            }
            total += read;
#else
            ssize_t n = recv(conn.handle, reinterpret_cast<char*>(buf + total),
                             size - total, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                return static_cast<ssize_t>(total);
            }
            if (n == 0) {
                return static_cast<ssize_t>(total);
            }
            total += static_cast<size_t>(n);
#endif
        }
        return static_cast<ssize_t>(total);
    }

    // ------------------------------------------------------------------
    // Promise fulfillment helpers
    // ------------------------------------------------------------------

    void fulfill_value(std::shared_ptr<std::promise<Response>> promise,
                       uint64_t req_id, Response resp) {
        promise->set_value(std::move(resp));
    }

    void fulfill_error(std::shared_ptr<std::promise<Response>> promise,
                       uint64_t req_id, long status_code, const std::string& body) {
        Response resp;
        resp.status_code = status_code;
        resp.body = body;
        promise->set_value(std::move(resp));
    }
};

std::shared_ptr<ITransport> CreateNamedPipeTransport(const std::string& pipe_name) {
    return std::make_shared<NamedPipeTransport>(pipe_name);
}

} // namespace libopenai

#endif // LIBOPENAI_USE_NAMED_PIPE
