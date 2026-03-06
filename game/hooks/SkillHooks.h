#ifndef SKILL_HOOKS_H
#define SKILL_HOOKS_H

#include <functional>
#include <string>
#include "GameContext.h"
#include "EventBus.h"
#include "AIOrchestrator.h"

struct SkillHookCallbacks {
    std::function<void(int)> handle_sheriff_death;
    std::function<bool(int, const std::string&, bool)> publish_player_death;
    std::function<bool()> handle_win_if_needed;
};

class SkillHooks {
public:
    static void register_hooks(
        GameContext& context,
        EventBus& event_bus,
        AIOrchestrator& ai,
        const SkillHookCallbacks& callbacks
    );
};

#endif
