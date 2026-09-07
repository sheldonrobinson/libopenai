#include <iostream>
#include "libopenai/libopenai.hpp"

int main() {
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) {
        std::cerr << "Please set OPENAI_API_KEY\n";
        return 1;
    }

    libopenai::Client client(key);

    std::cout << "Testing ListModels...\n";
    auto models = client.ListModels();
    std::cout << models.dump(2) << "\n\n";

    std::cout << "Testing ChatCompletion...\n";
    std::vector<libopenai::Message> messages = {
        {"system", "You are a helpful assistant."},
        {"user", "Say hello in one short sentence."}
    };
    auto chat_res = client.ChatCompletion(messages, "gpt-4o-mini", 64, 0.7);
    std::cout << chat_res.dump(2) << "\n";

    return 0;
}
