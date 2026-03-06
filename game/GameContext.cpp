#include "GameContext.h"

Player* GameContext::find_player(int id) {
    for (auto& player : players) {
        if (player->get_id() == id) {
            return player.get();
        }
    }
    return nullptr;
}

const Player* GameContext::find_player(int id) const {
    for (const auto& player : players) {
        if (player->get_id() == id) {
            return player.get();
        }
    }
    return nullptr;
}

std::vector<Player*> GameContext::alive_players() {
    std::vector<Player*> result;
    result.reserve(players.size());
    for (auto& player : players) {
        if (player->is_alive()) {
            result.push_back(player.get());
        }
    }
    return result;
}

std::vector<const Player*> GameContext::alive_players() const {
    std::vector<const Player*> result;
    result.reserve(players.size());
    for (const auto& player : players) {
        if (player->is_alive()) {
            result.push_back(player.get());
        }
    }
    return result;
}

std::vector<Player*> GameContext::alive_players_with_roles(const std::vector<Role>& roles) {
    std::vector<Player*> result;
    result.reserve(players.size());

    for (auto& player : players) {
        if (!player->is_alive()) {
            continue;
        }

        for (Role role : roles) {
            if (player->get_role() == role) {
                result.push_back(player.get());
                break;
            }
        }
    }

    return result;
}

bool GameContext::is_player_alive(int id) const {
    const Player* player = find_player(id);
    return player != nullptr && player->is_alive();
}

int GameContext::player_count() const {
    return static_cast<int>(players.size());
}
