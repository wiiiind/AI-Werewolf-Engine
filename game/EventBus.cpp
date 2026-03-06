#include "EventBus.h"

namespace {
const char* event_type_to_string(EventType type) {
    switch (type) {
        case EventType::BROADCAST: return "BROADCAST";
        case EventType::PUBLIC_RECORD: return "PUBLIC_RECORD";
        case EventType::PRIVATE_RECORD: return "PRIVATE_RECORD";
        case EventType::PLAYER_DIED: return "PLAYER_DIED";
        case EventType::PHASE_ENTER: return "PHASE_ENTER";
        default: return "CUSTOM";
    }
}

bool should_print_broadcast_to_console(const Event& event) {
    return event.speaker == "系统";
}
}
#include <algorithm>
#include <iostream>
#include "log.h"

EventBus::EventBus(GameContext& context) : m_context(context) {}

EventBus::HookId EventBus::subscribe(EventType type, Hook hook) {
    const HookId hook_id = m_next_hook_id++;
    m_type_hooks[type].push_back({hook_id, std::move(hook)});
    return hook_id;
}

EventBus::HookId EventBus::subscribe(const std::string& name, Hook hook) {
    const HookId hook_id = m_next_hook_id++;
    m_named_hooks[name].push_back({hook_id, std::move(hook)});
    return hook_id;
}

bool EventBus::unsubscribe(HookId hook_id) {
    auto remove_from = [hook_id](auto& hooks) {
        bool removed = false;
        for (auto& [key, entries] : hooks) {
            const auto new_end = std::remove_if(entries.begin(), entries.end(), [hook_id](const HookEntry& entry) {
                return entry.id == hook_id;
            });
            if (new_end != entries.end()) {
                entries.erase(new_end, entries.end());
                removed = true;
            }
        }
        return removed;
    };

    return remove_from(m_type_hooks) || remove_from(m_named_hooks);
}

bool EventBus::publish(Event event) {
    LOG_INFO("[GAME][EVENT] type=%s name=%s actor=%d target=%d visible=%s speaker=%s message=%s payload=%s",
             event_type_to_string(event.type),
             event.name.c_str(),
             event.actor_id,
             event.target_id,
             event.visible_to_all ? "true" : "false",
             event.speaker.c_str(),
             event.message.c_str(),
             event.payload.dump().c_str());
    if (run_type_hooks(event) || run_named_hooks(event)) {
        LOG_INFO("[GAME][EVENT] intercepted type=%s name=%s", event_type_to_string(event.type), event.name.c_str());
        return true;
    }

    switch (event.type) {
        case EventType::BROADCAST:
            if (should_print_broadcast_to_console(event)) {
                std::cout << "\n[" << event.speaker << "]: " << event.message << std::endl;
            }
            if (event.visible_to_all) {
                m_context.global_history.push_back({{"role", "user"}, {"content", to_history_line(event.speaker, event.message)}});
            }
            break;
        case EventType::PUBLIC_RECORD:
            m_context.global_history.push_back({{"role", "user"}, {"content", to_history_line(event.speaker, event.message)}});
            break;
        case EventType::PRIVATE_RECORD: {
            Player* player = m_context.find_player(event.target_id);
            if (player) {
                player->add_private_memory(event.message);
            }
            break;
        }
        default:
            break;
    }

    return false;
}

void EventBus::broadcast(const std::string& speaker, const std::string& message) {
    Event event;
    event.type = EventType::BROADCAST;
    event.name = "broadcast";
    event.speaker = speaker;
    event.message = message;
    event.visible_to_all = true;
    publish(std::move(event));
}

void EventBus::append_public_record(const std::string& speaker, const std::string& message) {
    Event event;
    event.type = EventType::PUBLIC_RECORD;
    event.name = "public_record";
    event.speaker = speaker;
    event.message = message;
    publish(std::move(event));
}

void EventBus::append_private_record(Player& player, const std::string& message) {
    Event event;
    event.type = EventType::PRIVATE_RECORD;
    event.name = "private_record";
    event.target_id = player.get_id();
    event.message = message;
    event.visible_to_all = false;
    publish(std::move(event));
}

const json& EventBus::history() const {
    return m_context.global_history;
}

bool EventBus::run_type_hooks(Event& event) {
    auto it = m_type_hooks.find(event.type);
    if (it == m_type_hooks.end()) {
        return false;
    }

    for (auto& entry : it->second) {
        if (entry.hook && entry.hook(event, m_context)) {
            return true;
        }
    }
    return false;
}

bool EventBus::run_named_hooks(Event& event) {
    if (event.name.empty()) {
        return false;
    }

    auto it = m_named_hooks.find(event.name);
    if (it == m_named_hooks.end()) {
        return false;
    }

    for (auto& entry : it->second) {
        if (entry.hook && entry.hook(event, m_context)) {
            return true;
        }
    }
    return false;
}

std::string EventBus::to_history_line(const std::string& speaker, const std::string& message) const {
    if (speaker.empty()) {
        return message;
    }
    return speaker + ": " + message;
}
