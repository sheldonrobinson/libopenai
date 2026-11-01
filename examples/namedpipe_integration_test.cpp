#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <string>
#include <mutex>
#include <future>

#include <uv.h>

#include "libopenai/libopenai.hpp"
#include "libopenai/transport.hpp"

using namespace libopenai;

// Framing used by both NamedPipeTransport and UVPipeOpenAIServer:
//   4-byte little-endian length prefix + JSON payload
static constexpr char TEST_PIPE[] = "libopenai-integration-test-pipe";

static void srv_alloc(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
    buf->base = new char[suggested];
    buf->len = (unsigned int)suggested;
}

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// Shared server state, reachable from callbacks via uv_handle_t->data.
struct SrvCtx {
    uv_pipe_t* client = nullptr;
    std::string buf;
    std::mutex mtx;
    std::atomic<bool> full{false};
};

static void srv_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* b) {
    auto* ctx = static_cast<SrvCtx*>(stream->data);
    if (nread > 0) {
        std::lock_guard<std::mutex> lock(ctx->mtx);
        ctx->buf.append(b->base, (size_t)nread);
        if (ctx->buf.size() >= 4) {
            uint32_t len = le32((const unsigned char*)ctx->buf.data());
            if (len > 0 && len <= 10 * 1024 * 1024 &&
                ctx->buf.size() >= 4 + (size_t)len) {
                ctx->full.store(true);
            }
        }
    }
    delete[] b->base;
    if (nread < 0) {
        uv_read_stop(stream);
        uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
    }
}

static void srv_connection(uv_stream_t* server, int status) {
    if (status < 0) return;
    auto* ctx = static_cast<SrvCtx*>(server->data);
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(ctx->client)) != 0) return;
    ctx->client->data = ctx;
    uv_read_start(reinterpret_cast<uv_stream_t*>(ctx->client), srv_alloc, srv_read);
    uv_read_stop(server); // one connection only
}

// Build a framed response buffer.
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

// Server thread: accepts one client, reads one framed JSON request, sends a
// canned framed JSON response (same shape UVPipeOpenAIServer returns), then closes.
static void run_server(std::atomic<bool>& ready, std::atomic<bool>& done) {
#ifdef _WIN32
    std::string full_name = "\\\\.\\pipe\\" + std::string(TEST_PIPE);
#else
    std::string full_name = TEST_PIPE;
    unlink(full_name.c_str());
#endif

    uv_loop_t* loop = uv_default_loop();
    uv_pipe_t srv;
    uv_pipe_t client;
    // A listening named-pipe handle must use ipc=0 on Windows; uv_listen
    // rejects ipc=1 handles with WSAEINVAL (see uv__pipe_listen).
    uv_pipe_init(loop, &srv, 0);
    uv_pipe_init(loop, &client, 1);

    SrvCtx ctx;
    ctx.client = &client;
    srv.data = &ctx;

    int rc = uv_pipe_bind(&srv, full_name.c_str());
    if (rc != 0) {
        std::cerr << "[server] bind failed: " << uv_strerror(rc) << std::endl;
        done.store(true);
        return;
    }
    rc = uv_listen(reinterpret_cast<uv_stream_t*>(&srv), 16, srv_connection);
    if (rc != 0) {
        std::cerr << "[server] listen failed: " << uv_strerror(rc) << std::endl;
        done.store(true);
        return;
    }
    ready.store(true);

    auto start = std::chrono::steady_clock::now();
    while (!ctx.full.load() &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 10.0) {
        uv_run(loop, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (ctx.full.load()) {
        // Canned success response mirroring UVPipeOpenAIServer's shape.
        json jresp;
        jresp["status_code"] = 200;
        jresp["body"] = "{\"data\":[]}";
        jresp["json_data"] = {{"object", "list"}, {"data", json::array()}};
        std::string frame = make_frame(jresp.dump());

        uv_write_t* req = new uv_write_t();
        uv_buf_t buf = uv_buf_init((char*)frame.data(), (unsigned int)frame.size());
        uv_write(req, reinterpret_cast<uv_stream_t*>(&client), &buf, 1,
                 [](uv_write_t* r, int) { delete r; });

        auto wstart = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - wstart).count() < 1.0) {
            uv_run(loop, UV_RUN_NOWAIT);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    uv_close(reinterpret_cast<uv_handle_t*>(&srv), nullptr);
    done.store(true);
}

int main() {
    std::cout << "=== NamedPipeTransport <-> pipe server integration test ===" << std::endl;

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_done{false};

    std::thread server(run_server, std::ref(server_ready), std::ref(server_done));

    // Wait for server to start listening
    for (int i = 0; i < 100 && !server_ready.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    bool test_passed = false;
    try {
        auto transport = CreateNamedPipeTransport(TEST_PIPE);
        Client client("dummy-key", "http://localhost", transport);

        std::cout << "[client] transport: " << client.TransportName() << std::endl;

        // Issue a ListModels request over the named pipe.
        json result = client.ListModels();

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
