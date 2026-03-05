#ifndef PLAYER_H
#define PLAYER_H

#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

enum class Role { WOLF, WHITE_WOLF_KING, VILLAGER, SEER, WITCH, HUNTER, GUARD, IDIOT };

class Player {
public:
    Player(int id, Role role, const std::string& personality)
        : m_id(id), m_role(role), m_personality(personality), m_is_alive(true) {}

    int get_id() const { return m_id; }
    Role get_role() const { return m_role; }
    std::string get_personality() const { return m_personality; }
    bool is_alive() const { return m_is_alive; }
    void kill() { m_is_alive = false; }

    void add_private_memory(const std::string& content) {
        m_private_history.push_back({{"role", "assistant"}, {"content", content}});
    }

    const json& get_private_memory() const { return m_private_history; }


private:
    int m_id;
    Role m_role;
    bool m_is_alive;
    std::string m_personality;
    json m_private_history;
};

#endif