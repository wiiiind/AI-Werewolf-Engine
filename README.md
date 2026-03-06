# 🐺 AI Werewolf Engine (C++)

[![Language](https://img.shields.io/badge/language-C%2B%2B17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B)
[![Platform](https://img.shields.io/badge/platform-Linux%20%2F%20WSL-lightgrey.svg)]()
[![LLM](https://img.shields.io/badge/LLM-DeepSeek%20V3-success.svg)]()
[![Build](https://img.shields.io/badge/build-CMake-brightgreen.svg)]()

> **一句话简介**：一个基于 C++ 高性能异步架构构建的 **多智能体博弈推演引擎 (Multi-Agent Game Engine)**。目前以 12 人标准《狼人杀》为场景，实现多个 LLM 在复杂隐藏信息下的真实并发推理、逻辑欺骗与状态回滚。

A high-performance, asynchronous Multi-Agent System (MAS) game engine built in C++17. Originally evolved from a custom Epoll Reactor WebServer, it now serves as a robust foundation for orchestrating complex LLM (Large Language Model) interactions, featuring optimistic concurrency control, context isolation, and DAG-based task scheduling.

## 🚀 核心特性 (Core Features)

### 1. 极致压榨性能的并发编排 (DAG Task Orchestration)
完全抛弃了 Python 脚本中常见的低效单线程轮询。基于 `std::future` 和自研的 `TaskExecutor`，引擎在“黑夜阶段”实现了复杂的**有向无环图 (DAG)** 任务编排：
*   **独立并行**：预言家、守卫在独立的后台线程中并发思考。
*   **依赖串行**：狼人阵营完成“Map-Reduce”式的投票指刀后，立刻在后台唤醒女巫进行结算，多条流水线互不阻塞。

### 2. 乐观并发控制与状态回滚 (Optimistic Concurrency & Rollback)
为了实现“白狼王可以在任何人发言时随时打断并自爆”的真实游戏机制，引擎引入了高级的并发事务控制：
*   **生产者-消费者模型**：白天发言时，主线程（生产者）不等待，直接推进流程；独立的白狼王线程（消费者）在后台通过 `block_queue` 异步审判每一句发言。
*   **原子中断与回滚**：一旦白狼王决定自爆，通过 `std::atomic` 触发高优先级中断，引擎立刻截断当前发言，并**像数据库事务一样回滚 (Rollback)** 触发点之后的全局上下文（Global History），确保游戏状态严格一致。

### 3. 缓存友好的上下文管理 (Cache-Friendly Context Design)
针对长文本大模型（如 DeepSeek-V3）API 费用高昂的问题，深度重构了 Prompt 结构：
*   **分离公有与私有记忆**：全局共享的“系统规则”与“白天发言记录”被固定在 Prompt 前缀。
*   **最大化缓存命中率**：将特定 Agent 的“私有身份”、“暗中操作”以及“当前指令”置于 Prompt 尾部。在 12 人局的高频请求中，有效利用了 LLM 的 **Context Caching（前缀缓存）**，将 API 成本降低了 80% 以上。

### 4. 彻底解耦的事件驱动架构 (Event-Driven Architecture)
*   消灭了“上帝类 (God Class)”。
*   引入 **EventBus (事件总线)** 与 **SkillHooks (技能钩子)**。角色技能（如猎人开枪、白狼王自爆）均通过事件拦截触发，添加新角色无需修改核心状态机代码。
*   纯数据驱动：所有的角色性格、提示词模板均抽离在 `roles.json` 和 `templates.json` 中。

## 🏗️ 系统架构图 (Architecture Diagram)
![流程图](resources/Architecture%20Diagram.png)

## 📂 核心目录说明 (Directory Structure)

*   `net/` & `base/`: 底层基础设施（Epoll 封装、阻塞队列、异步日志、字符串工具）。
*   `ai/`: AI 通信网关，封装了底层的 `libcurl` 请求与 `nlohmann/json` 解析，支持流式 (SSE) 与静默并发请求。
*   `game/`: 游戏引擎核心。
    *   `GameContext.h`: 游戏全局状态隔离。
    *   `EventBus.h`: 事件发布-订阅总线。
    *   `RuleEngine.h`: 纯逻辑计算（计票、同守同救冲突判定），100% 单元测试覆盖。
    *   `phases/`: 状态机拆分（入夜、警长竞选、白天发言、投票）。
*   `tests/`: 提供“残局快照 (Snapshot)”的依赖注入测试，用于验证复杂的异步回滚逻辑。

## 🛠️ 编译与运行 (Build & Run)

### 环境要求 (Prerequisites)
*   Linux (Ubuntu 22.04+ / WSL2 推荐)
*   CMake (3.20+)
*   GCC (支持 C++17)
*   依赖库：`libcurl4-openssl-dev`, `nlohmann-json3-dev`

### 构建步骤 (Steps)

```bash
# 1. 克隆仓库
git clone https://github.com/wiiiind/AI-Werewolf-Engine.git
cd AI-Werewolf-Engine

# 2. 配置环境变量 (替换为你的真实 API Key)
export DEEPSEEK_API_KEY="sk-xxxxxxxxxxxxxxxx"

# 3. 编译
mkdir build && cd build
cmake ..
make

# 4. 运行 12 人白狼王局实况推演
./WerewolfEngine

# 5. 运行核心回滚逻辑测试
./DaySpeakWwkInterruptTest
```

## 📜 精彩战报展示 (Gameplay Highlights)

*(引擎能够生成极具沉浸感和逻辑深度的 AI 欺骗与博弈。以下为真实运行截取：)*
*   **白狼王的悍跳与自爆**：在警长竞选中，真女巫跳出报银水，潜伏的白狼王判定局势不利，触发异步中断，果断自爆带走女巫，并通过遗言成功污蔑女巫身份，导致真预言家在白天被好人抗推。
*   **深水狼的伪装**：配置为“装可怜的深水狼”的 Agent，在队友全部阵亡后，完美扮演“晕民”，在残局中诱导猎人开错枪，完成极限翻盘。

---
*Powered by DeepSeek V3 & C++ Reactor Architecture*