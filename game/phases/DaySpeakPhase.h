#ifndef DAY_SPEAK_PHASE_H
#define DAY_SPEAK_PHASE_H

#include <functional>
#include <string>
#include "GameContext.h"
#include "EventBus.h"
#include "AIOrchestrator.h"

struct DaySpeakPhaseCallbacks {
    std::function<std::string(Role)> role_to_string;
    std::function<bool()> handle_win_if_needed;
};

class DaySpeakPhase {
public:
    DaySpeakPhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const DaySpeakPhaseCallbacks& callbacks);

    void execute();

private:
    GameContext& m_context;
    EventBus& m_event_bus;
    AIOrchestrator& m_ai;
    DaySpeakPhaseCallbacks m_callbacks;
};

#endif
