#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

#include <uv.h>

using namespace std::chrono;

/**
 * IPC Benchmark: Named Pipe (uv_pipe_t) vs TCP Socket (localhost)
 *
 * Measures:
 * - Messages per second
 * - Latency per message (microseconds)
 * - Total data transferred
 */

constexpr size_t MESSAGE_SIZE = 1024;  // bytes per message
constexpr size_t NUM_MESSAGES = 10000;
constexpr char PIPE_NAME[] = "ipc-benchmark-pipe";
constexpr int TCP_PORT = 19876;

// Shared counter for coordination
std::atomic<size_t> messages_received{0};
std::atomic<bool> stop_benchmark{false};
std::atomic<bool> server_ready{false};

// ---------------------------------------------------------------
// Named Pipe Server (libuv uv_pipe_t)
// ---------------------------------------------------------------
static void pipe_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = static_cast<unsigned int>(suggested_size);
}

static void pipe_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread > 0) {
        messages_received.fetch_add(1);
    }
    delete[] buf->base;
    if (nread < 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
    }
}

static void pipe_connection(uv_stream_t* server, int status) {
    if (status < 0) return;
    uv_pipe_t* client = new uv_pipe_t();
    uv_pipe_init(uv_default_loop(), client, 1);
    uv_accept(server, reinterpret_cast<uv_stream_t*>(client));
    uv_read_start(reinterpret_cast<uv_stream_t*>(client), pipe_alloc, pipe_read);
}

static void run_pipe_server() {
#ifdef _WIN32
    std::string full_name = "\\\\.\\pipe\\" + std::string(PIPE_NAME);
#else
    std::string full_name = PIPE_NAME;
    unlink(full_name.c_str());
#endif

    uv_pipe_t srv;
    // A listening named-pipe handle must use ipc=0 on Windows;
    // uv_listen rejects ipc=1 handles with WSAEINVAL.
    uv_pipe_init(uv_default_loop(), &srv, 0);
    uv_pipe_bind(&srv, full_name.c_str());
    uv_listen(reinterpret_cast<uv_stream_t*>(&srv), 128, pipe_connection);

    server_ready.store(true);

    while (!stop_benchmark.load()) {
        uv_run(uv_default_loop(), UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    uv_close(reinterpret_cast<uv_handle_t*>(&srv), nullptr);
}

// ---------------------------------------------------------------
// Named Pipe Client
// ---------------------------------------------------------------
static void run_pipe_client() {
    // Wait for server
    while (!server_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

#ifdef _WIN32
    std::string full_name = "\\\\.\\pipe\\" + std::string(PIPE_NAME);
#else
    std::string full_name = PIPE_NAME;
#endif

    uv_pipe_t pipe;
    uv_loop_t* client_loop = uv_loop_new();
    uv_pipe_init(client_loop, &pipe, 1);

    uv_connect_t* connect_req = new uv_connect_t();
    uv_pipe_connect(connect_req, &pipe, full_name.c_str(), [](uv_connect_t*, int) {});

    // Run event loop to complete connection
    uv_run(client_loop, UV_RUN_NOWAIT);

    // Create message buffer
    std::string message(MESSAGE_SIZE, 'X');

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < NUM_MESSAGES && !stop_benchmark.load(); i++) {
        uv_write_t* req = new uv_write_t();
        uv_buf_t buf = uv_buf_init(const_cast<char*>(message.data()), static_cast<unsigned int>(message.size()));
        uv_write(req, reinterpret_cast<uv_stream_t*>(&pipe), &buf, 1, [](uv_write_t* req, int) {
            delete req;
        });
    }

    // Wait for all messages to be processed
    while (messages_received.load() < NUM_MESSAGES && !stop_benchmark.load()) {
        uv_run(client_loop, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto end = high_resolution_clock::now();

    double elapsed_sec = duration_cast<microseconds>(end - start).count() / 1000000.0;
    double msgs_per_sec = NUM_MESSAGES / elapsed_sec;
    double latency_us = (elapsed_sec * 1000000.0) / NUM_MESSAGES;
    double mb_per_sec = (NUM_MESSAGES * MESSAGE_SIZE) / (1024.0 * 1024.0 * elapsed_sec);

    std::cout << "\n=== Named Pipe Benchmark ===" << std::endl;
    std::cout << "Messages: " << NUM_MESSAGES << std::endl;
    std::cout << "Message size: " << MESSAGE_SIZE << " bytes" << std::endl;
    std::cout << "Elapsed: " << elapsed_sec << " seconds" << std::endl;
    std::cout << "Messages/sec: " << msgs_per_sec << std::endl;
    std::cout << "Avg latency: " << latency_us << " microseconds" << std::endl;
    std::cout << "Throughput: " << mb_per_sec << " MB/sec" << std::endl;

    // Drain the client loop so the close completes, then tear it down.
    uv_run(client_loop, UV_RUN_NOWAIT);
    uv_close(reinterpret_cast<uv_handle_t*>(&pipe), nullptr);
    uv_run(client_loop, UV_RUN_NOWAIT);
    uv_loop_close(client_loop);
    uv_loop_delete(client_loop);
}

// ---------------------------------------------------------------
// TCP Server
// ---------------------------------------------------------------
static std::atomic<size_t> tcp_messages_received{0};

static void tcp_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = static_cast<unsigned int>(suggested_size);
}

static void tcp_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread > 0) {
        tcp_messages_received.fetch_add(1);
    }
    delete[] buf->base;
    if (nread < 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
    }
}

static void tcp_connection(uv_stream_t* server, int status) {
    if (status < 0) return;
    uv_tcp_t* client = new uv_tcp_t();
    uv_tcp_init(uv_default_loop(), client);
    uv_accept(server, reinterpret_cast<uv_stream_t*>(client));
    uv_read_start(reinterpret_cast<uv_stream_t*>(client), tcp_alloc, tcp_read);
}

static void run_tcp_server() {
    uv_tcp_t srv;
    uv_tcp_init(uv_default_loop(), &srv);

    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", TCP_PORT, &addr);
    uv_tcp_bind(&srv, reinterpret_cast<const struct sockaddr*>(&addr), 0);
    uv_listen(reinterpret_cast<uv_stream_t*>(&srv), 128, tcp_connection);

    while (!stop_benchmark.load()) {
        uv_run(uv_default_loop(), UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    uv_close(reinterpret_cast<uv_handle_t*>(&srv), nullptr);
}

// ---------------------------------------------------------------
// TCP Client
// ---------------------------------------------------------------
static void run_tcp_client() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    uv_tcp_t sock;
    uv_tcp_init(uv_default_loop(), &sock);

    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", TCP_PORT, &addr);

    uv_connect_t* connect_req = new uv_connect_t();
    uv_tcp_connect(connect_req, &sock, reinterpret_cast<const struct sockaddr*>(&addr),
                   [](uv_connect_t* req, int status) {
                       delete req;
                   });

    uv_run(uv_default_loop(), UV_RUN_NOWAIT);

    std::string message(MESSAGE_SIZE, 'Y');

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < NUM_MESSAGES && !stop_benchmark.load(); i++) {
        uv_write_t* req = new uv_write_t();
        uv_buf_t buf = uv_buf_init(const_cast<char*>(message.data()), static_cast<unsigned int>(message.size()));
        uv_write(req, reinterpret_cast<uv_stream_t*>(&sock), &buf, 1, [](uv_write_t* req, int) {
            delete req;
        });
    }

    while (tcp_messages_received.load() < NUM_MESSAGES && !stop_benchmark.load()) {
        uv_run(uv_default_loop(), UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto end = high_resolution_clock::now();

    double elapsed_sec = duration_cast<microseconds>(end - start).count() / 1000000.0;
    double msgs_per_sec = NUM_MESSAGES / elapsed_sec;
    double latency_us = (elapsed_sec * 1000000.0) / NUM_MESSAGES;
    double mb_per_sec = (NUM_MESSAGES * MESSAGE_SIZE) / (1024.0 * 1024.0 * elapsed_sec);

    std::cout << "\n=== TCP Socket Benchmark ===" << std::endl;
    std::cout << "Messages: " << NUM_MESSAGES << std::endl;
    std::cout << "Message size: " << MESSAGE_SIZE << " bytes" << std::endl;
    std::cout << "Elapsed: " << elapsed_sec << " seconds" << std::endl;
    std::cout << "Messages/sec: " << msgs_per_sec << std::endl;
    std::cout << "Avg latency: " << latency_us << " microseconds" << std::endl;
    std::cout << "Throughput: " << mb_per_sec << " MB/sec" << std::endl;

    uv_close(reinterpret_cast<uv_handle_t*>(&sock), nullptr);
}

int main() {
    std::cout << "IPC Benchmark: Named Pipe vs TCP Socket" << std::endl;
    std::cout << "========================================" << std::endl;

    // Run named pipe benchmark
    {
        messages_received.store(0);
        stop_benchmark.store(false);
        server_ready.store(false);

        std::thread pipe_server(run_pipe_server);
        std::thread pipe_client(run_pipe_client);

        pipe_client.join();
        stop_benchmark.store(true);
        pipe_server.join();
    }

    // Reset for TCP benchmark
    {
        tcp_messages_received.store(0);
        stop_benchmark.store(false);
        server_ready.store(false);

        std::thread tcp_server(run_tcp_server);
        std::thread tcp_client(run_tcp_client);

        tcp_client.join();
        stop_benchmark.store(true);
        tcp_server.join();
    }

    std::cout << "\nBenchmark complete." << std::endl;
    return 0;
}
