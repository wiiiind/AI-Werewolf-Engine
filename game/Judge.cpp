#include "Judge.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <thread>

namespace {
std::string clean_response(const std::string& s) {
    size_t start = s.find('{');
    size_t end = s.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end >= start) {
        return s.substr(start, end - start + 1);
    }
    return s;
}

std::string join_ids(const std::vector<int>& ids, const std::string& sep = " ") {
    std::string result;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            result += sep;
        }
        result += std::to_string(ids[i]);
    }
    return result;
}

std::string join_player_ids(const std::vector<Player*>& players, const std::string& sep = " ") {
    std::vector<int> ids;
    ids.reserve(players.size());
    for (Player* player : players) {
        ids.push_back(player->get_id());
    }
    return join_ids(ids, sep);
}

std::string format_vote_records(const std::map<int, std::vector<int>>& records) {
    std::string details;
    for (const auto& [candidate, voters] : records) {
        if (candidate == -1) {
            details += "【弃票/废票】: ";
        } else {
            details += "【投" + std::to_string(candidate) + "号】: ";
        }
        for (size_t i = 0; i < voters.size(); ++i) {
            details += std::to_string(voters[i]);
            if (i + 1 < voters.size()) {
                details += ", ";
            }
        }
        details += "; ";
    }
    return details;
}

int parse_target_from_reply(const std::string& raw_reply, int fallback = -1) {
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

std::string build_death_message(const std::vector<int>& deaths_last_night) {
    if (deaths_last_night.empty()) {
        return "昨晚是平安夜";
    }

    std::string message = "昨晚死亡的是：";
    for (size_t i = 0; i < deaths_last_night.size(); ++i) {
        message += std::to_string(deaths_last_night[i]) + "号";
        if (i + 1 < deaths_last_night.size()) {
            message += "、";
        }
    }
    return message;
}
}

Judge::Judge() : m_event_bus(m_context), m_ai(m_context, m_event_bus) {
    init_config();
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

bool Judge::handle_win_if_needed() {
    const WinCheckResult result = RuleEngine::check_win_condition(m_context);
    if (!result.game_over) {
        return false;
    }

    const std::string winner = result.winner == "GOOD" ? "好人阵营" : "狼人阵营";
    m_event_bus.broadcast("系统", "游戏结束：" + winner + "胜利。原因：" + result.reason);
    m_context.state = GameState::GAME_OVER;
    return true;
}

void Judge::run_night() {
    std::cout << "\n========== 第 " << m_context.day_count << " 天 黑夜降临 ==========" << std::endl;
    m_context.dead_last_night = -1;
    m_context.guard_target = -1;

    const std::vector<Player*> alive_players = m_context.alive_players();
    const std::string alive_list = join_player_ids(alive_players);
    std::vector<Player*> alive_wolves = m_context.alive_players_with_roles({Role::WOLF, Role::WHITE_WOLF_KING});

    if (!alive_wolves.empty()) {
        std::string round1_history;
        bool consensus_reached = false;

        for (int round = 1; round <= 2; ++round) {
            std::vector<int> wolf_votes(m_context.player_count() + 1, 0);
            std::string current_round_actions;

            for (Player* wolf : alive_wolves) {
                std::string teammates;
                for (Player* other : alive_wolves) {
                    if (other->get_id() == wolf->get_id()) {
                        continue;
                    }
                    if (!teammates.empty()) {
                        teammates += " ";
                    }
                    teammates += std::to_string(other->get_id());
                }
                if (teammates.empty()) {
                    teammates = "无";
                }

                std::string instruction = m_ai.render_named_template(
                    "night_wolf_instruction",
                    {
                        {"day_count", std::to_string(m_context.day_count)},
                        {"teammates_str", teammates},
                        {"alive_list", alive_list},
                        {"player_id", std::to_string(wolf->get_id())}
                    }
                );
                instruction += "\n\n【夜间沟通频道】\n";
                if (round == 2) {
                    instruction += "⚠️警告：由于上一轮平票，这是第二轮重投；如果再次平票，今晚将无人死亡。\n";
                    instruction += "上一轮全队投票历史如下：\n" + round1_history + "\n";
                }
                if (!current_round_actions.empty()) {
                    instruction += "本轮前置位队友的指认如下：\n" + current_round_actions + "\n";
                }

                AIOrchestrator::AIResult result = m_ai.ask_player(*wolf, "night_wolf_instruction", instruction);
                int target = parse_target_from_reply(result.raw_reply, 0);
                if (target < 0 || target > m_context.player_count()) {
                    target = 0;
                }
                wolf_votes[target]++;
                current_round_actions += "【" + std::to_string(wolf->get_id()) + "号狼人指认了: " + std::to_string(target) + "号】\n";
                m_event_bus.append_private_record(*wolf, "第" + std::to_string(round) + "轮夜间指认: " + std::to_string(target) + "号");
            }

            VoteResolution vote_resolution = RuleEngine::resolve_top_votes(wolf_votes, 0, m_context.player_count());
            if (vote_resolution.leaders.size() == 1) {
                m_context.dead_last_night = vote_resolution.leaders.front() == 0 ? -1 : vote_resolution.leaders.front();
                consensus_reached = true;
                break;
            }

            if (round == 1) {
                round1_history = current_round_actions;
            } else {
                m_context.dead_last_night = -1;
            }
        }

        const std::string final_memory = consensus_reached
            ? (m_context.dead_last_night == -1
                ? "【夜间结算】狼人阵营达成共识，今晚空刀。"
                : "【夜间结算】狼人阵营最终决定击杀: " + std::to_string(m_context.dead_last_night) + "号。")
            : "【夜间结算】狼人阵营因平票协商失败，被迫空刀。";

        for (Player* wolf : alive_wolves) {
            m_event_bus.append_private_record(*wolf, final_memory);
        }
    }

    int poison_target = -1;
    bool witch_used_save_tonight = false;

    for (Player* player : m_context.alive_players()) {
        if (player->get_role() == Role::SEER) {
            const std::string instruction = m_ai.render_named_template(
                "night_seer_instruction",
                {
                    {"day_count", std::to_string(m_context.day_count)},
                    {"alive_list", alive_list},
                    {"player_id", std::to_string(player->get_id())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*player, "night_seer_instruction", instruction);
            int target_id = parse_target_from_reply(result.raw_reply);
            Player* target = m_context.find_player(target_id);
            std::string identity = (target != nullptr && RuleEngine::is_wolf(target->get_role())) ? "狼人" : "好人";
            m_event_bus.append_private_record(
                *player,
                "第" + std::to_string(m_context.day_count) + "晚查验了 " + std::to_string(target_id) + "号，结果是：【" + identity + "】。"
            );
        }
    }

    for (Player* player : m_context.alive_players()) {
        if (player->get_role() == Role::GUARD) {
            const std::string last_guard_target = m_context.last_guard_target == -1
                ? "无"
                : std::to_string(m_context.last_guard_target);
            const std::string instruction = m_ai.render_named_template(
                "night_guard_instruction",
                {
                    {"day_count", std::to_string(m_context.day_count)},
                    {"alive_list", alive_list},
                    {"last_guard_target", last_guard_target},
                    {"player_id", std::to_string(player->get_id())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*player, "night_guard_instruction", instruction);
            int target = parse_target_from_reply(result.raw_reply);
            if (target == m_context.last_guard_target && target != -1) {
                m_context.guard_target = -1;
            } else {
                m_context.guard_target = target;
            }
            m_event_bus.append_private_record(
                *player,
                "第" + std::to_string(m_context.day_count) + "晚守护了: " + std::to_string(m_context.guard_target) + "号"
            );
        }
    }

    for (Player* player : m_context.alive_players()) {
        if (player->get_role() == Role::WITCH) {
            const std::string night_situation = m_context.dead_last_night == -1
                ? "今晚是平安夜，狼人没有杀人。"
                : "今晚狼人杀害了 " + std::to_string(m_context.dead_last_night) + " 号玩家。";
            std::string save_status = m_context.witch_save_used ? "不可用(已用)" : "可用";
            if (m_context.dead_last_night == player->get_id()) {
                save_status = "不可用(首夜女巫不可自救！)";
            }
            const std::string poison_status = m_context.witch_poison_used ? "不可用(已用)" : "可用";
            const std::string instruction = m_ai.render_named_template(
                "night_witch_instruction",
                {
                    {"day_count", std::to_string(m_context.day_count)},
                    {"alive_list", alive_list},
                    {"night_situation", night_situation},
                    {"save_status", save_status},
                    {"poison_status", poison_status},
                    {"player_id", std::to_string(player->get_id())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*player, "night_witch_instruction", instruction);
            try {
                json parsed = json::parse(clean_response(result.raw_reply));
                const bool use_save = parsed["data"]["save"];
                const int use_poison = parsed["data"]["poison"];

                if (use_save && !m_context.witch_save_used && m_context.dead_last_night != -1 && m_context.dead_last_night != player->get_id()) {
                    m_context.witch_save_used = true;
                    witch_used_save_tonight = true;
                    m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚使用了解药。");
                } else if (use_poison != -1 && !m_context.witch_poison_used && !use_save) {
                    m_context.witch_poison_used = true;
                    poison_target = use_poison;
                    m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚使用毒药毒了 " + std::to_string(poison_target) + "号。");
                } else {
                    m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚什么也没做。");
                }
            } catch (...) {
                m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚操作解析失败，视为无动作。");
            }
        }
    }

    NightDeathResolution resolution = RuleEngine::resolve_night_deaths(
        m_context,
        m_context.dead_last_night,
        poison_target,
        m_context.guard_target,
        witch_used_save_tonight
    );
    m_context.deaths_last_night = resolution.deaths;
    m_context.last_guard_target = m_context.guard_target;

    for (int dead_id : m_context.deaths_last_night) {
        Player* player = m_context.find_player(dead_id);
        if (player == nullptr) {
            continue;
        }
        player->kill();
        handle_sheriff_death(dead_id);
        if (player->get_role() == Role::HUNTER && dead_id != poison_target) {
            handle_hunter_shoot(dead_id);
        }
    }

    if (m_context.day_count == 1) {
        m_context.state = GameState::SHERIFF_ELECTION_PHASE;
    } else {
        m_context.state = GameState::DAY_SPEAK_PHASE;
    }
}

void Judge::run_day_speak() {
    std::cout << "\n========== 第 " << m_context.day_count << " 天 白天 ==========" << std::endl;

    const std::string death_message = build_death_message(m_context.deaths_last_night);
    m_event_bus.broadcast("系统", death_message);
    if (handle_win_if_needed()) {
        return;
    }

    Player* alive_wwk = nullptr;
    for (Player* player : m_context.alive_players_with_roles({Role::WHITE_WOLF_KING})) {
        alive_wwk = player;
        break;
    }

    if (m_context.day_count == 1 && m_context.enable_first_night_last_words) {
        for (int dead_id : m_context.deaths_last_night) {
            Player* dead_player = m_context.find_player(dead_id);
            if (dead_player == nullptr) {
                continue;
            }

            const std::string instruction = m_ai.render_named_template(
                "day_last_words_instruction",
                {
                    {"player_id", std::to_string(dead_player->get_id())},
                    {"role_name", role_to_string(dead_player->get_role())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*dead_player, "day_last_words_instruction", instruction);
            try {
                std::string speech = parse_speech_from_reply(result.raw_reply);

                if (alive_wwk && alive_wwk->get_id() != dead_player->get_id()) {
                    const std::string interrupt_instruction = m_ai.render_named_template(
                        "wwk_interrupt_instruction",
                        {
                            {"speaker_id", std::to_string(dead_player->get_id())},
                            {"speech_content", speech}
                        }
                    );
                    AIOrchestrator::AIResult interrupt_result = m_ai.ask_player(*alive_wwk, "wwk_interrupt_instruction", interrupt_instruction);
                    try {
                        json interrupt = json::parse(clean_response(interrupt_result.raw_reply));
                        if (interrupt.value("explode", false)) {
                            const int target_id = interrupt.value("target", -1);
                            const std::string after_word = interrupt.value("after_word", "");
                            if (!after_word.empty()) {
                                size_t pos = speech.find(after_word);
                                if (pos != std::string::npos) {
                                    speech = speech.substr(0, pos + after_word.length()) + "......（话音未落，只听“砰”的一声巨响！）";
                                }
                            }
                            m_event_bus.broadcast(std::to_string(dead_player->get_id()) + "号(遗言)", speech);
                            m_event_bus.broadcast("系统", "白狼王 " + std::to_string(alive_wwk->get_id()) + " 号在遗言环节自爆，带走了 " + std::to_string(target_id) + " 号！");
                            alive_wwk->kill();
                            handle_sheriff_death(alive_wwk->get_id());
                            if (Player* target = m_context.find_player(target_id)) {
                                target->kill();
                                handle_sheriff_death(target_id);
                                if (target->get_role() == Role::HUNTER) {
                                    handle_hunter_shoot(target_id);
                                }
                            }
                            if (handle_win_if_needed()) {
                                return;
                            }
                            m_context.day_count++;
                            m_context.state = GameState::NIGHT_PHASE;
                            return;
                        }
                    } catch (...) {
                    }
                }

                m_event_bus.broadcast(std::to_string(dead_player->get_id()) + "号(遗言)", speech);
            } catch (...) {
                m_event_bus.append_public_record("系统", std::to_string(dead_player->get_id()) + "号遗言解析失败。");
            }
        }
    }

    std::vector<int> speak_order;
    const int player_count = m_context.player_count();
    Player* sheriff = m_context.find_player(m_context.sheriff_id);
    if (sheriff != nullptr && !sheriff->is_alive()) {
        sheriff = nullptr;
    }

    if (sheriff != nullptr) {
        std::vector<int> forward_seq;
        std::string forward_order;
        for (int i = 1; i < player_count; ++i) {
            int pid = (m_context.sheriff_id - 1 + i) % player_count + 1;
            forward_seq.push_back(pid);
            if (!forward_order.empty()) {
                forward_order += " ";
            }
            forward_order += std::to_string(pid);
        }
        forward_seq.push_back(m_context.sheriff_id);

        std::vector<int> backward_seq;
        std::string backward_order;
        for (int i = 1; i < player_count; ++i) {
            int pid = (m_context.sheriff_id - 1 - i + player_count) % player_count + 1;
            backward_seq.push_back(pid);
            if (!backward_order.empty()) {
                backward_order += " ";
            }
            backward_order += std::to_string(pid);
        }
        backward_seq.push_back(m_context.sheriff_id);

        const std::string instruction = m_ai.render_named_template(
            "sheriff_decide_order_instruction",
            {
                {"day_count", std::to_string(m_context.day_count)},
                {"death_message", death_message},
                {"forward_order", forward_order},
                {"backward_order", backward_order},
                {"player_id", std::to_string(sheriff->get_id())},
                {"role_name", role_to_string(sheriff->get_role())}
            }
        );
        AIOrchestrator::AIResult result = m_ai.ask_player(*sheriff, "sheriff_decide_order_instruction", instruction);
        bool is_forward = true;
        try {
            json parsed = json::parse(clean_response(result.raw_reply));
            is_forward = parsed["data"]["order_type"] != "backward";
        } catch (...) {
        }
        speak_order = is_forward ? forward_seq : backward_seq;
    } else {
        int start_index = 0;
        if (!m_context.deaths_last_night.empty()) {
            start_index = *std::min_element(m_context.deaths_last_night.begin(), m_context.deaths_last_night.end()) % player_count;
        }
        for (int i = 0; i < player_count; ++i) {
            speak_order.push_back((start_index + i) % player_count + 1);
        }
    }

    for (int player_id : speak_order) {
        Player* player = m_context.find_player(player_id);
        if (player == nullptr || !player->is_alive()) {
            continue;
        }

        const std::string instruction = m_ai.render_named_template(
            "day_speak_instruction",
            {
                {"day_count", std::to_string(m_context.day_count)},
                {"death_message", death_message},
                {"player_id", std::to_string(player->get_id())},
                {"role_name", role_to_string(player->get_role())}
            }
        );
        AIOrchestrator::AIResult result = m_ai.ask_player(*player, "day_speak_instruction", instruction);
        try {
            std::string speech = parse_speech_from_reply(result.raw_reply);

            if (alive_wwk && alive_wwk->get_id() != player->get_id()) {
                const std::string interrupt_instruction = m_ai.render_named_template(
                    "wwk_interrupt_instruction",
                    {
                        {"speaker_id", std::to_string(player->get_id())},
                        {"speech_content", speech}
                    }
                );
                AIOrchestrator::AIResult interrupt_result = m_ai.ask_player(*alive_wwk, "wwk_interrupt_instruction", interrupt_instruction);
                try {
                    json interrupt = json::parse(clean_response(interrupt_result.raw_reply));
                    if (interrupt.value("explode", false)) {
                        const int target_id = interrupt.value("target", -1);
                        m_event_bus.broadcast(std::to_string(player->get_id()) + "号", speech + "......（发言被自爆打断！）");
                        m_event_bus.broadcast("系统", "白狼王 " + std::to_string(alive_wwk->get_id()) + " 号强行自爆，带走了 " + std::to_string(target_id) + " 号！");
                        alive_wwk->kill();
                        handle_sheriff_death(alive_wwk->get_id());
                        if (Player* target = m_context.find_player(target_id)) {
                            target->kill();
                            handle_sheriff_death(target_id);
                        }
                        if (handle_win_if_needed()) {
                            return;
                        }
                        m_context.day_count++;
                        m_context.state = GameState::NIGHT_PHASE;
                        return;
                    }
                } catch (...) {
                }
            }

            m_event_bus.broadcast(std::to_string(player->get_id()) + "号", speech);
        } catch (...) {
            m_event_bus.append_public_record("系统", std::to_string(player->get_id()) + "号玩家发言解析失败。");
        }
    }

    m_context.state = GameState::DAY_VOTE_PHASE;
}

void Judge::run_vote() {
    std::cout << "\n========== 投票阶段 ==========" << std::endl;

    const std::vector<Player*> alive_players = m_context.alive_players();
    const std::string alive_list = join_player_ids(alive_players);
    std::vector<int> vote_counts(m_context.player_count() + 1, 0);
    std::map<int, std::vector<int>> round1_records;

    const auto round1_results = m_ai.ask_all_players_concurrently(
        alive_players,
        "day_vote_instruction",
        [this, &alive_list](Player& player) {
            return m_ai.render_named_template(
                "day_vote_instruction",
                {
                    {"alive_list", alive_list},
                    {"player_id", std::to_string(player.get_id())},
                    {"role_name", role_to_string(player.get_role())}
                }
            );
        }
    );

    for (const auto& result : round1_results) {
        Player* voter = m_context.find_player(result.player_id);
        if (voter == nullptr || !voter->is_alive()) {
            continue;
        }

        const int target = parse_target_from_reply(result.raw_reply);
        const bool valid_target = target >= 1 && target <= m_context.player_count() && m_context.is_player_alive(target);
        if (valid_target) {
            vote_counts[target]++;
            round1_records[target].push_back(voter->get_id());
        } else {
            round1_records[-1].push_back(voter->get_id());
        }
    }

    const std::string round1_details = format_vote_records(round1_records);
    VoteResolution round1_resolution = RuleEngine::resolve_top_votes(vote_counts, 1, m_context.player_count());
    round1_resolution = RuleEngine::apply_sheriff_tiebreak(round1_resolution, m_context.sheriff_id, round1_records, m_context);

    int exiled_id = -1;
    if (round1_resolution.leaders.empty()) {
        m_event_bus.broadcast("系统", "无人有效投票，平安日。");
    } else if (round1_resolution.leaders.size() == 1) {
        exiled_id = round1_resolution.leaders.front();
    } else {
        const std::string pk_names = join_ids(round1_resolution.leaders);
        m_event_bus.broadcast("系统", "出现平票！PK台候选人: " + pk_names + "。\n第一轮详细票型: " + round1_details);

        for (int candidate_id : round1_resolution.leaders) {
            Player* candidate = m_context.find_player(candidate_id);
            if (candidate == nullptr) {
                continue;
            }
            std::vector<int> rivals;
            for (int other_id : round1_resolution.leaders) {
                if (other_id != candidate_id) {
                    rivals.push_back(other_id);
                }
            }
            const std::string instruction = m_ai.render_named_template(
                "day_pk_speech_instruction",
                {
                    {"rivals", join_ids(rivals)},
                    {"vote_history_details", round1_details},
                    {"player_id", std::to_string(candidate->get_id())},
                    {"role_name", role_to_string(candidate->get_role())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*candidate, "day_pk_speech_instruction", instruction);
            try {
                m_event_bus.broadcast(std::to_string(candidate->get_id()) + "号[PK发言]", parse_speech_from_reply(result.raw_reply));
            } catch (...) {
                m_event_bus.append_public_record("系统", std::to_string(candidate->get_id()) + "号PK发言解析失败。");
            }
        }

        std::vector<Player*> pk_voters;
        for (Player* player : m_context.alive_players()) {
            if (std::find(round1_resolution.leaders.begin(), round1_resolution.leaders.end(), player->get_id()) == round1_resolution.leaders.end()) {
                pk_voters.push_back(player);
            }
        }
        if (pk_voters.empty()) {
            pk_voters = m_context.alive_players();
        }

        std::fill(vote_counts.begin(), vote_counts.end(), 0);
        std::map<int, std::vector<int>> round2_records;
        const auto round2_results = m_ai.ask_all_players_concurrently(
            pk_voters,
            "day_pk_vote_instruction",
            [this, &pk_names, &round1_details](Player& player) {
                return m_ai.render_named_template(
                    "day_pk_vote_instruction",
                    {
                        {"pk_names", pk_names},
                        {"vote_history_details", round1_details},
                        {"player_id", std::to_string(player.get_id())},
                        {"role_name", role_to_string(player.get_role())}
                    }
                );
            }
        );

        for (const auto& result : round2_results) {
            Player* voter = m_context.find_player(result.player_id);
            if (voter == nullptr || !voter->is_alive()) {
                continue;
            }

            const int target = parse_target_from_reply(result.raw_reply);
            const bool valid_target = std::find(round1_resolution.leaders.begin(), round1_resolution.leaders.end(), target) != round1_resolution.leaders.end();
            if (valid_target) {
                vote_counts[target]++;
                round2_records[target].push_back(voter->get_id());
            } else {
                round2_records[-1].push_back(voter->get_id());
            }
        }

        const std::string round2_details = format_vote_records(round2_records);
        VoteResolution round2_resolution = RuleEngine::resolve_top_votes(vote_counts, 1, m_context.player_count());
        round2_resolution = RuleEngine::apply_sheriff_tiebreak(round2_resolution, m_context.sheriff_id, round2_records, m_context);

        std::vector<int> filtered;
        for (int id : round2_resolution.leaders) {
            if (std::find(round1_resolution.leaders.begin(), round1_resolution.leaders.end(), id) != round1_resolution.leaders.end()) {
                filtered.push_back(id);
            }
        }
        round2_resolution.leaders = filtered;

        if (round2_resolution.leaders.size() == 1) {
            exiled_id = round2_resolution.leaders.front();
            m_event_bus.broadcast("系统", "PK结果：放逐 " + std::to_string(exiled_id) + " 号。\n详情: " + round2_details);
        } else {
            m_event_bus.broadcast("系统", "PK台再次平票或全员弃票，本轮流局，无人出局！\n详情: " + round2_details);
        }
    }

    if (exiled_id != -1) {
        m_event_bus.broadcast("系统", std::to_string(exiled_id) + "号玩家在表决中被投票出局");
        Player* exiled = m_context.find_player(exiled_id);
        if (exiled != nullptr) {
            exiled->kill();
            handle_sheriff_death(exiled_id);
            if (exiled->get_role() == Role::HUNTER) {
                handle_hunter_shoot(exiled_id);
            }
        }

        if (handle_win_if_needed()) {
            return;
        }

        if (Player* dead_player = m_context.find_player(exiled_id)) {
            const std::string instruction = m_ai.render_named_template(
                "day_last_words_instruction",
                {
                    {"player_id", std::to_string(dead_player->get_id())},
                    {"role_name", role_to_string(dead_player->get_role())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*dead_player, "day_last_words_instruction", instruction);
            try {
                m_event_bus.broadcast(std::to_string(exiled_id) + "号(遗言)", parse_speech_from_reply(result.raw_reply));
            } catch (...) {
                m_event_bus.append_public_record("系统", std::to_string(exiled_id) + "号遗言解析失败。");
            }
        }
    }

    if (handle_win_if_needed()) {
        return;
    }

    m_context.day_count++;
    m_context.state = GameState::NIGHT_PHASE;
}

void Judge::run_sheriff_election() {
    std::cout << "\n========== 第 1 天 警长竞选 ==========" << std::endl;
    m_event_bus.broadcast("系统", "警长竞选开始，正在等待玩家决定是否上警...");

    std::vector<Player*> all_players;
    all_players.reserve(m_context.players.size());
    for (auto& player : m_context.players) {
        all_players.push_back(player.get());
    }

    Player* alive_wwk = nullptr;
    for (Player* player : m_context.alive_players_with_roles({Role::WHITE_WOLF_KING})) {
        alive_wwk = player;
        break;
    }

    std::vector<int> candidates;
    std::map<int, std::string> election_speeches;
    const auto run_results = m_ai.ask_all_players_concurrently(
        all_players,
        "sheriff_run_instruction",
        [this](Player& player) {
            return m_ai.render_named_template(
                "sheriff_run_instruction",
                {
                    {"history_text", "（并发竞选阶段，无前置位发言）"},
                    {"player_id", std::to_string(player.get_id())},
                    {"role_name", role_to_string(player.get_role())}
                }
            );
        }
    );

    for (const auto& result : run_results) {
        Player* player = m_context.find_player(result.player_id);
        if (player == nullptr) {
            continue;
        }
        try {
            json parsed = json::parse(clean_response(result.raw_reply));
            if (!parsed["data"]["run_for_sheriff"].get<bool>()) {
                continue;
            }

            std::string speech = parsed["data"]["speech"];
            if (alive_wwk && alive_wwk->get_id() != player->get_id()) {
                const std::string interrupt_instruction = m_ai.render_named_template(
                    "wwk_interrupt_instruction",
                    {
                        {"speaker_id", std::to_string(player->get_id())},
                        {"speech_content", speech}
                    }
                );
                AIOrchestrator::AIResult interrupt_result = m_ai.ask_player(*alive_wwk, "wwk_interrupt_instruction", interrupt_instruction);
                try {
                    json interrupt = json::parse(clean_response(interrupt_result.raw_reply));
                    if (interrupt.value("explode", false)) {
                        const int target_id = interrupt.value("target", -1);
                        m_event_bus.broadcast(std::to_string(player->get_id()) + "号(警上)", speech + "......（发言被自爆打断！）");
                        m_event_bus.broadcast("系统", "白狼王 " + std::to_string(alive_wwk->get_id()) + " 号在警上自爆，并带走了 " + std::to_string(target_id) + " 号！");
                        alive_wwk->kill();
                        if (Player* target = m_context.find_player(target_id)) {
                            target->kill();
                        }
                        m_context.sheriff_id = -1;
                        if (handle_win_if_needed()) {
                            return;
                        }
                        m_context.day_count++;
                        m_context.state = GameState::NIGHT_PHASE;
                        return;
                    }
                } catch (...) {
                }
            }

            candidates.push_back(player->get_id());
            election_speeches[player->get_id()] = speech;
        } catch (...) {
        }
    }

    if (candidates.empty()) {
        m_context.sheriff_id = -1;
        m_event_bus.broadcast("系统", "无人参与警长竞选，本局无警徽。");
        m_context.state = GameState::DAY_SPEAK_PHASE;
        return;
    }

    if (candidates.size() == 1) {
        m_context.sheriff_id = candidates.front();
        m_event_bus.broadcast("系统", std::to_string(m_context.sheriff_id) + "号玩家自动当选警长。");
        m_context.state = GameState::DAY_SPEAK_PHASE;
        return;
    }

    const std::string candidate_list = join_ids(candidates);
    for (int candidate_id : candidates) {
        m_event_bus.broadcast(std::to_string(candidate_id) + "号(警上)", election_speeches[candidate_id]);
    }
    m_event_bus.broadcast("系统", "竞选者为: " + candidate_list + "。请警下玩家开始投票！");

    std::vector<Player*> voters;
    for (Player* player : m_context.alive_players()) {
        if (std::find(candidates.begin(), candidates.end(), player->get_id()) == candidates.end()) {
            voters.push_back(player);
        }
    }

    std::vector<int> vote_counts(m_context.player_count() + 1, 0);
    std::map<int, std::vector<int>> vote_records;
    const auto vote_results = m_ai.ask_all_players_concurrently(
        voters,
        "sheriff_vote_instruction",
        [this, &candidate_list](Player& player) {
            return m_ai.render_named_template(
                "sheriff_vote_instruction",
                {
                    {"candidate_list", candidate_list},
                    {"player_id", std::to_string(player.get_id())},
                    {"role_name", role_to_string(player.get_role())}
                }
            );
        }
    );

    for (const auto& result : vote_results) {
        const int target = parse_target_from_reply(result.raw_reply);
        if (std::find(candidates.begin(), candidates.end(), target) != candidates.end()) {
            vote_counts[target]++;
            vote_records[target].push_back(result.player_id);
        } else {
            vote_records[-1].push_back(result.player_id);
        }
    }

    VoteResolution resolution = RuleEngine::resolve_top_votes(vote_counts, 1, m_context.player_count());
    std::vector<int> filtered;
    for (int id : resolution.leaders) {
        if (std::find(candidates.begin(), candidates.end(), id) != candidates.end()) {
            filtered.push_back(id);
        }
    }
    resolution.leaders = filtered;

    if (resolution.leaders.size() == 1) {
        m_context.sheriff_id = resolution.leaders.front();
        m_event_bus.broadcast("系统", "警长投票结束。票型：" + format_vote_records(vote_records) + "\n恭喜 " + std::to_string(m_context.sheriff_id) + " 号当选警长！");
    } else if (resolution.leaders.empty()) {
        m_context.sheriff_id = -1;
        m_event_bus.broadcast("系统", "警长投票无人有效投票，警徽流失。");
    } else {
        const std::string pk_names = join_ids(resolution.leaders);
        m_event_bus.broadcast("系统", "警长竞选进入PK：" + pk_names + "。");

        for (int candidate_id : resolution.leaders) {
            Player* candidate = m_context.find_player(candidate_id);
            if (candidate == nullptr) {
                continue;
            }
            std::vector<int> rivals;
            for (int other_id : resolution.leaders) {
                if (other_id != candidate_id) {
                    rivals.push_back(other_id);
                }
            }
            const std::string instruction = m_ai.render_named_template(
                "sheriff_pk_speech_instruction",
                {
                    {"rivals", join_ids(rivals)},
                    {"vote_history_details", format_vote_records(vote_records)},
                    {"player_id", std::to_string(candidate->get_id())},
                    {"role_name", role_to_string(candidate->get_role())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*candidate, "sheriff_pk_speech_instruction", instruction);
            try {
                m_event_bus.broadcast(std::to_string(candidate->get_id()) + "号[警长PK拉票]", parse_speech_from_reply(result.raw_reply));
            } catch (...) {
                m_event_bus.append_public_record("系统", std::to_string(candidate->get_id()) + "号警长PK发言解析失败。");
            }
        }

        std::fill(vote_counts.begin(), vote_counts.end(), 0);
        std::map<int, std::vector<int>> round2_records;
        std::vector<Player*> pk_voters;
        for (Player* player : m_context.alive_players()) {
            if (std::find(resolution.leaders.begin(), resolution.leaders.end(), player->get_id()) == resolution.leaders.end()) {
                pk_voters.push_back(player);
            }
        }

        const auto pk_vote_results = m_ai.ask_all_players_concurrently(
            pk_voters,
            "sheriff_pk_vote_instruction",
            [this, &pk_names, &vote_records](Player& player) {
                return m_ai.render_named_template(
                    "sheriff_pk_vote_instruction",
                    {
                        {"pk_names", pk_names},
                        {"vote_history_details", format_vote_records(vote_records)},
                        {"player_id", std::to_string(player.get_id())},
                        {"role_name", role_to_string(player.get_role())}
                    }
                );
            }
        );

        for (const auto& result : pk_vote_results) {
            const int target = parse_target_from_reply(result.raw_reply);
            if (std::find(resolution.leaders.begin(), resolution.leaders.end(), target) != resolution.leaders.end()) {
                vote_counts[target]++;
                round2_records[target].push_back(result.player_id);
            } else {
                round2_records[-1].push_back(result.player_id);
            }
        }

        VoteResolution pk_resolution = RuleEngine::resolve_top_votes(vote_counts, 1, m_context.player_count());
        std::vector<int> final_candidates;
        for (int id : pk_resolution.leaders) {
            if (std::find(resolution.leaders.begin(), resolution.leaders.end(), id) != resolution.leaders.end()) {
                final_candidates.push_back(id);
            }
        }
        pk_resolution.leaders = final_candidates;

        if (pk_resolution.leaders.size() == 1) {
            m_context.sheriff_id = pk_resolution.leaders.front();
            m_event_bus.broadcast("系统", "警长PK结束，" + std::to_string(m_context.sheriff_id) + "号当选警长。");
        } else {
            m_context.sheriff_id = -1;
            m_event_bus.broadcast("系统", "警长PK再次平票，警徽流失。");
        }
    }

    if (m_context.sheriff_id != -1) {
        Player* sheriff = m_context.find_player(m_context.sheriff_id);
        if (sheriff != nullptr && !sheriff->is_alive()) {
            m_event_bus.broadcast("系统", "刚当选的警长昨晚已死亡，立即处理警徽移交。");
            handle_sheriff_death(m_context.sheriff_id);
        }
    }

    m_context.state = GameState::DAY_SPEAK_PHASE;
}

void Judge::handle_sheriff_death(int dead_id) {
    if (m_context.sheriff_id != dead_id) {
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
    const int target = parse_target_from_reply(result.raw_reply);

    if (target != -1 && m_context.is_player_alive(target)) {
        m_context.sheriff_id = target;
        m_event_bus.broadcast("系统", "警长 " + std::to_string(dead_id) + " 号将警徽移交给了 " + std::to_string(target) + " 号。");
    } else {
        m_context.sheriff_id = -1;
        m_event_bus.broadcast("系统", "警长 " + std::to_string(dead_id) + " 号撕毁了警徽，本局不再有警长。");
    }
}

void Judge::handle_hunter_shoot(int hunter_id) {
    Player* hunter = m_context.find_player(hunter_id);
    if (hunter == nullptr) {
        return;
    }

    const std::string instruction = m_ai.render_named_template(
        "hunter_shoot_instruction",
        {
            {"player_id", std::to_string(hunter->get_id())},
            {"alive_list", join_player_ids(m_context.alive_players())}
        }
    );
    AIOrchestrator::AIResult result = m_ai.ask_player(*hunter, "hunter_shoot_instruction", instruction);

    try {
        json parsed = json::parse(clean_response(result.raw_reply));
        if (!parsed["data"]["shoot"].get<bool>()) {
            return;
        }
        const int target_id = parsed["data"]["target"];
        if (!m_context.is_player_alive(target_id)) {
            return;
        }

        m_event_bus.broadcast("系统", "猎人 " + std::to_string(hunter_id) + " 号开枪带走了 " + std::to_string(target_id) + " 号！");
        if (Player* target = m_context.find_player(target_id)) {
            target->kill();
            handle_sheriff_death(target_id);
        }
    } catch (...) {
    }
}

void Judge::start_game() {
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
}
