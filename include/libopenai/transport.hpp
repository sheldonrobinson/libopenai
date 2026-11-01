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
 * Implementations must be thread-safe with respect to Execute() being called
 * concurrently from multiple threads. The Client serialises configuration
 * changes via its own mutex, but the transport itself should not assume
 * single-threaded use unless documented.
 */
class ITransport {
public:
    virtual ~ITransport() = default;

    /**
     * Execute an HTTP request and return the response.
     * @param req The request to execute.
     * @return Response containing status, body, headers and parsed JSON.
     */
    virtual Response Execute(const Request& req) = 0;

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
 * @brief Create a libuv named pipe based transport for socketless IPC.
 * @param pipe_name The named pipe name (e.g. "myapp-pipe" or "/tmp/myapp-pipe").
 */
std::shared_ptr<ITransport> CreateNamedPipeTransport(const std::string& pipe_name);
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
