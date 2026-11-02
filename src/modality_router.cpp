#include "libopenai/modality_router.hpp"

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <algorithm>
#include <sstream>

namespace libopenai {

// -----------------------------------------------------------------------
// Modality detection
// -----------------------------------------------------------------------

Modality ModalityRouter::DetectModality(const Request& req) {
    // First, check the endpoint for hints
    std::string ep = req.endpoint;
    std::transform(ep.begin(), ep.end(), ep.begin(), ::tolower);

    if (ep.find("/chat/completions") != std::string::npos ||
        ep.find("/completions") != std::string::npos) {
        return Modality::Text;
    }
    if (ep.find("/embeddings") != std::string::npos) {
        return Modality::Embedding;
    }
    if (ep.find("/images/") != std::string::npos ||
        ep.find("/image") != std::string::npos) {
        return Modality::Image;
    }
    if (ep.find("/audio/") != std::string::npos ||
        ep.find("/speech") != std::string::npos ||
        ep.find("/transcription") != std::string::npos ||
        ep.find("/translation") != std::string::npos) {
        return Modality::Audio;
    }
    if (ep.find("/video") != std::string::npos) {
        return Modality::Video;
    }

    // Inspect the payload for modality clues
    if (!req.payload.empty()) {
        try {
            auto j = json::parse(req.payload);

            // Check for image inputs (multimodal)
            if (j.contains("messages")) {
                for (const auto& msg : j["messages"]) {
                    if (msg.contains("content")) {
                        auto& content = msg["content"];
                        if (content.is_array()) {
                            for (const auto& part : content) {
                                if (part.contains("type")) {
                                    std::string type = part["type"].get<std::string>();
                                    if (type == "image_url" || type == "image") {
                                        return Modality::Image;
                                    }
                                    if (type == "audio" || type == "input_audio") {
                                        return Modality::Audio;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Check for audio input
            if (j.contains("input") && j["input"].is_array()) {
                return Modality::Audio;
            }

            // Check for image in payload
            if (j.contains("image") || j.contains("image_url")) {
                return Modality::Image;
            }

            // Check model name for modality hints
            if (j.contains("model")) {
                std::string model = j["model"].get<std::string>();
                std::transform(model.begin(), model.end(), model.begin(), ::tolower);
                if (model.find("whisper") != std::string::npos ||
                    model.find("stt") != std::string::npos ||
                    model.find("transcribe") != std::string::npos) {
                    return Modality::SpeechToText;
                }
                if (model.find("tts") != std::string::npos ||
                    model.find("speech") != std::string::npos) {
                    return Modality::TextToSpeech;
                }
                if (model.find("embed") != std::string::npos) {
                    return Modality::Embedding;
                }
                if (model.find("vision") != std::string::npos ||
                    model.find("vl") != std::string::npos ||
                    model.find("llava") != std::string::npos) {
                    return Modality::Image;
                }
            }
        } catch (...) {
            // Ignore parse errors, fall through to default
        }
    }

    return Modality::Text; // default fallback
}

std::optional<std::string> ModalityRouter::ExtractModel(const Request& req) {
    if (req.payload.empty()) return std::nullopt;
    try {
        auto j = json::parse(req.payload);
        if (j.contains("model") && j["model"].is_string()) {
            return j["model"].get<std::string>();
        }
    } catch (...) {}
    return std::nullopt;
}

// -----------------------------------------------------------------------
// ModalityRouter
// -----------------------------------------------------------------------

ProviderPtr ModalityRouter::Route(const Request& req) const {
    auto all = RouteAll(req);
    if (all.empty()) return nullptr;
    return all[0]; // already sorted by priority
}

std::vector<ProviderPtr> ModalityRouter::RouteAll(const Request& req) const {
    Modality mod = DetectModality(req);
    auto model = ExtractModel(req);

    // Get providers for this modality
    auto providers = registry_->GetProvidersForModality(mod);

    // Filter by model if specified
    if (model.has_value()) {
        std::vector<ProviderPtr> filtered;
        for (auto& p : providers) {
            if (p->SupportsModel(*model)) {
                filtered.push_back(p);
            }
        }
        // If no provider supports the specific model but some support the
        // modality, fall back to all modality providers (they may accept
        // any model)
        if (filtered.empty() && !providers.empty()) {
            filtered = providers;
        }
        providers = std::move(filtered);
    }

    // Sort by priority
    std::sort(providers.begin(), providers.end(),
        [](const ProviderPtr& a, const ProviderPtr& b) {
            return a->Info().priority < b->Info().priority;
        });

    return providers;
}

// -----------------------------------------------------------------------
// WeightedModalityRouter
// -----------------------------------------------------------------------

ProviderPtr WeightedModalityRouter::Route(const Request& req) const {
    return RouteWithFallback(req, true);
}

ProviderPtr WeightedModalityRouter::RouteWithFallback(const Request& req,
                                                        bool prefer_device) const {
    auto candidates = RouteAll(req);
    if (candidates.empty()) return nullptr;

    // Group by tier for device-cloud collaborative routing
    std::vector<ProviderPtr> device_providers;
    std::vector<ProviderPtr> hybrid_providers;
    std::vector<ProviderPtr> cloud_providers;

    for (auto& p : candidates) {
        switch (p->Tier()) {
            case ProviderTier::Device:
                device_providers.push_back(p);
                break;
            case ProviderTier::Hybrid:
                hybrid_providers.push_back(p);
                break;
            case ProviderTier::Cloud:
                cloud_providers.push_back(p);
                break;
        }
    }

    // Select based on preference
    if (prefer_device) {
        // Try device first, then hybrid, then cloud
        if (!device_providers.empty()) return device_providers[0];
        if (!hybrid_providers.empty()) return hybrid_providers[0];
        if (!cloud_providers.empty()) return cloud_providers[0];
    } else {
        // Cloud-first preference
        if (!cloud_providers.empty()) return cloud_providers[0];
        if (!hybrid_providers.empty()) return hybrid_providers[0];
        if (!device_providers.empty()) return device_providers[0];
    }

    return candidates[0]; // fallback
}

} // namespace libopenai