//
// Created by FengYijia on 26-3-1.
//
#include "ai_client.h"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include "log.h"

// 静态成员初始化
std::string AIClient::m_api_key = "";
std::string AIClient::m_base_url = "https://api.deepseek.com/chat/completions";
std::string AIClient::m_beta_base_url = "https://api.deepseek.com/beta/chat/completions";

namespace {
struct StreamingResponseBuffer {
    std::string response;
    bool stream_output = true;
    AIClient::UsageInfo usage;
};

bool should_prepend_json_prefix(const AIClient::RequestOptions& options) {
    return options.json_output && !options.assistant_prefix.empty();
}

size_t streaming_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
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
                if (j.contains("usage")) {
                    const auto& usage = j["usage"];
                    if (usage.contains("prompt_cache_hit_tokens")) {
                        buffer->usage.hit = usage["prompt_cache_hit_tokens"].get<int>();
                    }
                    if (usage.contains("prompt_cache_miss_tokens")) {
                        buffer->usage.miss = usage["prompt_cache_miss_tokens"].get<int>();
                    }
                    if (usage.contains("completion_tokens")) {
                        buffer->usage.completion = usage["completion_tokens"].get<int>();
                    }
                }
            } catch (...) {}
        }
    }
    return totalSize;
}

AIClient::ChatResult perform_streaming_chat_request(
    const std::string& api_key,
    const std::string& base_url,
    const json& messages,
    const AIClient::RequestOptions& options
) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("Failed to initialize CURL for DeepSeek request.");
    }

    StreamingResponseBuffer buffer;
    buffer.stream_output = options.stream_output;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, auth.c_str());

    json request_messages = messages;
    if (should_prepend_json_prefix(options)) {
        request_messages.push_back({
            {"role", "assistant"},
            {"content", options.assistant_prefix},
            {"prefix", true}
        });
    }

    json request_body = {
        {"model", "deepseek-chat"},
        {"messages", request_messages},
        {"stream", true},
        {"stream_options", {{"include_usage", true}}},
        {"max_tokens", options.max_tokens}
    };
    if (options.json_output) {
        request_body["response_format"] = {{"type", "json_object"}};
    }
    const std::string json_data = request_body.dump();

    curl_easy_setopt(curl, CURLOPT_URL, base_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_data.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streaming_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    const CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("DeepSeek request failed: " + std::string(curl_easy_strerror(res)));
    }

    if (buffer.stream_output) {
        std::cout << std::endl;
    }
    if (should_prepend_json_prefix(options) && !buffer.response.empty()) {
        if (buffer.response.rfind(options.assistant_prefix, 0) != 0) {
            buffer.response.insert(0, options.assistant_prefix);
        }
    }
    return AIClient::ChatResult{buffer.response, buffer.usage};
}

size_t append_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total_size = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), total_size);
    return total_size;
}
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

void AIClient::verify_service_available() {
    if (m_api_key.empty()) {
        throw std::runtime_error("DeepSeek API key is not initialized.");
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("Failed to initialize CURL for DeepSeek probe.");
    }

    std::string response_body;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string auth = "Authorization: Bearer " + m_api_key;
    headers = curl_slist_append(headers, auth.c_str());

    const json request_body = {
        {"model", "deepseek-chat"},
        {"messages", json::array({{{"role", "user"}, {"content", "ping"}}})},
        {"stream", false},
        {"max_tokens", 1}
    };
    const std::string json_data = request_body.dump();

    curl_easy_setopt(curl, CURLOPT_URL, m_base_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_data.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    const CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("DeepSeek service probe failed: " + std::string(curl_easy_strerror(res)));
    }

    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error(
            "DeepSeek service probe failed with HTTP " + std::to_string(http_code) + ": " + response_body
        );
    }

    try {
        const json parsed = json::parse(response_body);
        if (!parsed.contains("choices") || parsed["choices"].empty()) {
            throw std::runtime_error("DeepSeek service probe returned an unexpected response: " + response_body);
        }
    } catch (const std::exception& ex) {
        throw std::runtime_error("DeepSeek service probe returned invalid JSON: " + std::string(ex.what()));
    }

    LOG_INFO("[AI][PROBE] DeepSeek service probe passed");
}

size_t AIClient::write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    return streaming_write_callback(contents, size, nmemb, userp);
}

std::string AIClient::chat_sync(const json& messages) {
    return chat_sync(messages, RequestOptions{true, true, "", 2048});
}

std::string AIClient::chat_sync(const json& messages, const RequestOptions& options) {
    return chat_sync_with_metadata(messages, options).content;
}

AIClient::ChatResult AIClient::chat_sync_with_metadata(const json& messages, const RequestOptions& options) {
    if (m_api_key.empty()) {
        throw std::runtime_error("DeepSeek API key is not initialized.");
    }

    const std::string& request_url = should_prepend_json_prefix(options) ? m_beta_base_url : m_base_url;
    ChatResult result = perform_streaming_chat_request(m_api_key, request_url, messages, options);
    if (options.json_output && result.content.empty()) {
        LOG_WARN("[AI][JSON_OUTPUT] empty content returned, retrying without response_format");
        RequestOptions fallback_options = options;
        fallback_options.json_output = false;
        fallback_options.assistant_prefix.clear();
        result = perform_streaming_chat_request(m_api_key, m_base_url, messages, fallback_options);
    }
    return result;
}
