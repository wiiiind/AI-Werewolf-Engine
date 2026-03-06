#ifndef VOTE_PHASE_H
#define VOTE_PHASE_H

#include <functional>
#include <string>
#include "GameContext.h"
#include "EventBus.h"
#include "AIOrchestrator.h"

struct VotePhaseCallbacks {
    std::function<std::string(Role)> role_to_string;
    std::function<bool(int, const std::string&, bool)> publish_player_death;
    std::function<bool()> handle_win_if_needed;
};

class VotePhase {
public:
    VotePhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const VotePhaseCallbacks& callbacks);

    void execute();

private:
    GameContext& m_context;
    EventBus& m_event_bus;
    AIOrchestrator& m_ai;
    VotePhaseCallbacks m_callbacks;
};

#endif
