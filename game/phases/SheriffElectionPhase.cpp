#include "phases/SheriffElectionPhase.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <utility>
#include "log.h"
#include "RuleEngine.h"
#include "support/PhaseUtils.h"
#include "support/PromptParsers.h"

SheriffElectionPhase::SheriffElectionPhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const SheriffElectionPhaseCallbacks& callbacks)
    : m_context(context), m_event_bus(event_bus), m_ai(ai), m_callbacks(callbacks) {}

void SheriffElectionPhase::execute() {
    std::cout << "\n========== 第 1 天 警长竞选 ==========" << std::endl;
    LOG_INFO("[GAME][PHASE][SHERIFF_ELECTION] begin");
    const bool forward = is_single_forward_double_reverse();
    m_event_bus.broadcast(
        "系统",
        std::string("警长竞选开始，正在等待玩家决定是否上警。本次警上询问顺序按当前时间执行") +
        (forward ? "单顺。" : "双逆。")
    );

    std::vector<int> candidates;
    std::string current_election_history;
    std::vector<int> inquiry_order;
    inquiry_order.reserve(m_context.player_count());
    for (const auto& player_ptr : m_context.players) {
        inquiry_order.push_back(player_ptr->get_id());
    }
    inquiry_order = sort_ids_by_time_rule(std::move(inquiry_order));
    LOG_INFO("[GAME][SHERIFF][INQUIRY_ORDER] forward=%s order=%s", forward ? "true" : "false", join_ids(inquiry_order).c_str());

    for (int player_id : inquiry_order) {
        Player* player = m_context.find_player(player_id);
        if (player == nullptr) {
            continue;
        }

        const std::string history_text = current_election_history.empty()
            ? "（暂无前置位发言）"
            : current_election_history;

        const std::string instruction = m_ai.render_named_template(
            "sheriff_run_instruction",
            {
                {"history_text", history_text},
                {"player_id", std::to_string(player->get_id())},
                {"role_name", m_callbacks.role_to_string(player->get_role())}
            }
        );

        AIOrchestrator::AIResult result = m_ai.ask_player(*player, "sheriff_run_instruction", instruction);

        try {
            json parsed = json::parse(clean_response(result.raw_reply));
            const std::string thought = parsed.contains("thought") ? parsed["thought"].get<std::string>() : "（未提供思考）";
            const bool run_for_sheriff = parsed["data"]["run_for_sheriff"].get<bool>();
            LOG_INFO("[GAME][P%d][THOUGHT][sheriff_run] %s", player->get_id(), thought.c_str());

            if (run_for_sheriff) {
                const std::string speech = parsed["data"]["speech"];
                LOG_INFO("[GAME][P%d][DECISION][sheriff_run] run=true speech=%s", player->get_id(), speech.c_str());
                candidates.push_back(player->get_id());
                current_election_history += "【" + std::to_string(player->get_id()) + "号警上发言】: " + speech + "\n";

                Event speech_event;
                speech_event.type = EventType::BROADCAST;
                speech_event.name = "sheriff_speech";
                speech_event.actor_id = player->get_id();
                speech_event.speaker = std::to_string(player->get_id()) + "号(警上)";
                speech_event.message = speech;
                if (m_event_bus.publish(std::move(speech_event))) {
                    return;
                }
            } else {
                LOG_INFO("[GAME][P%d][DECISION][sheriff_run] run=false", player->get_id());
                current_election_history += "【" + std::to_string(player->get_id()) + "号玩家】: 选择不上警\n";
                m_event_bus.broadcast(std::to_string(player->get_id()) + "号", "选择不上警");
            }
        } catch (...) {
            LOG_WARN("[GAME][P%d][DECISION][sheriff_run] parse_failed default=not_run", player->get_id());
            current_election_history += "【" + std::to_string(player->get_id()) + "号玩家】: 决策解析失败，视为不上警\n";
            m_event_bus.broadcast(std::to_string(player->get_id()) + "号", "决策解析失败，视为不上警");
        }
    }

    if (!current_election_history.empty()) {
        m_event_bus.append_public_record("系统", "警长竞选完整记录:\n" + current_election_history);
    }

    if (candidates.empty()) {
        m_context.sheriff_id = -1;
        LOG_INFO("[GAME][SHERIFF][RESULT] sheriff_lost via=no_valid_vote");
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
                    {"role_name", m_callbacks.role_to_string(player.get_role())}
                }
            );
        }
    );

    for (const auto& result : vote_results) {
        const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
        const int target = parse_target_from_reply(result.raw_reply);
        LOG_INFO("[GAME][P%d][THOUGHT][sheriff_vote] %s", result.player_id, thought.c_str());
        LOG_INFO("[GAME][P%d][DECISION][sheriff_vote] target=%d", result.player_id, target);
        if (std::find(candidates.begin(), candidates.end(), target) != candidates.end()) {
            vote_counts[target]++;
            vote_records[target].push_back(result.player_id);
        } else {
            vote_records[-1].push_back(result.player_id);
        }
    }

    LOG_INFO("[GAME][SHERIFF][VOTE_ROUND1] details=%s", format_vote_records(vote_records).c_str());
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
        LOG_INFO("[GAME][SHERIFF][RESULT] sheriff=%d via=round1", m_context.sheriff_id);
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
                    {"role_name", m_callbacks.role_to_string(candidate->get_role())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*candidate, "sheriff_pk_speech_instruction", instruction);
            LOG_INFO("[GAME][P%d][THOUGHT][sheriff_pk_speech] %s", candidate->get_id(), parse_thought_from_reply(result.raw_reply, "（未提供思考）").c_str());
            try {
                Event speech_event;
                speech_event.type = EventType::BROADCAST;
                speech_event.name = "sheriff_pk_speech";
                speech_event.actor_id = candidate->get_id();
                speech_event.speaker = std::to_string(candidate->get_id()) + "号[警长PK拉票]";
                speech_event.message = parse_speech_from_reply(result.raw_reply);
                LOG_INFO("[GAME][P%d][DECISION][sheriff_pk_speech] speech=%s", candidate->get_id(), speech_event.message.c_str());
                if (m_event_bus.publish(std::move(speech_event))) {
                    return;
                }
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
                        {"role_name", m_callbacks.role_to_string(player.get_role())}
                    }
                );
            }
        );

        for (const auto& result : pk_vote_results) {
            const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
            const int target = parse_target_from_reply(result.raw_reply);
            LOG_INFO("[GAME][P%d][THOUGHT][sheriff_pk_vote] %s", result.player_id, thought.c_str());
            LOG_INFO("[GAME][P%d][DECISION][sheriff_pk_vote] target=%d", result.player_id, target);
            if (std::find(resolution.leaders.begin(), resolution.leaders.end(), target) != resolution.leaders.end()) {
                vote_counts[target]++;
                round2_records[target].push_back(result.player_id);
            } else {
                round2_records[-1].push_back(result.player_id);
            }
        }

        LOG_INFO("[GAME][SHERIFF][VOTE_PK] details=%s", format_vote_records(round2_records).c_str());
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
            LOG_INFO("[GAME][SHERIFF][RESULT] sheriff=%d via=pk", m_context.sheriff_id);
            m_event_bus.broadcast("系统", "警长PK结束，" + std::to_string(m_context.sheriff_id) + "号当选警长。");
        } else {
            m_context.sheriff_id = -1;
            LOG_INFO("[GAME][SHERIFF][RESULT] sheriff_lost via=pk_tie");
            m_event_bus.broadcast("系统", "警长PK再次平票，警徽流失。");
        }
    }

    if (m_context.sheriff_id != -1) {
        Player* sheriff = m_context.find_player(m_context.sheriff_id);
        if (sheriff != nullptr && !sheriff->is_alive()) {
            m_event_bus.broadcast("系统", "刚当选的警长昨晚已死亡，立即处理警徽移交。");
            m_callbacks.handle_sheriff_death(m_context.sheriff_id);
        }
    }

    m_context.state = GameState::DAY_SPEAK_PHASE;
}
