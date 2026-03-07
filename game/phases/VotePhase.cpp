#include "phases/VotePhase.h"
#include <atomic>
#include <algorithm>
#include <iostream>
#include "log.h"
#include <map>
#include <mutex>
#include <thread>
#include "block_queue.h"
#include "RuleEngine.h"
#include "support/PhaseUtils.h"
#include "support/PromptParsers.h"

namespace {
std::string select_wwk_interrupt_prompt_name(const GameContext& context) {
    return "wwk_interrupt_other_day_instruction";
}

struct SpeechCheckpoint {
    int speaker_id = -1;
    std::size_t history_size_before = 0;
    std::string speaker_label;
    std::size_t tracked_line_index = std::string::npos;
};

struct ExplosionDecision {
    int white_wolf_king_id = -1;
    int trigger_speaker_id = -1;
    int target_id = -1;
    std::string after_word;
};

std::string build_interrupted_speech(const std::string& original_speech, const std::string& after_word) {
    if (!after_word.empty()) {
        const std::size_t pos = original_speech.find(after_word);
        if (pos != std::string::npos) {
            return original_speech.substr(0, pos + after_word.length()) + "......（话音未落，只听“砰”的一声巨响！）";
        }
        return original_speech + "......（突然被自爆打断！）";
    }
    return original_speech + "......（发言被自爆打断！）";
}

std::string build_history_line(const std::string& speaker, const std::string& speech) {
    return speaker + ": " + speech;
}

void apply_interrupted_speech_to_history(
    json& history,
    const std::vector<SpeechCheckpoint>& checkpoints,
    int trigger_speaker_id,
    const std::string& after_word
) {
    for (const SpeechCheckpoint& checkpoint : checkpoints) {
        if (checkpoint.speaker_id != trigger_speaker_id) {
            continue;
        }
        if (checkpoint.history_size_before >= history.size()) {
            return;
        }
        json& record = history[checkpoint.history_size_before];
        if (!record.contains("content") || !record["content"].is_string()) {
            return;
        }
        const std::string content = record["content"].get<std::string>();
        const std::string prefix = checkpoint.speaker_label + ": ";
        const std::string speech = content.rfind(prefix, 0) == 0 ? content.substr(prefix.length()) : content;
        record["content"] = prefix + build_interrupted_speech(speech, after_word);
        return;
    }
}

void apply_interrupted_speech_to_tracked_lines(
    std::vector<std::string>& lines,
    const std::vector<SpeechCheckpoint>& checkpoints,
    int trigger_speaker_id,
    const std::string& after_word
) {
    for (const SpeechCheckpoint& checkpoint : checkpoints) {
        if (checkpoint.speaker_id != trigger_speaker_id || checkpoint.tracked_line_index == std::string::npos) {
            continue;
        }
        if (checkpoint.tracked_line_index >= lines.size()) {
            return;
        }
        const std::string prefix = checkpoint.speaker_label + ": ";
        const std::string& content = lines[checkpoint.tracked_line_index];
        const std::string speech = content.rfind(prefix, 0) == 0 ? content.substr(prefix.length()) : content;
        lines[checkpoint.tracked_line_index] = prefix + build_interrupted_speech(speech, after_word);
        return;
    }
}

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

void rollback_tracked_lines_after_trigger(
    std::vector<std::string>& lines,
    const std::vector<SpeechCheckpoint>& checkpoints,
    int trigger_speaker_id
) {
    bool erase_next = false;
    std::size_t rollback_index = lines.size();

    for (const SpeechCheckpoint& checkpoint : checkpoints) {
        if (checkpoint.tracked_line_index == std::string::npos) {
            continue;
        }
        if (erase_next) {
            rollback_index = checkpoint.tracked_line_index;
            break;
        }
        if (checkpoint.speaker_id == trigger_speaker_id) {
            erase_next = true;
        }
    }

    while (lines.size() > rollback_index) {
        lines.pop_back();
    }
}
}

VotePhase::VotePhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const VotePhaseCallbacks& callbacks)
    : m_context(context), m_event_bus(event_bus), m_ai(ai), m_callbacks(callbacks) {}

void VotePhase::execute() {
    std::cout << "\n========== 投票阶段 ==========" << std::endl;
    LOG_INFO("[GAME][PHASE][VOTE] day=%d begin", m_context.day_count);

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
                    {"role_name", m_callbacks.role_to_string(player.get_role())}
                }
            );
        }
    );
    m_ai.print_batch_results("白天第一轮投票", round1_results);

    for (const auto& result : round1_results) {
        LOG_INFO("[GAME][P%d][THOUGHT][day_vote] %s", result.player_id, parse_thought_from_reply(result.raw_reply, "（未提供思考）").c_str());
        Player* voter = m_context.find_player(result.player_id);
        if (voter == nullptr || !voter->is_alive()) {
            continue;
        }

        const int target = parse_target_from_reply(result.raw_reply);
        LOG_INFO("[GAME][P%d][DECISION][day_vote] target=%d", result.player_id, target);
        const bool valid_target = target >= 1 && target <= m_context.player_count() && m_context.is_player_alive(target);
        if (valid_target) {
            vote_counts[target]++;
            round1_records[target].push_back(voter->get_id());
        } else {
            round1_records[-1].push_back(voter->get_id());
        }
    }

    const std::string round1_details = format_vote_records(round1_records);
    LOG_INFO("[GAME][VOTE][ROUND1] details=%s", round1_details.c_str());
    VoteResolution round1_resolution = RuleEngine::resolve_top_votes(vote_counts, 1, m_context.player_count());
    round1_resolution = RuleEngine::apply_sheriff_tiebreak(round1_resolution, m_context.sheriff_id, round1_records, m_context);

    int exiled_id = -1;
    if (round1_resolution.leaders.empty()) {
        m_event_bus.broadcast("系统", "无人有效投票，平安日。");
    } else if (round1_resolution.leaders.size() == 1) {
        exiled_id = round1_resolution.leaders.front();
    } else {
        Player* white_wolf_king = nullptr;
        for (Player* player : m_context.alive_players_with_roles({Role::WHITE_WOLF_KING})) {
            white_wolf_king = player;
            break;
        }
        std::atomic<bool> explosion_triggered(false);
        std::mutex explosion_mutex;
        ExplosionDecision explosion_decision;
        std::vector<SpeechCheckpoint> speech_checkpoints;
        block_queue<Event> judgement_queue(std::max(8, static_cast<int>(round1_resolution.leaders.size()) + 2));
        std::thread adjudication_thread;
        if (white_wolf_king != nullptr) {
            adjudication_thread = std::thread([this, &judgement_queue, &explosion_triggered, &explosion_mutex, &explosion_decision, white_wolf_king]() {
                const AIOrchestrator::AskOptions silent_options{AIOrchestrator::RequestMode::Sequential, false};
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
                        decision.after_word = parsed.value("after_word", "");
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

        const std::string pk_names = join_ids(round1_resolution.leaders);
        m_event_bus.broadcast("系统", "出现平票！PK台候选人: " + pk_names + "。\n第一轮详细票型: " + round1_details);

        for (int candidate_id : round1_resolution.leaders) {
            if (explosion_triggered.load()) {
                break;
            }
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
                    {"role_name", m_callbacks.role_to_string(candidate->get_role())}
                }
            );
            AIOrchestrator::AIResult result = m_ai.ask_player(*candidate, "day_pk_speech_instruction", instruction);
            LOG_INFO("[GAME][P%d][THOUGHT][day_pk_speech] %s", candidate->get_id(), parse_thought_from_reply(result.raw_reply, "（未提供思考）").c_str());
            try {
                const std::string speech = parse_speech_from_reply(result.raw_reply);
                const std::size_t history_size_before = m_context.global_history.size();
                Event speak_event;
                speak_event.type = EventType::BROADCAST;
                speak_event.name = "player_speak";
                speak_event.actor_id = candidate->get_id();
                speak_event.speaker = std::to_string(candidate->get_id()) + "号[PK发言]";
                speak_event.message = speech;
                LOG_INFO("[GAME][P%d][DECISION][day_pk_speech] speech=%s", candidate->get_id(), speak_event.message.c_str());
                if (m_event_bus.publish(std::move(speak_event))) {
                    if (adjudication_thread.joinable()) {
                        Event stop_event;
                        stop_event.name = "__stop__";
                        judgement_queue.push(stop_event);
                        adjudication_thread.join();
                    }
                    return;
                }
                std::size_t tracked_line_index = std::string::npos;
                if (m_context.day_count == 1) {
                    tracked_line_index = m_context.day1_day_speech_lines.size();
                    m_context.day1_day_speech_lines.push_back(build_history_line(speak_event.speaker, speech));
                }
                speech_checkpoints.push_back({candidate->get_id(), history_size_before, speak_event.speaker, tracked_line_index});
                if (adjudication_thread.joinable()) {
                    Event judgement_event;
                    judgement_event.name = "judge_day_pk_speech";
                    judgement_event.actor_id = candidate->get_id();
                    judgement_event.message = speech;
                    judgement_queue.push(judgement_event);
                }
            } catch (...) {
                m_event_bus.append_public_record("系统", std::to_string(candidate->get_id()) + "号PK发言解析失败。");
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

            apply_interrupted_speech_to_history(
                m_context.global_history,
                speech_checkpoints,
                decision.trigger_speaker_id,
                decision.after_word
            );
            apply_interrupted_speech_to_tracked_lines(
                m_context.day1_day_speech_lines,
                speech_checkpoints,
                decision.trigger_speaker_id,
                decision.after_word
            );
            rollback_history_after_trigger(m_context.global_history, speech_checkpoints, decision.trigger_speaker_id);
            rollback_tracked_lines_after_trigger(
                m_context.day1_day_speech_lines,
                speech_checkpoints,
                decision.trigger_speaker_id
            );
            m_context.exploded_wwk_commander_id = decision.white_wolf_king_id;
            m_event_bus.broadcast(
                "系统",
                "白狼王 " + std::to_string(decision.white_wolf_king_id) +
                " 号在 PK 发言阶段自爆，带走了 " + std::to_string(decision.target_id) + " 号！"
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
            m_event_bus.broadcast("系统", "白狼王自爆，白天发言立即中止，直接进入黑夜。");
            m_context.day_count++;
            m_context.state = GameState::NIGHT_PHASE;
            return;
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
                        {"role_name", m_callbacks.role_to_string(player.get_role())}
                    }
                );
            }
        );
        m_ai.print_batch_results("白天PK投票", round2_results);

        for (const auto& result : round2_results) {
            LOG_INFO("[GAME][P%d][THOUGHT][day_pk_vote] %s", result.player_id, parse_thought_from_reply(result.raw_reply, "（未提供思考）").c_str());
            Player* voter = m_context.find_player(result.player_id);
            if (voter == nullptr || !voter->is_alive()) {
                continue;
            }

            const int target = parse_target_from_reply(result.raw_reply);
            LOG_INFO("[GAME][P%d][DECISION][day_pk_vote] target=%d", result.player_id, target);
            const bool valid_target = std::find(round1_resolution.leaders.begin(), round1_resolution.leaders.end(), target) != round1_resolution.leaders.end();
            if (valid_target) {
                vote_counts[target]++;
                round2_records[target].push_back(voter->get_id());
            } else {
                round2_records[-1].push_back(voter->get_id());
            }
        }

        const std::string round2_details = format_vote_records(round2_records);
        LOG_INFO("[GAME][VOTE][PK] details=%s", round2_details.c_str());
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
        LOG_INFO("[GAME][VOTE][EXILE] player=%d", exiled_id);
        m_event_bus.broadcast("系统", std::to_string(exiled_id) + "号玩家在表决中被投票出局");
        m_callbacks.publish_player_death(exiled_id, "vote", true);

        if (m_callbacks.handle_win_if_needed()) {
            return;
        }

        if (Player* dead_player = m_context.find_player(exiled_id)) {
            const std::string instruction = m_ai.render_named_template(
                "day_last_words_instruction",
                {
                    {"death_context", describe_death_context("vote")},
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
                speak_event.speaker = std::to_string(exiled_id) + "号(遗言)";
                speak_event.message = parse_speech_from_reply(result.raw_reply);
                LOG_INFO("[GAME][P%d][DECISION][last_words] speech=%s", dead_player->get_id(), speak_event.message.c_str());
                if (m_event_bus.publish(std::move(speak_event))) {
                    return;
                }
            } catch (...) {
                m_event_bus.append_public_record("系统", std::to_string(exiled_id) + "号遗言解析失败。");
            }
        }
    }

    if (m_callbacks.handle_win_if_needed()) {
        return;
    }

    m_context.day_count++;
    m_context.state = GameState::NIGHT_PHASE;
}
