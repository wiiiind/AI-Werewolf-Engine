#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "GameContext.h"

using json = nlohmann::json;

enum class EventType {
    BROADCAST,
    PUBLIC_RECORD,
    PRIVATE_RECORD,
    PLAYER_DIED,
    PHASE_ENTER,
    CUSTOM
};

struct Event {
    EventType type = EventType::CUSTOM;
    std::string name;
    std::string speaker;
    std::string message;
    int actor_id = -1;
    int target_id = -1;
    bool visible_to_all = true;
    json payload = json::object();
};

class EventBus {
public:
    using Hook = std::function<bool(Event&, GameContext&)>;
    using HookId = std::size_t;

    explicit EventBus(GameContext& context);

    HookId subscribe(EventType type, Hook hook);
    HookId subscribe(const std::string& name, Hook hook);
    bool unsubscribe(HookId hook_id);

    // Return true when a hook intercepts the event and the default pipeline should stop.
    bool publish(Event event);

    void broadcast(const std::string& speaker, const std::string& message);
    void append_public_record(const std::string& speaker, const std::string& message);
    void append_private_record(Player& player, const std::string& message);

    const json& history() const;

private:
    struct HookEntry {
        HookId id = 0;
        Hook hook;
    };

    bool run_type_hooks(Event& event);
    bool run_named_hooks(Event& event);
    std::string to_history_line(const std::string& speaker, const std::string& message) const;

private:
    GameContext& m_context;
    HookId m_next_hook_id = 1;
    std::unordered_map<EventType, std::vector<HookEntry>> m_type_hooks;
    std::unordered_map<std::string, std::vector<HookEntry>> m_named_hooks;
};

#endif
