# libopenai

A modern, comprehensive C++17 OpenAI API SDK implemented using **CURL** and **OpenSSL**.

## Features

- **CURL & OpenSSL Backed**: High-performance and secure network requests with SSL certificate verification.
- **Full API Coverage**:
  - Chat Completions (`/chat/completions`) with multi-role message support and streaming callback hooks.
  - Legacy Completions (`/completions`).
  - Embeddings (`/embeddings`).
  - Audio (`/audio/speech`, `/audio/transcriptions`, `/audio/translations`).
  - Images (`/images/generations`, `/images/edits`, `/images/variations`).
  - Files (`/files`, upload, delete, retrieve, download content).
  - Fine-Tuning Jobs (`/fine_tuning/jobs`).
  - Models (`/models`, list, retrieve, delete).
  - Moderations (`/moderations`).
  - Batches (`/batches`).
  - Assistants, Threads, Messages, Runs (Beta API).
- **Modern C++ & Dependencies**: Uses `nlohmann/json` (automatically fetched if not present on system) and C++17 standard features.

---

## Requirements

- C++17 compiler (GCC 9+, Clang 9+, MSVC 2019+)
- CMake 3.14 or higher
- `libcurl` development library (with OpenSSL support)
- `OpenSSL` development libraries (`libssl-dev`, `libcrypto`)

---

## Building and Installation

```bash
cd libopenai
cmake -B build -S .
cmake --build build --parallel
```

---

## Example Usage

```cpp
#include <iostream>
#include "libopenai/libopenai.hpp"

int main() {
    // Initialize client (automatically reads OPENAI_API_KEY from environment if empty)
    libopenai::Client client("your-api-key");

    // List available models
    auto models = client.ListModels();
    std::cout << models.dump(2) << "\n";

    // Chat Completion
    std::vector<libopenai::Message> messages = {
        {"system", "You are a helpful assistant."},
        {"user", "Hello! How are you?"}
    };
    auto response = client.ChatCompletion(messages, "gpt-4o-mini", 256, 0.7);
    std::cout << response.dump(2) << "\n";

    return 0;
}
```

---

## License

MIT License.
