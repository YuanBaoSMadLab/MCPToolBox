// ============================================================================
// MCPToolboxSkillService — Skill 内容管理服务 (God Object 拆分阶段 A1)
// ============================================================================
// 设计动机:
//   SMCPToolboxChatWidget 是 God Object(已知问题 #5),承担 12+ 职责。
//   本阶段把 Skill 相关逻辑抽取为独立 Service,降低 widget 耦合度,提升可测试性。
//
// 职责边界:
//   - Skill 文件加载(ParseFrontmatter / LoadRulesMD / LoadSkillIndex / GenerateDefaultRulesMD)
//   - Skill 懒加载协议(ListSkills / GetSkill + LRU 缓存)
//   - Skill 启用/禁用状态(DisabledSkills + ToggleSkillEnabled + IsSkillDisabled)
//
// 解耦方式:
//   - SkillService 持有所有 Skill 状态(SkillContentCache / SkillLRUOrder / DisabledSkills)
//   - 持久化通过 SetStateChangedCallback 解耦:widget 注册回调,
//     ToggleSkillEnabled 修改状态后触发回调,widget 执行 SaveWidgetState
//   - LoadWidgetState/SaveWidgetState 留在 widget,通过
//     GetDisabledSkills() / SetDisabledSkills() 与 SkillService 交互
//
// 线程安全:仅在游戏线程调用(MCP 工具回调 + UI 事件),无需加锁。
// ============================================================================
#pragma once

#include "CoreMinimal.h"

/**
 * Skill 内容管理服务。
 *
 * 从 SMCPToolboxChatWidget 抽取的独立 Service(God Object 拆分阶段 A1)。
 * 负责 Skill 文件加载、懒加载协议、启停状态管理。
 *
 * 使用模式:
 *   // widget 持有实例:
 *   FMCPToolboxSkillService SkillService;
 *
 *   // 注册持久化回调(在 Construct 中):
 *   SkillService.SetStateChangedCallback([this]() { SaveWidgetState(); });
 *
 *   // BuildSystemPrompt:
 *   Prompt += SkillService.LoadSkillIndex();
 *   Prompt += SkillService.LoadRulesMD();
 *
 *   // LoadWidgetState:
 *   SkillService.SetDisabledSkills(LoadedArray);
 *
 *   // SaveWidgetState:
 *   for (const FString& S : SkillService.GetDisabledSkills()) { ... }
 */
class MCPTOOLBOX_API FMCPToolboxSkillService
{
public:
	FMCPToolboxSkillService() = default;
	~FMCPToolboxSkillService() = default;

	// ── Skill 文件加载 ──

	/**
	 * 解析 YAML frontmatter,返回 (description, body)。
	 * 输入: "---\n<yaml>\n---\n<body>" → 输出: (description, body)
	 * 无 frontmatter 时返回 ("", text.trimmed())
	 * VibeUE _parse_frontmatter 的 C++ 移植版。
	 */
	static void ParseFrontmatter(const FString& Text, FString& OutDescription, FString& OutBody);

	/**
	 * 加载 .mcptoolbox/rules.md;不存在时用 GenerateDefaultRulesMD() 创建并返回。
	 * 含 WRONG/CORRECT 范式 + scientific vibe(八荣八耻)。
	 */
	FString LoadRulesMD();

	/**
	 * 加载 .mcptoolbox/index.md(skill 索引,~2K tokens)。
	 * 不存在时返回预构建的精简提示词,引导用户点"刷新工具"按钮。
	 */
	FString LoadSkillIndex();

	/**
	 * 生成默认 rules.md 内容(VibeUE WRONG/CORRECT 范式 + 项目 scientific vibe)。
	 * 仅在 rules.md 不存在时写入一次,之后用户可自由编辑覆盖。
	 */
	static FString GenerateDefaultRulesMD();

	// ── Skill 懒加载协议(Stage 2: list_skills / get_skills 虚拟工具) ──

	/**
	 * 列出所有可用 skill 的轻量摘要(name + description + toolset + keywords)。
	 * Stage 6.3: 过滤掉 DisabledSkills 中的项(LLM 看不到已禁用的 skill)。
	 * 返回 JSON: {"status":"ok","count":N,"skills":[{"name":"...","description":"..."},...]}
	 */
	FString ListSkills();

	/**
	 * 按需加载指定 skill 的完整 body。
	 * 1. 先查 LRU 缓存,命中则直接返回(cache hit)
	 * 2. 未命中则从磁盘加载 .mcptoolbox/toolsets/<SkillName>.md
	 * 3. 解析 frontmatter,返回 body
	 * 4. 将结果写入 LRU 缓存
	 * Stage 6.3: 拒绝加载被用户禁用的 skill(防止绕过 ListSkills 直接调用 GetSkill)
	 */
	FString GetSkill(const FString& SkillName);

	/**
	 * 触摸 LRU 缓存:将 skill 移到最近使用位置。
	 * 如果缓存超过 MaxSkillCacheEntries,淘汰最旧条目。
	 */
	void TouchSkillCache(const FString& SkillName, const FString& Content);

	/** 清空 LRU 缓存(禁用 skill 时调用,防止泄漏) */
	void ClearSkillCache() { SkillContentCache.Reset(); SkillLRUOrder.Reset(); }

	// ── Skill 启用/禁用状态(Stage 6.3) ──

	/** 查询指定 skill 是否被用户禁用 */
	bool IsSkillDisabled(const FString& SkillName) const;

	/**
	 * 切换指定 skill 的启用/禁用状态。
	 * 禁用时清掉 LRU 缓存中的内容,防止泄漏。
	 * 修改后触发 StateChangedCallback(让 widget 执行 SaveWidgetState)。
	 */
	void ToggleSkillEnabled(const FString& SkillName);

	// ── 持久化交互(供 widget 的 LoadWidgetState/SaveWidgetState 调用) ──

	/** 获取当前禁用的 skill 列表(用于 SaveWidgetState 序列化) */
	const TArray<FString>& GetDisabledSkills() const { return DisabledSkills; }

	/** 设置禁用的 skill 列表(用于 LoadWidgetState 反序列化) */
	void SetDisabledSkills(const TArray<FString>& InDisabledSkills) { DisabledSkills = InDisabledSkills; }

	/**
	 * 注册状态变更回调。ToggleSkillEnabled 修改状态后会调用此回调。
	 * widget 通常注册为 [this]() { SaveWidgetState(); } 以持久化到 widget_state.json。
	 */
	using FStateChangedCallback = TFunction<void()>;
	void SetStateChangedCallback(FStateChangedCallback Callback) { StateChangedCallback = MoveTemp(Callback); }

private:
	// ── LRU 缓存(会话级) ──

	/** Skill 内容缓存: skill_name → body content(frontmatter 已剥离) */
	TMap<FString, FString> SkillContentCache;

	/** LRU 顺序跟踪: front = oldest, back = most recently used */
	TArray<FString> SkillLRUOrder;

	/** 内存中保留的 skill body 最大数量(超过则淘汰最旧) */
	static constexpr int32 MaxSkillCacheEntries = 15;

	// ── 启停状态 ──

	/** 用户禁用的 skill 名列表(文件名不含 .md),持久化到 widget_state.json */
	TArray<FString> DisabledSkills;

	// ── 持久化回调 ──

	/** 状态变更回调(ToggleSkillEnabled 后触发,widget 注册以执行 SaveWidgetState) */
	FStateChangedCallback StateChangedCallback;
};
