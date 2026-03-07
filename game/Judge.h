#ifndef JUDGE_H
#define JUDGE_H

#include <string>
#include "GameContext.h"
#include "EventBus.h"
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
    bool publish_player_death(int dead_id, const std::string& cause, bool allow_hunter);
    bool handle_win_if_needed();
    void maybe_compress_public_history();
    bool compress_history_block(std::vector<std::string>& lines, const std::string& stage_name, bool& compressed_flag);

private:
    GameContext m_context;
    EventBus m_event_bus;
    AIOrchestrator m_ai;
};

#endif
