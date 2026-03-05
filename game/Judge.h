#ifndef JUDGE_H
#define JUDGE_H

#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "player.h"

using json = nlohmann::json;

enum class GameState { NIGHT_PHASE, SHERIFF_ELECTION_PHASE, DAY_SPEAK_PHASE, DAY_VOTE_PHASE, GAME_OVER };

class Judge {
public:
    Judge();
    ~Judge();
    void start_game();

private:
    // 初始化与配置加载
    void init_config();
    Role string_to_role(const std::string& str);
    std::string role_to_string(Role r);

    // 【核心】缓存友好型请求构造
    json build_ai_request(Player* p, const std::string& instruction);
    
    // 广播逻辑
    void broadcast(const std::string& speaker, const std::string& message);

    // 状态机阶段
    void run_night();
    void run_day_speak();
    void run_vote();
    bool check_win_condition();

private:
    std::vector<Player*> m_players;
    json m_templates;       // 存储 templates.json
    json m_global_history;  // 全局公有历史记录
    GameState m_state;
    int m_day_count;
    int m_dead_last_night;
    std::vector<int> m_deaths_last_night; // 记录昨晚最终所有的死者
    bool m_witch_save_used;   // 解药是否已用
    bool m_witch_poison_used; // 毒药是否已用
    bool m_enable_first_night_last_words;   // 首夜遗言
    std::string m_game_config_string; // 全局设定
    int m_sheriff_id; // 记录当前警长是谁，-1代表无警徽
    void run_sheriff_election(); // 警长竞选流程
    void handle_sheriff_death(int dead_id);
    void handle_hunter_shoot(int hunter_id);
    int m_guard_target;      // 今晚守护目标
    int m_last_guard_target; // 昨晚守护目标 (用于防止连续守)
};

#endif