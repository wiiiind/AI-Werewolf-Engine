#include "Judge.h"
#include "ai_client.h"
#include "string_utils.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <random>
#include <thread>
#include <map>
#include <numeric>

// 辅助函数：清洗 AI 返回的 Markdown 标签，只保留 JSON 部分
static std::string clean_response(const std::string& s) {
    size_t start = s.find('{');
    size_t end = s.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end >= start) {
        return s.substr(start, end - start + 1);
    }
    return s;
}

Judge::Judge() : m_state(GameState::NIGHT_PHASE), m_day_count(1), m_dead_last_night(-1), m_witch_save_used(false), m_witch_poison_used(false), m_enable_first_night_last_words(true), m_sheriff_id(-1), m_guard_target(-1), m_last_guard_target(-1) {
    init_config();
}

Judge::~Judge() {
}

void Judge::init_config() {
    // 1. 加载模板
    std::ifstream t_file("../resources/templates.json");
    t_file >> m_templates;
    m_global_history = json::array();

    // 2. 加载角色题库并洗牌
    std::ifstream r_file("../resources/roles.json");
    json roles_pool;
    r_file >> roles_pool;

    auto rng = std::default_random_engine(std::random_device{}());
    std::shuffle(roles_pool.begin(), roles_pool.end(), rng);

    // 3. 按照 12 人局标准分配 (1白狼王 3狼 4民 1预 1女 1猎 1守)
    std::vector<json> selected_configs;
    int wwk=0, w=0, v=0, s=0, wt=0, h=0, g=0; // g for Guard

    for(const auto& item : roles_pool) {
        std::string r = item["role"];
        if (r == "WHITE_WOLF_KING" && wwk < 1) { selected_configs.push_back(item); wwk++; }
        else if (r == "WOLF" && w < 3) { selected_configs.push_back(item); w++; } // 3只普狼
        else if (r == "SEER" && s < 1) { selected_configs.push_back(item); s++; }
        else if (r == "WITCH" && wt < 1) { selected_configs.push_back(item); wt++; }
        else if (r == "HUNTER" && h < 1) { selected_configs.push_back(item); h++; }
        else if (r == "GUARD" && g < 1) { selected_configs.push_back(item); g++; } // 守卫
        else if (r == "VILLAGER" && v < 4) { selected_configs.push_back(item); v++; } // 4个民
    }

    // 4. 再次洗牌（分配座位号）
    std::shuffle(selected_configs.begin(), selected_configs.end(), rng);

    std::cout << "正在初始化 12 人白狼王守卫局 (1白 3狼 4民 4神)..." << std::endl;

    for (int i = 0; i < selected_configs.size(); ++i) {
        m_players.push_back(new Player(i+1, string_to_role(selected_configs[i]["role"]), selected_configs[i]["personality"]));

        // 打印本局配置
        std::cout << "🪑 " << (i+1) << "号:[" << role_to_string(m_players[i]->get_role()) << "] "
                  << m_players[i]->get_personality().substr(0, 50) << "..." << std::endl;
    }
    std::cout << "✅ 12人白狼王局初始化完毕！" << std::endl;

    // 5. 动态生成本局《基本法》规则字符串
    std::map<Role, int> role_counts;
    for (const auto& p : m_players) {
        role_counts[p->get_role()]++;
    }

    std::string role_config_str;
    for (auto const&[role, count] : role_counts) {
        role_config_str += std::to_string(count) + role_to_string(role) + ", ";
    }
    if (!role_config_str.empty()) {
        role_config_str = role_config_str.substr(0, role_config_str.length() - 2);
    }

    // 传入的占位符：总人数、角色配置、编号上限
    m_game_config_string = str_utils::format(
        m_templates["system_init"].get<std::string>(),
        {
            std::to_string(m_players.size()),
            role_config_str,
            std::to_string(m_players.size())
        }
    );

    std::cout << "\n[系统规则]: " << m_game_config_string << std::endl;
}

// 缓存优化逻辑：前缀完全一致
json Judge::build_ai_request(Player* p, const std::string& instruction) {
    json request = json::array();

    request.push_back({{"role", "system"}, {"content", m_game_config_string}});

    // 1. 系统设定 (全员一致)
    request.push_back({{"role", "system"}, {"content", m_templates["system_init"].get<std::string>()}});

    // 2. 全局历史 (全员一致 - 白天发言、公开处决信息)
    for (const auto& msg : m_global_history) {
        request.push_back(msg);
    }

    // 3. 个人档案 (放在中间或后面均可，为了逻辑连贯放在这里)
    std::string role_name = role_to_string(p->get_role());
    std::string profile = str_utils::format(m_templates["private_profile"].get<std::string>(),
        { std::to_string(p->get_id()), role_name, p->get_personality() });

    // 这里我们把档案作为一条 System 提醒插入
    request.push_back({{"role", "system"}, {"content", profile}});

    // 4. 私有历史 (每个人不同 - 晚上的秘密)
    // 狼人会看到自己昨晚刀了谁，预言家会看到昨晚验了谁
    const auto& private_mem = p->get_private_memory();
    for (const auto& msg : private_mem) {
        request.push_back(msg);
    }

    // 5. 当前指令
    request.push_back({{"role", "user"}, {"content", instruction}});

    return request;
}

void Judge::run_night() {
    std::cout << "\n========== 第 " << m_day_count << " 天 黑夜降临 ==========" << std::endl;
    m_dead_last_night = -1;
    m_guard_target = -1; // 重置今晚守护

    // --- 1. 狼人集体决策（串行协商与平票重投逻辑） ---

    // 获取存活玩家列表字符串
    std::string alive_str = "";
    for(auto t: m_players) if(t->is_alive()) alive_str += std::to_string(t->get_id()) + " ";

    // 把活着的狼人单独拎出来，保证发言顺序固定
    std::vector<Player*> alive_wolves;
    for(auto t: m_players) {
        if(t->is_alive() && (t->get_role() == Role::WOLF || t->get_role() == Role::WHITE_WOLF_KING)) {
            alive_wolves.push_back(t);
        }
    }

    if (!alive_wolves.empty()) {
        std::string round1_history = ""; // 用于记录第一轮的投票详情
        bool consensus_reached = false;

        // 最多进行两轮投票
        for (int round = 1; round <= 2; ++round) {
            std::vector<int> wolf_votes(m_players.size() + 1, 0);       // 本轮计票器
            std::string current_round_actions = "";   // 本轮前面狼人的实时动作

            if (round == 2) {
                std::cout << "\n>> [系统] 狼人阵营第一轮发生平票，进入第二轮生死协商！" << std::endl;
            }

            // 存活狼人依次串行发言/指认
            for (auto p : alive_wolves) {
                // ------------------------------------------------------
                // [新增逻辑] 寻找当前 p 的队友
                std::string teammates_str = "";
                for (auto other : alive_wolves) {
                    if (other->get_id() != p->get_id()) {
                        teammates_str += std::to_string(other->get_id()) + " ";
                    }
                }
                if (teammates_str.empty()) teammates_str = "无 (你是场上唯一的狼)";
                // ------------------------------------------------------

                // 1. 提取基础模板指令 (注意：这里现在需要传入 3 个参数：天数、队友、存活列表)
                // 参数顺序必须和 templates.json 里的 {} 顺序一致
                std::string base_ins = str_utils::format(
                    m_templates["night_wolf_instruction"].get<std::string>(),
                    {
                        std::to_string(m_day_count), // 第 {} 晚
                        teammates_str,               // 你的狼同伴是：{}
                        alive_str,                   // 存活玩家：{}
                        std::to_string(p->get_id()),
        std::to_string(p->get_id()), role_to_string(p->get_role())
                    }
                );

                std::string dynamic_ins = base_ins + "\n\n【夜间沟通频道】\n";

                // 2. 如果是第二轮，加上上一轮的历史和警告
                if (round == 2) {
                    dynamic_ins += "⚠️警告：由于你们上一轮发生平票，这是第二轮重投！如果再次平票，今晚将无人死亡（空刀）！\n";
                    dynamic_ins += "上一轮全队的投票历史如下：\n" + round1_history + "\n";
                }

                // 3. 加上本轮排在前面的队友的指认
                if (!current_round_actions.empty()) {
                    dynamic_ins += "在本轮中，还没指认的队友，排在你前面的队友指认如下：\n" + current_round_actions;
                    dynamic_ins += "请结合前面队友的想法，给出你的思考和最终指认。";
                } else {
                    dynamic_ins += "你是本轮第一个表态的狼人，请给出你的思考和指认。";
                }

                // 发送请求
                json req = build_ai_request(p, dynamic_ins);
                std::cout << p->get_id() << "号狼人(第" << round << "轮)思考中：" << std::flush;
                std::string reply = AIClient::chat_sync(req);

                try {
                    std::string clean_json = clean_response(reply);
                    json j = json::parse(clean_json);

                    std::string thought = j["thought"];
                    int target = j["data"]["target"];

                    // 【修复 1】：必须加安全防护！防止 AI 乱填数字导致 wolf_votes 数组越界崩溃
                    if (target >= 0 && target <= m_players.size()) {
                        wolf_votes[target]++;
                    } else {
                        target = 0; // 乱写的数字强制当做 0 (空刀)
                        wolf_votes[0]++;
                    }

                    // 计票，并将其行动追加到当前轮次记录中给后面的狼人看
                    current_round_actions += "【" + std::to_string(p->get_id()) + "号狼人指认了: " + std::to_string(target) + "号】\n";

                    // 存入个人私有记忆（记录下自己每一轮的思考，方便白天复盘）
                    p->add_private_memory("第" + std::to_string(round) + "轮夜间刀人思考: " + thought + "，指认: " + std::to_string(target) + "号");
                    std::cout << "指认了 " << target << " 号。" << std::endl;

                } catch (...) {
                    std::cout << "[解析失败，视为弃票(空刀)] -> " << reply << std::endl;
                    wolf_votes[0]++; // 解析失败也算作空刀
                }
            }

            // --- 单轮投票结束，开始计票 ---
            int max_v = 0;
            std::vector<int> tied_targets;
            for(int i = 0; i <= m_players.size(); i++) {
                if(wolf_votes[i] > max_v) {
                    max_v = wolf_votes[i];
                    tied_targets.clear();
                    tied_targets.push_back(i);
                } else if (wolf_votes[i] == max_v && max_v > 0) {
                    tied_targets.push_back(i);
                }
            }

            // 裁决本轮结果
            if (tied_targets.size() == 1) {
                // 唯一最高票，达成共识
                if (tied_targets[0] == 0) {
                    m_dead_last_night = -1;
                } else {
                    m_dead_last_night = tied_targets[0];
                }

                consensus_reached = true;
                break; // 跳出大循环，不需要第二轮了
            } else if (tied_targets.size() > 1) {
                // 发生平票
                if (round == 1) {
                    round1_history = current_round_actions; // 将第一轮的动作存为历史，供第二轮使用
                } else {
                    // 第二轮依然平票 -> 彻底空刀
                    m_dead_last_night = -1;
                }
            }
        } // 结束 2 轮大循环

        // --- 将最终结果硬塞进狼人的脑子（极其重要） ---
        std::string final_memory;
        // 根据是否“达成共识”来区分不同的空刀情况
        if (consensus_reached) {
            if (m_dead_last_night == -1) {
                final_memory = "【夜间结算】狼人阵营达成共识，战术决定今晚空刀（无人死亡）。";
                std::cout << ">> 狼人阵营达成共识，决定战术空刀！" << std::endl;
            } else {
                final_memory = "【夜间结算】狼人阵营最终决定击杀: " + std::to_string(m_dead_last_night) + "号。";
                std::cout << ">> 狼人阵营达成共识，今晚准备杀害: " << m_dead_last_night << " 号" << std::endl;
            }
        } else {
            // 这里才是真正的发生了两轮平票
            final_memory = "【夜间结算】糟糕！狼人阵营因为意见分歧导致平票，被迫今晚空刀（无人死亡）。你要想好白天怎么应对。";
            std::cout << ">> 狼人阵营协商失败（二次平票），被迫空刀！" << std::endl;
        }

        // 统一注入全狼记忆
        for(auto p : alive_wolves) {
            p->add_private_memory(final_memory);

            // =========================================================
            // [核心修复] 在此为每个狼人补充当前的队友信息
            // =========================================================
            std::string teammates_str = "";
            for (auto other_wolf : alive_wolves) {
                if (p->get_id() != other_wolf->get_id()) {
                    teammates_str += std::to_string(other_wolf->get_id()) + " ";
                }
            }

            std::string team_memory;
            if (teammates_str.empty()) {
                team_memory = "【身份提醒】你现在是场上唯一的狼人了，必须孤军奋战。";
            } else {
                team_memory = "【身份提醒】你目前的狼队友是：" + teammates_str + "号。请保护他们，并与他们配合行动。";
            }
            p->add_private_memory(team_memory);
        }

    }

// 预言家
    for (auto p : m_players) {
        if (p->is_alive() && p->get_role() == Role::SEER) {
            std::string ins = str_utils::format(
            m_templates["night_seer_instruction"].get<std::string>(),
            {
                std::to_string(m_day_count), // 对应 "今天是第 {} 晚"
                alive_str,                // 对应 "存活玩家：[{}]"
                std::to_string(p->get_id()),
        std::to_string(p->get_id()), role_to_string(p->get_role())
            }
            );

            json req = build_ai_request(p, ins);
            std::cout << p->get_id() << "号预言家思考中..." << std::flush;
            std::string reply = AIClient::chat_sync(req);

            try {
                std::string clean_json = clean_response(reply);
                json j = json::parse(clean_json);
                int target_id = j["data"]["target"];
                std::string thought = j["thought"];

                // 【系统裁决】判断对方身份
                std::string identity = "好人";
                for (auto t : m_players) {
                    if (t->get_id() == target_id && (t->get_role() == Role::WOLF || t->get_role() == Role::WHITE_WOLF_KING)) {
                        identity = "狼人";
                        break;
                    }
                }

                std::cout << " 查验了 " << target_id << " 号：" << identity << std::endl;

                // 【关键】将查验结果写入预言家记忆
                // 这样白天发言时，他才知道该报谁是查杀，谁是金水
                std::string memory = "第" + std::to_string(m_day_count) + "晚查验了 "
                                   + std::to_string(target_id) + "号，结果是：【" + identity + "】。";

                p->add_private_memory(memory + " 当时想法: " + thought);

            } catch (...) {
                std::cout << " [预言家行动异常]" << std::endl;
            }
        }
    }

    // ==========================================
    // 3. 女巫行动 (救人与毒人)
    // ==========================================
    int poison_target = -1; // 记录女巫毒了谁
    bool witch_used_save_tonight = false;

    for (auto p : m_players) {
        if (p->is_alive() && p->get_role() == Role::WITCH) {

            // 构造状态描述
            std::string night_situation = (m_dead_last_night == -1)
                                        ? "今晚是平安夜，狼人没有杀人。（狼人没有杀人，不是守卫守住了）"
                                        : "今晚狼人杀害了 " + std::to_string(m_dead_last_night) + " 号玩家。";

            std::string save_status = m_witch_save_used ? "不可用(已用)" : "可用";
            if (m_dead_last_night == p->get_id()) {
                save_status = "不可用(首夜女巫不可自救！)";
            }
            std::string poison_status = m_witch_poison_used ? "不可用(已用)" : "可用";

            // 【修改点】：传入 m_day_count
            std::string ins = str_utils::format(
                m_templates["night_witch_instruction"].get<std::string>(),
                {
                    std::to_string(m_day_count), // 对应 "今天是第 {} 晚"
                    alive_str,                   // 对应 "存活玩家：[{}]"
                    night_situation,
                    save_status,
                    poison_status,
                    std::to_string(p->get_id()),
        std::to_string(p->get_id()), role_to_string(p->get_role())
                }
            );

            json req = build_ai_request(p, ins);
            std::cout << p->get_id() << "号女巫思考中..." << std::flush;
            std::string reply = AIClient::chat_sync(req);

            try {
                std::string clean_json = clean_response(reply);
                json j = json::parse(clean_json);

                bool use_save = j["data"]["save"];
                int use_poison = j["data"]["poison"];
                std::string thought = j["thought"];

                std::string action_log = "什么也没做";

                // --- 处理解药 ---
                // 只有药没用过，且有人死，且没有同时开毒(规则通常不允许同晚开两药)，才能救
                if (use_save && !m_witch_save_used && m_dead_last_night != -1) {
                    m_witch_save_used = true;
                    witch_used_save_tonight = true; // 记录下来，不要重置 m_dead_last_night
                    action_log = "使用解药救了 " + std::to_string(m_dead_last_night) + "号";
                }

                // --- 处理毒药 ---
                // 只有药没用过，且刚刚没开解药，且目标合法
                else if (use_poison != -1 && !m_witch_poison_used && !use_save) {
                    m_witch_poison_used = true;
                    poison_target = use_poison;
                    action_log = "使用毒药毒死 " + std::to_string(poison_target) + "号";
                }

                std::cout << " (行动结束)" << std::endl; // 不公开具体动作

                // 写入记忆
                p->add_private_memory("第" + std::to_string(m_day_count) + "晚操作: " + action_log + "。思路: " + thought);

            } catch (...) {
                std::cout << " [女巫行动异常]" << std::endl;
            }
        }
    }
    // ==========================================
    // 4. 守卫行动 (新增)
    // ==========================================
    for (auto p : m_players) {
        if (p->is_alive() && p->get_role() == Role::GUARD) {
            std::string last_guard_str = (m_last_guard_target == -1) ? "无" : std::to_string(m_last_guard_target);

            std::string ins = str_utils::format(
                m_templates["night_guard_instruction"].get<std::string>(),
                {
                    std::to_string(m_day_count), alive_str, last_guard_str,
                    std::to_string(p->get_id())
                }
            );

            json req = build_ai_request(p, ins);
            std::cout << p->get_id() << "号守卫思考中..." << std::flush;
            std::string reply = AIClient::chat_sync(req);

            try {
                json j = json::parse(clean_response(reply));
                int target = j["data"]["target"];

                // 规则检查：不能连续守同一人
                if (target == m_last_guard_target && target != -1) {
                    std::cout << " (试图连守被系统驳回，视为空守)";
                    m_guard_target = -1;
                } else {
                    m_guard_target = target;
                    std::cout << " (守护了 " << target << " 号)";
                }

                // 更新私有记忆
                p->add_private_memory("第" + std::to_string(m_day_count) + "晚守护了: " + std::to_string(m_guard_target) + "号");

            } catch (...) { m_guard_target = -1; }
            std::cout << std::endl;
        }
    }

    // 更新上一轮守护记录 (要在结算前更新，还是结算后？其实无所谓，只要明天能用就行)
    m_last_guard_target = m_guard_target;


    // ==========================================
    // 5. 最终死亡结算 (同守同救逻辑)
    // ==========================================
    m_deaths_last_night.clear();

    // --- A. 结算狼刀 ---
    if (m_dead_last_night != -1) {
        int victim = m_dead_last_night;
        bool is_guarded = (victim == m_guard_target);
        bool is_saved = witch_used_save_tonight; // 假设女巫救的一定是狼刀目标(且女巫只能救狼刀位)

        if (is_guarded && is_saved) {
            // 【同守同救】：死！
            m_deaths_last_night.push_back(victim);
            std::cout << ">> [系统判定] " << victim << " 号遭遇同守同救，不幸身亡！" << std::endl;
        }
        else if (is_guarded) {
            // 守卫守护成功：活
            std::cout << ">> [系统判定] 守卫成功抵挡了狼刀！" << std::endl;
        }
        else if (is_saved) {
            // 女巫救人成功：活
            std::cout << ">> [系统判定] 女巫使用银水救活了玩家！" << std::endl;
        }
        else {
            // 无人管：死
            m_deaths_last_night.push_back(victim);
        }
    }

    // --- B. 结算毒药 ---
    if (poison_target != -1) {
        // 守卫守不住毒药
        if (std::find(m_deaths_last_night.begin(), m_deaths_last_night.end(), poison_target) == m_deaths_last_night.end()) {
            m_deaths_last_night.push_back(poison_target);
        }
    }

    // --- C. 执行处决 ---
    for (int pid : m_deaths_last_night) {
        for (auto p : m_players) {
            if (p->get_id() == pid) {
                p->kill();
                handle_sheriff_death(pid);
                // 猎人被刀(非毒)开枪在 run_night 外层逻辑处理，或者在这里调用
                // 这里要特别注意：如果是同守同救死的，算被刀死还是被奶死？
                // 通常算“被刀死+被奶死”，猎人技能通常允许发动（只要不是被毒）。
                // 所以我们需要区分死因。如果是毒死，不能发动。

                if (p->get_role() == Role::HUNTER) {
                    if (pid == poison_target) {
                        // 被毒，不能开枪
                    } else {
                        // 狼刀、同守同救，可以开枪
                        handle_hunter_shoot(pid);
                    }
                }
                break;
            }
        }
    }

    // 状态跳转
    if (m_day_count == 1) m_state = GameState::SHERIFF_ELECTION_PHASE;
    else m_state = GameState::DAY_SPEAK_PHASE;
}

void Judge::run_day_speak() {
    std::cout << "\n========== 第 " << m_day_count << " 天 白天 ==========" << std::endl;

    // 1. 动态生成死讯字符串
    std::string death_msg;
    if (m_deaths_last_night.empty()) {
        death_msg = "昨晚是平安夜";
    } else {
        death_msg = "昨晚死亡的是：";
        for (size_t i = 0; i < m_deaths_last_night.size(); ++i) {
            death_msg += std::to_string(m_deaths_last_night[i]) + "号";
            if (i != m_deaths_last_night.size() - 1) death_msg += "、";
        }
    }
    broadcast("系统", death_msg);
    m_global_history.push_back({{"role", "user"}, {"content", "[系统]: " + death_msg}});

    if (check_win_condition()) { m_state = GameState::GAME_OVER; return; }

    // 提取存活白狼王
    Player* alive_wwk = nullptr;
    for (auto p : m_players) if (p->get_role() == Role::WHITE_WOLF_KING && p->is_alive()) alive_wwk = p;

    // 2. 处理首夜遗言
    if (m_day_count == 1 && m_enable_first_night_last_words && !m_deaths_last_night.empty()) {
        for (int dead_id : m_deaths_last_night) {
            Player* dead_player = nullptr;
            for (auto p : m_players) if (p->get_id() == dead_id) dead_player = p;

            if (dead_player) {
                std::cout << ">> [系统] 请 " << dead_id << " 号死者发表遗言..." << std::endl;

                std::string ins = str_utils::format(
                    m_templates["day_last_words_instruction"].get<std::string>(),
                    {
                        std::to_string(dead_player->get_id()), role_to_string(dead_player->get_role()),
                        std::to_string(dead_player->get_id()), role_to_string(dead_player->get_role())
                    }
                );

                json req = build_ai_request(dead_player, ins);
                std::string reply = AIClient::chat_sync(req);

                try {
                    json j = json::parse(clean_response(reply));
                    std::string speech = j["data"]["speech"];

                    // === 白狼王遗言自爆判定 ===
                    if (alive_wwk) {
                        std::string wwk_ins = str_utils::format(
                            m_templates["wwk_interrupt_instruction"].get<std::string>(),
                            { std::to_string(dead_id), speech }
                        );
                        json wwk_req = build_ai_request(alive_wwk, wwk_ins);
                        std::string wwk_reply = AIClient::chat_sync(wwk_req);
                        try {
                            json w_j = json::parse(clean_response(wwk_reply));
                            if (w_j.contains("explode") && w_j["explode"] == true) {
                                int target_id = w_j["target"];
                                std::string after_word = w_j["after_word"];
                                if (!after_word.empty()) {
                                    size_t pos = speech.find(after_word);
                                    if (pos != std::string::npos) speech = speech.substr(0, pos + after_word.length()) + "......（话音未落，只听“砰”的一声巨响！）";
                                    else speech += "......（突然被一阵自爆声打断！）";
                                } else speech += "......（遗言被自爆打断！）";



                                broadcast(std::to_string(dead_id) + "号(遗言)", speech);
                                m_global_history.push_back({{"role", "user"}, {"content", std::to_string(dead_id) + "号遗言(被打断): " + speech}});

                                std::string boom_msg = "🚨 【突发状况】白狼王 " + std::to_string(alive_wwk->get_id()) + " 号在遗言环节强行自爆！带走了 " + std::to_string(target_id) + " 号！";
                                broadcast("系统", boom_msg);
                                m_global_history.push_back({{"role", "user"}, {"content", boom_msg}});

                                alive_wwk->kill();
                                handle_sheriff_death(alive_wwk->get_id());
                                for(auto t : m_players) if(t->get_id() == target_id) {
                                    t->kill();
                                    handle_sheriff_death(target_id);

                                    if (t->get_role() == Role::HUNTER) {
                                        handle_hunter_shoot(target_id);
                                    }
                                }

                                if (check_win_condition()) { m_state = GameState::GAME_OVER; return; }
                                m_day_count++;

                                m_state = GameState::NIGHT_PHASE;
                                return;
                            }
                        } catch(...) {}
                    }
                    // =========================

                    broadcast(std::to_string(dead_id) + "号(遗言)", speech);
                    m_global_history.push_back({{"role", "user"}, {"content", std::to_string(dead_id) + "号遗言: " + speech}});
                } catch(...) {}
            }
        }
    }

// ==========================================
    // 3. 确定发言顺序 (核心修改)
    // ==========================================
    std::vector<int> speak_order;
    int player_count = m_players.size();

    // 检查警长是否存活
    Player* sheriff = nullptr;
    if (m_sheriff_id != -1) {
        for (auto p : m_players) if (p->get_id() == m_sheriff_id) sheriff = p;
        if (sheriff && !sheriff->is_alive()) sheriff = nullptr; // 警长死了就不算
    }

    if (sheriff) {
        // --- 方案 A: 警长存活，由警长决定 (保证警长最后发言) ---
        std::cout << ">> [系统] 请警长 " << m_sheriff_id << " 号决定发言顺序..." << std::endl;

        // 1. 构建顺时针链条 (从 警长+1 开始，到 警长 结束)
        std::vector<int> forward_seq;
        std::string forward_str = "";
        for (int i = 1; i < player_count; ++i) {
            int pid = (m_sheriff_id - 1 + i) % player_count + 1;
            forward_seq.push_back(pid);
            forward_str += std::to_string(pid) + " ";
        }
        forward_seq.push_back(m_sheriff_id); // 警长最后
        forward_str += "... " + std::to_string(m_sheriff_id);

        // 2. 构建逆时针链条 (从 警长-1 开始，到 警长 结束)
        std::vector<int> backward_seq;
        std::string backward_str = "";
        for (int i = 1; i < player_count; ++i) {
            int pid = (m_sheriff_id - 1 - i + player_count) % player_count + 1;
            backward_seq.push_back(pid);
            backward_str += std::to_string(pid) + " ";
        }
        backward_seq.push_back(m_sheriff_id); // 警长最后
        backward_str += "... " + std::to_string(m_sheriff_id);

        // 3. 询问警长
        std::string ins = str_utils::format(
            m_templates["sheriff_decide_order_instruction"].get<std::string>(),
            {
                std::to_string(m_day_count), death_msg,
                forward_str, backward_str,
                std::to_string(sheriff->get_id()), role_to_string(sheriff->get_role())
            }
        );

        json req = build_ai_request(sheriff, ins);
        std::string reply = AIClient::chat_sync(req);

        bool is_forward = true;
        try {
            json j = json::parse(clean_response(reply));
            std::string type = j["data"]["order_type"];
            if (type == "backward") is_forward = false;

            std::string dir_msg = is_forward ? "顺序发言 (警左)" : "逆序发言 (警右)";
            std::string msg = ">> [系统] 警长决定采用：" + dir_msg + "。由 " + std::to_string(is_forward ? forward_seq[0] : backward_seq[0]) + " 号开始发言，警长归票。";
            broadcast("系统", msg);
            m_global_history.push_back({{"role", "user"}, {"content", msg}});

        } catch (...) {
            std::cout << ">> [系统] 警长决定解析失败，默认顺时针。" << std::endl;
        }

        speak_order = is_forward ? forward_seq : backward_seq;

    } else {
        // --- 方案 B: 无警长 (或警长死亡) ---
        // 规则：
        // 1. 平安夜：从 1 号开始顺时针。
        // 2. 有死者：从 (死者号 + 1) 开始顺时针。
        // 3. 多死者：取号码最小的死者，从他后面那个开始。

        int start_index_0based = 0; // 默认 1号 (下标0)

        if (!m_deaths_last_night.empty()) {
            // 找到最小的死者号码
            int min_dead_id = 999;
            for(int did : m_deaths_last_night) {
                if(did < min_dead_id) min_dead_id = did;
            }
            // 从死者下一位开始 (min_dead_id - 1 是下标，再 +1 是下一位下标)
            start_index_0based = min_dead_id % player_count;

            std::cout << ">> [系统] 昨晚 " << min_dead_id << " 号(最小号)死亡，无警长，从 " << (start_index_0based+1) << " 号开始发言。" << std::endl;
        } else {
            std::cout << ">> [系统] 昨晚平安夜且无警长，从 1 号开始发言。" << std::endl;
        }

        // 顺时针生成列表
        for (int i = 0; i < player_count; ++i) {
            speak_order.push_back((start_index_0based + i) % player_count + 1);
        }
    }

    // 4. 按顺序执行发言
    for (int pid : speak_order) {
        Player* p = nullptr;
        for(auto t : m_players) if(t->get_id() == pid) p = t;

        // 跳过死人 (关键：speak_order 里包含所有人，这里要过滤)
        if (!p || !p->is_alive()) continue;

        std::string ins = str_utils::format(
            m_templates["day_speak_instruction"].get<std::string>(),
            {
                std::to_string(m_day_count), death_msg,
                std::to_string(p->get_id()), role_to_string(p->get_role()),
                std::to_string(p->get_id()), role_to_string(p->get_role())
            }
        );

        json req = build_ai_request(p, ins);
        std::cout << p->get_id() << "号玩家思考中..." << std::flush;
        std::string reply = AIClient::chat_sync(req);

        try {
            json j = json::parse(clean_response(reply));
            std::string speech = j["data"]["speech"];

            // === 白狼王白天发言自爆判定 ===
            if (alive_wwk && alive_wwk->get_id() != p->get_id()) {
                std::string wwk_ins = str_utils::format(
                    m_templates["wwk_interrupt_instruction"].get<std::string>(),
                    { std::to_string(p->get_id()), speech }
                );
                json wwk_req = build_ai_request(alive_wwk, wwk_ins);
                std::string wwk_reply = AIClient::chat_sync(wwk_req);
                try {
                    json w_j = json::parse(clean_response(wwk_reply));
                    if (w_j.contains("explode") && w_j["explode"] == true) {
                        int target_id = w_j["target"];
                        std::string after_word = w_j["after_word"];
                        if (!after_word.empty()) {
                            size_t pos = speech.find(after_word);
                            if (pos != std::string::npos) speech = speech.substr(0, pos + after_word.length()) + "......（话音未落，只听“砰”的一声巨响！）";
                            else speech += "......（突然被一阵自爆声打断！）";
                        } else speech += "......（发言被自爆打断！）";

                        broadcast(std::to_string(p->get_id()) + "号", speech);
                        m_global_history.push_back({{"role", "user"}, {"content", std::to_string(p->get_id()) + "号发言(被打断): " + speech}});

                        std::string boom_msg = "🚨 【突发状况】白狼王 " + std::to_string(alive_wwk->get_id()) + " 号强行自爆！带走了 " + std::to_string(target_id) + " 号！";
                        broadcast("系统", boom_msg);
                        m_global_history.push_back({{"role", "user"}, {"content", boom_msg}});

                        alive_wwk->kill();
                        handle_sheriff_death(alive_wwk->get_id());
                        for(auto t : m_players) if(t->get_id() == target_id) { t->kill(); handle_sheriff_death(target_id); }

                        if (check_win_condition()) {
                            m_state = GameState::GAME_OVER;
                            return;
                        }

                        m_day_count++;
                        m_state = GameState::NIGHT_PHASE;
                        return;
                    }
                } catch(...) {}
            }
            // ============================

            broadcast(std::to_string(p->get_id()) + "号", speech);
            m_global_history.push_back({{"role", "user"}, {"content", std::to_string(p->get_id()) + "号玩家发言: " + speech}});

        } catch(...) { std::cout << ">> 发言解析失败" << std::endl; }
    }

    m_state = GameState::DAY_VOTE_PHASE;
}

void Judge::broadcast(const std::string& speaker, const std::string& message) {
    std::cout << "\n[" << speaker << "]: " << message << std::endl;
}

Role Judge::string_to_role(const std::string& str) {
    if (str == "WOLF") return Role::WOLF;
    if (str == "WHITE_WOLF_KING") return Role::WHITE_WOLF_KING;
    if (str == "SEER") return Role::SEER;
    if (str == "WITCH") return Role::WITCH;
    if (str == "HUNTER") return Role::HUNTER;
    if (str == "GUARD") return Role::GUARD;
    return Role::VILLAGER;
}

std::string Judge::role_to_string(Role r) {
    switch(r) {
        case Role::WOLF: return "狼人";
        case Role::WHITE_WOLF_KING: return "白狼王"; // 新增
        case Role::SEER: return "预言家";
        case Role::WITCH: return "女巫";
        case Role::HUNTER: return "猎人";
        case Role::GUARD: return "守卫";
        default: return "平民";
    }
}

// ... check_win_condition 和 run_vote 参考之前的逻辑即可 ...
// 记得在 run_vote 结束后 m_day_count++; 并切换回 NIGHT_PHASE

void Judge::start_game() {
    while (m_state != GameState::GAME_OVER) {
        switch (m_state) {
            case GameState::NIGHT_PHASE:
                run_night();
                break;
            case GameState::SHERIFF_ELECTION_PHASE:
                run_sheriff_election();
                break;
            case GameState::DAY_SPEAK_PHASE:
                run_day_speak();
                break;
            case GameState::DAY_VOTE_PHASE:
                run_vote();
                break;
            default:
                break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// 判定胜负逻辑
bool Judge::check_win_condition() {
    int wolf_count = 0, god_count = 0, villager_count = 0;

    for (auto p : m_players) {
        if (p->is_alive()) {
            Role r = p->get_role();
            if (r == Role::WOLF || r == Role::WHITE_WOLF_KING) wolf_count++;
            else if (r == Role::VILLAGER) villager_count++;
            else god_count++; // 预言家、女巫、猎人等神职
        }
    }

    if (wolf_count == 0) {
        broadcast("系统", "🏆 游戏结束：狼人阵营全部覆灭，【好人阵营】胜利！");
        return true;
    }
    // 屠边判定：神死光 或 民死光
    if (god_count == 0) {
        broadcast("系统", "🏆 游戏结束：神职全部阵亡，【狼人阵营】屠边胜利！");
        return true;
    }
    if (villager_count == 0) {
        broadcast("系统", "🏆 游戏结束：平民全部阵亡，【狼人阵营】屠边胜利！");
        return true;
    }
    return false;
}


// 投票阶段
void Judge::run_vote() {

    // 定义警长破平票的闭包函数 (不使用 double，纯逻辑判断)
    auto resolve_tie_with_sheriff = [&](std::vector<int>& tied, const std::map<int, std::vector<int>>& records) {
        if (tied.size() > 1 && m_sheriff_id != -1) {
            bool sheriff_alive = false;
            for (auto p : m_players) if (p->get_id() == m_sheriff_id && p->is_alive()) sheriff_alive = true;

            if (sheriff_alive) {
                int sheriff_vote = -1;
                // 找出警长投给了谁
                for (auto const& [candidate, voters] : records) {
                    if (std::find(voters.begin(), voters.end(), m_sheriff_id) != voters.end()) {
                        sheriff_vote = candidate;
                        break;
                    }
                }

                // 如果警长投的人正好在平票名单里，该候选人胜出
                if (std::find(tied.begin(), tied.end(), sheriff_vote) != tied.end()) {
                    std::cout << ">>[系统] 出现平票！但警长(" << m_sheriff_id << "号)的 0.5 票特权打破了僵局！" << std::endl;
                    tied.clear();
                    tied.push_back(sheriff_vote);
                }
            }
        }
    };

    std::cout << "\n========== 投票阶段 ==========" << std::endl;

    // 1. 构造存活列表 (供 AI 参考)
    std::string alive_list = "";
    for(auto t : m_players) if(t->is_alive()) alive_list += std::to_string(t->get_id()) + " ";

    // ==========================================
    // 第一轮：全员自由投票
    // ==========================================
    std::map<int, std::vector<int>> round1_records;
    std::vector<int> votes_count(10, 0);

    for (auto p : m_players) {
        if (!p->is_alive()) continue;

        // 【修改点】：使用新的模板，传入身份信息
        std::string ins = str_utils::format(
            m_templates["day_vote_instruction"].get<std::string>(),
            {
                alive_list,
                std::to_string(p->get_id()), role_to_string(p->get_role()),
                std::to_string(p->get_id()), role_to_string(p->get_role())
            }
        );
        json req = build_ai_request(p, ins);

        std::cout << p->get_id() << "号投票思考中..." << std::flush;
        std::string reply = AIClient::chat_sync(req);

        int target = -1;
        try {
            std::string clean = clean_response(reply);
            json j = json::parse(clean);
            if (j["data"].contains("target")) target = j["data"]["target"];
        } catch (...) { }

        // 验证有效性
        bool is_valid = false;
        if (target >= 0 && target < 10) {
            for(auto t : m_players) if (t->get_id() == target && t->is_alive()) { is_valid = true; break; }
        }

        if (is_valid) {
            votes_count[target]++;
            round1_records[target].push_back(p->get_id());
            std::cout << " -> 投给 " << target << "号" << std::endl;
        } else {
            round1_records[-1].push_back(p->get_id());
            std::cout << " -> 弃票" << std::endl;
        }
    }

    // --- 生成第一轮票型详情 ---
    std::string vote_history_details = "";
    for (auto const& [candidate, voters] : round1_records) {
        if (candidate == -1) vote_history_details += "【弃票】: ";
        else vote_history_details += "【投" + std::to_string(candidate) + "号】: ";
        for (size_t i = 0; i < voters.size(); ++i) vote_history_details += std::to_string(voters[i]) + (i == voters.size()-1 ? "" : ", ");
        vote_history_details += "; ";
    }
    std::cout << "\n>> 第一轮详情: " << vote_history_details << std::endl;

    // --- 统计第一轮结果 ---
    int max_v = 0;
    std::vector<int> tied_targets;
    for (int i = 1; i <= m_players.size(); i++) {
        if (votes_count[i] > max_v) {
            max_v = votes_count[i];
            tied_targets.clear();
            tied_targets.push_back(i);
        } else if (votes_count[i] == max_v && max_v > 0) {
            tied_targets.push_back(i);
        }
    }
    resolve_tie_with_sheriff(tied_targets, round1_records);

    int exiled_id = -1;

    // ==========================================
    // 逻辑分支：无人 / 单死 / PK
    // ==========================================
    if (tied_targets.empty()) {
        std::cout << ">> [系统] 无人有效投票，平安日。" << std::endl;
        m_global_history.push_back({{"role", "user"}, {"content", "[系统]: 平安日(无人投票)。"}});
    }
    else if (tied_targets.size() == 1) {
        exiled_id = tied_targets[0];
    }
    else {
        // ==========================================
        // 进入 PK 阶段
        // ==========================================
        std::string pk_names = "";
        for(int t : tied_targets) pk_names += std::to_string(t) + " ";
        std::string alert = ">> [系统] 出现平票！PK台候选人: " + pk_names + "。\n>> [系统] 第一轮详细票型: " + vote_history_details;
        broadcast("系统", alert);
        m_global_history.push_back({{"role", "user"}, {"content", alert}});

        // --- (A) PK 辩护 ---
        for (int candidate_id : tied_targets) {
            Player* p = nullptr;
            for(auto t : m_players) if(t->get_id() == candidate_id) p = t;

            if (p) {
                std::string rivals = "";
                for(int r : tied_targets) if(r != candidate_id) rivals += std::to_string(r) + " ";

                // 【修改点】：使用新的模板，传入身份信息
                std::string ins = str_utils::format(
                    m_templates["day_pk_speech_instruction"].get<std::string>(),
                    {
                        rivals, vote_history_details,
                        std::to_string(p->get_id()), role_to_string(p->get_role()),
                        std::to_string(p->get_id()), role_to_string(p->get_role())
                    }
                );
                json req = build_ai_request(p, ins);

                std::cout << p->get_id() << "号(PK台)辩护中..." << std::flush;
                std::string reply = AIClient::chat_sync(req);
                try {
                    json j = json::parse(clean_response(reply));
                    std::string speech = j["data"]["speech"];
                    broadcast(std::to_string(p->get_id()) + "号[PK发言]", speech);
                    m_global_history.push_back({{"role", "user"}, {"content", std::to_string(p->get_id()) + "号PK辩护: " + speech}});
                } catch(...) {}
            }
        }

        // --- (B) PK 第二轮投票 ---
        std::vector<Player*> pk_voters;
        for (auto p : m_players) {
            if (p->is_alive()) {
                // 检查该玩家是否在 PK 台上
                bool is_on_pk = false;
                for (int cid : tied_targets) {
                    if (p->get_id() == cid) { is_on_pk = true; break; }
                }
                if (!is_on_pk) pk_voters.push_back(p);
            }
        }

        // 处理边界情况：如果所有活着的玩家都在PK台上（比如3V3平票）
        // 则按照你的要求，所有人都可以重新投票
        bool everyone_votes = false;
        if (pk_voters.empty()) {
            everyone_votes = true;
            for (auto p : m_players) if (p->is_alive()) pk_voters.push_back(p);
            std::cout << ">> [系统] 所有人均在PK台上，全员获得投票权！" << std::endl;
        } else {
            std::cout << ">> [系统] PK台上的选手(" << pk_names << ")禁言并失去投票权，由其余玩家表决。" << std::endl;
        }

        // --- (C) PK 第二轮投票正式开始 ---
        std::cout << "\n>> [系统] PK发言结束，投票开始！" << std::endl;

        std::fill(votes_count.begin(), votes_count.end(), 0);
        std::map<int, std::vector<int>> round2_records;

        for (auto p : pk_voters) {
            // 构造指令，明确告诉 AI 谁在 PK 台，且它必须在其中选一个
            std::string ins = str_utils::format(
                m_templates["day_pk_vote_instruction"].get<std::string>(),
                {
                    pk_names,               // 目前 {} 号玩家平票
                    vote_history_details,   // 第一轮票型详情
                    pk_names,               // 只能在候选人 [{}] 中选
                    std::to_string(p->get_id()), role_to_string(p->get_role()), // 身份锚点
                    std::to_string(p->get_id()), role_to_string(p->get_role())
                }
            );

            json req = build_ai_request(p, ins);
            std::cout << p->get_id() << "号投票中..." << std::flush;
            std::string reply = AIClient::chat_sync(req);

            int target = -1;
            try {
                json j = json::parse(clean_response(reply));
                if (j["data"].contains("target")) target = j["data"]["target"];

                // 严格验证：目标必须在 PK 台上
                bool is_valid_candidate = false;
                for(int t : tied_targets) if(t == target) is_valid_candidate = true;

                if (is_valid_candidate) {
                    votes_count[target]++;
                    round2_records[target].push_back(p->get_id());
                    std::cout << " -> 投给 " << target << "号" << std::endl;
                } else {
                    round2_records[-1].push_back(p->get_id());
                    std::cout << " -> [废票] 目标不在PK台" << std::endl;
                }
            } catch (...) {
                round2_records[-1].push_back(p->get_id());
                std::cout << " -> [废票] 解析失败" << std::endl;
            }
        }

        // --- (D) 统计 PK 结果 ---
        max_v = 0;
        std::vector<int> pk_winners;
        for (int t : tied_targets) {
            if (votes_count[t] > max_v) {
                max_v = votes_count[t];
                pk_winners.clear();
                pk_winners.push_back(t);
            } else if (votes_count[t] == max_v && max_v > 0) {
                pk_winners.push_back(t);
            }
        }
        resolve_tie_with_sheriff(tied_targets, round1_records);

        // 生成 PK 轮票型用于打印和历史
        std::string pk_vote_details = "";
        for (auto const& [candidate, voters] : round2_records) {
            if (candidate == -1) pk_vote_details += "【弃票/废票】: ";
            else pk_vote_details += "【投" + std::to_string(candidate) + "号】: ";
            for (size_t i = 0; i < voters.size(); ++i) pk_vote_details += std::to_string(voters[i]) + (i == voters.size()-1 ? "" : ", ");
            pk_vote_details += "; ";
        }

        if (pk_winners.size() == 1) {
            exiled_id = pk_winners[0]; // 产生唯一最高票
            std::string win_msg = ">> [系统] PK结果：放逐 " + std::to_string(exiled_id) + " 号。\n详情: " + pk_vote_details;
            broadcast("系统", win_msg);
            m_global_history.push_back({{"role", "user"}, {"content", win_msg}});
        } else {
            // 再次平票或无人投票 -> 流局
            std::string msg = ">> [系统] PK台再次平票或全员弃票，本轮流局，无人出局！\n详情: " + pk_vote_details;
            broadcast("系统", msg);
            m_global_history.push_back({{"role", "user"}, {"content", msg}});
        }
    }

    // ==========================================
    // 最终结算与处决
    // ==========================================
    if (exiled_id != -1) {
        // 1. 先执行死亡逻辑
        std::string result_msg = std::to_string(exiled_id) + "号玩家在表决中被投票出局";
        broadcast("系统", result_msg);
        m_global_history.push_back({{"role", "user"}, {"content", "[系统投票结果]: " + result_msg}});

        for (auto p : m_players) {
            if (p->get_id() == exiled_id) {
                p->kill();
                handle_sheriff_death(exiled_id);
                if (p->get_role() == Role::HUNTER) {
                    handle_hunter_shoot(exiled_id);
                }
                break;
            }
            // 检查胜负 (因为猎人可能带走最后一只狼)
            if (check_win_condition()) { m_state = GameState::GAME_OVER; return; }
        }

        // 2. 【核心修改】立即检查胜负
        // 如果这人一死游戏就结束了，直接切状态并退出，省下遗言的 Token
        if (check_win_condition()) {
            m_state = GameState::GAME_OVER;
            return; // 直接结束函数，不再执行后面的遗言逻辑
        }

        // 3. 如果游戏没结束，再发表遗言
        std::cout << ">> [系统] 游戏继续，请被放逐者发表遗言..." << std::endl;
        Player* dead_p = nullptr;
        for(auto p : m_players) if(p->get_id() == exiled_id) dead_p = p;

        if (dead_p) {
            std::string ins = str_utils::format(
                m_templates["day_last_words_instruction"].get<std::string>(),
                {
                    std::to_string(dead_p->get_id()), role_to_string(dead_p->get_role()),
                    std::to_string(dead_p->get_id()), role_to_string(dead_p->get_role())
                }
            );
            json req = build_ai_request(dead_p, ins);
            try {
                std::string reply = AIClient::chat_sync(req);
                json j = json::parse(clean_response(reply));
                broadcast(std::to_string(exiled_id) + "号(遗言)", j["data"]["speech"]);
                m_global_history.push_back({{"role", "user"}, {"content", std::to_string(exiled_id) + "号遗言: " + j["data"]["speech"].get<std::string>()}});
            } catch(...) {
                std::cout << ">> 遗言解析失败" << std::endl;
            }
        }
    }

    // 4. 这里的胜负判定虽然冗余但保留作为保险，关键是天数自增和状态切换
    if (!check_win_condition()) {
        m_day_count++;
        m_state = GameState::NIGHT_PHASE;
    } else {
        m_state = GameState::GAME_OVER;
    }
}

void Judge::run_sheriff_election() {
    std::cout << "\n========== 第 1 天 警长竞选 ==========" << std::endl;
    broadcast("系统", "警长竞选开始，正在等待玩家决定是否上警...");

    std::vector<int> candidates;
    std::map<int, std::string> election_speeches;
    std::string current_election_history = ""; // 公共保存区域：记录前置位发言

    // 提取存活的白狼王 (用于警上自爆判定)
    Player* alive_wwk = nullptr;
    for (auto p : m_players) if (p->get_role() == Role::WHITE_WOLF_KING && p->is_alive()) alive_wwk = p;

    // 1. 询问所有人（包括死人！）是否上警
    for (auto p : m_players) {
        // 【注意】这里没有 if (!p->is_alive()) continue; 死人可以上警

        std::string history_text = current_election_history.empty() ? "（暂无前置位发言）" : current_election_history;

        std::string ins = str_utils::format(
            m_templates["sheriff_run_instruction"].get<std::string>(),
            {
                history_text, // 传入前面人的发言历史
                std::to_string(p->get_id()), role_to_string(p->get_role()), // 身份锚点
                std::to_string(p->get_id()), role_to_string(p->get_role())
            }
        );

        json req = build_ai_request(p, ins);
        std::cout << p->get_id() << "号思考是否竞选..." << std::flush;
        std::string reply = AIClient::chat_sync(req);

        try {
            json j = json::parse(clean_response(reply));
            bool is_running = j["data"]["run_for_sheriff"];

            if (is_running) {
                std::string speech = j["data"]["speech"];

                // ========================================================
                // 【核心】警上白狼王自爆拦截 (吞警徽机制)
                // ========================================================
                if (alive_wwk && alive_wwk->get_id() != p->get_id()) {
                    std::string wwk_ins = str_utils::format(
                        m_templates["wwk_interrupt_instruction"].get<std::string>(),
                        { std::to_string(p->get_id()), speech }
                    );

                    json wwk_req = build_ai_request(alive_wwk, wwk_ins);
                    std::string wwk_reply = AIClient::chat_sync(wwk_req);

                    try {
                        json w_j = json::parse(clean_response(wwk_reply));
                        if (w_j.contains("explode") && w_j["explode"] == true) {
                            int target_id = w_j["target"];
                            std::string after_word = w_j["after_word"];

                            // 截断发言
                            if (!after_word.empty()) {
                                size_t pos = speech.find(after_word);
                                if (pos != std::string::npos) {
                                    speech = speech.substr(0, pos + after_word.length());
                                    speech += "......（话音未落，只听“砰”的一声巨响！）";
                                } else { speech += "......（突然被一阵自爆声打断！）"; }
                            } else { speech += "......（发言被自爆打断！）"; }

                            if (!current_election_history.empty()) {
                                m_global_history.push_back({{"role", "user"}, {"content", ">> [系统] 在突发自爆前，已有以下玩家发表了警上宣言：\n" + current_election_history}});
                            }

                            broadcast(std::to_string(p->get_id()) + "号(警上)", speech);
                            m_global_history.push_back({{"role", "user"}, {"content", std::to_string(p->get_id()) + "号警上发言(被打断): " + speech}});

                            std::string boom_msg = "🚨 【突发状况】白狼王 " + std::to_string(alive_wwk->get_id()) + " 号在警上强行自爆！并扑咬带走了 " + std::to_string(target_id) + " 号！\n💥 【吞警徽】警长竞选强制终止，本局游戏警徽流失！";
                            broadcast("系统", boom_msg);
                            m_global_history.push_back({{"role", "user"}, {"content", boom_msg}});

                            // 结算双死
                            alive_wwk->kill();
                            for (auto t : m_players) if(t->get_id() == target_id) t->kill();

                            m_sheriff_id = -1; // 警徽流失
                            if (check_win_condition()) { m_state = GameState::GAME_OVER; return; }
                            m_state = GameState::NIGHT_PHASE; // 直接入夜
                            m_day_count++;
                            return;
                        }
                    } catch(...) {}
                }
                // ========================================================

                candidates.push_back(p->get_id());
                election_speeches[p->get_id()] = speech;
                // 记录历史
                current_election_history += "【" + std::to_string(p->get_id()) + "号警上发言】: " + speech + "\n";
                std::cout << " -> 上警发言: " << speech.substr(0, 30) << "..." << std::endl;
            } else {
                std::cout << " -> 放弃上警" << std::endl;
            }
        } catch (...) { std::cout << " -> 解析失败" << std::endl; }
    }

    // 2. 播报竞选者
    if (candidates.empty()) {
        std::string msg = ">> [系统] 无人参与警长竞选，本局游戏无警徽。";
        broadcast("系统", msg);
        m_global_history.push_back({{"role", "user"}, {"content", msg}});
        m_sheriff_id = -1;
    }
    else if (candidates.size() == 1) {
        int auto_win_id = candidates[0];
        std::string msg = ">>[系统] 只有 " + std::to_string(auto_win_id) + " 号玩家上警，自动当选警长！\n他的竞选宣言：" + election_speeches[auto_win_id];
        broadcast("系统", msg);
        m_global_history.push_back({{"role", "user"}, {"content", msg}});
        m_sheriff_id = auto_win_id;
    }
    else {
        // 多人上警
        std::string c_list = "";
        for (int c : candidates) c_list += std::to_string(c) + " ";
        // AI 已经知道发言了，这里只对控制台做简要播报
        broadcast("系统", "竞选者为: " + c_list + "。请警下玩家开始投票！");

        std::string summary_msg = ">> [系统] 警长竞选发言阶段结束。竞选者为: " + c_list + "\n【详细警上发言记录】：\n" + current_election_history;
        broadcast("系统", "上警发言结束，请警下玩家开始投票！");
        m_global_history.push_back({{"role", "user"}, {"content", summary_msg}});

        // 3. 警下投票
        std::vector<int> votes_count(m_players.size() + 1, 0);
        std::string vote_details = "";

        for (auto p : m_players) {
            if (!p->is_alive()) continue; // 只有活人能投票
            if (std::find(candidates.begin(), candidates.end(), p->get_id()) != candidates.end()) continue; // 竞选者不能投

            std::string ins = str_utils::format(
                m_templates["sheriff_vote_instruction"].get<std::string>(),
                {
                    c_list,
                    std::to_string(p->get_id()), role_to_string(p->get_role()),
                    std::to_string(p->get_id()), role_to_string(p->get_role())
                }
            );

            json req = build_ai_request(p, ins);
            std::string reply = AIClient::chat_sync(req);

            int target = -1;
            try {
                json j = json::parse(clean_response(reply));
                if (j["data"].contains("target")) target = j["data"]["target"];

                if (std::find(candidates.begin(), candidates.end(), target) != candidates.end()) {
                    votes_count[target]++;
                    vote_details += "[" + std::to_string(p->get_id()) + "投给" + std::to_string(target) + "] ";
                } else {
                    vote_details += "[" + std::to_string(p->get_id()) + "弃票] ";
                }
            } catch (...) { vote_details += "[" + std::to_string(p->get_id()) + "弃票] "; }
        }

// 4. 结算警徽第一轮
        int max_v = 0;
        std::vector<int> winners;
        for (int c : candidates) {
            if (votes_count[c] > max_v) { max_v = votes_count[c]; winners.clear(); winners.push_back(c); }
            else if (votes_count[c] == max_v && max_v > 0) { winners.push_back(c); }
        }

        if (winners.size() == 1) {
            m_sheriff_id = winners[0];
            std::string msg = ">> [系统] 警长投票结束。票型：" + vote_details + "\n恭喜 " + std::to_string(m_sheriff_id) + " 号当选警长！";
            broadcast("系统", msg);
            m_global_history.push_back({{"role", "user"}, {"content", msg}});
        } else if (winners.empty()) {
            m_sheriff_id = -1;
            std::string msg = ">> [系统] 警长投票无人有效投票！本局游戏警徽流失。票型：" + vote_details;
            broadcast("系统", msg);
            m_global_history.push_back({{"role", "user"}, {"content", msg}});
        } else {
            // ==========================================
            // 进入 警长 PK 阶段
            // ==========================================
            std::string pk_names = "";
            for(int t : winners) pk_names += std::to_string(t) + " ";
            std::string alert = ">> [系统] 警上出现平票！警长PK台候选人: " + pk_names + "。\n>> [系统] 第一轮详细票型: " + vote_details;
            broadcast("系统", alert);
            m_global_history.push_back({{"role", "user"}, {"content", alert}});

            // --- (A) 警长 PK 辩护发言 ---
            for (int candidate_id : winners) {
                Player* p = nullptr;
                for(auto t : m_players) if(t->get_id() == candidate_id) p = t;

                if (p) {
                    std::string rivals = "";
                    for(int r : winners) if(r != candidate_id) rivals += std::to_string(r) + " ";

                    std::string ins = str_utils::format(
                        m_templates["sheriff_pk_speech_instruction"].get<std::string>(),
                        {
                            rivals, vote_details,
                            std::to_string(p->get_id()), role_to_string(p->get_role()),
                            std::to_string(p->get_id()), role_to_string(p->get_role())
                        }
                    );
                    json req = build_ai_request(p, ins);

                    std::cout << p->get_id() << "号(警长PK台)拉票中..." << std::flush;
                    std::string reply = AIClient::chat_sync(req);
                    try {
                        json j = json::parse(clean_response(reply));
                        std::string speech = j["data"]["speech"];

                        // === 白狼王在PK台自爆拦截 ===
                        if (alive_wwk && alive_wwk->get_id() != p->get_id()) {
                            std::string wwk_ins = str_utils::format(
                                m_templates["wwk_interrupt_instruction"].get<std::string>(),
                                { std::to_string(p->get_id()), speech }
                            );
                            json wwk_req = build_ai_request(alive_wwk, wwk_ins);
                            std::string wwk_reply = AIClient::chat_sync(wwk_req);
                            try {
                                json w_j = json::parse(clean_response(wwk_reply));
                                if (w_j.contains("explode") && w_j["explode"] == true) {
                                    int target_id = w_j["target"];
                                    std::string after_word = w_j["after_word"];
                                    if (!after_word.empty()) {
                                        size_t pos = speech.find(after_word);
                                        if (pos != std::string::npos) speech = speech.substr(0, pos + after_word.length()) + "......（话音未落，只听“砰”的一声巨响！）";
                                        else speech += "......（突然被一阵自爆声打断！）";
                                    } else speech += "......（发言被自爆打断！）";

                                    broadcast(std::to_string(p->get_id()) + "号(PK台)", speech);
                                    m_global_history.push_back({{"role", "user"}, {"content", std::to_string(p->get_id()) + "号警长PK发言(被打断): " + speech}});

                                    std::string boom_msg = "🚨 【突发状况】白狼王 " + std::to_string(alive_wwk->get_id()) + " 号在警长PK时强行自爆！并带走了 " + std::to_string(target_id) + " 号！\n💥 【吞警徽】警长竞选强制终止，警徽流失！";
                                    broadcast("系统", boom_msg);
                                    m_global_history.push_back({{"role", "user"}, {"content", boom_msg}});

                                    alive_wwk->kill();
                                    for(auto t : m_players) if(t->get_id() == target_id) t->kill();

                                    m_sheriff_id = -1;
                                    if (check_win_condition()) { m_state = GameState::GAME_OVER; return; }
                                    m_state = GameState::NIGHT_PHASE;
                                    m_day_count++;
                                    return;
                                }
                            } catch(...) {}
                        }
                        // ============================

                        broadcast(std::to_string(p->get_id()) + "号[警长PK拉票]", speech);
                        m_global_history.push_back({{"role", "user"}, {"content", std::to_string(p->get_id()) + "号警长PK发言: " + speech}});
                    } catch(...) {}
                }
            }

            // --- (B) 警长 PK 第二轮投票 ---
            std::cout << "\n>> [系统] 警长PK发言结束，警下玩家请再次投票！" << std::endl;
            std::fill(votes_count.begin(), votes_count.end(), 0);
            std::string pk_vote_details = "";

            for (auto p : m_players) {
                if (!p->is_alive()) continue;
                // 不在 PK 台上的人才能投票
                bool on_pk = false;
                for (int c : winners) if (c == p->get_id()) on_pk = true;
                if (on_pk) continue;

                std::string ins = str_utils::format(
                    m_templates["sheriff_pk_vote_instruction"].get<std::string>(),
                    {
                        pk_names, vote_details, pk_names,
                        std::to_string(p->get_id()), role_to_string(p->get_role()),
                        std::to_string(p->get_id()), role_to_string(p->get_role())
                    }
                );
                json req = build_ai_request(p, ins);
                std::string reply = AIClient::chat_sync(req);

                int target = -1;
                try {
                    json j = json::parse(clean_response(reply));
                    if (j["data"].contains("target")) target = j["data"]["target"];

                    // 只能投给 PK 台上的人
                    if (std::find(winners.begin(), winners.end(), target) != winners.end()) {
                        votes_count[target]++;
                        pk_vote_details += "[" + std::to_string(p->get_id()) + "投给" + std::to_string(target) + "] ";
                    } else {
                        pk_vote_details += "[" + std::to_string(p->get_id()) + "弃票] ";
                    }
                } catch (...) { pk_vote_details += "[" + std::to_string(p->get_id()) + "弃票] "; }
            }

            // --- (C) 统计 PK 结果 ---
            max_v = 0;
            std::vector<int> pk_winners;
            for (int c : winners) {
                if (votes_count[c] > max_v) { max_v = votes_count[c]; pk_winners.clear(); pk_winners.push_back(c); }
                else if (votes_count[c] == max_v && max_v > 0) { pk_winners.push_back(c); }
            }

            if (pk_winners.size() == 1) {
                m_sheriff_id = pk_winners[0];
                std::string msg = ">> [系统] 警长PK投票结束。票型：" + pk_vote_details + "\n恭喜 " + std::to_string(m_sheriff_id) + " 号当选警长！";
                broadcast("系统", msg);
                m_global_history.push_back({{"role", "user"}, {"content", msg}});
            } else {
                m_sheriff_id = -1;
                std::string msg = ">> [系统] 警长PK台再次平票或全员弃票！本局游戏警徽彻底流失。终局票型：" + pk_vote_details;
                broadcast("系统", msg);
                m_global_history.push_back({{"role", "user"}, {"content", msg}});
            }
        }
    }

    // ==========================================
    // 【核心】当选后立刻检查：警长是否是昨晚的死人
    // ==========================================
    if (m_sheriff_id != -1) {
        Player* sheriff_p = nullptr;
        for (auto p : m_players) if(p->get_id() == m_sheriff_id) sheriff_p = p;

        if (sheriff_p && !sheriff_p->is_alive()) {
            std::cout << "\n>> [系统] 刚当选的警长 " << m_sheriff_id << " 号昨晚已阵亡！" << std::endl;
            broadcast("系统", "警长昨晚已死亡，必须立刻移交警徽。");
            handle_sheriff_death(m_sheriff_id);
        }
    }
    m_state = GameState::DAY_SPEAK_PHASE;
}

void Judge::handle_sheriff_death(int dead_id) {
    if (m_sheriff_id != dead_id) return; // 死者不是警长，不处理

    Player* dead_sheriff = nullptr;
    for (auto p : m_players) if (p->get_id() == dead_id) dead_sheriff = p;
    if (!dead_sheriff) return;

    std::cout << "\n>> [系统] 警长 " << dead_id << " 号阵亡，请决定警徽移交..." << std::endl;

    std::string alive_list = "";
    for (auto t : m_players) if (t->is_alive() && t->get_id() != dead_id) alive_list += std::to_string(t->get_id()) + " ";

    std::string ins = str_utils::format(
        m_templates["sheriff_pass_instruction"].get<std::string>(),
        {
            alive_list,
            std::to_string(dead_sheriff->get_id()), role_to_string(dead_sheriff->get_role()),
            std::to_string(dead_sheriff->get_id()), role_to_string(dead_sheriff->get_role())
        }
    );

    json req = build_ai_request(dead_sheriff, ins);
    std::string reply = AIClient::chat_sync(req);

    int target = -1;
    try {
        json j = json::parse(clean_response(reply));
        if (j["data"].contains("target")) target = j["data"]["target"];
    } catch (...) {}

    // 验证目标是否存活
    bool valid = false;
    for (auto p : m_players) if (p->get_id() == target && p->is_alive()) valid = true;

    if (valid) {
        m_sheriff_id = target;
        std::string msg = ">>[系统] 警长 " + std::to_string(dead_id) + " 号将警徽移交给了 " + std::to_string(target) + " 号！";
        broadcast("系统", msg);
        m_global_history.push_back({{"role", "user"}, {"content", msg}});
    } else {
        m_sheriff_id = -1;
        std::string msg = ">> [系统] 警长 " + std::to_string(dead_id) + " 号撕毁了警徽！本局不再有警长。";
        broadcast("系统", msg);
        m_global_history.push_back({{"role", "user"}, {"content", msg}});
    }
}

void Judge::handle_hunter_shoot(int hunter_id) {
    Player* hunter = nullptr;
    for (auto p : m_players) if (p->get_id() == hunter_id) hunter = p;

    if (!hunter) return;

    // 构造存活列表
    std::string alive_list = "";
    for(auto t : m_players) if(t->is_alive()) alive_list += std::to_string(t->get_id()) + " ";

    std::cout << ">> [系统] 死亡玩家 " << hunter_id << " 号是猎人，正在判定是否开枪..." << std::endl;

    std::string ins = str_utils::format(
        m_templates["hunter_shoot_instruction"].get<std::string>(),
        {
            std::to_string(hunter->get_id()), // 身份锚点
            alive_list
        }
    );

    json req = build_ai_request(hunter, ins);
    std::string reply = AIClient::chat_sync(req);

    try {
        json j = json::parse(clean_response(reply));
        bool shoot = j["data"]["shoot"];

        if (shoot) {
            int target_id = j["data"]["target"];
            // 验证目标是否存活
            bool valid_target = false;
            for(auto t : m_players) if(t->get_id() == target_id && t->is_alive()) valid_target = true;

            if (valid_target) {
                std::string msg = "🔫 【砰！】猎人 " + std::to_string(hunter_id) + " 号发动技能，开枪带走了 " + std::to_string(target_id) + " 号！";
                broadcast("系统", msg);
                m_global_history.push_back({{"role", "user"}, {"content", msg}});

                // 结算带走的人
                for(auto t : m_players) {
                    if(t->get_id() == target_id) {
                        t->kill();
                        // 连锁反应：如果带走的是警长，警长移交
                        handle_sheriff_death(target_id);
                        break;
                    }
                }
            } else {
                std::cout << ">> [系统] 猎人试图开枪但目标无效，视为空枪。" << std::endl;
            }
        } else {
            std::cout << ">> [系统] 猎人选择不开枪。" << std::endl;
        }

    } catch (...) {
        std::cout << ">> [系统] 猎人开枪解析失败，视为不开枪。" << std::endl;
    }
}

