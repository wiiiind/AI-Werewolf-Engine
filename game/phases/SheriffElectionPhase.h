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
    std::function<bool(int, const std::string&, bool)> publish_player_death;
    std::function<bool()> handle_win_if_needed;
};

class SheriffElectionPhase {
public:
    SheriffElectionPhase(GameContext& context, EventBus& event_bus, AIOrchestrator& ai, const SheriffElectionPhaseCallbacks& callbacks);

    void execute();

private:
    std::vector<int> collect_candidates();
    bool run_candidate_speeches(const std::vector<int>& candidates, std::string& election_history);
    std::vector<int> collect_withdrawals(const std::vector<int>& candidates, const std::string& election_history);
    void publish_deferred_first_night_deaths();
    void run_pending_first_night_last_words();

    GameContext& m_context;
    EventBus& m_event_bus;
    AIOrchestrator& m_ai;
    SheriffElectionPhaseCallbacks m_callbacks;
};

#endif
