#ifndef SHERIFF_ELECTION_PHASE_H
#define SHERIFF_ELECTION_PHASE_H

#include <functional>
#include <string>
#include "GameContext.h"
#include "EventBus.h"
#include "AIOrchestrator.h"

struct SheriffElectionPhaseCallbacks {
    std::function<std::string(Role)> role_to_string;
    std::function<void(int)> handle_sheriff_death;
};

class SheriffElectionPhase {
public:
    SheriffElectionPhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const SheriffElectionPhaseCallbacks& callbacks);

    void execute();

private:
    GameContext& m_context;
    EventBus& m_event_bus;
    AIOrchestrator& m_ai;
    SheriffElectionPhaseCallbacks m_callbacks;
};

#endif
