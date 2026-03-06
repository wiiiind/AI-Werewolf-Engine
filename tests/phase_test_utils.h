#ifndef PHASE_TEST_UTILS_H
#define PHASE_TEST_UTILS_H

#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include "AIOrchestrator.h"
#include "EventBus.h"
#include "GameContext.h"
#include "log.h"

struct ScriptedReply {
    std::string raw_reply;
    int delay_ms = 0;
};

class FakeAIOrchestrator : public AIOrchestrator {
public:
    FakeAIOrchestrator(GameContext& context, EventBus& event_bus)
        : AIOrchestrator(context, event_bus, ConcurrencyConfig{2}) {}

    void add_reply(int player_id, const std::string& prompt_name, const std::string& raw_reply, int delay_ms = 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_replies[make_key(player_id, prompt_name)].push_back({raw_reply, delay_ms});
    }

    std::string render_named_template(const std::string& template_name, const TemplateValues& values) const override {
        (void)values;
        return template_name;
    }

    AIResult ask_player(
        Player& player,
        const std::string& prompt_name,
        const std::string& instruction,
        const AskOptions& options
    ) const override {
        (void)instruction;
        (void)options;

        ScriptedReply scripted_reply;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_replies.find(make_key(player.get_id(), prompt_name));
            if (it == m_replies.end() || it->second.empty()) {
                throw std::runtime_error(
                    "missing scripted reply for player=" + std::to_string(player.get_id()) +
                    " prompt=" + prompt_name
                );
            }
            scripted_reply = it->second.front();
            it->second.pop_front();
        }

        if (scripted_reply.delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(scripted_reply.delay_ms));
        }

        AIResult result;
        result.player_id = player.get_id();
        result.prompt_name = prompt_name;
        result.instruction = instruction;
        result.raw_reply = scripted_reply.raw_reply;
        result.ok = true;
        return result;
    }

private:
    static std::string make_key(int player_id, const std::string& prompt_name) {
        return std::to_string(player_id) + "|" + prompt_name;
    }

    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, std::deque<ScriptedReply>> m_replies;
};

inline bool history_contains(const json& history, const std::string& needle) {
    for (const auto& entry : history) {
        if (entry.contains("content") && entry["content"].is_string()) {
            if (entry["content"].get<std::string>().find(needle) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

[[noreturn]] inline void fail_test(const std::string& message) {
    throw std::runtime_error(message);
}

inline void expect_true(bool condition, const std::string& message) {
    if (!condition) {
        fail_test(message);
    }
}

inline std::string role_to_string_for_test(Role role) {
    switch (role) {
        case Role::WOLF: return "狼人";
        case Role::WHITE_WOLF_KING: return "白狼王";
        case Role::VILLAGER: return "平民";
        case Role::SEER: return "预言家";
        case Role::WITCH: return "女巫";
        case Role::HUNTER: return "猎人";
        case Role::GUARD: return "守卫";
        case Role::IDIOT: return "白痴";
    }
    return "未知";
}

inline void init_test_log(const std::string& file_name) {
    static std::once_flag once;
    std::call_once(once, [&file_name]() {
        const std::filesystem::path log_dir = std::filesystem::temp_directory_path() / "werewolf_phase_tests";
        std::filesystem::create_directories(log_dir);
        const std::filesystem::path log_file = log_dir / file_name;
        if (!Log::get_instance()->init(log_file.string().c_str(), 1024, 1000, 0)) {
            fail_test("failed to init log");
        }
    });
}

#endif
