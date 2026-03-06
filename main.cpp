#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "ai_client.h"
#include "Judge.h"
#include "log.h"

int main() {
    // 必须关闭缓冲，否则看 AI 聊天会卡死
    setbuf(stdout, NULL);

    std::filesystem::create_directories("../logs");
    if (!Log::get_instance()->init("../logs/werewolf.log", 8192, 500000, 0)) {
        throw std::runtime_error("failed to initialize log file");
    }
    LOG_INFO("[GAME][BOOT] process started");

    std::cout << "初始化 AI 引擎..." << std::endl;
    LOG_INFO("[GAME][BOOT] initializing ai client");
    AIClient::init("");

    std::cout << "正在召唤数字生命，准备开启狼人杀测试局...\n" << std::endl;

    Judge judge;
    judge.start_game();

    LOG_INFO("[GAME][BOOT] game loop finished, cleaning up ai client");
    AIClient::cleanup();
    Log::get_instance()->flush();
    return 0;
}
