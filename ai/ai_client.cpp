//
// Created by FengYijia on 26-3-1.
//
#include "ai_client.h"
#include <iostream>
#include <sstream>


// 静态成员初始化
std::string AIClient::m_api_key = "";
std::string AIClient::m_base_url = "https://api.deepseek.com/chat/completions";

void AIClient::init(const std::string& api_key) {
    // 如果传入的参数为空，尝试从系统环境变量里读取
    if (api_key.empty()) {
        const char* env_key = std::getenv("DEEPSEEK_API_KEY");
        if (env_key) {
            m_api_key = std::string(env_key);
        } else {
            std::cerr << "错误: 未找到 API Key，请设置环境变量 DEEPSEEK_API_KEY" << std::endl;
        }
    } else {
        m_api_key = api_key;
    }
    curl_global_init(CURL_GLOBAL_ALL);
}
void AIClient::cleanup() {
    curl_global_cleanup();
}

// 内部回调函数：处理 API 返回的原始数据包
size_t AIClient::write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* response_ptr = static_cast<std::string*>(userp);

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
                        // 把解析出来的 Token 追加到结果字符串里
                        *response_ptr += content;
                        // 为了调试爽快，控制台依然实时打印一下
                        std::cout << content << std::flush;
                    }
                }
            } catch (...) {}
        }
    }
    return totalSize;
}

std::string AIClient::chat_sync(const json& messages) {
    std::cout << messages << std::endl;
    CURL* curl = curl_easy_init();
    std::string full_response = "";

    if (curl) {
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth = "Authorization: Bearer " + m_api_key;
        headers = curl_slist_append(headers, auth.c_str());

        json request_body = {
            {"model", "deepseek-chat"},
            {"messages", messages},
            {"stream", true} // 我们依然用流式，但在这个函数里我们会阻塞直到收完
        };
        std::string json_data = request_body.dump();

        curl_easy_setopt(curl, CURLOPT_URL, m_base_url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &full_response);

        // 执行请求，这会阻塞当前工作线程直到 AI 说完
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            full_response = "Error: " + std::string(curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    std::cout << std::endl; // 回复完了换个行
    return full_response;
}