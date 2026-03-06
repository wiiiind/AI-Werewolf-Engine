#ifndef RULE_ENGINE_H
#define RULE_ENGINE_H

#include <map>
#include <string>
#include <vector>
#include "GameContext.h"

struct VoteResolution {
    int max_votes = 0;
    std::vector<int> leaders;
};

struct NightDeathResolution {
    std::vector<int> deaths;
    bool wolf_kill_blocked_by_guard = false;
    bool wolf_kill_blocked_by_witch = false;
    bool same_guard_save_conflict = false;
};

struct WinCheckResult {
    bool game_over = false;
    std::string winner;
    std::string reason;
};

class RuleEngine {
public:
    static bool is_wolf(Role role);
    static bool is_god(Role role);

    static VoteResolution resolve_top_votes(const std::vector<int>& vote_counts, int min_candidate_id, int max_candidate_id);
    static VoteResolution resolve_top_votes(const std::map<int, std::vector<int>>& vote_records);
    static VoteResolution apply_sheriff_tiebreak(
        const VoteResolution& resolution,
        int sheriff_id,
        const std::map<int, std::vector<int>>& vote_records,
        const GameContext& context
    );

    static NightDeathResolution resolve_night_deaths(
        const GameContext& context,
        int wolf_target,
        int poison_target,
        int guard_target,
        bool witch_used_save_tonight
    );

    static WinCheckResult check_win_condition(const GameContext& context);
};

#endif
