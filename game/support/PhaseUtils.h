#ifndef PHASE_UTILS_H
#define PHASE_UTILS_H

#include <map>
#include <string>
#include <vector>
#include "player.h"

std::string join_ids(const std::vector<int>& ids, const std::string& sep = " ");
std::string join_player_ids(const std::vector<Player*>& players, const std::string& sep = " ");
std::string format_vote_records(const std::map<int, std::vector<int>>& records);
std::string build_death_message(const std::vector<int>& deaths_last_night);
int current_time_tail_digit();
bool is_single_forward_double_reverse();
std::vector<int> sort_ids_by_time_rule(std::vector<int> ids);
std::vector<int> build_circular_order_from_anchor(int anchor_id, int player_count, bool forward);
int select_last_dead_anchor(const std::vector<int>& deaths_last_night, int poison_target, bool forward);

#endif
