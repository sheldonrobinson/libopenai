cmake_minimum_required(VERSION 3.14)
project(libopenai LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find CURL and OpenSSL
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

include(FetchContent)

# Check if nlohmann_json is already available in the environment / package manager,
# otherwise fetch it via FetchContent so we don't pollute the namespace unnecessarily.
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(libopenai STATIC src/libopenai.cpp)
target_include_directories(libopenai PUBLIC 
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/libopenai"
  ${CURL_INCLUDE_DIRS}
)

target_link_libraries(libopenai PUBLIC 
  nlohmann_json::nlohmann_json
  ${CURL_LIBRARIES} 
  OpenSSL::SSL 
  OpenSSL::Crypto
)

# Examples
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/chat.cpp")
  add_executable(example_chat examples/chat.cpp)
  target_link_libraries(example_chat PRIVATE libopenai)
endif()
