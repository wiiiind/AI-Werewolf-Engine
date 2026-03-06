#include "phases/DaySpeakPhase.h"
#include <algorithm>
#include <iostream>
#include "log.h"
#include "support/PhaseUtils.h"
#include "support/PromptParsers.h"

DaySpeakPhase::DaySpeakPhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const DaySpeakPhaseCallbacks& callbacks)
    : m_context(context), m_event_bus(event_bus), m_ai(ai), m_callbacks(callbacks) {}

void DaySpeakPhase::execute() {
    std::cout << "\n========== 第 " << m_context.day_count << " 天 白天 ==========" << std::endl;
    LOG_INFO("[GAME][PHASE][DAY_SPEAK] day=%d begin", m_context.day_count);

    const std::string death_message = build_death_message(m_context.deaths_last_night);
    LOG_INFO("[GAME][DAY][DEATH_MESSAGE] %s", death_message.c_str());
    m_event_bus.broadcast("系统", death_message);
    for (const auto& pending_message : m_context.pending_day_messages) {
        m_event_bus.broadcast("系统", pending_message);
    }
    m_context.pending_day_messages.clear();
    if (m_callbacks.handle_win_if_needed()) {
        return;
    }

    if (m_context.day_count == 1 && m_context.enable_first_night_last_words) {
        std::vector<int> last_words_order = sort_ids_by_time_rule(m_context.deaths_last_night);
        for (int dead_id : last_words_order) {
            Player* dead_player = m_context.find_player(dead_id);
            if (dead_player == nullptr) {
                continue;
            }

            const std::string instruction = m_ai.render_named_template(
                "day_last_words_instruction",
                {
                    {"death_context", describe_death_context(dead_id == m_context.last_night_poison_target ? "poison" : "wolf")},
                    {"player_id", std::to_string(dead_player->get_id())},
                    {"role_name", m_callbacks.role_to_string(dead_player->get_role())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*dead_player, "day_last_words_instruction", instruction);
            LOG_INFO("[GAME][P%d][THOUGHT][last_words] %s", dead_player->get_id(), parse_thought_from_reply(result.raw_reply, "（未提供思考）").c_str());
            try {
                Event speak_event;
                speak_event.type = EventType::BROADCAST;
                speak_event.name = "last_words";
                speak_event.actor_id = dead_player->get_id();
                speak_event.speaker = std::to_string(dead_player->get_id()) + "号(遗言)";
                speak_event.message = parse_speech_from_reply(result.raw_reply);
                LOG_INFO("[GAME][P%d][DECISION][last_words] speech=%s", dead_player->get_id(), speak_event.message.c_str());
                if (m_event_bus.publish(std::move(speak_event))) {
                    return;
                }
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
                {"role_name", m_callbacks.role_to_string(sheriff->get_role())}
            }
        );
        AIOrchestrator::AIResult result = m_ai.ask_player(*sheriff, "sheriff_decide_order_instruction", instruction);
        LOG_INFO("[GAME][P%d][THOUGHT][sheriff_order] %s", sheriff->get_id(), parse_thought_from_reply(result.raw_reply, "（未提供思考）").c_str());
        bool is_forward = true;
        try {
            json parsed = json::parse(clean_response(result.raw_reply));
            is_forward = parsed["data"]["order_type"] != "backward";
        } catch (...) {
        }
        speak_order = is_forward ? forward_seq : backward_seq;
        LOG_INFO("[GAME][P%d][DECISION][sheriff_order] order=%s", sheriff->get_id(), is_forward ? "forward" : "backward");
    } else {
        const bool forward = is_single_forward_double_reverse();
        int anchor_id = select_last_dead_anchor(m_context.deaths_last_night, m_context.last_night_poison_target, forward);
        if (anchor_id == -1) {
            anchor_id = forward ? player_count : 1;
        }
        speak_order = build_circular_order_from_anchor(anchor_id, player_count, forward);
        LOG_INFO("[GAME][DAY][ORDER_NO_SHERIFF] anchor=%d forward=%s order=%s", anchor_id, forward ? "true" : "false", join_ids(speak_order).c_str());
    }

    std::vector<int> alive_speak_order;
    for (int player_id : speak_order) {
        Player* player = m_context.find_player(player_id);
        if (player != nullptr && player->is_alive()) {
            alive_speak_order.push_back(player_id);
        }
    }
    const std::string alive_list = join_player_ids(m_context.alive_players());
    LOG_INFO("[GAME][DAY][ORDER] actual_alive_order=%s", join_ids(alive_speak_order).c_str());
    const std::string speak_order_text = join_ids(alive_speak_order);

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
                {"alive_list", alive_list},
                {"speak_order", speak_order_text},
                {"player_id", std::to_string(player->get_id())},
                {"role_name", m_callbacks.role_to_string(player->get_role())}
            }
        );
        AIOrchestrator::AIResult result = m_ai.ask_player(*player, "day_speak_instruction", instruction);
        LOG_INFO("[GAME][P%d][THOUGHT][day_speak] %s", player->get_id(), parse_thought_from_reply(result.raw_reply, "（未提供思考）").c_str());
        try {
            Event speak_event;
            speak_event.type = EventType::BROADCAST;
            speak_event.name = "player_speak";
            speak_event.actor_id = player->get_id();
            speak_event.speaker = std::to_string(player->get_id()) + "号";
            speak_event.message = parse_speech_from_reply(result.raw_reply);
            LOG_INFO("[GAME][P%d][DECISION][day_speak] speech=%s", player->get_id(), speak_event.message.c_str());
            if (m_event_bus.publish(std::move(speak_event))) {
                return;
            }
        } catch (...) {
            m_event_bus.append_public_record("系统", std::to_string(player->get_id()) + "号玩家发言解析失败。");
        }
    }

    m_context.state = GameState::DAY_VOTE_PHASE;
}
