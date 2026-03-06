#ifndef AI_ORCHESTRATOR_H
#define AI_ORCHESTRATOR_H

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "GameContext.h"
#include "EventBus.h"
#include "ai_client.h"
#include "log.h"

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

        for (const auto& message : m_context.global_history) {
            request.push_back(message);
        }

        std::string sheriff_status = m_context.sheriff_id == -1
            ? "【当前警长状态】本局当前没有警长，警徽已流失或尚未产生。"
            : "【当前警长状态】当前警长是 " + std::to_string(m_context.sheriff_id) + " 号玩家。";
        request.push_back({{"role", "user"}, {"content", sheriff_status}});

        std::string private_profile = render_named_template(
            "private_profile",
            {
                {"player_id", std::to_string(player.get_id())},
                {"role_name", role_to_string(player.get_role())},
                {"personality", player.get_personality()}
            }
        );
        request.push_back({{"role", "system"}, {"content", private_profile}});

        if (player.get_role() == Role::WOLF || player.get_role() == Role::WHITE_WOLF_KING) {
            std::vector<int> alive_teammates;
            std::vector<int> dead_teammates;
            for (const auto& candidate_ptr : m_context.players) {
                const Player* candidate = candidate_ptr.get();
                if (!candidate || candidate->get_id() == player.get_id()) {
                    continue;
                }
                if (candidate->get_role() != Role::WOLF && candidate->get_role() != Role::WHITE_WOLF_KING) {
                    continue;
                }
                if (candidate->is_alive()) {
                    alive_teammates.push_back(candidate->get_id());
                } else {
                    dead_teammates.push_back(candidate->get_id());
                }
            }

            auto join_ids_local = [](const std::vector<int>& ids) {
                std::string result;
                for (std::size_t i = 0; i < ids.size(); ++i) {
                    if (i > 0) {
                        result += " ";
                    }
                    result += std::to_string(ids[i]);
                }
                return result;
            };

            std::string teammate_status = "【身份提醒】你的狼队友有：";
            teammate_status += "【存活：" + (alive_teammates.empty() ? std::string("无") : join_ids_local(alive_teammates)) + "号】";
            teammate_status += "【死亡：" + (dead_teammates.empty() ? std::string("无") : join_ids_local(dead_teammates)) + "号】";
            if (alive_teammates.empty()) {
                teammate_status += "。你现在是场上唯一存活的狼人，必须独自完成推进。";
            } else {
                teammate_status += "。请保护存活队友，并继续协同行动。";
            }
            request.push_back({{"role", "assistant"}, {"content", teammate_status}});
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

        const json request = build_request(player, instruction);
        LOG_INFO("[GAME][P%d][REQUEST][%s] %s", player.get_id(), prompt_name.c_str(), request.dump().c_str());

        try {
            result.raw_reply = AIClient::chat_sync(request);
            result.ok = true;
            LOG_INFO("[GAME][P%d][REPLY][%s] %s", player.get_id(), prompt_name.c_str(), result.raw_reply.c_str());
        } catch (const std::exception& ex) {
            result.error = ex.what();
            LOG_ERROR("[GAME][P%d][ERROR][%s] %s", player.get_id(), prompt_name.c_str(), result.error.c_str());
        } catch (...) {
            result.error = "unknown_error";
            LOG_ERROR("[GAME][P%d][ERROR][%s] %s", player.get_id(), prompt_name.c_str(), result.error.c_str());
        }

        return result;
    }

    std::vector<AIResult> ask_all_players_concurrently(
        const std::vector<Player*>& players,
        const std::string& prompt_name,
        const std::function<std::string(Player&)>& instruction_builder
    ) const {
        std::vector<AIResult> results;
        results.reserve(players.size());

        for (Player* player : players) {
            results.push_back(ask_player(*player, prompt_name, instruction_builder(*player)));
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
