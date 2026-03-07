#include "Judge.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <thread>
#include "log.h"
#include "RuleEngine.h"
#include "hooks/SkillHooks.h"
#include "support/PromptParsers.h"
#include "support/PhaseUtils.h"
#include "phases/VotePhase.h"
#include "phases/DaySpeakPhase.h"
#include "phases/SheriffElectionPhase.h"
#include "phases/NightPhase.h"

namespace {
std::string join_lines(const std::vector<std::string>& lines) {
    std::string result;
    for (const std::string& line : lines) {
        result += line;
        result += "\n";
    }
    return result;
}
}

Judge::Judge() : m_event_bus(m_context), m_ai(m_context, m_event_bus) {
    init_config();
    LOG_INFO("[GAME][INIT] judge constructed");

    SkillHooks::register_hooks(
        m_context,
        m_event_bus,
        m_ai,
        SkillHookCallbacks{
            [this](int dead_id) { handle_sheriff_death(dead_id); },
            [this](int dead_id, const std::string& cause, bool allow_hunter) {
                return publish_player_death(dead_id, cause, allow_hunter);
            },
            [this]() { return handle_win_if_needed(); }
        }
    );
}

void Judge::init_config() {
    std::ifstream t_file("../resources/templates.json");
    t_file >> m_context.templates;
    m_context.global_history = json::array();

    std::ifstream r_file("../resources/roles.json");
    json roles_pool;
    r_file >> roles_pool;

    auto rng = std::default_random_engine(std::random_device{}());
    std::shuffle(roles_pool.begin(), roles_pool.end(), rng);

    std::vector<json> selected_configs;
    int wwk = 0, w = 0, v = 0, s = 0, wt = 0, h = 0, g = 0;

    for (const auto& item : roles_pool) {
        const std::string role = item["role"];
        if (role == "WHITE_WOLF_KING" && wwk < 1) { selected_configs.push_back(item); wwk++; }
        else if (role == "WOLF" && w < 3) { selected_configs.push_back(item); w++; }
        else if (role == "SEER" && s < 1) { selected_configs.push_back(item); s++; }
        else if (role == "WITCH" && wt < 1) { selected_configs.push_back(item); wt++; }
        else if (role == "HUNTER" && h < 1) { selected_configs.push_back(item); h++; }
        else if (role == "GUARD" && g < 1) { selected_configs.push_back(item); g++; }
        else if (role == "VILLAGER" && v < 4) { selected_configs.push_back(item); v++; }
    }

    std::shuffle(selected_configs.begin(), selected_configs.end(), rng);

    std::cout << "正在初始化 12 人白狼王守卫局 (1白 3狼 4民 4神)..." << std::endl;
    for (size_t i = 0; i < selected_configs.size(); ++i) {
        m_context.players.push_back(std::make_unique<Player>(
            static_cast<int>(i) + 1,
            string_to_role(selected_configs[i]["role"]),
            selected_configs[i]["personality"]
        ));

        std::cout << "🪑 " << (i + 1) << "号:[" << role_to_string(m_context.players[i]->get_role()) << "] "
                  << m_context.players[i]->get_personality().substr(0, 50) << "..." << std::endl;
    }
    std::cout << "✅ 12人白狼王局初始化完毕！" << std::endl;
    LOG_INFO("[GAME][INIT] selected 12 players for current game");

    std::map<Role, int> role_counts;
    for (const auto& player : m_context.players) {
        role_counts[player->get_role()]++;
    }

    std::string role_config;
    for (const auto& [role, count] : role_counts) {
        if (!role_config.empty()) {
            role_config += ", ";
        }
        role_config += std::to_string(count) + role_to_string(role);
    }

    m_context.game_config_string = m_ai.render_named_template(
        "system_init",
        {
            {"game_config", role_config},
            {"player_count", std::to_string(m_context.player_count())}
        }
    );

    std::cout << "\n[系统规则]: " << m_context.game_config_string << std::endl;
    LOG_INFO("[GAME][CONFIG] rules=%s", m_context.game_config_string.c_str());
    for (const auto& player : m_context.players) {
        LOG_INFO("[GAME][SEAT] player=%d role=%s personality=%s", player->get_id(), role_to_string(player->get_role()).c_str(), player->get_personality().c_str());
    }
}

Role Judge::string_to_role(const std::string& str) const {
    if (str == "WOLF") return Role::WOLF;
    if (str == "WHITE_WOLF_KING") return Role::WHITE_WOLF_KING;
    if (str == "SEER") return Role::SEER;
    if (str == "WITCH") return Role::WITCH;
    if (str == "HUNTER") return Role::HUNTER;
    if (str == "GUARD") return Role::GUARD;
    if (str == "IDIOT") return Role::IDIOT;
    return Role::VILLAGER;
}

std::string Judge::role_to_string(Role role) const {
    switch (role) {
        case Role::WOLF: return "狼人";
        case Role::WHITE_WOLF_KING: return "白狼王";
        case Role::SEER: return "预言家";
        case Role::WITCH: return "女巫";
        case Role::HUNTER: return "猎人";
        case Role::GUARD: return "守卫";
        case Role::IDIOT: return "白痴";
        default: return "平民";
    }
}

bool Judge::compress_history_block(std::vector<std::string>& lines, const std::string& stage_name, bool& compressed_flag) {
    if (compressed_flag || lines.empty()) {
        return false;
    }

    const std::string system_prompt = m_ai.render_named_template("history_compression_system_instruction", {});
    const std::string user_prompt = m_ai.render_named_template(
        "history_compression_user_instruction",
        {
            {"stage_name", stage_name},
            {"raw_stage_text", join_lines(lines)}
        }
    );

    json request = json::array();
    request.push_back({{"role", "system"}, {"content", system_prompt}});
    request.push_back({{"role", "user"}, {"content", user_prompt}});

    AIOrchestrator::AIResult result = m_ai.ask_messages(
        "[GAME][HISTORY_COMPRESSION][" + stage_name + "]",
        request,
        AIOrchestrator::AskOptions{AIOrchestrator::RequestMode::Sequential, false},
        "{\"summary_text\":\""
    );
    if (!result.ok) {
        LOG_WARN("[GAME][HISTORY_COMPRESSION] stage=%s action=skip reason=request_failed", stage_name.c_str());
        return false;
    }

    std::string summary_text;
    try {
        const json parsed = json::parse(clean_response(result.raw_reply));
        summary_text = parsed.value("summary_text", "");
    } catch (...) {
        LOG_WARN("[GAME][HISTORY_COMPRESSION] stage=%s action=skip reason=parse_failed", stage_name.c_str());
        return false;
    }

    if (summary_text.empty()) {
        LOG_WARN("[GAME][HISTORY_COMPRESSION] stage=%s action=skip reason=empty_summary", stage_name.c_str());
        return false;
    }

    std::vector<std::size_t> indices;
    indices.reserve(lines.size());
    std::size_t cursor = 0;
    for (const std::string& target_line : lines) {
        bool found = false;
        while (cursor < m_context.global_history.size()) {
            const json& item = m_context.global_history[cursor];
            if (item.contains("content") && item["content"].is_string() && item["content"].get<std::string>() == target_line) {
                indices.push_back(cursor);
                ++cursor;
                found = true;
                break;
            }
            ++cursor;
        }
        if (!found) {
            LOG_WARN("[GAME][HISTORY_COMPRESSION] stage=%s action=skip reason=history_mismatch", stage_name.c_str());
            return false;
        }
    }

    const std::size_t insert_index = indices.front();
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        m_context.global_history.erase(m_context.global_history.begin() + static_cast<json::difference_type>(*it));
    }
    m_context.global_history.insert(
        m_context.global_history.begin() + static_cast<json::difference_type>(insert_index),
        {{"role", "user"}, {"content", "系统: 【压缩摘要：" + stage_name + "】\n" + summary_text}}
    );

    compressed_flag = true;
    lines.clear();
    LOG_INFO("[GAME][HISTORY_COMPRESSION] stage=%s action=compressed", stage_name.c_str());
    return true;
}

void Judge::maybe_compress_public_history() {
    if (m_context.day_count >= 3) {
        compress_history_block(
            m_context.opening_sheriff_speech_lines,
            "首轮警长竞选发言",
            m_context.opening_sheriff_speech_compressed
        );
        compress_history_block(
            m_context.day1_day_speech_lines,
            "第1天白天发言",
            m_context.day1_day_speech_compressed
        );
    }
}

bool Judge::handle_win_if_needed() {
    const WinCheckResult result = RuleEngine::check_win_condition(m_context);
    if (!result.game_over) {
        return false;
    }

    const std::string winner = result.winner == "GOOD" ? "好人阵营" : "狼人阵营";
    LOG_INFO("[GAME][WIN] winner=%s reason=%s", winner.c_str(), result.reason.c_str());
    m_event_bus.broadcast("系统", "游戏结束：" + winner + "胜利。原因：" + result.reason);
    m_context.state = GameState::GAME_OVER;
    return true;
}

bool Judge::publish_player_death(int dead_id, const std::string& cause, bool allow_hunter) {
    LOG_INFO("[GAME][DEATH] publish player=%d cause=%s allow_hunter=%s", dead_id, cause.c_str(), allow_hunter ? "true" : "false");
    Player* player = m_context.find_player(dead_id);
    if (player == nullptr || !player->is_alive()) {
        return false;
    }

    player->kill();
    handle_sheriff_death(dead_id);

    Event death_event;
    death_event.type = EventType::PLAYER_DIED;
    death_event.name = "player_died";
    death_event.actor_id = dead_id;
    death_event.payload["cause"] = cause;
    death_event.payload["allow_hunter"] = allow_hunter;
    m_event_bus.publish(std::move(death_event));
    return true;
}

void Judge::run_night() {
    NightPhase phase(
        m_context,
        m_event_bus,
        m_ai,
        NightPhaseCallbacks{
            [this](int dead_id, const std::string& cause, bool allow_hunter) {
                return publish_player_death(dead_id, cause, allow_hunter);
            }
        }
    );
    phase.execute();
}

void Judge::run_day_speak() {
    maybe_compress_public_history();
    DaySpeakPhase phase(
        m_context,
        m_event_bus,
        m_ai,
        DaySpeakPhaseCallbacks{
            [this](Role role) { return role_to_string(role); },
            [this](int dead_id, const std::string& cause, bool allow_hunter) {
                return publish_player_death(dead_id, cause, allow_hunter);
            },
            [this]() { return handle_win_if_needed(); }
        }
    );
    phase.execute();
}

void Judge::run_vote() {
    VotePhase phase(
        m_context,
        m_event_bus,
        m_ai,
        VotePhaseCallbacks{
            [this](Role role) { return role_to_string(role); },
            [this](int dead_id, const std::string& cause, bool allow_hunter) {
                return publish_player_death(dead_id, cause, allow_hunter);
            },
            [this]() { return handle_win_if_needed(); }
        }
    );
    phase.execute();
}

void Judge::run_sheriff_election() {
    maybe_compress_public_history();
    SheriffElectionPhase phase(
        m_context,
        m_event_bus,
        m_ai,
        SheriffElectionPhaseCallbacks{
            [this](Role role) { return role_to_string(role); },
            [this](int dead_id) { handle_sheriff_death(dead_id); },
            [this](int dead_id, const std::string& cause, bool allow_hunter) {
                return publish_player_death(dead_id, cause, allow_hunter);
            },
            [this]() { return handle_win_if_needed(); }
        }
    );
    phase.execute();
}

void Judge::handle_sheriff_death(int dead_id) {
    if (m_context.sheriff_id != dead_id) {
        return;
    }

    const WinCheckResult win_result = RuleEngine::check_win_condition(m_context);
    if (win_result.game_over) {
        LOG_INFO("[GAME][SHERIFF] dead_sheriff=%d action=skip_pass_due_to_game_over winner=%s reason=%s",
                 dead_id,
                 win_result.winner.c_str(),
                 win_result.reason.c_str());
        m_context.sheriff_id = -1;
        return;
    }

    Player* dead_sheriff = m_context.find_player(dead_id);
    if (dead_sheriff == nullptr) {
        return;
    }

    std::vector<int> alive_ids;
    for (Player* player : m_context.alive_players()) {
        if (player->get_id() != dead_id) {
            alive_ids.push_back(player->get_id());
        }
    }

    const std::string instruction = m_ai.render_named_template(
        "sheriff_pass_instruction",
        {
            {"alive_list", join_ids(alive_ids)},
            {"player_id", std::to_string(dead_sheriff->get_id())},
            {"role_name", role_to_string(dead_sheriff->get_role())}
        }
    );
    AIOrchestrator::AIResult result = m_ai.ask_player(*dead_sheriff, "sheriff_pass_instruction", instruction);
    const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
    const int target = parse_target_from_reply(result.raw_reply);
    LOG_INFO("[GAME][P%d][THOUGHT][sheriff_pass] %s", dead_sheriff->get_id(), thought.c_str());

    if (target != -1 && m_context.is_player_alive(target)) {
        m_context.sheriff_id = target;
        LOG_INFO("[GAME][SHERIFF] dead_sheriff=%d action=pass target=%d", dead_id, target);
        m_event_bus.broadcast("系统", "警长 " + std::to_string(dead_id) + " 号将警徽移交给了 " + std::to_string(target) + " 号。");
    } else {
        m_context.sheriff_id = -1;
        LOG_INFO("[GAME][SHERIFF] dead_sheriff=%d action=destroy_badge", dead_id);
        m_event_bus.broadcast("系统", "警长 " + std::to_string(dead_id) + " 号撕毁了警徽，本局不再有警长。");
    }
}

void Judge::start_game() {
    LOG_INFO("[GAME][START] entering main loop");
    while (m_context.state != GameState::GAME_OVER) {
        switch (m_context.state) {
            case GameState::NIGHT_PHASE:
                run_night();
                break;
            case GameState::SHERIFF_ELECTION_PHASE:
                run_sheriff_election();
                break;
            case GameState::DAY_SPEAK_PHASE:
                run_day_speak();
                break;
            case GameState::DAY_VOTE_PHASE:
                run_vote();
                break;
            case GameState::GAME_OVER:
                break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    LOG_INFO("[GAME][END] main loop exited");
}
