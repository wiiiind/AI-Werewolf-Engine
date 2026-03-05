#include <iostream>
#include "ai_client.h"
#include "Judge.h"

int main() {
    // 必须关闭缓冲，否则看 AI 聊天会卡死
    setbuf(stdout, NULL);

    std::cout << "初始化 AI 引擎..." << std::endl;
    // 记得在 CLion 环境变量配置好 DEEPSEEK_API_KEY
    AIClient::init("");

    std::cout << "正在召唤数字生命，准备开启狼人杀测试局...\n" << std::endl;

    Judge judge;
    judge.start_game();

    AIClient::cleanup();
    return 0;
}