#include <iostream>
#include <utility>
#include <vector>
#include "EventBus.h"
#include "GameContext.h"
#include "phase_test_utils.h"
#include "phases/SheriffElectionPhase.h"

int main() {
    init_test_log("sheriff_election_wwk_interrupt.log");

    GameContext context;
    context.state = GameState::SHERIFF_ELECTION_PHASE;
    context.day_count = 1;
    context.enable_first_night_last_words = true;
    context.deaths_last_night = {1};
    context.first_night_deaths_deferred = true;

    context.players.push_back(std::make_unique<Player>(1, Role::VILLAGER, "candidate-1"));
    context.players.push_back(std::make_unique<Player>(2, Role::WHITE_WOLF_KING, "wwk"));
    context.players.push_back(std::make_unique<Player>(3, Role::VILLAGER, "candidate-3"));
    context.players.push_back(std::make_unique<Player>(4, Role::VILLAGER, "candidate-4"));

    EventBus event_bus(context);
    FakeAIOrchestrator ai(context, event_bus);

    ai.add_reply(1, "sheriff_register_instruction", R"({"thought":"我要上警","data":{"run_for_sheriff":true}})");
    ai.add_reply(2, "sheriff_register_instruction", R"({"thought":"我不上警","data":{"run_for_sheriff":false}})");
    ai.add_reply(3, "sheriff_register_instruction", R"({"thought":"我要上警","data":{"run_for_sheriff":true}})");
    ai.add_reply(4, "sheriff_register_instruction", R"({"thought":"我要上警","data":{"run_for_sheriff":true}})");
    ai.add_reply(1, "sheriff_speech_instruction", R"({"thought":"1号警上发言","data":{"speech":"1号警上发言"}})");
    ai.add_reply(3, "sheriff_speech_instruction", R"({"thought":"3号警上发言","data":{"speech":"3号警上发言"}})");
    ai.add_reply(4, "sheriff_speech_instruction", R"({"thought":"4号警上发言","data":{"speech":"4号警上发言"}})", 5);
    ai.add_reply(2, "wwk_interrupt_sheriff_day1_instruction", R"({"explode":false,"target":-1})");
    ai.add_reply(2, "wwk_interrupt_sheriff_day1_instruction", R"({"explode":true,"target":4})", 30);
    ai.add_reply(2, "wwk_interrupt_sheriff_day1_instruction", R"({"explode":false,"target":-1})");
    ai.add_reply(2, "wwk_last_words_instruction", R"({"thought":"留遗言","data":{"speech":"白狼王竞选遗言"}})");
    ai.add_reply(1, "day_last_words_instruction", R"({"thought":"首夜遗言","data":{"speech":"1号首夜遗言"}})");

    std::vector<std::pair<int, std::string>> death_records;
    bool sheriff_death_called = false;
    SheriffElectionPhase phase(
        context,
        event_bus,
        ai,
        SheriffElectionPhaseCallbacks{
            role_to_string_for_test,
            [&sheriff_death_called](int sheriff_id) {
                (void)sheriff_id;
                sheriff_death_called = true;
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

    expect_true(context.state == GameState::NIGHT_PHASE, "sheriff election should jump to night after first-day explosion");
    expect_true(context.day_count == 2, "day count should advance after sheriff election postponement");
    expect_true(context.sheriff_id == -1, "there should still be no sheriff immediately after postponement");
    expect_true(context.sheriff_election_resume_vote_only, "sheriff election should be marked to rerun on the next morning");
    expect_true(!context.find_player(2)->is_alive(), "white wolf king should die in sheriff election explosion");
    expect_true(!context.find_player(4)->is_alive(), "explosion target should die in sheriff election explosion");
    expect_true(history_contains(context.global_history, "1号(警上): 1号警上发言"), "speech before trigger should remain");
    expect_true(history_contains(context.global_history, "3号(警上): 3号警上发言"), "trigger speech should remain");
    expect_true(!history_contains(context.global_history, "4号(警上): 4号警上发言"), "speech after trigger should be rolled back");
    expect_true(
        history_contains(context.global_history, "系统: 白狼王 2 号在警长竞选发言阶段自爆，带走了 4 号！本轮警长竞选延期到明天。"),
        "sheriff election postponement announcement should be recorded"
    );
    expect_true(
        history_contains(context.global_history, "2号(白狼王遗言): 白狼王竞选遗言"),
        "white wolf king sheriff-election last words should be recorded"
    );
    expect_true(
        history_contains(context.global_history, "1号(遗言): 1号首夜遗言"),
        "first-night victim should speak immediately after the explosion sequence"
    );
    expect_true(
        history_contains(context.global_history, "系统: 白狼王自爆，首日警长竞选中止。首夜死者遗言结束后进入黑夜，明天早晨重新进行完整的警长竞选流程。"),
        "full-rerun notice should be recorded"
    );
    expect_true(death_records.size() == 3, "first-night death plus explosion deaths should be recorded");
    expect_true(!sheriff_death_called, "normal sheriff death callback should not run in this path");

    std::cout << "SheriffElectionWwkInterruptTest passed" << std::endl;
    return 0;
}
