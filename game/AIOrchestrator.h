#ifndef AI_ORCHESTRATOR_H
#define AI_ORCHESTRATOR_H

#include <future>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "GameContext.h"
#include "EventBus.h"
#include "ai_client.h"

using json = nlohmann::json;

class AIOrchestrator {
public:
    using TemplateValues = std::unordered_map<std::string, std::string>;

    struct AIResult {
        int player_id = -1;
        std::string prompt_name;
        std::string instruction;
        std::string raw_reply;
        bool ok = false;
        std::string error;
        json parsed_reply = json::object();
    };

    AIOrchestrator(GameContext& context, EventBus& event_bus)
        : m_context(context), m_event_bus(event_bus) {}

    std::string render_template(const std::string& template_text, const TemplateValues& values) const {
        std::string rendered = template_text;
        for (const auto& [key, value] : values) {
            const std::string placeholder = "{" + key + "}";
            std::size_t pos = 0;
            while ((pos = rendered.find(placeholder, pos)) != std::string::npos) {
                rendered.replace(pos, placeholder.size(), value);
                pos += value.size();
            }
        }
        return rendered;
    }

    std::string render_named_template(const std::string& template_name, const TemplateValues& values) const {
        return render_template(m_context.templates.at(template_name).get<std::string>(), values);
    }

    json build_request(Player& player, const std::string& instruction) const {
        json request = json::array();
        request.push_back({{"role", "system"}, {"content", m_context.game_config_string}});

        std::string private_profile = render_named_template(
            "private_profile",
            {
                {"player_id", std::to_string(player.get_id())},
                {"role_name", role_to_string(player.get_role())},
                {"personality", player.get_personality()}
            }
        );
        request.push_back({{"role", "system"}, {"content", private_profile}});

        for (const auto& message : m_context.global_history) {
            request.push_back(message);
        }
        for (const auto& message : player.get_private_memory()) {
            request.push_back(message);
        }

        request.push_back({{"role", "user"}, {"content", instruction}});
        return request;
    }

    AIResult ask_player(Player& player, const std::string& prompt_name, const std::string& instruction) const {
        AIResult result;
        result.player_id = player.get_id();
        result.prompt_name = prompt_name;
        result.instruction = instruction;

        try {
            result.raw_reply = AIClient::chat_sync(build_request(player, instruction));
            result.ok = true;
        } catch (const std::exception& ex) {
            result.error = ex.what();
        } catch (...) {
            result.error = "unknown_error";
        }

        return result;
    }

    std::vector<AIResult> ask_all_players_concurrently(
        const std::vector<Player*>& players,
        const std::string& prompt_name,
        const std::function<std::string(Player&)>& instruction_builder
    ) const {
        std::vector<std::future<AIResult>> futures;
        futures.reserve(players.size());

        for (Player* player : players) {
            futures.push_back(std::async(std::launch::async, [this, player, prompt_name, instruction_builder]() {
                return ask_player(*player, prompt_name, instruction_builder(*player));
            }));
        }

        std::vector<AIResult> results;
        results.reserve(players.size());
        for (auto& future : futures) {
            results.push_back(future.get());
        }
        return results;
    }

private:
    std::string role_to_string(Role role) const {
        switch (role) {
            case Role::WOLF: return "狼人";
            case Role::WHITE_WOLF_KING: return "白狼王";
            case Role::SEER: return "预言家";
            case Role::WITCH: return "女巫";
            case Role::HUNTER: return "猎人";
            case Role::GUARD: return "守卫";
            case Role::IDIOT: return "白痴";
            default: return "平民";
        }
    }

private:
    GameContext& m_context;
    EventBus& m_event_bus;
};

#endif
