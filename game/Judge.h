#ifndef JUDGE_H
#define JUDGE_H

#include <string>
#include "GameContext.h"
#include "EventBus.h"
#include "RuleEngine.h"
#include "AIOrchestrator.h"

class Judge {
public:
    Judge();
    ~Judge() = default;

    void start_game();

private:
    void init_config();

    Role string_to_role(const std::string& str) const;
    std::string role_to_string(Role role) const;

    void run_night();
    void run_sheriff_election();
    void run_day_speak();
    void run_vote();

    void handle_sheriff_death(int dead_id);
    void handle_hunter_shoot(int hunter_id);
    bool handle_win_if_needed();

private:
    GameContext m_context;
    EventBus m_event_bus;
    AIOrchestrator m_ai;
};

#endif
