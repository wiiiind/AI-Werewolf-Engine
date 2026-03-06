#ifndef NIGHT_PHASE_H
#define NIGHT_PHASE_H

#include <functional>
#include <string>
#include "GameContext.h"
#include "EventBus.h"
#include "AIOrchestrator.h"

struct NightPhaseCallbacks {
    std::function<bool(int, const std::string&, bool)> publish_player_death;
};

class NightPhase {
public:
    NightPhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const NightPhaseCallbacks& callbacks);

    void execute();

private:
    GameContext& m_context;
    EventBus& m_event_bus;
    AIOrchestrator& m_ai;
    NightPhaseCallbacks m_callbacks;
};

#endif
