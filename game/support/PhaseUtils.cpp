#include <algorithm>
#include <chrono>
#include <ctime>
#include "support/PhaseUtils.h"

std::string join_ids(const std::vector<int>& ids, const std::string& sep) {
    std::string result;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            result += sep;
        }
        result += std::to_string(ids[i]);
    }
    return result;
}

std::string join_player_ids(const std::vector<Player*>& players, const std::string& sep) {
    std::vector<int> ids;
    ids.reserve(players.size());
    for (Player* player : players) {
        ids.push_back(player->get_id());
    }
    return join_ids(ids, sep);
}

std::string format_vote_records(const std::map<int, std::vector<int>>& records) {
    std::string details;
    for (const auto& [candidate, voters] : records) {
        if (candidate == -1) {
            details += "【弃票/废票】: ";
        } else {
            details += "【投" + std::to_string(candidate) + "号】: ";
        }
        for (size_t i = 0; i < voters.size(); ++i) {
            details += std::to_string(voters[i]);
            if (i + 1 < voters.size()) {
                details += ", ";
            }
        }
        details += "; ";
    }
    return details;
}

std::string build_death_message(const std::vector<int>& deaths_last_night) {
    if (deaths_last_night.empty()) {
        return "昨晚是平安夜";
    }

    std::string message = "昨晚死亡的是：";
    for (size_t i = 0; i < deaths_last_night.size(); ++i) {
        message += std::to_string(deaths_last_night[i]) + "号";
        if (i + 1 < deaths_last_night.size()) {
            message += "、";
        }
    }
    return message;
}

int current_time_tail_digit() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t current_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time = *std::localtime(&current_time);
    return local_time.tm_min % 10;
}

bool is_single_forward_double_reverse() {
    return current_time_tail_digit() % 2 == 1;
}

std::vector<int> sort_ids_by_time_rule(std::vector<int> ids) {
    std::sort(ids.begin(), ids.end());
    if (!is_single_forward_double_reverse()) {
        std::reverse(ids.begin(), ids.end());
    }
    return ids;
}

std::vector<int> build_circular_order_from_anchor(int anchor_id, int player_count, bool forward) {
    std::vector<int> order;
    order.reserve(player_count);
    for (int i = 1; i <= player_count; ++i) {
        int pid = forward
            ? (anchor_id - 1 + i) % player_count + 1
            : (anchor_id - 1 - i + player_count * 2) % player_count + 1;
        order.push_back(pid);
    }
    return order;
}

int select_last_dead_anchor(const std::vector<int>& deaths_last_night, int poison_target, bool forward) {
    if (deaths_last_night.empty()) {
        return -1;
    }
    if (poison_target != -1 && std::find(deaths_last_night.begin(), deaths_last_night.end(), poison_target) != deaths_last_night.end()) {
        return poison_target;
    }
    return forward
        ? *std::max_element(deaths_last_night.begin(), deaths_last_night.end())
        : *std::min_element(deaths_last_night.begin(), deaths_last_night.end());
}

std::string describe_death_context(const std::string& cause) {
    if (cause == "wolf") {
        return "昨晚你在黑夜中遇害，你确认已死亡。";
    }
    if (cause == "poison") {
        return "昨晚你被女巫毒杀，你确认已死亡。";
    }
    if (cause == "vote") {
        return "你在刚刚的投票表决中被放逐，你确认已死亡。";
    }
    if (cause == "explosion") {
        return "你被白狼王自爆带走，你确认已死亡。";
    }
    if (cause == "hunter_shot") {
        return "你被猎人开枪带走，你确认已死亡。";
    }
    return "你确认已死亡。";
}
