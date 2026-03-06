#include "phases/NightPhase.h"
#include <iostream>
#include "log.h"
#include "RuleEngine.h"
#include "support/PhaseUtils.h"
#include "support/PromptParsers.h"

NightPhase::NightPhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const NightPhaseCallbacks& callbacks)
    : m_context(context), m_event_bus(event_bus), m_ai(ai), m_callbacks(callbacks) {}

void NightPhase::execute() {
    std::cout << "\n========== 第 " << m_context.day_count << " 天 黑夜降临 ==========" << std::endl;
    LOG_INFO("[GAME][PHASE][NIGHT] day=%d begin", m_context.day_count);
    m_context.dead_last_night = -1;
    m_context.guard_target = -1;
    m_context.last_night_poison_target = -1;

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
                const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
                int target = parse_target_from_reply(result.raw_reply, 0);
                if (target < 0 || target > m_context.player_count()) {
                    target = 0;
                }
                LOG_INFO("[GAME][P%d][THOUGHT][wolf_night] %s", wolf->get_id(), thought.c_str());
                LOG_INFO("[GAME][P%d][DECISION][wolf_night] day=%d round=%d target=%d", wolf->get_id(), m_context.day_count, round, target);
                wolf_votes[target]++;
                current_round_actions += "【" + std::to_string(wolf->get_id()) + "号狼人指认了: " + std::to_string(target) + "号】\n";
                m_event_bus.append_private_record(*wolf, "第" + std::to_string(m_context.day_count) + "晚第" + std::to_string(round) + "轮夜间指认: " + std::to_string(target) + "号");
            }

            VoteResolution vote_resolution = RuleEngine::resolve_top_votes(wolf_votes, 0, m_context.player_count());
            LOG_INFO("[GAME][NIGHT][WOLF_ROUND] day=%d round=%d leaders=%s", m_context.day_count, round, join_ids(vote_resolution.leaders).c_str());
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

        LOG_INFO("[GAME][NIGHT][WOLF_FINAL] day=%d target=%d consensus=%s", m_context.day_count, m_context.dead_last_night, consensus_reached ? "true" : "false");
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
            const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
            int target_id = parse_target_from_reply(result.raw_reply);
            Player* target = m_context.find_player(target_id);
            std::string identity = (target != nullptr && RuleEngine::is_wolf(target->get_role())) ? "狼人" : "好人";
            LOG_INFO("[GAME][P%d][THOUGHT][seer] %s", player->get_id(), thought.c_str());
            LOG_INFO("[GAME][P%d][DECISION][seer] day=%d target=%d result=%s", player->get_id(), m_context.day_count, target_id, identity.c_str());
            m_event_bus.append_private_record(
                *player,
                "第" + std::to_string(m_context.day_count) + "晚思考：" + thought
            );
            m_event_bus.append_private_record(
                *player,
                "第" + std::to_string(m_context.day_count) + "晚查验决定：查验了 " + std::to_string(target_id) + "号，结果是：【" + identity + "】。"
            );
        }
    }

    for (Player* player : m_context.alive_players()) {
        if (player->get_role() == Role::GUARD) {
            const std::string last_guard_target = m_context.last_guard_target == -1 ? "无" : std::to_string(m_context.last_guard_target);
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
            const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
            int target = parse_target_from_reply(result.raw_reply);
            if (target == m_context.last_guard_target && target != -1) {
                m_context.guard_target = -1;
            } else {
                m_context.guard_target = target;
            }
            LOG_INFO("[GAME][P%d][THOUGHT][guard] %s", player->get_id(), thought.c_str());
            LOG_INFO("[GAME][P%d][DECISION][guard] day=%d target=%d", player->get_id(), m_context.day_count, m_context.guard_target);
            m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚思考：" + thought);
            if (m_context.guard_target == -1) {
                m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚守护决定：空守/守护失败。");
            } else {
                m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚守护决定：守护了 " + std::to_string(m_context.guard_target) + "号。");
            }
        }
    }

    for (Player* player : m_context.alive_players()) {
        if (player->get_role() == Role::WITCH) {
            const std::string night_situation = m_context.witch_save_used
                ? "你已使用解药，无法看到今晚狼人杀害的目标。"
                : (m_context.dead_last_night == -1
                    ? "今晚是平安夜，狼人没有杀人。（不是守卫守对了，而是狼人没有杀人）"
                    : "今晚狼人杀害了 " + std::to_string(m_context.dead_last_night) + " 号玩家。");
            std::string save_status = m_context.witch_save_used ? "不可用(已用)" : "可用";
            if (m_context.dead_last_night == player->get_id()) {
                if (m_context.day_count == 1) {
                    save_status = "可用（首夜可自救）";
                } else {
                    save_status = "不可用（非首夜不可自救）";
                }
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
            const std::string wolf_target_info = m_context.witch_save_used
                ? "第" + std::to_string(m_context.day_count) + "晚夜情信息：你已使用解药，无法看到今晚狼人杀害的目标。"
                : (m_context.dead_last_night == -1
                    ? "第" + std::to_string(m_context.day_count) + "晚夜情信息：今晚狼人空刀（狼人选择不杀人）。"
                    : "第" + std::to_string(m_context.day_count) + "晚夜情信息：今晚狼人击杀的目标是 " + std::to_string(m_context.dead_last_night) + "号。");
            m_event_bus.append_private_record(*player, wolf_target_info);

            bool resolved = false;
            for (int attempt = 1; attempt <= 2 && !resolved; ++attempt) {
                AIOrchestrator::AIResult result = m_ai.ask_player(*player, "night_witch_instruction", instruction);
                try {
                    json parsed = json::parse(clean_response(result.raw_reply));
                    const std::string thought = parsed.contains("thought") ? parsed["thought"].get<std::string>() : "（未提供思考）";
                    const bool use_save = parsed["data"]["save"];
                    const int use_poison = parsed["data"]["poison"];

                    LOG_INFO("[GAME][P%d][THOUGHT][guard] %s", player->get_id(), thought.c_str());
            LOG_INFO("[GAME][P%d][DECISION][guard] day=%d target=%d", player->get_id(), m_context.day_count, m_context.guard_target);
            m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚思考：" + thought);

                    if (use_save && use_poison != -1) {
                        if (attempt == 1) {
                            LOG_WARN("[GAME][P%d][DECISION][witch] day=%d invalid=double_potion retry=true", player->get_id(), m_context.day_count);
                            m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚操作非法：同一晚同时声明使用了解药和毒药，系统要求重新决策一次。");
                            continue;
                        }
                        m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚操作仍非法：再次同时声明使用两瓶药，视为无动作。");
                    } else if (use_save && !m_context.witch_save_used && m_context.dead_last_night != -1 && m_context.dead_last_night != player->get_id()) {
                        LOG_INFO("[GAME][P%d][DECISION][witch] day=%d action=save target=%d", player->get_id(), m_context.day_count, m_context.dead_last_night);
                        m_context.witch_save_used = true;
                        witch_used_save_tonight = true;
                        m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚行动决定：使用了解药，目标是 " + std::to_string(m_context.dead_last_night) + "号。");
                        m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚使用了解药。");
                        resolved = true;
                        break;
                    } else if (use_poison != -1 && !m_context.witch_poison_used) {
                        LOG_INFO("[GAME][P%d][DECISION][witch] day=%d action=poison target=%d", player->get_id(), m_context.day_count, use_poison);
                        m_context.witch_poison_used = true;
                        poison_target = use_poison;
                        m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚行动决定：使用毒药，目标是 " + std::to_string(poison_target) + "号。");
                        m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚使用毒药毒了 " + std::to_string(poison_target) + "号。");
                        resolved = true;
                        break;
                    }

                    LOG_INFO("[GAME][P%d][DECISION][witch] day=%d action=none", player->get_id(), m_context.day_count);
                    m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚行动决定：什么也没做。");
                    m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚什么也没做。");
                    resolved = true;
                } catch (...) {
                    if (attempt == 2) {
                        LOG_WARN("[GAME][P%d][DECISION][witch] day=%d parse_failed default=none", player->get_id(), m_context.day_count);
                        m_event_bus.append_private_record(*player, "第" + std::to_string(m_context.day_count) + "晚操作解析失败，视为无动作。");
                        resolved = true;
                    }
                }
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
    LOG_INFO("[GAME][NIGHT][RESOLUTION] day=%d wolf_target=%d poison_target=%d guard_target=%d save_used=%s deaths=%s", m_context.day_count, m_context.dead_last_night, poison_target, m_context.guard_target, witch_used_save_tonight ? "true" : "false", join_ids(resolution.deaths).c_str());
    m_context.deaths_last_night = resolution.deaths;
    m_context.last_guard_target = m_context.guard_target;
    m_context.last_night_poison_target = poison_target;

    for (int dead_id : m_context.deaths_last_night) {
        const std::string cause = dead_id == poison_target ? "poison" : "wolf";
        const bool allow_hunter = cause != "poison";
        m_callbacks.publish_player_death(dead_id, cause, allow_hunter);
    }

    if (m_context.day_count == 1) {
        m_context.state = GameState::SHERIFF_ELECTION_PHASE;
    } else {
        m_context.state = GameState::DAY_SPEAK_PHASE;
    }
}
