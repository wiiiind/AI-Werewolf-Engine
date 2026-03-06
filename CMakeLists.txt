cmake_minimum_required(VERSION 3.20)
project(WerewolfEngine)

# 1. 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. 查找依赖库
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# 3. 设置头文件搜索路径 (包含你刚建的四个文件夹)
include_directories(net base ai game)

# 4. 显式列出所有源文件
set(SOURCE_FILES
        main.cpp
        net/epoll_utils.cpp
        net/http_conn.cpp
        base/log.cpp
        ai/ai_client.cpp
        ai/ai_client.h
        game/player.h
        game/GameContext.cpp
        game/GameContext.h
        game/EventBus.cpp
        game/EventBus.h
        game/RuleEngine.cpp
        game/RuleEngine.h
        game/AIOrchestrator.h
        game/support/PromptParsers.cpp
        game/support/PromptParsers.h
        game/support/PhaseUtils.cpp
        game/support/PhaseUtils.h
        game/support/TaskExecutor.cpp
        game/support/TaskExecutor.h
        game/hooks/SkillHooks.cpp
        game/hooks/SkillHooks.h
        game/phases/VotePhase.cpp
        game/phases/VotePhase.h
        game/phases/DaySpeakPhase.cpp
        game/phases/DaySpeakPhase.h
        game/phases/SheriffElectionPhase.cpp
        game/phases/SheriffElectionPhase.h
        game/phases/NightPhase.cpp
        game/phases/NightPhase.h
        game/Judge.cpp
        game/Judge.h
        base/string_utils.h

)

# 5. 生成可执行程序
add_executable(WerewolfEngine ${SOURCE_FILES})

# 6. 链接库 (连接线程库和 CURL)
target_link_libraries(WerewolfEngine PRIVATE
        Threads::Threads
        CURL::libcurl
)

add_executable(LogOverflowTest
        tests/log_overflow_test.cpp
        base/log.cpp
)

add_executable(DaySpeakWwkInterruptTest
        tests/day_speak_wwk_interrupt_test.cpp
        base/log.cpp
        ai/ai_client.cpp
        game/GameContext.cpp
        game/EventBus.cpp
        game/support/PromptParsers.cpp
        game/support/PhaseUtils.cpp
        game/support/TaskExecutor.cpp
        game/phases/DaySpeakPhase.cpp
)

add_executable(SheriffElectionWwkInterruptTest
        tests/sheriff_election_wwk_interrupt_test.cpp
        base/log.cpp
        ai/ai_client.cpp
        game/GameContext.cpp
        game/EventBus.cpp
        game/RuleEngine.cpp
        game/support/PromptParsers.cpp
        game/support/PhaseUtils.cpp
        game/support/TaskExecutor.cpp
        game/phases/SheriffElectionPhase.cpp
)

add_executable(VotePkWwkInterruptTest
        tests/vote_pk_wwk_interrupt_test.cpp
        base/log.cpp
        ai/ai_client.cpp
        game/GameContext.cpp
        game/EventBus.cpp
        game/RuleEngine.cpp
        game/support/PromptParsers.cpp
        game/support/PhaseUtils.cpp
        game/support/TaskExecutor.cpp
        game/phases/VotePhase.cpp
)

add_executable(SheriffElectionDay2WwkExplosionTest
        tests/sheriff_election_day2_wwk_explosion_test.cpp
        base/log.cpp
        ai/ai_client.cpp
        game/GameContext.cpp
        game/EventBus.cpp
        game/RuleEngine.cpp
        game/support/PromptParsers.cpp
        game/support/PhaseUtils.cpp
        game/support/TaskExecutor.cpp
        game/phases/SheriffElectionPhase.cpp
)

target_link_libraries(LogOverflowTest PRIVATE
        Threads::Threads
)

target_link_libraries(DaySpeakWwkInterruptTest PRIVATE
        Threads::Threads
        CURL::libcurl
)

target_link_libraries(SheriffElectionWwkInterruptTest PRIVATE
        Threads::Threads
        CURL::libcurl
)

target_link_libraries(VotePkWwkInterruptTest PRIVATE
        Threads::Threads
        CURL::libcurl
)

target_link_libraries(SheriffElectionDay2WwkExplosionTest PRIVATE
        Threads::Threads
        CURL::libcurl
)
