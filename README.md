# libopenai

A modern, comprehensive C++17 OpenAI API SDK implemented using **CURL** and **OpenSSL**.

## Features

- **CURL & OpenSSL Backed**: High-performance and secure network requests with SSL certificate verification.
- **Full API Coverage**:
  - **Decisions** (`/decisions`) - Classify or score input with predicate/choice/score questions.
  - **Chat Completions** (`/chat/completions`) with multi-role message support.
  - **Legacy Completions** (`/completions`).
  - **Embeddings** (`/embeddings`) with support for dimensions, encoding_format, and user.
  - **Audio** (`/audio/speech`, `/audio/transcriptions`, `/audio/translations`).
    - Speech: TTS with voice selection, instructions, speed, response_format, stream_format.
    - Transcription: VAD chunking, diarization, speaker references, logprobs, timestamp granularities, streaming.
    - Translation: Whisper and gpt-transcribe model support with response_format and temperature.
  - **Images** (`/images/generations`, `/images/edits`, `/images/variations`).
    - Generation: GPT image models with background, moderation, quality, size, output_format, safety_identifier.
    - Edit: GPT image models with multiple images, mask, markup, input_fidelity.
    - Variation: Legacy DALL-E variations.
  - **Files** (`/files`, upload, delete, retrieve, download content).
  - **Fine-Tuning Jobs** (`/fine_tuning/jobs`, events).
  - **Models** (`/models`, list, retrieve, delete).
  - **Moderations** (`/moderations`) with text and multi-modal image input, omni-moderation models.
  - **Batches** (`/batches`, create, retrieve, cancel, list).
  - **Assistants, Threads, Messages, Runs** (Beta API).
  - **Realtime Translations** (`/realtime/translations/client_secrets`, WebSocket connect URL helper).
- **Modern C++ & Dependencies**: Uses `nlohmann/json` (automatically fetched if not present on system) and C++17 standard features.
- **Thread-safe configuration**: All setter methods are protected by a mutex.

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

On Windows with vcpkg:

```bash
cd libopenai
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
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

## New API Endpoints

### Decisions

```cpp
json request = {
    {"model", "gpt-6-luna"},
    {"input", "The package arrived with a broken screen."},
    {"questions", json::array({
        {
            {"type", "predicate"},
            {"name", "damaged"},
            {"instructions", "Does the customer report a damaged item?"}
        }
    })}
};
auto result = client.CreateDecision(request);
```

### Audio - Speech (TTS)

```cpp
// Generate speech with custom voice and instructions
json voice = {{"id", "voice_1234"}}; // or use a string like "alloy"
bool success = client.CreateSpeech(
    "Hello, world!",
    "gpt-4o-mini-tts",
    voice,
    "Speak in a cheerful tone.",
    "mp3",
    1.0,
    "speech.mp3"
);
```

### Audio - Transcription (STT)

```cpp
// Transcribe with diarization and speaker references
json chunking = {{"type", "server_vad"}, {"silence_duration_ms", 200}};
auto result = client.CreateTranscription(
    "audio.mp3",
    "gpt-4o-transcribe-diarize",
    std::nullopt,   // prompt
    std::nullopt,   // language
    "diarized_json", // response_format
    chunking,        // chunking_strategy
    std::nullopt,   // include
    std::nullopt,   // keywords
    json::array({"agent", "customer"}), // known_speaker_names
    json::array({"data:audio/wav;base64,..."}), // known_speaker_references
    std::nullopt,   // languages
    std::nullopt,   // stream
    0.0,            // temperature
    json::array({"segment"}) // timestamp_granularities
);
```

### Audio - Translation

```cpp
auto result = client.CreateTranslation(
    "audio.mp3",
    "whisper-1",
    std::nullopt,   // prompt
    "json",         // response_format
    0.0             // temperature
);
```

### Images - Generate

```cpp
json request = {
    {"model", "gpt-image-2.5-flare"},
    {"prompt", "A cute baby sea otter"},
    {"n", 1},
    {"size", "1024x1024"},
    {"background", "transparent"},
    {"moderation", "low"},
    {"output_format", "png"},
    {"quality", "medium"},
    {"safety_identifier", "user-1234"}
};
auto result = client.CreateImage(request);
```

### Images - Edit

```cpp
json request = {
    {"images", json::array({
        {{"image_url", "https://example.com/source.png"}}
    })},
    {"prompt", "Add a watercolor effect"},
    {"model", "gpt-image-1.5"},
    {"quality", "high"},
    {"size", "1024x1024"}
};
auto result = client.CreateImageEdit(request);
```

### Embeddings

```cpp
auto result = client.CreateEmbeddings(
    "Hello world",
    "text-embedding-3-small",
    3,                           // dimensions
    "float",                     // encoding_format
    "user-1234"                  // user
);
```

### Moderations

```cpp
// Simple text moderation
auto result = client.CreateModeration("text to classify", "omni-moderation-latest");

// Multi-modal (text + image) moderation
json request = {
    {"model", "omni-moderation-latest"},
    {"input", json::array({
        {{"type", "text"}, {"text", "text to classify"}},
        {{"type", "image_url"}, {"image_url", {{"url", "https://example.com/image.png"}}}}
    })}
};
auto result = client.CreateModeration(request);
```

### Realtime Translations

```cpp
// Create a client secret for frontend use
json request = {
    {"session", {
        {"model", "gpt-realtime-translate"},
        {"audio", {
            {"output", {{"language", "es"}}}
        }}
    }}
};
auto secret = client.CreateRealtimeTranslationClientSecret(request);

// Get WebSocket URL for connecting
std::string ws_url = client.RealtimeTranslationWebSocketUrl("gpt-realtime-translate");
// ws_url = "wss://api.openai.com/v1/realtime/translations?model=gpt-realtime-translate"
```

---

## License

MIT License.
