#include "support/PromptParsers.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::string clean_response(const std::string& raw_reply) {
    size_t start = raw_reply.find('{');
    size_t end = raw_reply.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end >= start) {
        return raw_reply.substr(start, end - start + 1);
    }
    return raw_reply;
}

int parse_target_from_reply(const std::string& raw_reply, int fallback) {
    try {
        json parsed = json::parse(clean_response(raw_reply));
        if (parsed.contains("data") && parsed["data"].contains("target")) {
            return parsed["data"]["target"];
        }
    } catch (...) {
    }
    return fallback;
}

std::string parse_speech_from_reply(const std::string& raw_reply) {
    json parsed = json::parse(clean_response(raw_reply));
    return parsed["data"]["speech"];
}

std::string parse_thought_from_reply(const std::string& raw_reply, const std::string& fallback) {
    try {
        json parsed = json::parse(clean_response(raw_reply));
        if (parsed.contains("thought")) {
            return parsed["thought"];
        }
    } catch (...) {
    }
    return fallback;
}
