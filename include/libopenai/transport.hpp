#pragma once

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <future>
#include <nlohmann/json.hpp>

namespace libopenai {

using json = nlohmann::json;

struct Message {
    std::string role;
    std::string content;
};

/**
 * @brief HTTP Request structure carrying all data needed to execute a request.
 *
 * The Request is designed to be movable and copyable. For zero-copy scenarios,
 * callers can pass string_views into payload and headers where the underlying
 * storage outlives the transport call.
 */
struct Request {
    std::string method;                      // GET, POST, DELETE, etc.
    std::string url;                         // Full base URL (e.g. https://api.openai.com/v1)
    std::string endpoint;                    // API path (e.g. /chat/completions)
    std::map<std::string, std::string> headers;
    std::string payload;                     // JSON body for POST/PUT
    std::map<std::string, std::string> multipart_text;
    std::map<std::string, std::string> multipart_files;  // key -> file path
    bool is_download = false;
    std::string download_path;
    long timeout_seconds = 60;
    std::string proxy_url;

    // Optional streaming callback. Return true to continue streaming, false to abort.
    std::function<bool(const std::string&)> stream_callback;
};

/**
 * @brief HTTP Response structure holding status code, headers, body and parsed JSON.
 */
struct Response {
    long status_code = 0;
    std::string body;
    std::string x_request_id;
    json json_data;

    bool is_error() const {
        return status_code < 200 || status_code >= 300;
    }
};

/**
 * @brief Abstract transport interface. All networking backends implement this.
 *
 * Implementations must be thread-safe with respect to Execute() and
 * ExecuteAsync() being called concurrently from multiple threads.
 *
 * ExecuteAsync() enables multiple in-flight requests on a single transport
 * instance. Each call returns a std::future<Response> immediately; the request
 * runs on a background worker thread and the future is fulfilled when the
 * response arrives.
 *
 * For shared-connection backends (named-pipe, in-memory), each request is
 * tagged with a unique 64-bit request ID so responses are matched to the
 * correct pending request even when they arrive out of order.
 *
 * The transport is decoupled from both the API interface (Client) and from
 * platform-specific connectors. Each backend is implemented using only the
 * C++ standard library plus platform-native APIs.
 */
class ITransport {
public:
    virtual ~ITransport() = default;

    /**
     * Execute an HTTP request and return the response (blocking).
     * @param req The request to execute.
     * @return Response containing status, body, headers and parsed JSON.
     */
    virtual Response Execute(const Request& req) = 0;

    /**
     * Execute an HTTP request asynchronously (non-blocking).
     *
     * The request is dispatched to a background worker and this method returns
     * immediately with a std::future<Response>. Call get() on the future to
     * block until the response arrives. Multiple concurrent calls are supported;
     * each returns an independent future.
     *
     * @param req The request to execute.
     * @return Future that will be fulfilled with the response when it arrives.
     */
    virtual std::future<Response> ExecuteAsync(const Request& req) = 0;

    /**
     * Optional: returns a human-readable name of the backend (useful for logging).
     */
    virtual std::string Name() const { return "ITransport"; }
};

/**
 * @brief Factory function type for creating transports.
 */
using TransportFactory = std::function<std::shared_ptr<ITransport>()>;

/**
 * @brief Create a cpp-httplib based transport. Always available.
 */
std::shared_ptr<ITransport> CreateHttplibTransport();

#ifdef LIBOPENAI_USE_CURL
/**
 * @brief Create a libcurl based transport. Available only when LIBOPENAI_USE_CURL is defined.
 */
std::shared_ptr<ITransport> CreateCurlTransport();
#endif

#ifdef LIBOPENAI_USE_MONGOOSE
/**
 * @brief Create a mongoose based transport. Available only when LIBOPENAI_USE_MONGOOSE is defined.
 */
std::shared_ptr<ITransport> CreateMongooseTransport();
#endif

#ifdef LIBOPENAI_USE_CIVETWEB
/**
 * @brief Create a civetweb based transport. Available only when LIBOPENAI_USE_CIVETWEB is defined.
 */
std::shared_ptr<ITransport> CreateCivetwebTransport();
#endif

#ifdef LIBOPENAI_USE_NAMED_PIPE
/**
 * @brief Create a platform-native named pipe / Unix domain socket transport.
 *        Uses pure C++17 with platform-native APIs (no libuv).
 * @param pipe_name The named pipe name (e.g. "myapp-pipe" or "/tmp/myapp.sock").
 */
std::shared_ptr<ITransport> CreateNamedPipeTransport(const std::string& pipe_name);
#endif

#ifdef LIBOPENAI_USE_IN_MEMORY
/**
 * @brief Create an in-memory socketless transport using InMemoryStream.
 *        Pure C++17 implementation (no libuv, no cpp-httplib).
 * @param stream Shared InMemoryStream connected to an InMemoryServer.
 * @param timeout_ms Maximum time to wait for a response.
 */
std::shared_ptr<ITransport> CreateInMemoryTransport(
    std::shared_ptr<class InMemoryStream> stream, int timeout_ms = 5000);
#endif

#ifdef LIBOPENAI_USE_LIBHTTP
/**
 * @brief Create a libhttp (mongoose fork) based transport. Available only when LIBOPENAI_USE_LIBHTTP is defined.
 * libhttp is a multi-platform HTTP and HTTPS library (MIT license).
 */
std::shared_ptr<ITransport> CreateLibHttpTransport();
#endif

#ifdef LIBOPENAI_USE_SEASTAR
/**
 * @brief Create a seastar (ScyllaDB framework) based transport. Available only when LIBOPENAI_USE_SEASTAR is defined.
 * Seastar is a high-performance shard-per-core server-side application framework.
 */
std::shared_ptr<ITransport> CreateSeastarTransport();
#endif

#ifdef LIBOPENAI_USE_PROXYGEN
/**
 * @brief Create a proxygen (Meta HTTP library) based transport. Available only when LIBOPENAI_USE_PROXYGEN is defined.
 * Proxygen is a collection of C++ HTTP libraries from Meta.
 */
std::shared_ptr<ITransport> CreateProxygenTransport();
#endif

#ifdef LIBOPENAI_USE_BEAST
/**
 * @brief Create a Boost.Beast based transport. Requires Boost libraries.
 */
std::shared_ptr<ITransport> CreateBeastTransport();
#endif

#ifdef LIBOPENAI_USE_WORKFLOW
/**
 * @brief Create a C++ Workflow based transport. Requires Workflow library.
 */
std::shared_ptr<ITransport> CreateWorkflowTransport();
#endif

#ifdef LIBOPENAI_USE_CPPSERVER
/**
 * @brief Create a CppServer based transport. Requires CppServer library.
 */
std::shared_ptr<ITransport> CreateCppServerTransport();
#endif

} // namespace libopenai
