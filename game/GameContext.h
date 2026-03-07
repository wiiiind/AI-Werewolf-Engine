#ifndef GAME_CONTEXT_H
#define GAME_CONTEXT_H

#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "player.h"

using json = nlohmann::json;

enum class GameState {
    NIGHT_PHASE,
    SHERIFF_ELECTION_PHASE,
    DAY_SPEAK_PHASE,
    DAY_VOTE_PHASE,
    GAME_OVER
};

class GameContext {
public:
    GameContext() = default;

    Player* find_player(int id);
    const Player* find_player(int id) const;

    std::vector<Player*> alive_players();
    std::vector<const Player*> alive_players() const;
    std::vector<Player*> alive_players_with_roles(const std::vector<Role>& roles);

    bool is_player_alive(int id) const;
    int player_count() const;

public:
    std::vector<std::unique_ptr<Player>> players;
    json templates = json::object();
    json global_history = json::array();
    std::vector<std::string> pending_day_messages;

    GameState state = GameState::NIGHT_PHASE;
    int day_count = 1;
    int dead_last_night = -1;
    std::vector<int> deaths_last_night;

    bool witch_save_used = false;
    bool witch_poison_used = false;
    bool enable_first_night_last_words = true;
    bool first_night_deaths_deferred = false;
    bool first_night_last_words_pending = false;
    std::vector<int> first_night_last_words_ids;
    int first_night_last_words_poison_target = -1;
    bool sheriff_election_resume_vote_only = false;
    bool sheriff_election_resume_explosion_loses_badge = false;
    std::vector<int> deferred_sheriff_candidates;
    int exploded_wwk_commander_id = -1;
    std::vector<std::string> opening_sheriff_speech_lines;
    std::vector<std::string> day1_day_speech_lines;
    bool opening_sheriff_speech_compressed = false;
    bool day1_day_speech_compressed = false;

    std::string game_config_string;
    int sheriff_id = -1;
    int guard_target = -1;
    int last_guard_target = -1;
    int last_night_poison_target = -1;
};

#endif
