//
// Created by FengYijia on 26-3-1.
//
#include "ai_client.h"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>

// 静态成员初始化
std::string AIClient::m_api_key = "";
std::string AIClient::m_base_url = "https://api.deepseek.com/chat/completions";

namespace {
struct StreamingResponseBuffer {
    std::string response;
    bool stream_output = true;
};
}

void AIClient::init(const std::string& api_key) {
    if (api_key.empty()) {
        const char* env_key = std::getenv("DEEPSEEK_API_KEY");
        if (env_key != nullptr && *env_key != '\0') {
            m_api_key = std::string(env_key);
        } else {
            throw std::runtime_error("未找到 DeepSeek API 密钥。请在启动游戏前设置环境变量 DEEPSEEK_API_KEY 或传入 DeepSeek API 密钥。");
        }
    } else {
        m_api_key = api_key;
    }

    curl_global_init(CURL_GLOBAL_ALL);
}

void AIClient::cleanup() {
    curl_global_cleanup();
}

size_t AIClient::write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    StreamingResponseBuffer* buffer = static_cast<StreamingResponseBuffer*>(userp);

    std::string raw_data(static_cast<char*>(contents), totalSize);
    std::istringstream stream(raw_data);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty() || line == "\r") continue;
        if (line.find("data: ") == 0) {
            std::string json_str = line.substr(6);
            if (json_str.find("[DONE]") != std::string::npos) break;

            try {
                auto j = json::parse(json_str);
                if (j.contains("choices") && j["choices"][0].contains("delta")) {
                    auto& delta = j["choices"][0]["delta"];
                    if (delta.contains("content")) {
                        std::string content = delta["content"];
                        buffer->response += content;
                        if (buffer->stream_output) {
                            std::cout << content << std::flush;
                        }
                    }
                }
            } catch (...) {}
        }
    }
    return totalSize;
}

std::string AIClient::chat_sync(const json& messages) {
    return chat_sync(messages, RequestOptions{true});
}

std::string AIClient::chat_sync(const json& messages, const RequestOptions& options) {
    if (m_api_key.empty()) {
        throw std::runtime_error("DeepSeek API key is not initialized.");
    }

    // std::cout << messages << std::endl;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("Failed to initialize CURL for DeepSeek request.");
    }

    StreamingResponseBuffer buffer;
    buffer.stream_output = options.stream_output;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + m_api_key;
    headers = curl_slist_append(headers, auth.c_str());

    json request_body = {
        {"model", "deepseek-chat"},
        {"messages", messages},
        {"stream", true}
    };
    std::string json_data = request_body.dump();

    curl_easy_setopt(curl, CURLOPT_URL, m_base_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("DeepSeek request failed: " + std::string(curl_easy_strerror(res)));
    }

    if (buffer.stream_output) {
        std::cout << std::endl;
    }
    return buffer.response;
}
