#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>
#include "EventBus.h"
#include "GameContext.h"
#include "phases/DaySpeakPhase.h"
#include "phase_test_utils.h"

namespace {
}

int main() {
    init_test_log("day_speak_wwk_interrupt.log");

    GameContext context;
    context.state = GameState::DAY_SPEAK_PHASE;
    context.day_count = 2;
    context.sheriff_id = 1;
    context.enable_first_night_last_words = false;

    context.players.push_back(std::make_unique<Player>(1, Role::VILLAGER, "test-sheriff"));
    context.players.push_back(std::make_unique<Player>(2, Role::WHITE_WOLF_KING, "test-wwk"));
    context.players.push_back(std::make_unique<Player>(3, Role::VILLAGER, "test-villager-3"));
    context.players.push_back(std::make_unique<Player>(4, Role::VILLAGER, "test-villager-4"));

    EventBus event_bus(context);
    FakeAIOrchestrator ai(context, event_bus);

    ai.add_reply(1, "sheriff_decide_order_instruction", R"({"thought":"正序","data":{"order_type":"forward"}})");
    ai.add_reply(2, "day_speak_instruction", R"({"thought":"2号先发言","data":{"speech":"2号白天发言"}})");
    ai.add_reply(3, "day_speak_instruction", R"({"thought":"3号正常发言","data":{"speech":"3号白天发言"}})");
    ai.add_reply(4, "day_speak_instruction", R"({"thought":"4号后置位发言","data":{"speech":"4号白天发言"}})", 5);
    ai.add_reply(1, "day_speak_instruction", R"({"thought":"1号最后发言","data":{"speech":"1号白天发言"}})", 5);
    ai.add_reply(2, "wwk_interrupt_other_day_instruction", R"({"explode":false,"target":-1})");
    ai.add_reply(2, "wwk_interrupt_other_day_instruction", R"({"explode":true,"target":4})", 30);
    ai.add_reply(2, "wwk_interrupt_other_day_instruction", R"({"explode":false,"target":-1})");
    ai.add_reply(2, "wwk_interrupt_other_day_instruction", R"({"explode":false,"target":-1})");
    ai.add_reply(2, "wwk_last_words_instruction", R"({"thought":"留遗言","data":{"speech":"白狼王遗言"}})");

    std::vector<std::pair<int, std::string>> death_records;
    DaySpeakPhase phase(
        context,
        event_bus,
        ai,
        DaySpeakPhaseCallbacks{
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

    expect_true(context.state == GameState::NIGHT_PHASE, "day speak should jump to night after white wolf king explosion");
    expect_true(context.day_count == 3, "day count should advance after explosion");
    expect_true(!context.find_player(2)->is_alive(), "white wolf king should be dead");
    expect_true(!context.find_player(4)->is_alive(), "explosion target should be dead");
    expect_true(context.find_player(1)->is_alive(), "non-target player should stay alive");
    expect_true(context.find_player(3)->is_alive(), "trigger speaker should stay alive");

    expect_true(history_contains(context.global_history, "2号: 2号白天发言"), "speaker before trigger should remain in history");
    expect_true(history_contains(context.global_history, "3号: 3号白天发言"), "trigger speaker should remain in history");
    expect_true(!history_contains(context.global_history, "4号: 4号白天发言"), "speech after trigger should be rolled back");
    expect_true(!history_contains(context.global_history, "1号: 1号白天发言"), "later speeches should be rolled back");
    expect_true(
        history_contains(context.global_history, "系统: 白狼王 2 号在 3 号玩家发言后选择自爆"),
        "system explosion announcement should be recorded"
    );
    expect_true(
        history_contains(context.global_history, "2号(白狼王遗言): 白狼王遗言"),
        "white wolf king last words should be recorded"
    );
    expect_true(death_records.size() == 2, "two players should be marked dead by explosion");

    std::cout << "DaySpeakWwkInterruptTest passed" << std::endl;
    return 0;
}
