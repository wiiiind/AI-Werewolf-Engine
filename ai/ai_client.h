//
// Created by FengYijia on 26-3-1.
//

#ifndef AI_CLIENT_H
#define AI_CLIENT_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

using json = nlohmann::json;

class AIClient {
public:
    // 初始化，传入 API Key
    static void init(const std::string& api_key);

    // 清理资源
    static void cleanup();

    // 同步聊天接口：传入完整的对话历史，返回 AI 的回复
    // 注意：在 V1.0 我们先做同步阻塞版，放在线程池里跑就不怕卡主线程
    static std::string chat_sync(const json& messages);

private:
    // libcurl 所需的回调函数，用于接收返回的数据
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);

    static std::string m_api_key;
    static std::string m_base_url;
};




#endif //AI_CLIENT_H
