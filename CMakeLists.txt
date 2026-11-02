cmake_minimum_required(VERSION 3.14)
project(libopenai LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

include(FetchContent)

# ---------------------------------------------------------------------------
# Transport backend selection (compile-time)
# ---------------------------------------------------------------------------
# Options: httplib (default), curl, mongoose, civetweb, named-pipe,
#          beast, workflow, cppserver
set(LIBOPENAI_TRANSPORT_BACKEND "httplib" CACHE STRING
    "HTTP transport backend: httplib, curl, mongoose, civetweb, named-pipe, beast, workflow, cppserver")

set(LIBOPENAI_USE_CURL OFF CACHE BOOL "Enable libcurl transport backend")
set(LIBOPENAI_USE_MONGOOSE OFF CACHE BOOL "Enable mongoose transport backend")
set(LIBOPENAI_USE_CIVETWEB OFF CACHE BOOL "Enable civetweb transport backend")
set(LIBOPENAI_USE_LIBHTTP OFF CACHE BOOL "Enable libhttp (mongoose fork) transport backend")
set(LIBOPENAI_USE_SEASTAR OFF CACHE BOOL "Enable seastar (ScyllaDB framework) transport backend")
set(LIBOPENAI_USE_PROXYGEN OFF CACHE BOOL "Enable proxygen (Meta HTTP library) transport backend")
set(LIBOPENAI_USE_NAMED_PIPE OFF CACHE BOOL "Enable platform-native named pipe transport backend")
set(LIBOPENAI_USE_IN_MEMORY OFF CACHE BOOL "Enable pure C++ in-memory socketless transport backend")
set(LIBOPENAI_USE_BEAST OFF CACHE BOOL "Enable Boost.Beast transport backend")
set(LIBOPENAI_USE_WORKFLOW OFF CACHE BOOL "Enable C++ Workflow transport backend")
set(LIBOPENAI_USE_CPPSERVER OFF CACHE BOOL "Enable CppServer transport backend")
set(LIBOPENAI_USE_IO_URING OFF CACHE BOOL "Enable Linux io_uring async I/O for named-pipe (Unix domain sockets)")

if(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "curl")
    set(LIBOPENAI_USE_CURL ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "mongoose")
    set(LIBOPENAI_USE_MONGOOSE ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "civetweb")
    set(LIBOPENAI_USE_CIVETWEB ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "libhttp")
    set(LIBOPENAI_USE_LIBHTTP ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "seastar")
    set(LIBOPENAI_USE_SEASTAR ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "proxygen")
    set(LIBOPENAI_USE_PROXYGEN ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "named-pipe")
    set(LIBOPENAI_USE_NAMED_PIPE ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "in-memory")
    set(LIBOPENAI_USE_IN_MEMORY ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "beast")
    set(LIBOPENAI_USE_BEAST ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "workflow")
    set(LIBOPENAI_USE_WORKFLOW ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "cppserver")
    set(LIBOPENAI_USE_CPPSERVER ON)
endif()

# ---------------------------------------------------------------------------
# nlohmann_json
# ---------------------------------------------------------------------------
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# cpp-httplib (default backend only)
# ---------------------------------------------------------------------------
find_package(httplib QUIET)
if(NOT httplib_FOUND)
  FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.16.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(httplib)
endif()

# ---------------------------------------------------------------------------
# libcurl (optional backend)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_CURL)
  find_package(CURL REQUIRED)
  add_compile_definitions(LIBOPENAI_USE_CURL)
endif()

# ---------------------------------------------------------------------------
# mongoose (optional backend)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_MONGOOSE)
  FetchContent_Declare(
    mongoose
    GIT_REPOSITORY https://github.com/cesanta/mongoose.git
    GIT_TAG origin/master
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(mongoose)
  add_compile_definitions(LIBOPENAI_USE_MONGOOSE)
endif()

# ---------------------------------------------------------------------------
# civetweb (optional backend)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_CIVETWEB)
  FetchContent_Declare(
    civetweb
    GIT_REPOSITORY https://github.com/civetweb/civetweb.git
    GIT_TAG origin/master
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(civetweb)
  add_compile_definitions(LIBOPENAI_USE_CIVETWEB)
endif()

# ---------------------------------------------------------------------------
# libhttp (mongoose fork, MIT license)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_LIBHTTP)
  FetchContent_Declare(
    libhttp
    GIT_REPOSITORY https://github.com/lammertb/libhttp.git
    GIT_TAG origin/master
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(libhttp)
  add_compile_definitions(LIBOPENAI_USE_LIBHTTP)
endif()

# ---------------------------------------------------------------------------
# seastar (ScyllaDB high-performance server framework)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_SEASTAR)
  FetchContent_Declare(
    seastar
    GIT_REPOSITORY https://github.com/scylladb/seastar.git
    GIT_TAG origin/master
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(seastar)
  add_compile_definitions(LIBOPENAI_USE_SEASTAR)
endif()

# ---------------------------------------------------------------------------
# proxygen (Meta HTTP library)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_PROXYGEN)
  FetchContent_Declare(
    proxygen
    GIT_REPOSITORY https://github.com/meta/proxygen.git
    GIT_TAG origin/master
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(proxygen)
  add_compile_definitions(LIBOPENAI_USE_PROXYGEN)
endif()

# ---------------------------------------------------------------------------
# Boost.Beast (optional backend)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_BEAST)
  find_package(Boost REQUIRED COMPONENTS system beast)
  add_compile_definitions(LIBOPENAI_USE_BEAST)
endif()

# ---------------------------------------------------------------------------
# C++ Workflow (optional backend)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_WORKFLOW)
  FetchContent_Declare(
    workflow
    GIT_REPOSITORY https://github.com/sogou/workflow.git
    GIT_TAG origin/master
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(workflow)
  add_compile_definitions(LIBOPENAI_USE_WORKFLOW)
endif()

# ---------------------------------------------------------------------------
# CppServer (optional backend)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_CPPSERVER)
  FetchContent_Declare(
    cppserver
    GIT_REPOSITORY https://github.com/chronoxor/CppServer.git
    GIT_TAG origin/master
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(cppserver)
  add_compile_definitions(LIBOPENAI_USE_CPPSERVER)
endif()

# ---------------------------------------------------------------------------
# liburing (Linux io_uring async I/O for named-pipe)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_IO_URING)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "LIBOPENAI_USE_IO_URING requires Linux")
  endif()
  FetchContent_Declare(
    liburing
    GIT_REPOSITORY https://github.com/axboe/liburing.git
    GIT_TAG origin/master
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(liburing)
  add_compile_definitions(LIBOPENAI_USE_IO_URING)
endif()

# ---------------------------------------------------------------------------
# Library sources
# ---------------------------------------------------------------------------
add_library(libopenai STATIC
  src/libopenai.cpp
  src/transports/httplib_transport.cpp
  src/inference_backend.cpp
  src/inference_provider.cpp
  src/provider_registry.cpp
  src/modality_router.cpp
  src/device_cloud_router.cpp
  src/cluster_backend.cpp
  src/inference_client.cpp
)

target_include_directories(libopenai PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/libopenai"
)

target_link_libraries(libopenai PUBLIC
  nlohmann_json::nlohmann_json
  httplib::httplib
  OpenSSL::SSL
  OpenSSL::Crypto
)

if(LIBOPENAI_USE_CURL)
  target_sources(libopenai PRIVATE src/transports/curl_transport.cpp)
  target_link_libraries(libopenai PUBLIC CURL::libcurl)
endif()

if(LIBOPENAI_USE_MONGOOSE)
  target_sources(libopenai PRIVATE src/transports/mongoose_transport.cpp)
  target_link_libraries(libopenai PUBLIC mongoose::mongoose)
endif()

if(LIBOPENAI_USE_CIVETWEB)
  target_sources(libopenai PRIVATE src/transports/civetweb_transport.cpp)
  target_link_libraries(libopenai PUBLIC civetweb::civetweb)
endif()

if(LIBOPENAI_USE_NAMED_PIPE)
  target_sources(libopenai PRIVATE src/transports/namedpipe_transport.cpp)
  add_compile_definitions(LIBOPENAI_USE_NAMED_PIPE)
  # Platform-native APIs: no external library dependency
endif()

if(LIBOPENAI_USE_IO_URING)
  target_sources(libopenai PRIVATE src/transports/namedpipe_transport.cpp)
  target_link_libraries(libopenai PUBLIC liburing::liburing)
endif()

if(LIBOPENAI_USE_IN_MEMORY)
  target_sources(libopenai PRIVATE src/transports/inmemory_transport.cpp)
  add_compile_definitions(LIBOPENAI_USE_IN_MEMORY)
  # Pure C++17: no external library dependency
endif()

if(LIBOPENAI_USE_LIBHTTP)
  target_sources(libopenai PRIVATE src/transports/libhttp_transport.cpp)
  target_link_libraries(libopenai PUBLIC libhttp::libhttp)
endif()

if(LIBOPENAI_USE_SEASTAR)
  target_sources(libopenai PRIVATE src/transports/seastar_transport.cpp)
  target_link_libraries(libopenai PUBLIC seastar::seastar)
endif()

if(LIBOPENAI_USE_PROXYGEN)
  target_sources(libopenai PRIVATE src/transports/proxygen_transport.cpp)
  target_link_libraries(libopenai PUBLIC proxygen::proxygen)
endif()

if(LIBOPENAI_USE_BEAST)
  target_sources(libopenai PRIVATE src/transports/beast_transport.cpp)
  target_link_libraries(libopenai PUBLIC Boost::boost Boost::system Boost::beast)
endif()

if(LIBOPENAI_USE_WORKFLOW)
  target_sources(libopenai PRIVATE src/transports/workflow_transport.cpp)
  target_link_libraries(libopenai PUBLIC workflow::workflow)
endif()

if(LIBOPENAI_USE_CPPSERVER)
  target_sources(libopenai PRIVATE src/transports/cppserver_transport.cpp)
  target_link_libraries(libopenai PUBLIC cppserver::cppserver)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/chat.cpp")
  add_executable(example_chat examples/chat.cpp)
  target_link_libraries(example_chat PRIVATE libopenai)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/namedpipe_example.cpp")
  add_executable(example_namedpipe examples/namedpipe_example.cpp)
  target_link_libraries(example_namedpipe PRIVATE libopenai)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/namedpipe_integration_test.cpp" AND LIBOPENAI_USE_NAMED_PIPE)
  add_executable(namedpipe_integration_test examples/namedpipe_integration_test.cpp)
  target_link_libraries(namedpipe_integration_test PRIVATE libopenai)
  target_compile_definitions(namedpipe_integration_test PRIVATE LIBOPENAI_USE_NAMED_PIPE)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/ipc_benchmark.cpp" AND LIBOPENAI_USE_NAMED_PIPE)
  add_executable(ipc_benchmark examples/ipc_benchmark.cpp)
  target_link_libraries(ipc_benchmark PRIVATE libopenai)
  target_compile_definitions(ipc_benchmark PRIVATE LIBOPENAI_USE_NAMED_PIPE)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/inference_frontend_example.cpp")
  add_executable(inference_frontend_example examples/inference_frontend_example.cpp)
  target_link_libraries(inference_frontend_example PRIVATE libopenai)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/inmemory_example.cpp" AND LIBOPENAI_USE_IN_MEMORY)
  add_executable(inmemory_example examples/inmemory_example.cpp)
  target_link_libraries(inmemory_example PRIVATE libopenai)
  target_compile_definitions(inmemory_example PRIVATE LIBOPENAI_USE_IN_MEMORY)
endif()
