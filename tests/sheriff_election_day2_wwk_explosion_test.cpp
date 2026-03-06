#include <iostream>
#include <utility>
#include <vector>
#include "EventBus.h"
#include "GameContext.h"
#include "phase_test_utils.h"
#include "phases/SheriffElectionPhase.h"

int main() {
    init_test_log("sheriff_election_day2_wwk_explosion.log");

    GameContext context;
    context.state = GameState::SHERIFF_ELECTION_PHASE;
    context.day_count = 2;
    context.enable_first_night_last_words = true;
    context.sheriff_election_resume_vote_only = true;
    context.sheriff_election_resume_explosion_loses_badge = true;
    context.first_night_last_words_pending = false;

    context.players.push_back(std::make_unique<Player>(1, Role::VILLAGER, "candidate-1"));
    context.players.push_back(std::make_unique<Player>(2, Role::WHITE_WOLF_KING, "wwk"));
    context.players.push_back(std::make_unique<Player>(3, Role::VILLAGER, "candidate-3"));
    context.players.push_back(std::make_unique<Player>(4, Role::VILLAGER, "candidate-4"));

    EventBus event_bus(context);
    FakeAIOrchestrator ai(context, event_bus);

    ai.add_reply(1, "sheriff_register_instruction", R"({"thought":"我要上警","data":{"run_for_sheriff":true}})");
    ai.add_reply(2, "sheriff_register_instruction", R"({"thought":"我不上警","data":{"run_for_sheriff":false}})");
    ai.add_reply(3, "sheriff_register_instruction", R"({"thought":"我要上警","data":{"run_for_sheriff":true}})");
    ai.add_reply(4, "sheriff_register_instruction", R"({"thought":"我不上警","data":{"run_for_sheriff":false}})");
    ai.add_reply(1, "sheriff_speech_instruction", R"({"thought":"1号第二天警上发言","data":{"speech":"1号第二天警上发言"}})");
    ai.add_reply(3, "sheriff_speech_instruction", R"({"thought":"3号第二天警上发言","data":{"speech":"3号第二天警上发言"}})", 5);
    ai.add_reply(2, "wwk_interrupt_sheriff_day2_instruction", R"({"explode":true,"target":3})", 30);
    ai.add_reply(2, "wwk_interrupt_sheriff_day2_instruction", R"({"explode":false,"target":-1})");
    ai.add_reply(2, "wwk_last_words_instruction", R"({"thought":"第二天竞选遗言","data":{"speech":"白狼王第二天竞选遗言"}})");

    std::vector<std::pair<int, std::string>> death_records;
    SheriffElectionPhase phase(
        context,
        event_bus,
        ai,
        SheriffElectionPhaseCallbacks{
            role_to_string_for_test,
            [](int sheriff_id) {
                (void)sheriff_id;
            },
            [&context, &death_records](int player_id, const std::string& cause, bool visible_to_all) {
                (void)visible_to_all;
                Player* player = context.find_player(player_id);
                if (player == nullptr) {
                    return false;
                }
                player->kill();
                death_records.push_back({player_id, cause});
                return false;
            },
            []() { return false; }
        }
    );

    phase.execute();

    expect_true(context.state == GameState::NIGHT_PHASE, "second-day sheriff explosion should jump directly to night");
    expect_true(context.day_count == 3, "day count should advance after second-day sheriff explosion");
    expect_true(context.sheriff_id == -1, "badge should be lost after second-day sheriff explosion");
    expect_true(!context.sheriff_election_resume_vote_only, "second-day explosion should not schedule another rerun");
    expect_true(!context.find_player(2)->is_alive(), "white wolf king should die");
    expect_true(!context.find_player(3)->is_alive(), "target should die");
    expect_true(history_contains(context.global_history, "1号(警上): 1号第二天警上发言"), "speech before trigger should remain");
    expect_true(!history_contains(context.global_history, "3号(警上): 3号第二天警上发言"), "triggered-after speech should be rolled back");
    expect_true(
        history_contains(context.global_history, "系统: 白狼王 2 号在警长竞选发言阶段自爆，带走了 3 号！警徽流失。"),
        "second-day explosion should explicitly lose the badge"
    );
    expect_true(
        history_contains(context.global_history, "2号(白狼王遗言): 白狼王第二天竞选遗言"),
        "white wolf king last words should be recorded"
    );
    expect_true(death_records.size() == 2, "two deaths should be recorded");

    std::cout << "SheriffElectionDay2WwkExplosionTest passed" << std::endl;
    return 0;
}
