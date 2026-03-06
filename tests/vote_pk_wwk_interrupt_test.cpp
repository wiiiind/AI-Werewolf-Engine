#include <iostream>
#include <utility>
#include <vector>
#include "EventBus.h"
#include "GameContext.h"
#include "phase_test_utils.h"
#include "phases/VotePhase.h"

int main() {
    init_test_log("vote_pk_wwk_interrupt.log");

    GameContext context;
    context.state = GameState::DAY_VOTE_PHASE;
    context.day_count = 2;
    context.enable_first_night_last_words = false;

    context.players.push_back(std::make_unique<Player>(1, Role::WHITE_WOLF_KING, "wwk"));
    context.players.push_back(std::make_unique<Player>(2, Role::VILLAGER, "pk-2"));
    context.players.push_back(std::make_unique<Player>(3, Role::VILLAGER, "pk-3"));
    context.players.push_back(std::make_unique<Player>(4, Role::VILLAGER, "voter-4"));

    EventBus event_bus(context);
    FakeAIOrchestrator ai(context, event_bus);

    ai.add_reply(1, "day_vote_instruction", R"({"thought":"投2","data":{"target":2}})");
    ai.add_reply(2, "day_vote_instruction", R"({"thought":"投3","data":{"target":3}})");
    ai.add_reply(3, "day_vote_instruction", R"({"thought":"投2","data":{"target":2}})");
    ai.add_reply(4, "day_vote_instruction", R"({"thought":"投3","data":{"target":3}})");
    ai.add_reply(2, "day_pk_speech_instruction", R"({"thought":"2号PK发言","data":{"speech":"2号PK发言"}})");
    ai.add_reply(3, "day_pk_speech_instruction", R"({"thought":"3号PK发言","data":{"speech":"3号PK发言"}})", 5);
    ai.add_reply(1, "wwk_interrupt_other_day_instruction", R"({"explode":true,"target":3})", 30);
    ai.add_reply(1, "wwk_interrupt_other_day_instruction", R"({"explode":false,"target":-1})");
    ai.add_reply(1, "wwk_last_words_instruction", R"({"thought":"留遗言","data":{"speech":"白狼王PK遗言"}})");

    std::vector<std::pair<int, std::string>> death_records;
    VotePhase phase(
        context,
        event_bus,
        ai,
        VotePhaseCallbacks{
            role_to_string_for_test,
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

    expect_true(context.state == GameState::NIGHT_PHASE, "vote phase should jump to night after PK explosion");
    expect_true(context.day_count == 3, "day count should advance after PK explosion");
    expect_true(!context.find_player(1)->is_alive(), "white wolf king should die in PK explosion");
    expect_true(!context.find_player(3)->is_alive(), "PK explosion target should die");
    expect_true(history_contains(context.global_history, "2号[PK发言]: 2号PK发言"), "first PK speech should remain");
    expect_true(!history_contains(context.global_history, "3号[PK发言]: 3号PK发言"), "later PK speech should be rolled back");
    expect_true(
        history_contains(context.global_history, "系统: 白狼王 1 号在 PK 发言阶段自爆，带走了 3 号！"),
        "PK explosion announcement should be recorded"
    );
    expect_true(
        history_contains(context.global_history, "1号(白狼王遗言): 白狼王PK遗言"),
        "white wolf king PK last words should be recorded"
    );
    expect_true(death_records.size() == 2, "two deaths should be recorded in PK explosion");

    std::cout << "VotePkWwkInterruptTest passed" << std::endl;
    return 0;
}
