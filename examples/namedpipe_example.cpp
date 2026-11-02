#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

#include "libopenai/libopenai.hpp"

using namespace libopenai;

// Example: demonstrates injecting a custom ITransport into the Client.
// With LIBOPENAI_USE_NAMED_PIPE defined, the default transport would be
// CreateNamedPipeTransport("llama-openai-pipe").
//
// To use a named pipe transport explicitly:
//   auto pipe_transport = CreateNamedPipeTransport("my-pipe-name");
//   Client client("api-key", "http://localhost", pipe_transport);
//
// For this example, we use the default httplib transport to show the API.

int main() {
    std::cout << "=== libopenai Transport Injection Example ===" << std::endl;

    // Client with default transport (cpp-httplib)
    Client client("dummy-key", "http://localhost");

    std::cout << "Active transport: " << client.TransportName() << std::endl;

    // Example: List models
    std::cout << "\nCalling ListModels()..." << std::endl;
    auto models = client.ListModels();
    std::cout << "Result type: " << (models.is_null() ? "error/null" : "json") << std::endl;

    // Example: Chat completion
    std::cout << "\nCalling ChatCompletion()..." << std::endl;
    std::vector<Message> messages = {
        {"system", "You are a helpful assistant."},
        {"user", "Say hello!"}
    };
    auto chat = client.ChatCompletion(messages, "gpt-4o-mini", 64, 0.7);
    std::cout << "Result type: " << (chat.is_null() ? "error/null" : "json") << std::endl;

    // Example: Embeddings
    std::cout << "\nCalling CreateEmbeddings()..." << std::endl;
    auto emb = client.CreateEmbeddings("hello world");
    std::cout << "Result type: " << (emb.is_null() ? "error/null" : "json") << std::endl;

    // Example: Moderation
    std::cout << "\nCalling CreateModeration()..." << std::endl;
    auto mod = client.CreateModeration(std::string("test input"));
    std::cout << "Result type: " << (mod.is_null() ? "error/null" : "json") << std::endl;

    // Example: Audio Translation
    // (requires audio file - skipped in this example)

    // Example: Image generation
    std::cout << "\nCalling CreateImage()..." << std::endl;
    json img_body;
    img_body["prompt"] = "A beautiful sunset";
    img_body["n"] = 1;
    img_body["size"] = "1024x1024";
    auto img = client.CreateImage(img_body);
    std::cout << "Result type: " << (img.is_null() ? "error/null" : "json") << std::endl;

    // Example: Decision
    std::cout << "\nCalling CreateDecision()..." << std::endl;
    json decision_body;
    decision_body["input"] = "Is this text positive or negative?";
    decision_body["predicates"] = {"positive", "negative"};
    auto dec = client.CreateDecision(decision_body);
    std::cout << "Result type: " << (dec.is_null() ? "error/null" : "json") << std::endl;

    std::cout << "\n=== Example complete ===" << std::endl;
    return 0;
}
