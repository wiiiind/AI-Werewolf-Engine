#include "phases/NightPhase.h"

#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <utility>

#include "log.h"
#include "RuleEngine.h"
#include "support/PhaseUtils.h"
#include "support/PromptParsers.h"

namespace {
using AskOptions = AIOrchestrator::AskOptions;
using RequestMode = AIOrchestrator::RequestMode;
using AIResult = AIOrchestrator::AIResult;

struct SeerOutcome {
    bool has_result = false;
    int target_id = -1;
    std::string thought;
    std::string identity;
    AIResult result;
};

struct GuardOutcome {
    bool has_result = false;
    int target_id = -1;
    std::string thought;
    AIResult result;
};

struct WitchOutcome {
    bool has_result = false;
    bool use_save = false;
    int poison_target = -1;
    bool parsed = false;
    std::string thought;
    std::vector<AIResult> attempts;
};

struct WolfPipelineOutcome {
    int wolf_target = -1;
    bool consensus_reached = false;
    bool commanded_by_exploded_wwk = false;
    std::string round1_history;
    std::vector<AIResult> round1_results;
    std::vector<AIResult> round2_results;
    std::vector<std::pair<int, std::string>> private_records;
};

struct PipelineAOutcome {
    WolfPipelineOutcome wolf;
    WitchOutcome witch;
};

bool future_ready(std::future_status status) {
    return status == std::future_status::ready;
}
}

NightPhase::NightPhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const NightPhaseCallbacks& callbacks)
    : m_context(context), m_event_bus(event_bus), m_ai(ai), m_callbacks(callbacks) {}

void NightPhase::execute() {
    std::cout << "\n========== 第 " << m_context.day_count << " 天 黑夜降临 ==========" << std::endl;
    LOG_INFO("[GAME][PHASE][NIGHT] day=%d begin", m_context.day_count);
    LOG_INFO("[GAME][PHASE][NIGHT] day=%d mode=three_parallel_serial_pipelines", m_context.day_count);
    m_context.dead_last_night = -1;
    m_context.guard_target = -1;
    m_context.last_night_poison_target = -1;

    const std::vector<Player*> alive_players = m_context.alive_players();
    const std::string alive_list = join_player_ids(alive_players);
    const AskOptions silent_options{RequestMode::Sequential, false};
    const int day_count = m_context.day_count;
    const int player_count = m_context.player_count();
    const bool witch_save_used_before = m_context.witch_save_used;
    const bool witch_poison_used_before = m_context.witch_poison_used;
    const int last_guard_target_before = m_context.last_guard_target;

    std::vector<Player*> alive_wolves = m_context.alive_players_with_roles({Role::WOLF, Role::WHITE_WOLF_KING});
    Player* alive_seer = nullptr;
    Player* alive_guard = nullptr;
    Player* alive_witch = nullptr;
    for (Player* player : alive_players) {
        if (player->get_role() == Role::SEER) {
            alive_seer = player;
        } else if (player->get_role() == Role::GUARD) {
            alive_guard = player;
        } else if (player->get_role() == Role::WITCH) {
            alive_witch = player;
        }
    }

    const int exploded_wwk_commander_id = m_context.exploded_wwk_commander_id;
    std::future<PipelineAOutcome> pipeline_a_future = m_ai.submit_task([this, alive_wolves, alive_witch, alive_list, day_count, player_count, silent_options, witch_save_used_before, witch_poison_used_before, exploded_wwk_commander_id]() {
        PipelineAOutcome outcome;

        if (exploded_wwk_commander_id != -1) {
            Player* commander = m_context.find_player(exploded_wwk_commander_id);
            if (commander != nullptr) {
                const std::string instruction = m_ai.render_named_template(
                    "night_exploded_wwk_command_instruction",
                    {
                        {"day_count", std::to_string(day_count)},
                        {"alive_list", alive_list},
                        {"player_id", std::to_string(commander->get_id())}
                    }
                );
                AIResult result = m_ai.ask_player(*commander, "night_exploded_wwk_command_instruction", instruction, silent_options);
                outcome.wolf.round1_results.push_back(result);
                const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
                int target = parse_target_from_reply(result.raw_reply, 0);
                if (target <= 0 || target > player_count || !m_context.is_player_alive(target)) {
                    target = 0;
                }
                outcome.wolf.wolf_target = target == 0 ? -1 : target;
                outcome.wolf.consensus_reached = true;
                outcome.wolf.commanded_by_exploded_wwk = true;
                LOG_INFO("[GAME][P%d][THOUGHT][exploded_wwk_command] %s", commander->get_id(), thought.c_str());
                LOG_INFO("[GAME][P%d][DECISION][exploded_wwk_command] day=%d target=%d", commander->get_id(), day_count, outcome.wolf.wolf_target);

                for (Player* wolf : alive_wolves) {
                    outcome.wolf.private_records.push_back({
                        wolf->get_id(),
                        "【前日自爆白狼王指刀：" + std::to_string(outcome.wolf.wolf_target == -1 ? 0 : outcome.wolf.wolf_target) + "号】"
                    });
                }
            }
        } else if (!alive_wolves.empty()) {
            LOG_INFO("[GAME][NIGHT][PIPELINE_A] day=%d action=wolves_start count=%zu", day_count, alive_wolves.size());
            for (int round = 1; round <= 2; ++round) {
                std::vector<int> wolf_votes(player_count + 1, 0);
                std::string current_round_actions;
                std::vector<AIResult>* current_round_results = round == 1 ? &outcome.wolf.round1_results : &outcome.wolf.round2_results;
                current_round_results->reserve(alive_wolves.size());

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
                            {"day_count", std::to_string(day_count)},
                            {"teammates_str", teammates},
                            {"alive_list", alive_list},
                            {"player_id", std::to_string(wolf->get_id())}
                        }
                    );
                    instruction += "\n\n【夜间沟通频道】\n";
                    if (round == 2) {
                        instruction += "⚠️警告：由于上一轮平票，这是第二轮重投；如果再次平票，今晚将无人死亡。\n";
                        instruction += "上一轮全队投票历史如下：\n" + outcome.wolf.round1_history + "\n";
                    }
                    if (!current_round_actions.empty()) {
                        instruction += "本轮前置位队友的指认如下：\n" + current_round_actions + "\n";
                    }

                    AIResult result = m_ai.ask_player(*wolf, "night_wolf_instruction", instruction, silent_options);
                    current_round_results->push_back(result);

                    const std::string thought = parse_thought_from_reply(result.raw_reply, "（未提供思考）");
                    int target = parse_target_from_reply(result.raw_reply, 0);
                    if (target < 0 || target > player_count) {
                        target = 0;
                    }

                    LOG_INFO("[GAME][P%d][THOUGHT][wolf_night] %s", result.player_id, thought.c_str());
                    LOG_INFO("[GAME][P%d][DECISION][wolf_night] day=%d round=%d target=%d", result.player_id, day_count, round, target);
                    wolf_votes[target]++;
                    current_round_actions += "【" + std::to_string(result.player_id) + "号狼人指认了: " + std::to_string(target) + "号】\n";
                    outcome.wolf.private_records.push_back({
                        result.player_id,
                        "第" + std::to_string(day_count) + "晚第" + std::to_string(round) + "轮夜间指认: " + std::to_string(target) + "号"
                    });
                }

                VoteResolution vote_resolution = RuleEngine::resolve_top_votes(wolf_votes, 0, player_count);
                LOG_INFO("[GAME][NIGHT][WOLF_ROUND] day=%d round=%d leaders=%s", day_count, round, join_ids(vote_resolution.leaders).c_str());
                if (vote_resolution.leaders.size() == 1) {
                    outcome.wolf.wolf_target = vote_resolution.leaders.front() == 0 ? -1 : vote_resolution.leaders.front();
                    outcome.wolf.consensus_reached = true;
                    break;
                }

                if (round == 1) {
                    outcome.wolf.round1_history = current_round_actions;
                }
            }

            const std::string final_memory = outcome.wolf.consensus_reached
                ? (outcome.wolf.wolf_target == -1
                    ? "【夜间结算】狼人阵营达成共识，今晚空刀。"
                    : "【夜间结算】狼人阵营最终决定击杀: " + std::to_string(outcome.wolf.wolf_target) + "号。")
                : "【夜间结算】狼人阵营因平票协商失败，被迫空刀。";
            LOG_INFO("[GAME][NIGHT][WOLF_FINAL] day=%d target=%d consensus=%s", day_count, outcome.wolf.wolf_target, outcome.wolf.consensus_reached ? "true" : "false");
            for (Player* wolf : alive_wolves) {
                outcome.wolf.private_records.push_back({wolf->get_id(), final_memory});
            }
        }

        if (alive_witch != nullptr) {
            LOG_INFO("[GAME][NIGHT][PIPELINE_A] day=%d action=witch_start wolf_target=%d", day_count, outcome.wolf.wolf_target);
            const std::string night_situation = witch_save_used_before
                ? "你已使用解药，无法看到今晚狼人杀害的目标。"
                : (outcome.wolf.wolf_target == -1
                    ? "今晚是平安夜，狼人没有杀人。（不是守卫守对了，而是狼人没有杀人）"
                    : "今晚狼人杀害了 " + std::to_string(outcome.wolf.wolf_target) + " 号玩家。");
            std::string save_status = witch_save_used_before ? "不可用(已用)" : "可用";
            if (outcome.wolf.wolf_target == alive_witch->get_id()) {
                if (day_count == 1) {
                    save_status = "可用（首夜可自救）";
                } else {
                    save_status = "不可用（非首夜不可自救）";
                }
            }
            const std::string poison_status = witch_poison_used_before ? "不可用(已用)" : "可用";
            const std::string instruction = m_ai.render_named_template(
                "night_witch_instruction",
                {
                    {"day_count", std::to_string(day_count)},
                    {"alive_list", alive_list},
                    {"night_situation", night_situation},
                    {"save_status", save_status},
                    {"poison_status", poison_status},
                    {"player_id", std::to_string(alive_witch->get_id())}
                }
            );

            for (int attempt = 1; attempt <= 2; ++attempt) {
                AIResult result = m_ai.ask_player(*alive_witch, "night_witch_instruction", instruction, silent_options);
                outcome.witch.attempts.push_back(result);
                try {
                    json parsed = json::parse(clean_response(result.raw_reply));
                    outcome.witch.has_result = true;
                    outcome.witch.parsed = true;
                    outcome.witch.thought = parsed.contains("thought") ? parsed["thought"].get<std::string>() : "（未提供思考）";
                    outcome.witch.use_save = parsed["data"]["save"];
                    outcome.witch.poison_target = parsed["data"]["poison"];
                    if (outcome.witch.use_save && outcome.witch.poison_target != -1) {
                        LOG_WARN("[GAME][P%d][DECISION][witch] day=%d invalid=double_potion retry=%s", alive_witch->get_id(), day_count, attempt == 1 ? "true" : "false");
                        if (attempt == 1) {
                            continue;
                        }
                    }
                    break;
                } catch (...) {
                    if (attempt == 2) {
                        outcome.witch.has_result = true;
                        outcome.witch.parsed = false;
                    }
                }
            }
        }

        return outcome;
    });

    std::future<AIResult> seer_future;
    std::future<AIResult> guard_future;
    bool has_seer_future = false;
    bool has_guard_future = false;

    if (alive_seer != nullptr) {
        const std::string instruction = m_ai.render_named_template(
            "night_seer_instruction",
            {
                {"day_count", std::to_string(day_count)},
                {"alive_list", alive_list},
                {"player_id", std::to_string(alive_seer->get_id())}
            }
        );
        LOG_INFO("[GAME][NIGHT][PIPELINE_B] day=%d action=seer_start", day_count);
        seer_future = m_ai.submit_player_request(*alive_seer, "night_seer_instruction", instruction, silent_options);
        has_seer_future = true;
    }

    if (alive_guard != nullptr) {
        const std::string last_guard_target = last_guard_target_before == -1 ? "无" : std::to_string(last_guard_target_before);
        const std::string instruction = m_ai.render_named_template(
            "night_guard_instruction",
            {
                {"day_count", std::to_string(day_count)},
                {"alive_list", alive_list},
                {"last_guard_target", last_guard_target},
                {"player_id", std::to_string(alive_guard->get_id())}
            }
        );
        LOG_INFO("[GAME][NIGHT][PIPELINE_C] day=%d action=guard_start", day_count);
        guard_future = m_ai.submit_player_request(*alive_guard, "night_guard_instruction", instruction, silent_options);
        has_guard_future = true;
    }

    bool a_done = false;
    bool b_done = !has_seer_future;
    bool c_done = !has_guard_future;
    PipelineAOutcome pipeline_a_outcome;
    SeerOutcome seer_outcome;
    GuardOutcome guard_outcome;

    while (!a_done || !b_done || !c_done) {
        bool progressed = false;

        if (!a_done && future_ready(pipeline_a_future.wait_for(std::chrono::milliseconds(0)))) {
            pipeline_a_outcome = pipeline_a_future.get();
            std::vector<AIResult> pipeline_a_results = pipeline_a_outcome.wolf.round1_results;
            pipeline_a_results.insert(
                pipeline_a_results.end(),
                pipeline_a_outcome.wolf.round2_results.begin(),
                pipeline_a_outcome.wolf.round2_results.end()
            );
            pipeline_a_results.insert(
                pipeline_a_results.end(),
                pipeline_a_outcome.witch.attempts.begin(),
                pipeline_a_outcome.witch.attempts.end()
            );
            m_ai.print_batch_results("第" + std::to_string(day_count) + "晚流水线A（狼人->女巫）", pipeline_a_results);
            a_done = true;
            progressed = true;
        }

        if (!b_done && future_ready(seer_future.wait_for(std::chrono::milliseconds(0)))) {
            seer_outcome.result = seer_future.get();
            seer_outcome.has_result = true;
            seer_outcome.thought = parse_thought_from_reply(seer_outcome.result.raw_reply, "（未提供思考）");
            seer_outcome.target_id = parse_target_from_reply(seer_outcome.result.raw_reply);
            Player* target = m_context.find_player(seer_outcome.target_id);
            seer_outcome.identity = (target != nullptr && RuleEngine::is_wolf(target->get_role())) ? "狼人" : "好人";
            LOG_INFO("[GAME][P%d][THOUGHT][seer] %s", seer_outcome.result.player_id, seer_outcome.thought.c_str());
            LOG_INFO("[GAME][P%d][DECISION][seer] day=%d target=%d result=%s", seer_outcome.result.player_id, day_count, seer_outcome.target_id, seer_outcome.identity.c_str());
            m_ai.print_batch_results("第" + std::to_string(day_count) + "晚流水线B（预言家）", std::vector<AIResult>{seer_outcome.result});
            b_done = true;
            progressed = true;
        }

        if (!c_done && future_ready(guard_future.wait_for(std::chrono::milliseconds(0)))) {
            guard_outcome.result = guard_future.get();
            guard_outcome.has_result = true;
            guard_outcome.thought = parse_thought_from_reply(guard_outcome.result.raw_reply, "（未提供思考）");
            guard_outcome.target_id = parse_target_from_reply(guard_outcome.result.raw_reply);
            if (guard_outcome.target_id == last_guard_target_before && guard_outcome.target_id != -1) {
                guard_outcome.target_id = -1;
            }
            LOG_INFO("[GAME][P%d][THOUGHT][guard] %s", guard_outcome.result.player_id, guard_outcome.thought.c_str());
            LOG_INFO("[GAME][P%d][DECISION][guard] day=%d target=%d", guard_outcome.result.player_id, day_count, guard_outcome.target_id);
            m_ai.print_batch_results("第" + std::to_string(day_count) + "晚流水线C（守卫）", std::vector<AIResult>{guard_outcome.result});
            c_done = true;
            progressed = true;
        }

        if (!progressed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::vector<std::pair<Player*, std::string>> pending_private_records;
    for (const auto& [player_id, message] : pipeline_a_outcome.wolf.private_records) {
        if (Player* player = m_context.find_player(player_id)) {
            pending_private_records.push_back({player, message});
        }
    }

    if (seer_outcome.has_result && alive_seer != nullptr) {
        pending_private_records.push_back({
            alive_seer,
            "第" + std::to_string(day_count) + "晚思考：" + seer_outcome.thought
        });
        pending_private_records.push_back({
            alive_seer,
            "第" + std::to_string(day_count) + "晚查验决定：查验了 " + std::to_string(seer_outcome.target_id) + "号，结果是：【" + seer_outcome.identity + "】。"
        });
    }

    if (guard_outcome.has_result && alive_guard != nullptr) {
        pending_private_records.push_back({
            alive_guard,
            "第" + std::to_string(day_count) + "晚思考：" + guard_outcome.thought
        });
        if (guard_outcome.target_id == -1) {
            pending_private_records.push_back({
                alive_guard,
                "第" + std::to_string(day_count) + "晚守护决定：空守/守护失败。"
            });
        } else {
            pending_private_records.push_back({
                alive_guard,
                "第" + std::to_string(day_count) + "晚守护决定：守护了 " + std::to_string(guard_outcome.target_id) + "号。"
            });
        }
    }

    int wolf_target = pipeline_a_outcome.wolf.wolf_target;
    int poison_target = -1;
    bool witch_used_save_tonight = false;
    if (alive_witch != nullptr) {
        const std::string wolf_target_info = witch_save_used_before
            ? "第" + std::to_string(day_count) + "晚夜情信息：你已使用解药，无法看到今晚狼人杀害的目标。"
            : (wolf_target == -1
                ? "第" + std::to_string(day_count) + "晚夜情信息：今晚狼人空刀（狼人选择不杀人）。"
                : "第" + std::to_string(day_count) + "晚夜情信息：今晚狼人击杀的目标是 " + std::to_string(wolf_target) + "号。");
        pending_private_records.push_back({alive_witch, wolf_target_info});

        if (pipeline_a_outcome.witch.has_result && pipeline_a_outcome.witch.parsed) {
            LOG_INFO("[GAME][P%d][THOUGHT][witch] %s", alive_witch->get_id(), pipeline_a_outcome.witch.thought.c_str());
            pending_private_records.push_back({
                alive_witch,
                "第" + std::to_string(day_count) + "晚思考：" + pipeline_a_outcome.witch.thought
            });

            if (pipeline_a_outcome.witch.use_save && pipeline_a_outcome.witch.poison_target != -1) {
                pending_private_records.push_back({
                    alive_witch,
                    "第" + std::to_string(day_count) + "晚操作仍非法：再次同时声明使用两瓶药，视为无动作。"
                });
                LOG_INFO("[GAME][P%d][DECISION][witch] day=%d action=none_after_invalid_retry", alive_witch->get_id(), day_count);
            } else if (
                pipeline_a_outcome.witch.use_save &&
                !m_context.witch_save_used &&
                wolf_target != -1 &&
                (wolf_target != alive_witch->get_id() || day_count == 1)
            ) {
                m_context.witch_save_used = true;
                witch_used_save_tonight = true;
                LOG_INFO("[GAME][P%d][DECISION][witch] day=%d action=save target=%d", alive_witch->get_id(), day_count, wolf_target);
                pending_private_records.push_back({
                    alive_witch,
                    "第" + std::to_string(day_count) + "晚行动决定：使用了解药，目标是 " + std::to_string(wolf_target) + "号。"
                });
                pending_private_records.push_back({
                    alive_witch,
                    "第" + std::to_string(day_count) + "晚使用了解药。"
                });
            } else if (pipeline_a_outcome.witch.poison_target != -1 && !m_context.witch_poison_used) {
                m_context.witch_poison_used = true;
                poison_target = pipeline_a_outcome.witch.poison_target;
                LOG_INFO("[GAME][P%d][DECISION][witch] day=%d action=poison target=%d", alive_witch->get_id(), day_count, poison_target);
                pending_private_records.push_back({
                    alive_witch,
                    "第" + std::to_string(day_count) + "晚行动决定：使用毒药，目标是 " + std::to_string(poison_target) + "号。"
                });
                pending_private_records.push_back({
                    alive_witch,
                    "第" + std::to_string(day_count) + "晚使用毒药毒了 " + std::to_string(poison_target) + "号。"
                });
            } else {
                LOG_INFO("[GAME][P%d][DECISION][witch] day=%d action=none", alive_witch->get_id(), day_count);
                pending_private_records.push_back({
                    alive_witch,
                    "第" + std::to_string(day_count) + "晚行动决定：什么也没做。"
                });
                pending_private_records.push_back({
                    alive_witch,
                    "第" + std::to_string(day_count) + "晚什么也没做。"
                });
            }
        } else if (alive_witch != nullptr) {
            LOG_WARN("[GAME][P%d][DECISION][witch] day=%d parse_failed default=none", alive_witch->get_id(), day_count);
            pending_private_records.push_back({
                alive_witch,
                "第" + std::to_string(day_count) + "晚操作解析失败，视为无动作。"
            });
        }
    }

    for (const auto& [player, message] : pending_private_records) {
        if (player != nullptr) {
            m_event_bus.append_private_record(*player, message);
        }
    }

    m_context.dead_last_night = wolf_target;
    m_context.guard_target = guard_outcome.has_result ? guard_outcome.target_id : -1;

    NightDeathResolution resolution = RuleEngine::resolve_night_deaths(
        m_context,
        wolf_target,
        poison_target,
        m_context.guard_target,
        witch_used_save_tonight
    );
    LOG_INFO(
        "[GAME][NIGHT][RESOLUTION] day=%d wolf_target=%d poison_target=%d guard_target=%d save_used=%s deaths=%s",
        day_count,
        wolf_target,
        poison_target,
        m_context.guard_target,
        witch_used_save_tonight ? "true" : "false",
        join_ids(resolution.deaths).c_str()
    );
    m_context.deaths_last_night = resolution.deaths;
    m_context.last_guard_target = m_context.guard_target;
    m_context.last_night_poison_target = poison_target;

    if (m_context.day_count == 1) {
        m_context.first_night_deaths_deferred = true;
    } else {
        for (int dead_id : m_context.deaths_last_night) {
            const std::string cause = dead_id == poison_target ? "poison" : "wolf";
            const bool allow_hunter = cause != "poison";
            m_callbacks.publish_player_death(dead_id, cause, allow_hunter);
        }
    }

    m_context.exploded_wwk_commander_id = -1;

    if (m_context.day_count == 1 || m_context.sheriff_election_resume_vote_only) {
        m_context.state = GameState::SHERIFF_ELECTION_PHASE;
    } else {
        m_context.state = GameState::DAY_SPEAK_PHASE;
    }
}
