#include "phases/SheriffElectionPhase.h"
#include <atomic>
#include <algorithm>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include "block_queue.h"
#include "log.h"
#include "RuleEngine.h"
#include "support/PhaseUtils.h"
#include "support/PromptParsers.h"

namespace {
using AskOptions = AIOrchestrator::AskOptions;
using RequestMode = AIOrchestrator::RequestMode;

std::string select_wwk_interrupt_prompt_name(const GameContext& context) {
    if (context.state == GameState::SHERIFF_ELECTION_PHASE) {
        return context.day_count == 1
            ? "wwk_interrupt_sheriff_day1_instruction"
            : "wwk_interrupt_sheriff_day2_instruction";
    }
    return "wwk_interrupt_other_day_instruction";
}

bool parse_bool_field(const std::string& raw_reply, const std::string& field, bool fallback = false) {
    try {
        json parsed = json::parse(clean_response(raw_reply));
        if (parsed.contains("data") && parsed["data"].contains(field)) {
            return parsed["data"][field].get<bool>();
        }
    } catch (...) {
    }
    return fallback;
}

struct SpeechCheckpoint {
    int speaker_id = -1;
    std::size_t history_size_before = 0;
};

struct ExplosionDecision {
    int white_wolf_king_id = -1;
    int trigger_speaker_id = -1;
    int target_id = -1;
};

void rollback_history_after_trigger(json& history, const std::vector<SpeechCheckpoint>& checkpoints, int trigger_speaker_id) {
    bool erase_next = false;
    std::size_t rollback_index = history.size();

    for (const SpeechCheckpoint& checkpoint : checkpoints) {
        if (erase_next) {
            rollback_index = checkpoint.history_size_before;
            break;
        }
        if (checkpoint.speaker_id == trigger_speaker_id) {
            erase_next = true;
        }
    }

    while (history.size() > rollback_index) {
        history.erase(history.end() - 1);
    }
}
}

SheriffElectionPhase::SheriffElectionPhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const SheriffElectionPhaseCallbacks& callbacks)
    : m_context(context), m_event_bus(event_bus), m_ai(ai), m_callbacks(callbacks) {}

void SheriffElectionPhase::publish_deferred_first_night_deaths() {
    if (!m_context.first_night_deaths_deferred) {
        return;
    }

    m_context.first_night_deaths_deferred = false;
    for (int dead_id : m_context.deaths_last_night) {
        const std::string cause = dead_id == m_context.last_night_poison_target ? "poison" : "wolf";
        const bool allow_hunter = cause != "poison";
        m_callbacks.publish_player_death(dead_id, cause, allow_hunter);
    }

    if (m_context.sheriff_id != -1 && !m_context.is_player_alive(m_context.sheriff_id)) {
        m_event_bus.broadcast("系统", "刚当选的警长属于首夜死者，立即处理警徽移交。");
        m_callbacks.handle_sheriff_death(m_context.sheriff_id);
    }
}

void SheriffElectionPhase::run_pending_first_night_last_words() {
    if (!m_context.first_night_last_words_pending || !m_context.enable_first_night_last_words) {
        return;
    }

    m_context.first_night_last_words_pending = false;
    std::vector<int> last_words_order = sort_ids_by_time_rule(m_context.first_night_last_words_ids);
    for (int dead_id : last_words_order) {
        Player* dead_player = m_context.find_player(dead_id);
        if (dead_player == nullptr) {
            continue;
        }

        const std::string instruction = m_ai.render_named_template(
            "day_last_words_instruction",
            {
                {"death_context", describe_death_context(dead_id == m_context.first_night_last_words_poison_target ? "poison" : "wolf")},
                {"player_id", std::to_string(dead_player->get_id())},
                {"role_name", m_callbacks.role_to_string(dead_player->get_role())}
            }
        );
        AIOrchestrator::AIResult result = m_ai.ask_player(*dead_player, "day_last_words_instruction", instruction);
        try {
            Event speak_event;
            speak_event.type = EventType::BROADCAST;
            speak_event.name = "last_words";
            speak_event.actor_id = dead_player->get_id();
            speak_event.speaker = std::to_string(dead_player->get_id()) + "号(遗言)";
            speak_event.message = parse_speech_from_reply(result.raw_reply);
            m_event_bus.publish(std::move(speak_event));
        } catch (...) {
            m_event_bus.append_public_record("系统", std::to_string(dead_player->get_id()) + "号遗言解析失败。");
        }
    }

    m_context.first_night_last_words_ids.clear();
    m_context.first_night_last_words_poison_target = -1;
}

std::vector<int> SheriffElectionPhase::collect_candidates() {
    std::vector<Player*> alive_players = m_context.alive_players();
    const auto register_results = m_ai.ask_all_players_concurrently(
        alive_players,
        "sheriff_register_instruction",
        [this](Player& player) {
            return m_ai.render_named_template(
                "sheriff_register_instruction",
                {
                    {"player_id", std::to_string(player.get_id())},
                    {"role_name", m_callbacks.role_to_string(player.get_role())}
                }
            );
        }
    );
    m_ai.print_batch_results("警长竞选并发上警", register_results);

    std::vector<int> candidates;
    candidates.reserve(register_results.size());
    for (const auto& result : register_results) {
        const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
        const bool run_for_sheriff = parse_bool_field(result.raw_reply, "run_for_sheriff", false);
        LOG_INFO("[GAME][P%d][THOUGHT][sheriff_register] %s", result.player_id, thought.c_str());
        LOG_INFO("[GAME][P%d][DECISION][sheriff_register] run=%s", result.player_id, run_for_sheriff ? "true" : "false");
        if (run_for_sheriff) {
            candidates.push_back(result.player_id);
        }
    }

    return candidates;
}

bool SheriffElectionPhase::run_candidate_speeches(const std::vector<int>& candidates, std::string& election_history) {
    const std::string candidate_list = join_ids(candidates);
    Player* white_wolf_king = nullptr;
    for (Player* player : m_context.alive_players_with_roles({Role::WHITE_WOLF_KING})) {
        white_wolf_king = player;
        break;
    }

    std::atomic<bool> explosion_triggered(false);
    std::mutex explosion_mutex;
    ExplosionDecision explosion_decision;
    std::vector<SpeechCheckpoint> speech_checkpoints;
    block_queue<Event> judgement_queue(std::max(8, static_cast<int>(candidates.size()) + 2));
    std::thread adjudication_thread;

    if (white_wolf_king != nullptr) {
        adjudication_thread = std::thread([this, &judgement_queue, &explosion_triggered, &explosion_mutex, &explosion_decision, white_wolf_king]() {
            const AskOptions silent_options{RequestMode::Sequential, false};
            while (true) {
                Event event;
                if (!judgement_queue.pop(event)) {
                    continue;
                }
                if (event.name == "__stop__") {
                    break;
                }
                if (explosion_triggered.load()) {
                    continue;
                }

                const std::string prompt_name = select_wwk_interrupt_prompt_name(m_context);
                const std::string instruction = m_ai.render_named_template(
                    prompt_name,
                    {
                        {"speaker_id", std::to_string(event.actor_id)},
                        {"speech_content", event.message}
                    }
                );
                AIOrchestrator::AIResult result = m_ai.ask_player(*white_wolf_king, prompt_name, instruction, silent_options);
                try {
                    json parsed = json::parse(clean_response(result.raw_reply));
                    if (!parsed.value("explode", false)) {
                        continue;
                    }

                    ExplosionDecision decision;
                    decision.white_wolf_king_id = white_wolf_king->get_id();
                    decision.trigger_speaker_id = event.actor_id;
                    decision.target_id = parsed.value("target", -1);
                    if (decision.target_id != -1 && !m_context.is_player_alive(decision.target_id)) {
                        decision.target_id = -1;
                    }

                    bool expected = false;
                    if (explosion_triggered.compare_exchange_strong(expected, true)) {
                        std::lock_guard<std::mutex> lock(explosion_mutex);
                        explosion_decision = decision;
                        break;
                    }
                } catch (...) {
                }
            }
        });
    }

    for (int candidate_id : candidates) {
        if (explosion_triggered.load()) {
            break;
        }
        Player* player = m_context.find_player(candidate_id);
        if (player == nullptr || !player->is_alive()) {
            continue;
        }

        const std::string history_text = election_history.empty()
            ? "（暂无前置位警上发言）"
            : election_history;
        const std::string instruction = m_ai.render_named_template(
            "sheriff_speech_instruction",
            {
                {"candidate_list", candidate_list},
                {"history_text", history_text},
                {"player_id", std::to_string(player->get_id())},
                {"role_name", m_callbacks.role_to_string(player->get_role())}
            }
        );

        AIOrchestrator::AIResult result = m_ai.ask_player(*player, "sheriff_speech_instruction", instruction);
        LOG_INFO("[GAME][P%d][THOUGHT][sheriff_speech] %s", player->get_id(), parse_thought_from_reply(result.raw_reply, "（未提供思考）").c_str());
        try {
            const std::string speech = parse_speech_from_reply(result.raw_reply);
            LOG_INFO("[GAME][P%d][DECISION][sheriff_speech] speech=%s", player->get_id(), speech.c_str());
            election_history += "【" + std::to_string(player->get_id()) + "号警上发言】: " + speech + "\n";
            const std::size_t history_size_before = m_context.global_history.size();

            Event speech_event;
            speech_event.type = EventType::BROADCAST;
            speech_event.name = "sheriff_speech";
            speech_event.actor_id = player->get_id();
            speech_event.speaker = std::to_string(player->get_id()) + "号(警上)";
            speech_event.message = speech;
            if (m_event_bus.publish(std::move(speech_event))) {
                if (adjudication_thread.joinable()) {
                    Event stop_event;
                    stop_event.name = "__stop__";
                    judgement_queue.push(stop_event);
                    adjudication_thread.join();
                }
                return false;
            }
            speech_checkpoints.push_back({player->get_id(), history_size_before});
            if (adjudication_thread.joinable()) {
                Event judgement_event;
                judgement_event.name = "judge_sheriff_speech";
                judgement_event.actor_id = player->get_id();
                judgement_event.message = speech;
                judgement_queue.push(judgement_event);
            }
        } catch (...) {
            LOG_WARN("[GAME][P%d][DECISION][sheriff_speech] parse_failed", player->get_id());
            election_history += "【" + std::to_string(player->get_id()) + "号警上发言】: 发言解析失败\n";
            m_event_bus.append_public_record("系统", std::to_string(player->get_id()) + "号警上发言解析失败。");
        }
    }

    if (adjudication_thread.joinable()) {
        Event stop_event;
        stop_event.name = "__stop__";
        judgement_queue.push(stop_event);
        adjudication_thread.join();
    }

    if (explosion_triggered.load()) {
        ExplosionDecision decision;
        {
            std::lock_guard<std::mutex> lock(explosion_mutex);
            decision = explosion_decision;
        }

        rollback_history_after_trigger(m_context.global_history, speech_checkpoints, decision.trigger_speaker_id);
        m_context.exploded_wwk_commander_id = decision.white_wolf_king_id;

        const bool postpone_election = (m_context.day_count == 1 && !m_context.sheriff_election_resume_explosion_loses_badge);
        if (postpone_election && m_context.last_night_poison_target == decision.white_wolf_king_id) {
            m_context.deaths_last_night.erase(
                std::remove(m_context.deaths_last_night.begin(), m_context.deaths_last_night.end(), decision.white_wolf_king_id),
                m_context.deaths_last_night.end()
            );
            m_context.last_night_poison_target = -1;
        }

        m_event_bus.broadcast(
            "系统",
            postpone_election
                ? ("白狼王 " + std::to_string(decision.white_wolf_king_id) +
                    " 号在警长竞选发言阶段自爆，带走了 " + std::to_string(decision.target_id) + " 号！本轮警长竞选延期到明天。")
                : ("白狼王 " + std::to_string(decision.white_wolf_king_id) +
                    " 号在警长竞选发言阶段自爆，带走了 " + std::to_string(decision.target_id) + " 号！警徽流失。")
        );
        m_callbacks.publish_player_death(decision.white_wolf_king_id, "explosion", false);
        if (decision.target_id != -1) {
            m_callbacks.publish_player_death(decision.target_id, "explosion", true);
        }
        if (Player* wwk = m_context.find_player(decision.white_wolf_king_id)) {
            const std::string last_words_instruction = m_ai.render_named_template(
                "wwk_last_words_instruction",
                {
                    {"player_id", std::to_string(wwk->get_id())},
                    {"role_name", m_callbacks.role_to_string(wwk->get_role())}
                }
            );
            AIOrchestrator::AIResult last_words_result = m_ai.ask_player(*wwk, "wwk_last_words_instruction", last_words_instruction);
            try {
                Event last_words_event;
                last_words_event.type = EventType::BROADCAST;
                last_words_event.name = "last_words";
                last_words_event.actor_id = wwk->get_id();
                last_words_event.speaker = std::to_string(wwk->get_id()) + "号(白狼王遗言)";
                last_words_event.message = parse_speech_from_reply(last_words_result.raw_reply);
                m_event_bus.publish(std::move(last_words_event));
            } catch (...) {
                m_event_bus.append_public_record("系统", std::to_string(wwk->get_id()) + "号白狼王遗言解析失败。");
            }
        }
        if (m_callbacks.handle_win_if_needed()) {
            return false;
        }

        if (postpone_election) {
            const std::string death_message = build_death_message(m_context.deaths_last_night);
            m_event_bus.broadcast("系统", death_message);
            for (const auto& pending_message : m_context.pending_day_messages) {
                m_event_bus.broadcast("系统", pending_message);
            }
            m_context.pending_day_messages.clear();
            publish_deferred_first_night_deaths();
            m_context.first_night_last_words_ids = m_context.deaths_last_night;
            m_context.first_night_last_words_poison_target = m_context.last_night_poison_target;
            m_context.first_night_last_words_pending = !m_context.first_night_last_words_ids.empty();
            run_pending_first_night_last_words();
            m_context.sheriff_election_resume_vote_only = true;
            m_context.sheriff_election_resume_explosion_loses_badge = true;
            m_context.sheriff_id = -1;
            m_event_bus.broadcast("系统", "白狼王自爆，首日警长竞选中止。首夜死者遗言结束后进入黑夜，明天早晨重新进行完整的警长竞选流程。");
        } else {
            m_context.sheriff_id = -1;
            m_event_bus.broadcast("系统", "白狼王自爆，警长竞选立即中止，直接进入黑夜。");
        }
        m_context.day_count++;
        m_context.state = GameState::NIGHT_PHASE;
        return false;
    }

    return true;
}

std::vector<int> SheriffElectionPhase::collect_withdrawals(const std::vector<int>& candidates, const std::string& election_history) {
    if (candidates.empty()) {
        return candidates;
    }

    std::vector<Player*> candidate_players;
    candidate_players.reserve(candidates.size());
    for (int candidate_id : candidates) {
        if (Player* player = m_context.find_player(candidate_id); player != nullptr && player->is_alive()) {
            candidate_players.push_back(player);
        }
    }

    const std::string candidate_list = join_ids(candidates);
    const auto withdraw_results = m_ai.ask_all_players_concurrently(
        candidate_players,
        "sheriff_withdraw_instruction",
        [this, &candidate_list, &election_history](Player& player) {
            return m_ai.render_named_template(
                "sheriff_withdraw_instruction",
                {
                    {"candidate_list", candidate_list},
                    {"history_text", election_history.empty() ? std::string("（暂无完整警上发言记录）") : election_history},
                    {"player_id", std::to_string(player.get_id())},
                    {"role_name", m_callbacks.role_to_string(player.get_role())}
                }
            );
        }
    );
    m_ai.print_batch_results("警长竞选并发退水", withdraw_results);

    std::vector<int> remaining_candidates;
    remaining_candidates.reserve(candidates.size());
    for (int candidate_id : candidates) {
        remaining_candidates.push_back(candidate_id);
    }

    for (const auto& result : withdraw_results) {
        const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
        const bool withdraw = parse_bool_field(result.raw_reply, "withdraw", false);
        LOG_INFO("[GAME][P%d][THOUGHT][sheriff_withdraw] %s", result.player_id, thought.c_str());
        LOG_INFO("[GAME][P%d][DECISION][sheriff_withdraw] withdraw=%s", result.player_id, withdraw ? "true" : "false");
        if (withdraw) {
            remaining_candidates.erase(
                std::remove(remaining_candidates.begin(), remaining_candidates.end(), result.player_id),
                remaining_candidates.end()
            );
            m_event_bus.broadcast("系统", std::to_string(result.player_id) + "号玩家选择退水，退出警长竞选。");
        }
    }

    return remaining_candidates;
}

void SheriffElectionPhase::execute() {
    std::cout << "\n========== 第 " << m_context.day_count << " 天 警长竞选 ==========" << std::endl;
    LOG_INFO("[GAME][PHASE][SHERIFF_ELECTION] begin");

    if (m_context.sheriff_election_resume_vote_only) {
        m_context.sheriff_election_resume_vote_only = false;
        m_event_bus.broadcast("系统", "昨天的警长竞选被白狼王自爆打断。今天重新进行完整的警长竞选流程。");
    }

    const bool forward = is_single_forward_double_reverse();
    m_event_bus.broadcast(
        "系统",
        std::string("警长竞选开始，正在等待玩家决定是否上警。本次警上询问顺序按当前时间执行") +
        (forward ? "单顺。" : "双逆。")
    );

    std::vector<int> candidates = collect_candidates();
    std::string current_election_history;

    if (candidates.empty()) {
        m_context.sheriff_id = -1;
        LOG_INFO("[GAME][SHERIFF][RESULT] sheriff_lost via=no_valid_vote");
        m_event_bus.broadcast("系统", "无人参与警长竞选，本局无警徽。");
        publish_deferred_first_night_deaths();
        m_context.state = GameState::DAY_SPEAK_PHASE;
        return;
    }

    if (!run_candidate_speeches(candidates, current_election_history)) {
        return;
    }

    if (!current_election_history.empty()) {
        m_event_bus.append_public_record("系统", "警长竞选完整记录:\n" + current_election_history);
    }

    if (candidates.size() == 1) {
        m_context.sheriff_id = candidates.front();
        m_event_bus.broadcast("系统", std::to_string(m_context.sheriff_id) + "号玩家自动当选警长。");
        publish_deferred_first_night_deaths();
        m_context.state = GameState::DAY_SPEAK_PHASE;
        return;
    }

    candidates = collect_withdrawals(candidates, current_election_history);
    if (!current_election_history.empty()) {
        m_event_bus.append_public_record("系统", "警长竞选完整记录:\n" + current_election_history);
    }

    if (candidates.empty()) {
        m_context.sheriff_id = -1;
        LOG_INFO("[GAME][SHERIFF][RESULT] sheriff_lost via=withdraw_all");
        m_event_bus.broadcast("系统", "所有警上玩家均已退水，警徽流失。");
        publish_deferred_first_night_deaths();
        m_context.state = GameState::DAY_SPEAK_PHASE;
        return;
    }

    if (candidates.size() == 1) {
        m_context.sheriff_id = candidates.front();
        LOG_INFO("[GAME][SHERIFF][RESULT] sheriff=%d via=withdraw_auto", m_context.sheriff_id);
        m_event_bus.broadcast("系统", std::to_string(m_context.sheriff_id) + "号玩家在退水后自动当选警长。");
        publish_deferred_first_night_deaths();
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
    m_ai.print_batch_results("警长竞选警下投票", vote_results);

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

        Player* white_wolf_king = nullptr;
        for (Player* player : m_context.alive_players_with_roles({Role::WHITE_WOLF_KING})) {
            white_wolf_king = player;
            break;
        }
        std::atomic<bool> explosion_triggered(false);
        std::mutex explosion_mutex;
        ExplosionDecision explosion_decision;
        std::vector<SpeechCheckpoint> pk_speech_checkpoints;
        block_queue<Event> judgement_queue(std::max(8, static_cast<int>(resolution.leaders.size()) + 2));
        std::thread adjudication_thread;
        if (white_wolf_king != nullptr) {
            adjudication_thread = std::thread([this, &judgement_queue, &explosion_triggered, &explosion_mutex, &explosion_decision, white_wolf_king]() {
                const AskOptions silent_options{RequestMode::Sequential, false};
                while (true) {
                    Event event;
                    if (!judgement_queue.pop(event)) {
                        continue;
                    }
                    if (event.name == "__stop__") {
                        break;
                    }
                    if (explosion_triggered.load()) {
                        continue;
                    }

                    const std::string prompt_name = select_wwk_interrupt_prompt_name(m_context);
                    const std::string instruction = m_ai.render_named_template(
                        prompt_name,
                        {
                            {"speaker_id", std::to_string(event.actor_id)},
                            {"speech_content", event.message}
                        }
                    );
                    AIOrchestrator::AIResult result = m_ai.ask_player(*white_wolf_king, prompt_name, instruction, silent_options);
                    try {
                        json parsed = json::parse(clean_response(result.raw_reply));
                        if (!parsed.value("explode", false)) {
                            continue;
                        }

                        ExplosionDecision decision;
                        decision.white_wolf_king_id = white_wolf_king->get_id();
                        decision.trigger_speaker_id = event.actor_id;
                        decision.target_id = parsed.value("target", -1);
                        if (decision.target_id != -1 && !m_context.is_player_alive(decision.target_id)) {
                            decision.target_id = -1;
                        }

                        bool expected = false;
                        if (explosion_triggered.compare_exchange_strong(expected, true)) {
                            std::lock_guard<std::mutex> lock(explosion_mutex);
                            explosion_decision = decision;
                            break;
                        }
                    } catch (...) {
                    }
                }
            });
        }

        for (int candidate_id : resolution.leaders) {
            if (explosion_triggered.load()) {
                break;
            }
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
                const std::size_t history_size_before = m_context.global_history.size();
                const std::string speech = parse_speech_from_reply(result.raw_reply);
                Event speech_event;
                speech_event.type = EventType::BROADCAST;
                speech_event.name = "sheriff_pk_speech";
                speech_event.actor_id = candidate->get_id();
                speech_event.speaker = std::to_string(candidate->get_id()) + "号[警长PK拉票]";
                speech_event.message = speech;
                LOG_INFO("[GAME][P%d][DECISION][sheriff_pk_speech] speech=%s", candidate->get_id(), speech_event.message.c_str());
                if (m_event_bus.publish(std::move(speech_event))) {
                    if (adjudication_thread.joinable()) {
                        Event stop_event;
                        stop_event.name = "__stop__";
                        judgement_queue.push(stop_event);
                        adjudication_thread.join();
                    }
                    return;
                }
                pk_speech_checkpoints.push_back({candidate->get_id(), history_size_before});
                if (adjudication_thread.joinable()) {
                    Event judgement_event;
                    judgement_event.name = "judge_sheriff_pk_speech";
                    judgement_event.actor_id = candidate->get_id();
                    judgement_event.message = speech;
                    judgement_queue.push(judgement_event);
                }
            } catch (...) {
                m_event_bus.append_public_record("系统", std::to_string(candidate->get_id()) + "号警长PK发言解析失败。");
            }
        }

        if (adjudication_thread.joinable()) {
            Event stop_event;
            stop_event.name = "__stop__";
            judgement_queue.push(stop_event);
            adjudication_thread.join();
        }

        if (explosion_triggered.load()) {
            ExplosionDecision decision;
            {
                std::lock_guard<std::mutex> lock(explosion_mutex);
                decision = explosion_decision;
            }

            rollback_history_after_trigger(m_context.global_history, pk_speech_checkpoints, decision.trigger_speaker_id);
            m_context.exploded_wwk_commander_id = decision.white_wolf_king_id;
            const bool postpone_election = (m_context.day_count == 1 && !m_context.sheriff_election_resume_explosion_loses_badge);
            if (postpone_election && m_context.last_night_poison_target == decision.white_wolf_king_id) {
                m_context.deaths_last_night.erase(
                    std::remove(m_context.deaths_last_night.begin(), m_context.deaths_last_night.end(), decision.white_wolf_king_id),
                    m_context.deaths_last_night.end()
                );
                m_context.last_night_poison_target = -1;
            }

            m_event_bus.broadcast(
                "系统",
                postpone_election
                    ? ("白狼王 " + std::to_string(decision.white_wolf_king_id) +
                        " 号在警长PK发言阶段自爆，带走了 " + std::to_string(decision.target_id) + " 号！本轮警长竞选延期到明天。")
                    : ("白狼王 " + std::to_string(decision.white_wolf_king_id) +
                        " 号在警长PK发言阶段自爆，带走了 " + std::to_string(decision.target_id) + " 号！警徽流失。")
            );
            m_callbacks.publish_player_death(decision.white_wolf_king_id, "explosion", false);
            if (decision.target_id != -1) {
                m_callbacks.publish_player_death(decision.target_id, "explosion", true);
            }
            if (Player* wwk = m_context.find_player(decision.white_wolf_king_id)) {
                const std::string last_words_instruction = m_ai.render_named_template(
                    "wwk_last_words_instruction",
                    {
                        {"player_id", std::to_string(wwk->get_id())},
                        {"role_name", m_callbacks.role_to_string(wwk->get_role())}
                    }
                );
                AIOrchestrator::AIResult last_words_result = m_ai.ask_player(*wwk, "wwk_last_words_instruction", last_words_instruction);
                try {
                    Event last_words_event;
                    last_words_event.type = EventType::BROADCAST;
                    last_words_event.name = "last_words";
                    last_words_event.actor_id = wwk->get_id();
                    last_words_event.speaker = std::to_string(wwk->get_id()) + "号(白狼王遗言)";
                    last_words_event.message = parse_speech_from_reply(last_words_result.raw_reply);
                    m_event_bus.publish(std::move(last_words_event));
                } catch (...) {
                    m_event_bus.append_public_record("系统", std::to_string(wwk->get_id()) + "号白狼王遗言解析失败。");
                }
            }
            if (m_callbacks.handle_win_if_needed()) {
                return;
            }

            if (postpone_election) {
                const std::string death_message = build_death_message(m_context.deaths_last_night);
                m_event_bus.broadcast("系统", death_message);
                for (const auto& pending_message : m_context.pending_day_messages) {
                    m_event_bus.broadcast("系统", pending_message);
                }
                m_context.pending_day_messages.clear();
                publish_deferred_first_night_deaths();
                m_context.first_night_last_words_ids = m_context.deaths_last_night;
                m_context.first_night_last_words_poison_target = m_context.last_night_poison_target;
                m_context.first_night_last_words_pending = !m_context.first_night_last_words_ids.empty();
                run_pending_first_night_last_words();
                m_context.sheriff_election_resume_vote_only = true;
                m_context.sheriff_election_resume_explosion_loses_badge = true;
                m_context.sheriff_id = -1;
                m_event_bus.broadcast("系统", "白狼王自爆，首日警长竞选中止。首夜死者遗言结束后进入黑夜，明天早晨重新进行完整的警长竞选流程。");
            } else {
                m_context.sheriff_id = -1;
                m_event_bus.broadcast("系统", "白狼王自爆，警长竞选立即中止，直接进入黑夜。");
            }

            m_context.day_count++;
            m_context.state = GameState::NIGHT_PHASE;
            return;
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
        m_ai.print_batch_results("警长竞选PK投票", pk_vote_results);

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

    publish_deferred_first_night_deaths();
    m_context.sheriff_election_resume_explosion_loses_badge = false;
    m_context.state = GameState::DAY_SPEAK_PHASE;
}
