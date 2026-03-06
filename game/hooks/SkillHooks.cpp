#include "hooks/SkillHooks.h"
#include "RuleEngine.h"
#include "support/PhaseUtils.h"
#include "support/PromptParsers.h"
#include "log.h"

namespace {
std::string role_to_string(Role role) {
    switch (role) {
        case Role::WOLF: return "狼人";
        case Role::WHITE_WOLF_KING: return "白狼王";
        case Role::SEER: return "预言家";
        case Role::WITCH: return "女巫";
        case Role::HUNTER: return "猎人";
        case Role::GUARD: return "守卫";
        case Role::IDIOT: return "白痴";
        default: return "平民";
    }
}

bool handle_white_wolf_king_interrupt(
    Event& event,
    GameContext& context,
    EventBus& event_bus,
    AIOrchestrator& ai,
    const SkillHookCallbacks& callbacks
) {
    if (event.type != EventType::BROADCAST) {
        return false;
    }
    if (event.name != "player_speak" &&
        event.name != "last_words" &&
        event.name != "sheriff_speech" &&
        event.name != "sheriff_pk_speech") {
        return false;
    }

    Player* white_wolf_king = nullptr;
    for (Player* player : context.alive_players_with_roles({Role::WHITE_WOLF_KING})) {
        white_wolf_king = player;
        break;
    }
    if (white_wolf_king == nullptr || white_wolf_king->get_id() == event.actor_id) {
        return false;
    }

    const std::string instruction = ai.render_named_template(
        "wwk_interrupt_instruction",
        {
            {"speaker_id", std::to_string(event.actor_id)},
            {"speech_content", event.message}
        }
    );

    const auto result = ai.ask_player(*white_wolf_king, "wwk_interrupt_instruction", instruction);
    try {
        json parsed = json::parse(clean_response(result.raw_reply));
        if (!parsed.value("explode", false)) {
            return false;
        }

        const int target_id = parsed.value("target", -1);
        const std::string after_word = parsed.value("after_word", "");
        LOG_INFO("[GAME][SKILL][WHITE_WOLF_KING] player=%d trigger=%s target=%d after_word=%s", white_wolf_king->get_id(), event.name.c_str(), target_id, after_word.c_str());

        std::string interrupted_speech = event.message;
        if (!after_word.empty()) {
            size_t pos = interrupted_speech.find(after_word);
            if (pos != std::string::npos) {
                interrupted_speech = interrupted_speech.substr(0, pos + after_word.length()) + "......（话音未落，只听“砰”的一声巨响！）";
            } else {
                interrupted_speech += "......（突然被自爆打断！）";
            }
        } else {
            interrupted_speech += "......（发言被自爆打断！）";
        }

        event_bus.broadcast(event.speaker, interrupted_speech);

        if (event.name == "sheriff_speech" || event.name == "sheriff_pk_speech") {
            context.sheriff_id = -1;
            event_bus.broadcast(
                "系统",
                "白狼王 " + std::to_string(white_wolf_king->get_id()) +
                " 号在警长竞选阶段自爆，带走了 " + std::to_string(target_id) + " 号！警徽流失。"
            );
        } else {
            event_bus.broadcast(
                "系统",
                "白狼王 " + std::to_string(white_wolf_king->get_id()) +
                " 号强行自爆，带走了 " + std::to_string(target_id) + " 号！"
            );
        }

        callbacks.publish_player_death(white_wolf_king->get_id(), "explosion", false);
        if (target_id != -1) {
            callbacks.publish_player_death(target_id, "explosion", true);
        }

        const std::string last_words_instruction = ai.render_named_template(
            "day_last_words_instruction",
            {
                {"player_id", std::to_string(white_wolf_king->get_id())},
                {"role_name", role_to_string(white_wolf_king->get_role())}
            }
        );
        const auto last_words_result = ai.ask_player(*white_wolf_king, "day_last_words_instruction", last_words_instruction);
        try {
            Event last_words_event;
            last_words_event.type = EventType::BROADCAST;
            last_words_event.name = "last_words";
            last_words_event.actor_id = white_wolf_king->get_id();
            last_words_event.speaker = std::to_string(white_wolf_king->get_id()) + "号(白狼王遗言)";
            last_words_event.message = parse_speech_from_reply(last_words_result.raw_reply);
            event_bus.publish(std::move(last_words_event));
        } catch (...) {
            event_bus.append_public_record("系统", std::to_string(white_wolf_king->get_id()) + "号白狼王遗言解析失败。");
        }

        if (callbacks.handle_win_if_needed()) {
            return true;
        }

        event_bus.broadcast("系统", "白狼王自爆，白天发言立即中止，直接进入黑夜。");
        context.day_count++;
        context.state = GameState::NIGHT_PHASE;
        return true;
    } catch (...) {
        return false;
    }
}

bool handle_hunter_death(
    Event& event,
    GameContext& context,
    EventBus& event_bus,
    AIOrchestrator& ai,
    const SkillHookCallbacks& callbacks
) {
    if (event.type != EventType::PLAYER_DIED) {
        return false;
    }

    const int dead_id = event.actor_id;
    Player* hunter = context.find_player(dead_id);
    if (hunter == nullptr || hunter->get_role() != Role::HUNTER) {
        return false;
    }

    const bool allow_hunter = event.payload.value("allow_hunter", true);
    const std::string cause = event.payload.value("cause", "");
    if (!allow_hunter || cause == "poison") {
        return false;
    }

    const std::string instruction = ai.render_named_template(
        "hunter_shoot_instruction",
        {
            {"player_id", std::to_string(hunter->get_id())},
            {"alive_list", join_player_ids(context.alive_players())}
        }
    );

    const auto result = ai.ask_player(*hunter, "hunter_shoot_instruction", instruction);
    try {
        json parsed = json::parse(clean_response(result.raw_reply));
        const std::string thought = parsed.contains("thought") ? parsed["thought"].get<std::string>() : "（未提供思考）";
        LOG_INFO("[GAME][P%d][THOUGHT][hunter_shoot] %s", hunter->get_id(), thought.c_str());
        if (!parsed.contains("data") || !parsed["data"].value("shoot", false)) {
            LOG_INFO("[GAME][P%d][DECISION][hunter_shoot] shoot=false", hunter->get_id());
            return false;
        }

        const int target_id = parsed["data"].value("target", -1);
        LOG_INFO("[GAME][P%d][DECISION][hunter_shoot] shoot=true target=%d", hunter->get_id(), target_id);
        if (!context.is_player_alive(target_id)) {
            return false;
        }

        const std::string hunter_message =
            "猎人 " + std::to_string(dead_id) + " 号发动技能，开枪带走了 " +
            std::to_string(target_id) + " 号！";
        if (context.state == GameState::NIGHT_PHASE) {
            context.pending_day_messages.push_back(hunter_message);
        } else {
            event_bus.broadcast("系统", hunter_message);
        }
        callbacks.publish_player_death(target_id, "hunter_shot", true);
        return false;
    } catch (...) {
        LOG_WARN("[GAME][P%d][DECISION][hunter_shoot] parse_failed", hunter->get_id());
        return false;
    }
}
}

void SkillHooks::register_hooks(
    GameContext& context,
    EventBus& event_bus,
    AIOrchestrator& ai,
    const SkillHookCallbacks& callbacks
) {
    auto named_hook = [&context, &event_bus, &ai, callbacks](Event& event, GameContext&) {
        return handle_white_wolf_king_interrupt(event, context, event_bus, ai, callbacks);
    };

    event_bus.subscribe("player_speak", named_hook);
    event_bus.subscribe("last_words", named_hook);
    event_bus.subscribe("sheriff_speech", named_hook);
    event_bus.subscribe("sheriff_pk_speech", named_hook);
    event_bus.subscribe(EventType::PLAYER_DIED, [&context, &event_bus, &ai, callbacks](Event& event, GameContext&) {
        return handle_hunter_death(event, context, event_bus, ai, callbacks);
    });
}
