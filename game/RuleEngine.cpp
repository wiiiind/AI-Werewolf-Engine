#include "RuleEngine.h"
#include <algorithm>

namespace {
int find_vote_target_of_player(int player_id, const std::map<int, std::vector<int>>& vote_records) {
    for (const auto& [candidate, voters] : vote_records) {
        if (std::find(voters.begin(), voters.end(), player_id) != voters.end()) {
            return candidate;
        }
    }
    return -1;
}
}

bool RuleEngine::is_wolf(Role role) {
    return role == Role::WOLF || role == Role::WHITE_WOLF_KING;
}

bool RuleEngine::is_god(Role role) {
    return role == Role::SEER || role == Role::WITCH || role == Role::HUNTER || role == Role::GUARD || role == Role::IDIOT;
}

VoteResolution RuleEngine::resolve_top_votes(const std::vector<int>& vote_counts, int min_candidate_id, int max_candidate_id) {
    VoteResolution resolution;

    if (vote_counts.empty() || min_candidate_id > max_candidate_id) {
        return resolution;
    }

    for (int candidate = min_candidate_id; candidate <= max_candidate_id; ++candidate) {
        if (candidate < 0 || candidate >= static_cast<int>(vote_counts.size())) {
            continue;
        }

        const int votes = vote_counts[candidate];
        if (votes > resolution.max_votes) {
            resolution.max_votes = votes;
            resolution.leaders = {candidate};
        } else if (votes == resolution.max_votes && votes > 0) {
            resolution.leaders.push_back(candidate);
        }
    }

    return resolution;
}

VoteResolution RuleEngine::resolve_top_votes(const std::map<int, std::vector<int>>& vote_records) {
    VoteResolution resolution;

    for (const auto& [candidate, voters] : vote_records) {
        if (candidate < 0) {
            continue;
        }

        const int votes = static_cast<int>(voters.size());
        if (votes > resolution.max_votes) {
            resolution.max_votes = votes;
            resolution.leaders = {candidate};
        } else if (votes == resolution.max_votes && votes > 0) {
            resolution.leaders.push_back(candidate);
        }
    }

    return resolution;
}

VoteResolution RuleEngine::apply_sheriff_tiebreak(
    const VoteResolution& resolution,
    int sheriff_id,
    const std::map<int, std::vector<int>>& vote_records,
    const GameContext& context
) {
    if (resolution.leaders.size() <= 1 || sheriff_id == -1 || !context.is_player_alive(sheriff_id)) {
        return resolution;
    }

    const int sheriff_vote = find_vote_target_of_player(sheriff_id, vote_records);
    if (sheriff_vote == -1) {
        return resolution;
    }

    if (std::find(resolution.leaders.begin(), resolution.leaders.end(), sheriff_vote) == resolution.leaders.end()) {
        return resolution;
    }

    VoteResolution resolved = resolution;
    resolved.leaders = {sheriff_vote};
    return resolved;
}

NightDeathResolution RuleEngine::resolve_night_deaths(
    const GameContext& context,
    int wolf_target,
    int poison_target,
    int guard_target,
    bool witch_used_save_tonight
) {
    NightDeathResolution result;

    if (wolf_target != -1) {
        const bool guarded = (wolf_target == guard_target);
        const bool saved = witch_used_save_tonight;

        if (guarded && saved) {
            result.same_guard_save_conflict = true;
            result.deaths.push_back(wolf_target);
        } else if (guarded) {
            result.wolf_kill_blocked_by_guard = true;
        } else if (saved) {
            result.wolf_kill_blocked_by_witch = true;
        } else {
            result.deaths.push_back(wolf_target);
        }
    }

    if (poison_target != -1 && context.is_player_alive(poison_target)) {
        if (std::find(result.deaths.begin(), result.deaths.end(), poison_target) == result.deaths.end()) {
            result.deaths.push_back(poison_target);
        }
    }

    return result;
}

WinCheckResult RuleEngine::check_win_condition(const GameContext& context) {
    int wolf_count = 0;
    int god_count = 0;
    int villager_count = 0;

    for (const auto& player : context.players) {
        if (!player->is_alive()) {
            continue;
        }

        if (is_wolf(player->get_role())) {
            ++wolf_count;
        } else if (player->get_role() == Role::VILLAGER) {
            ++villager_count;
        } else if (is_god(player->get_role())) {
            ++god_count;
        }
    }

    if (wolf_count == 0) {
        return {true, "GOOD", "狼人阵营全部覆灭"};
    }
    if (god_count == 0) {
        return {true, "WOLF", "神职全部阵亡"};
    }
    if (villager_count == 0) {
        return {true, "WOLF", "平民全部阵亡"};
    }

    return {false, "", ""};
}
