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
set(LIBOPENAI_USE_NAMED_PIPE OFF CACHE BOOL "Enable libuv named pipe transport backend")
set(LIBOPENAI_USE_BEAST OFF CACHE BOOL "Enable Boost.Beast transport backend")
set(LIBOPENAI_USE_WORKFLOW OFF CACHE BOOL "Enable C++ Workflow transport backend")
set(LIBOPENAI_USE_CPPSERVER OFF CACHE BOOL "Enable CppServer transport backend")

if(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "curl")
    set(LIBOPENAI_USE_CURL ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "mongoose")
    set(LIBOPENAI_USE_MONGOOSE ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "civetweb")
    set(LIBOPENAI_USE_CIVETWEB ON)
elseif(LIBOPENAI_TRANSPORT_BACKEND STREQUAL "named-pipe")
    set(LIBOPENAI_USE_NAMED_PIPE ON)
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
# cpp-httplib (default backend)
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
# libuv (named pipe backend)
# ---------------------------------------------------------------------------
if(LIBOPENAI_USE_NAMED_PIPE)
  FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG origin/v1.x
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(libuv)
  add_compile_definitions(LIBOPENAI_USE_NAMED_PIPE)
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
# Library sources
# ---------------------------------------------------------------------------
add_library(libopenai STATIC
  src/libopenai.cpp
  src/transports/httplib_transport.cpp
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
  target_link_libraries(libopenai PUBLIC uv)
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

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/ipc_benchmark.cpp" AND LIBOPENAI_USE_NAMED_PIPE)
  add_executable(ipc_benchmark examples/ipc_benchmark.cpp)
  target_link_libraries(ipc_benchmark PRIVATE uv)
  target_include_directories(ipc_benchmark PRIVATE ${libuv_SOURCE_DIR}/include)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/namedpipe_integration_test.cpp" AND LIBOPENAI_USE_NAMED_PIPE)
  add_executable(namedpipe_integration_test examples/namedpipe_integration_test.cpp)
  target_link_libraries(namedpipe_integration_test PRIVATE libopenai)
  target_include_directories(namedpipe_integration_test PRIVATE ${libuv_SOURCE_DIR}/include)
endif()
