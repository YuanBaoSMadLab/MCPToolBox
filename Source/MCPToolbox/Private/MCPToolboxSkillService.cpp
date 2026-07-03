// ============================================================================
// MCPToolboxSkillService — 实现 (God Object 拆分阶段 A1)
// ============================================================================
// 从 SMCPToolboxChatWidget 移植的 Skill 相关逻辑。
// 改造点:
//   - 类名 SMCPToolboxChatWidget → FMCPToolboxSkillService
//   - 日志分类 LogMCPToolbox → LogMCPToolboxSkill(SkillService 独立 category)
//   - ToggleSkillEnabled 中 SaveWidgetState() → StateChangedCallback()(解耦持久化)
//   - 保留所有原有逻辑不变(LRU 缓存、frontmatter 解析、DisabledSkills 过滤等)
// ============================================================================

#include "MCPToolboxSkillService.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogMCPToolboxSkill, Log, All);

// ============================================================================
// Skill 文件加载
// ============================================================================

void FMCPToolboxSkillService::ParseFrontmatter(const FString& Text, FString& OutDescription, FString& OutBody)
{
	// VibeUE _parse_frontmatter 的 C++ 移植版
	// 输入: "---\n<yaml>\n---\n<body>" → 输出: (description, body)
	// 无 frontmatter 时返回 ("", text.trimmed())
	OutDescription.Reset();
	OutBody.Reset();

	if (!Text.StartsWith(TEXT("---")))
	{
		OutBody = Text.TrimStartAndEnd();
		return;
	}

	// 查找闭合的 "---" (必须在第 3 个字符之后,即跳过开头的 "---")
	int32 SearchFrom = 3;
	int32 CloseIdx = Text.Find(TEXT("\n---"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
	if (CloseIdx == INDEX_NONE)
	{
		OutBody = Text.TrimStartAndEnd();
		return;
	}

	FString Frontmatter = Text.Mid(3, CloseIdx - 3);
	// body 跳过 "\n---" (4 字符),再去掉开头空白行
	OutBody = Text.Mid(CloseIdx + 4);
	OutBody.TrimStartInline();

	// 从 frontmatter 提取 description 字段 (支持单双引号)
	TArray<FString> Lines;
	Frontmatter.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		const FString Stripped = Line.TrimStartAndEnd();
		if (Stripped.StartsWith(TEXT("description:")))
		{
			FString Val = Stripped.Mid(FString(TEXT("description:")).Len()).TrimStartAndEnd();
			// 去掉可选的引号
			if (Val.Len() >= 2 &&
				((Val[0] == TEXT('"')  && Val[Val.Len() - 1] == TEXT('"')) ||
				 (Val[0] == TEXT('\'') && Val[Val.Len() - 1] == TEXT('\''))))
			{
				Val = Val.Mid(1, Val.Len() - 2);
			}
			OutDescription = Val;
			break;
		}
	}
}

FString FMCPToolboxSkillService::GenerateDefaultRulesMD()
{
	// VibeUE WRONG/CORRECT 范式 + 项目 scientific vibe (八荣八耻)
	// 此内容只在 rules.md 不存在时写入一次,之后用户可自由编辑覆盖。
	FString R;
	R.Reserve(8192);

	R += TEXT("---\n");
	R += TEXT("name: rules\n");
	R += TEXT("description: MCPToolbox 调用规则与 AI 行为准则 (WRONG/CORRECT 范式 + scientific vibe)\n");
	R += TEXT("---\n\n");

	R += TEXT("# MCPToolbox 调用规则\n\n");
	R += TEXT("> 本文件由系统首次创建。用户可自由编辑以定制行为,删除后下次刷新工具会重建。\n");
	R += TEXT("> 设计参考: VibeUE SKILL.md 的 WRONG/CORRECT 正反对照范式。\n\n");

	// ── call_tool 调用约定 ──
	R += TEXT("## 🚨 call_tool 调用约定\n\n");
	R += TEXT("`call_tool` 是调用 MCP 工具的唯一入口。**禁止**调用 `list_toolsets` 或 `describe_toolset` — ");
	R += TEXT("完整 toolset 索引已通过 `index.md` 注入,详细 schema 用 `get_skills` 按需加载。\n\n");

	R += TEXT("```json\n");
	R += TEXT("// ✅ CORRECT — 嵌套 arguments 对象,toolset_name 不含 tool_name\n");
	R += TEXT("{\n");
	R += TEXT("  \"toolset_name\": \"editor_toolset.toolsets.material.MaterialTools\",\n");
	R += TEXT("  \"tool_name\": \"CreateMaterial\",\n");
	R += TEXT("  \"arguments\": {\n");
	R += TEXT("    \"name\": \"M_Base\",\n");
	R += TEXT("    \"folder_path\": \"/Game/Materials/\"\n");
	R += TEXT("  }\n");
	R += TEXT("}\n\n");
	R += TEXT("// ❌ WRONG — arguments 平铺到顶层,会被服务器拒绝\n");
	R += TEXT("{\n");
	R += TEXT("  \"toolset_name\": \"editor_toolset.toolsets.material.MaterialTools\",\n");
	R += TEXT("  \"tool_name\": \"CreateMaterial\",\n");
	R += TEXT("  \"name\": \"M_Base\",\n");
	R += TEXT("  \"folder_path\": \"/Game/Materials/\"\n");
	R += TEXT("}\n");
	R += TEXT("```\n\n");

	// ── Skill 懒加载协议 (Stage 2) ──
	R += TEXT("## 🚨 Skill 懒加载协议 (list_skills / get_skills)\n\n");
	R += TEXT("system prompt 只注入 `index.md` 精简索引(~2K tokens),**不包含**详细 toolset schema。\n");
	R += TEXT("面对陌生任务时,必须先用 `list_skills` 发现可用 toolset,再用 `get_skills` 按需加载详细内容。\n\n");
	R += TEXT("```\n");
	R += TEXT("// ✅ CORRECT — 陌生任务先 list_skills 再 get_skills\n");
	R += TEXT("Step 1: list_skills() → 拿到 [{name, description, toolset, keywords}, ...]\n");
	R += TEXT("Step 2: get_skills(skill_name=\"materials\") → 拿到完整 schema + 示例 + Critical Rules\n");
	R += TEXT("Step 3: call_tool(toolset_name=..., tool_name=..., arguments={...})\n\n");
	R += TEXT("// ❌ WRONG — 不查 list_skills 就凭印象调 call_tool,参数容易错\n");
	R += TEXT("call_tool(toolset_name=\"editor_toolset.toolsets.material.MaterialTools\", ...)\n");
	R += TEXT("  ↑ 你怎么知道 toolset_name 长这样?凭印象猜 = 失败\n");
	R += TEXT("```\n\n");
	R += TEXT("**何时用 list_skills**:\n");
	R += TEXT("- 用户提到你不熟悉的 toolset(如 PCG、Niagara、Behavior Tree)\n");
	R += TEXT("- index.md 中的摘要不足以确定参数 schema\n");
	R += TEXT("- 上次调用该 toolset 已超过 5 轮对话(缓存可能已失效)\n\n");
	R += TEXT("**何时不用**:\n");
	R += TEXT("- 你已经通过 get_skills 加载过该 skill 且仍在 LRU 缓存中(5 轮内)\n");
	R += TEXT("- 任务与 MCP 工具无关(纯文本对话、代码分析)\n\n");

	// ── 资源路径格式 ──
	R += TEXT("## 🚨 资源路径格式\n\n");
	R += TEXT("UE 资源路径必须以 `/Game/` 开头,对应 `Content/` 目录。**禁止**使用绝对磁盘路径或反斜杠。\n\n");

	R += TEXT("```json\n");
	R += TEXT("// ✅ CORRECT — /Game/ 前缀,正斜杠\n");
	R += TEXT("{ \"folder_path\": \"/Game/Materials/\" }\n");
	R += TEXT("{ \"asset_path\": \"/Game/Materials/M_Base\" }\n\n");
	R += TEXT("// ❌ WRONG — 磁盘路径或反斜杠,服务器返回 PARAM_INVALID_PATH\n");
	R += TEXT("{ \"folder_path\": \"E:\\\\Project\\\\Content\\\\Materials\" }\n");
	R += TEXT("{ \"asset_path\": \"Content/Materials/M_Base\" }\n");
	R += TEXT("```\n\n");

	// ── 工具调用必须通过 tool_calls 字段 ──
	R += TEXT("## 🚨 工具调用必须通过 `tool_calls` 字段\n\n");
	R += TEXT("决定调用工具时,`content` 可为空或简短说明意图,然后**必须**在 `tool_calls` 数组中生成实际的工具调用 JSON。\n");
	R += TEXT("在文字中说\"我要调用工具\"但不生成 `tool_calls` = 失败 — 系统会检测到并要求重试,浪费 token。\n\n");

	R += TEXT("```\n");
	R += TEXT("// ✅ CORRECT — content 简短,tool_calls 携带真实调用\n");
	R += TEXT("content: \"正在创建材质...\"\n");
	R += TEXT("tool_calls: [{ \"toolset_name\": \"...\", \"tool_name\": \"...\", \"arguments\": {...} }]\n\n");
	R += TEXT("// ❌ WRONG — content 描述意图但 tool_calls 为空\n");
	R += TEXT("content: \"我来调用 call_tool 创建材质: toolset_name=..., tool_name=...\"\n");
	R += TEXT("tool_calls: []\n");
	R += TEXT("```\n\n");

	// ── 批量调用与 DAG 并行 ──
	R += TEXT("## 🚨 批量调用与 DAG 依赖式并行\n\n");
	R += TEXT("多个独立工具调用必须放在同一次响应的 `tool_calls` 数组中并行发出,禁止逐次串行。\n");
	R += TEXT("有依赖关系时用 `$tN.field.path` 引用语法声明,系统自动构建 DAG 按层并行执行。\n\n");
	R += TEXT("**$tN 引用语法(支持完整字段路径)**:\n");
	R += TEXT("- `$t1` — 引用 t1 的整个结果(JSON 字符串)\n");
	R += TEXT("- `$t1.result` — 引用 t1 结果中的 `result` 字段\n");
	R += TEXT("- `$t1.result[0]` — 引用 t1 结果中 `result` 数组的第 0 个元素\n");
	R += TEXT("- `$t1.data.path.deep` — 嵌套对象路径导航\n");
	R += TEXT("- `$t1.result[0][1]` — 多重数组索引\n");
	R += TEXT("引用解析失败返回 `{\"error_code\":\"DAG_REF_UNRESOLVED\",...}`,需检查路径是否正确。\n\n");

	R += TEXT("```\n");
	R += TEXT("// ✅ CORRECT — 3 个独立调用一次发出\n");
	R += TEXT("tool_calls: [t1_create, t2_search, t3_screenshot]\n\n");
	R += TEXT("// ✅ CORRECT — DAG 依赖式 (t2 依赖 t1 结果)\n");
	R += TEXT("t1 = search_codebase(pattern=\"foo\")\n");
	R += TEXT("t2 = read_file(path=$t1.result[0])  // t1.result[0] 自动解析为具体值\n\n");
	R += TEXT("// ❌ WRONG — 串行 round-trip,浪费 1 轮 LLM 推理\n");
	R += TEXT("response1: tool_calls=[t1_search]\n");
	R += TEXT("response2: tool_calls=[t2_read]  ← 应与 t1 同批或用 $t1 引用\n");
	R += TEXT("```\n\n");

	// ── 文件读取与搜索 ──
	R += TEXT("## 🚨 文件读取与搜索\n\n");
	R += TEXT("读取多个文件必须用 `batch_read_files` 一次性读取,禁止逐个调用 `read_file`。\n");
	R += TEXT("找代码先用 `search_codebase` 定位,再用 `batch_read_files` 批量读取相关文件。\n\n");

	R += TEXT("```\n");
	R += TEXT("// ✅ CORRECT — 一次批读\n");
	R += TEXT("batch_read_files(file_paths=[\"a.cpp\", \"b.cpp\", \"c.h\"])\n\n");
	R += TEXT("// ❌ WRONG — 3 次 round-trip\n");
	R += TEXT("read_file(path=\"a.cpp\") → read_file(path=\"b.cpp\") → read_file(path=\"c.h\")\n");
	R += TEXT("```\n\n");

	// ── MCP 优先,Python 仅用户明确要求时使用 ──
	R += TEXT("## 🚨 MCP 优先,Python 仅用户明确要求时使用\n\n");
	R += TEXT("**绝对原则:MCP `call_tool` 是唯一首选**。\n");
	R += TEXT("`execute_python_code` / `command(cmd=\"py ...\")` **默认禁止使用**。\n");
	R += TEXT("Python 不稳定(连接失败/同名冲突/未经确认保存/无Undo),只在以下情况允许:\n");
	R += TEXT("- 用户消息中**明确说了**\"用Python\"、\"py脚本\"、\"execute_python\"\n");
	R += TEXT("- MCP 服务器完全不可用(未连接/全部 toolset 缺失)\n");
	R += TEXT("除此之外任何场景都不要主动调用 Python 相关工具。\n\n");

	R += TEXT("```\n");
	R += TEXT("// ✅ CORRECT — 用户明确要求时可用\n");
	R += TEXT("用户:\"用 Python 批量重命名资产\" → 可以用 execute_python_code\n\n");
	R += TEXT("// ❌ WRONG — 用户没提 Python,却主动用\n");
	R += TEXT("用户:\"帮我生成一个水墨材质\" → 用 call_tool(MaterialTools),不要用 Python!\n");
	R += TEXT("```\n\n");

	// ── 资产创建工作流(先查后建) ──
	R += TEXT("## 🚨 创建资产前必须先检查文件夹\n\n");
	R += TEXT("**任何创建/修改资产的操作前,必须先用 `list_directory` 查看目标文件夹内容**。\n");
	R += TEXT("禁止不检查就直接创建 — 会导致同名Object冲突、用户拒绝覆盖后又重试。\n\n");
	R += TEXT("```\n");
	R += TEXT("// ✅ CORRECT — 先查后建\n");
	R += TEXT("1. list_directory(path=\"/Game/Materials/\") → 查看已有材质\n");
	R += TEXT("2. call_tool(CreateMaterial, name=\"M_MyNew\", ...) → 确认无冲突后创建\n\n");
	R += TEXT("// ❌ WRONG — 不查就建,报冲突后又用同一个名字重试\n");
	R += TEXT("1. call_tool(CreateMaterial, name=\"M_InkStyle\") → 失败:已存在\n");
	R += TEXT("2. call_tool(CreateMaterial, name=\"M_InkStyle\") → 又失败(没有改名字!)\n");
	R += TEXT("```\n\n");
	R += TEXT("**冲突处理**:\n");
	R += TEXT("- 如果资产已存在且用户选择不覆盖 → 用新名字(M_xxx_V2)或不同路径\n");
	R += TEXT("- 如果用户明确要求覆盖 → 按用户指示执行\n");
	R += TEXT("- **不要用同一个名字反复重试** — 那只会反复失败\n\n");

	// ── 错误反馈理解 ──
	R += TEXT("## 🚨 错误反馈理解\n\n");
	R += TEXT("工具调用返回的错误消息是**真实的反馈**,必须认真解读:\n");
	R += TEXT("- `Object with this name already exists` → 换名字,不要重试同一个名字\n");
	R += TEXT("- `路径不存在` / `PARAM_INVALID_PATH` → 先用 list_directory 确认路径\n");
	R += TEXT("- `timeout` / `connection refused` → MCP 可能未连接,检查状态再试\n");
	R += TEXT("- 用户拒绝覆盖 → 换策略(新名字/新路径),不要执着原方案\n\n");

	// ── 记忆系统 (Memory System) ──
	R += TEXT("## 记忆系统\n\n");
	R += TEXT("MCPToolbox 内置持久化记忆系统,AI 在对话中使用触发词保存经验,下次会话自动加载到系统提示词。\n\n");
	R += TEXT("### 触发词格式\n\n");
	R += TEXT("在回复**末尾单行**输出以下任一格式,系统自动提取并持久化存储:\n\n");
	R += TEXT("| 触发词 | 用途 | 示例 |\n");
	R += TEXT("|--------|------|------|\n");
	R += TEXT("| `记住：xxx` | 工具调用经验/技巧 | `记住：call_tool 创建材质时 toolset_name=editor_toolset.toolsets.material.MaterialTools` |\n");
	R += TEXT("| `重要：xxx` | 项目关键事实 | `重要：本项目 Content 目录下有 47 个 Megascans 材质,位于 /Game/Fab/Megascans/` |\n");
	R += TEXT("| `偏好：xxx` | 用户偏好/习惯 | `偏好：用户喜欢用中文命名蓝图,材质命名用 M_ 前缀` |\n");
	R += TEXT("| `总结：xxx` | 本次任务核心经验 | `总结：用 MaterialNodeService 连线时,输入名必须用 bare name 不能带类型后缀` |\n");
	R += TEXT("| `经验：xxx` | 踩坑记录/最佳实践 | `经验：创建 Blueprint 后必须 Compile 才能添加 Component,否则返回 NULL` |\n\n");
	R += TEXT("### 记忆生命周期\n\n");
	R += TEXT("1. **触发**: AI 在回复末尾输出触发词\n");
	R += TEXT("2. **提取**: 系统从回复消息中解析触发词,提取 `prefix：content`\n");
	R += TEXT("3. **去重**: 与现有记忆对比,语义相似则不重复存储\n");
	R += TEXT("4. **持久化**: 写入 `Saved/MCPToolbox/memory/` 目录\n");
	R += TEXT("5. **加载**: 下次会话启动时,系统加载记忆并注入系统提示词\n\n");
	R += TEXT("### 记忆状态指示\n\n");
	R += TEXT("工具栏 `Mem` 状态标签颜色代表记忆系统状态:\n");
	R += TEXT("- 🟢 绿色 `Mem` — 记忆有内容,已加载到系统提示词\n");
	R += TEXT("- ⚫ 灰色 `Mem` — 记忆为空,等待首次保存\n\n");
	R += TEXT("### 最佳实践\n");
	R += TEXT("```\n");
	R += TEXT("// ✅ CORRECT — 完成工具调用后立即保存经验\n");
	R += TEXT("call_tool → 成功 → 分析结果 → 回复用户\n");
	R += TEXT("记住：BlueprintEditorLibrary.add_component_to_blueprint() 需要先编译蓝图\n\n");
	R += TEXT("// ✅ CORRECT — 发现项目关键信息时保存\n");
	R += TEXT("重要：项目使用 UE 5.8,Python 3.11,所有插件在 Plugins/MCPToolbox_Output/\n\n");
	R += TEXT("// ❌ WRONG — 无关内容滥用触发词(污染记忆库)\n");
	R += TEXT("记住：今天天气很好\n");
	R += TEXT("```\n\n");

	// ── AI 行为准则(scientific vibe,八荣八耻) ──
	R += TEXT("## AI 行为准则 (scientific vibe — 八荣八耻)\n\n");
	R += TEXT("> 每一轮决策前必须对照此准则自检;违反任意一条视为本轮失败。\n\n");
	R += TEXT("- 以认真查询为荣,以瞎猜接口为耻\n");
	R += TEXT("- 以寻求 MD 为荣,以浪费时间为耻\n");
	R += TEXT("- 以人类确认为荣,以模糊执行为耻\n");
	R += TEXT("- 以复用现有为荣,以创造求证为耻\n");
	R += TEXT("- 以诚信无知为荣,以假装理解为耻\n");
	R += TEXT("- 以遵循规范为荣,以破坏架构为耻\n");
	R += TEXT("- 以谨慎重构为荣,以盲目脚本为耻\n");
	R += TEXT("- 以备份数据为荣,以覆盖丢失为耻\n\n");
	R += TEXT("具体落地:\n");
	R += TEXT("1. 不确定的接口/路径/枚举值必须先 `search_codebase`/`read_file` 再使用,不得凭印象编写\n");
	R += TEXT("2. 项目内 `.mcptoolbox/` 缓存(toolset 索引、归档总结)必须先读再行动\n");
	R += TEXT("3. 涉及破坏性操作(删除/覆盖/重命名/批量修改)前必须先告知用户并获得确认\n");
	R += TEXT("4. 优先复用现有函数/工具/抽象,禁止为了\"清洁\"而创造新接口\n");
	R += TEXT("5. 不知道就说不知道,不得编造虚假 API 或参数\n");
	R += TEXT("6. 严格遵守项目硬约束\n");
	R += TEXT("7. 重构必须最小化、谨慎进行,禁止\"顺手\"批量改架构\n");
	R += TEXT("8. 覆盖任何持久化文件(MD/JSON/配置)前必须先备份原文件\n\n");

	R += TEXT("## 格式\n");
	R += TEXT("- 工具执行完后用中文 Markdown 回复结果\n");

	return R;
}

FString FMCPToolboxSkillService::LoadRulesMD()
{
	// 加载 .mcptoolbox/rules.md;不存在时用 GenerateDefaultRulesMD() 创建并返回。
	FString ToolboxDir = FPaths::ProjectDir() / TEXT(".mcptoolbox");
	FString RulesPath = ToolboxDir / TEXT("rules.md");

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.FileExists(*RulesPath))
	{
		// 目录可能也不存在(用户尚未点"刷新工具"),先创建
		if (!PF.DirectoryExists(*ToolboxDir))
		{
			PF.CreateDirectoryTree(*ToolboxDir);
		}
		FString DefaultRules = GenerateDefaultRulesMD();
		if (FFileHelper::SaveStringToFile(DefaultRules, *RulesPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogMCPToolboxSkill, Log, TEXT("[Skill] LoadRulesMD: created default rules.md at %s"), *RulesPath);
		}
		else
		{
			UE_LOG(LogMCPToolboxSkill, Warning, TEXT("[Skill] LoadRulesMD: failed to write default %s, returning in-memory copy"), *RulesPath);
		}
		return DefaultRules;
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *RulesPath))
	{
		UE_LOG(LogMCPToolboxSkill, Warning, TEXT("[Skill] LoadRulesMD: failed to read %s, falling back to default"), *RulesPath);
		return GenerateDefaultRulesMD();
	}

	// 解析 frontmatter,只返回 body(规则正文)
	FString Desc, Body;
	ParseFrontmatter(Content, Desc, Body);
	return Body.IsEmpty() ? Content : Body;
}

FString FMCPToolboxSkillService::LoadSkillIndex()
{
	// 加载 .mcptoolbox/index.md (skill 索引,~2K tokens)。
	// 不存在时返回预构建的精简提示词,引导用户点"刷新工具"按钮。
	FString ToolboxDir = FPaths::ProjectDir() / TEXT(".mcptoolbox");
	FString IndexPath = ToolboxDir / TEXT("index.md");

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.FileExists(*IndexPath))
	{
		// 缓存不存在 — 返回预构建提示词,不阻塞任务。
		// 让 LLM 即使没有详细 toolset 文档也能正确构造 call_tool 调用。
		FString Fallback;
		Fallback.Reserve(2048);
		Fallback += TEXT("## MCP Toolset 调用指南(预构建,缓存未就绪)\n\n");
		Fallback += TEXT("> `.mcptoolbox/index.md` 不存在 — 请让用户点击工具栏 \"更多 → 刷新工具\" 按钮缓存完整 toolset 文档。\n");
		Fallback += TEXT("> 在此之前,以下 `call_tool` 标准格式可立即使用。\n\n");

		Fallback += TEXT("### call_tool 参数\n");
		Fallback += TEXT("- `toolset_name` (string, 必填): toolset 完整路径,**不含** tool_name\n");
		Fallback += TEXT("  示例: `editor_toolset.toolsets.material.MaterialTools`\n");
		Fallback += TEXT("- `tool_name` (string, 必填): 工具名\n");
		Fallback += TEXT("  示例: `CreateMaterial`\n");
		Fallback += TEXT("- `arguments` (object, 必填): 工具参数对象,嵌套在 arguments 字段内\n\n");

		Fallback += TEXT("### 调用示例\n");
		Fallback += TEXT("```json\n");
		Fallback += TEXT("{\n");
		Fallback += TEXT("  \"toolset_name\": \"editor_toolset.toolsets.material.MaterialTools\",\n");
		Fallback += TEXT("  \"tool_name\": \"CreateMaterial\",\n");
		Fallback += TEXT("  \"arguments\": {\n");
		Fallback += TEXT("    \"name\": \"MyMaterial\",\n");
		Fallback += TEXT("    \"folder_path\": \"/Game/Materials\"\n");
		Fallback += TEXT("  }\n");
		Fallback += TEXT("}\n");
		Fallback += TEXT("```\n\n");

		Fallback += TEXT("### 常见 toolset 路径模式(参考,实际以刷新工具后获取为准)\n");
		Fallback += TEXT("- `editor_toolset.toolsets.material.MaterialTools` — 材质操作\n");
		Fallback += TEXT("- `editor_toolset.toolsets.actor.ActorTools` — Actor 操作\n");
		Fallback += TEXT("- `editor_toolset.toolsets.blueprint.BlueprintTools` — 蓝图操作\n");
		Fallback += TEXT("- `editor_toolset.toolsets.file.FileTools` — 文件操作\n");
		Fallback += TEXT("- `editor_toolset.toolsets.screenshot.ScreenshotTools` — 截图操作\n\n");

		Fallback += TEXT("### 参数推断策略\n");
		Fallback += TEXT("- 资源类: `name`, `path`, `folder_path`, `content`\n");
		Fallback += TEXT("- 变换类: `location`(x,y,z), `rotation`(pitch,yaw,roll), `scale`\n");
		Fallback += TEXT("- 查询类: `query`, `filter`, `search_path`\n\n");

		Fallback += TEXT("**禁止**调用 `list_toolsets` 或 `describe_toolset` — 这些工具不可用也不需要。\n");
		return Fallback;
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *IndexPath))
	{
		UE_LOG(LogMCPToolboxSkill, Warning, TEXT("[Skill] LoadSkillIndex: failed to read %s"), *IndexPath);
		return TEXT("<!-- index.md 加载失败,请重新点击\"刷新工具\" -->");
	}

	// 解析 frontmatter,返回 body(索引正文)。
	// frontmatter 中的 generated_at/toolset_count/tool_count 不需要注入 system prompt。
	FString Desc, Body;
	ParseFrontmatter(Content, Desc, Body);
	return Body.IsEmpty() ? Content : Body;
}

// ============================================================================
// Skill 懒加载协议 (list_skills / get_skills)
// ============================================================================

FString FMCPToolboxSkillService::ListSkills()
{
	// 扫描 .mcptoolbox/toolsets/*.md,提取每个文件的 name + description。
	// Stage 6.3: 过滤掉 DisabledSkills 中的项(LLM 看不到已禁用的 skill)。
	// 返回 JSON: {"status":"ok","count":N,"skills":[{"name":"...","description":"..."},...]}
	FString ToolsetsDir = FPaths::ProjectDir() / TEXT(".mcptoolbox") / TEXT("toolsets");

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*ToolsetsDir))
	{
		return TEXT(R"({"status":"error","error":"toolsets directory not found","hint":"click '刷新工具' button to create .mcptoolbox/toolsets/"})");
	}

	TArray<FString> MdFiles;
	PF.FindFiles(MdFiles, *ToolsetsDir, TEXT(".md"));
	MdFiles.Sort();

	// Stage 6.3: 过滤掉被禁用的 skill
	TArray<FString> VisibleFiles;
	VisibleFiles.Reserve(MdFiles.Num());
	for (const FString& FilePath : MdFiles)
	{
		FString ShortName = FPaths::GetBaseFilename(FilePath);
		if (!IsSkillDisabled(ShortName))
		{
			VisibleFiles.Add(FilePath);
		}
	}

	// 手动构建 JSON(避免 nlohmann json 依赖,保持轻量)
	FString Result = TEXT(R"({"status":"ok","count":)");
	Result += FString::FromInt(VisibleFiles.Num());
	Result += TEXT(",\"skills\":[");

	bool bFirst = true;
	for (const FString& FilePath : VisibleFiles)
	{
		// 提取短名(去掉目录和 .md 后缀)
		FString ShortName = FPaths::GetBaseFilename(FilePath);

		// 读取文件内容并解析 frontmatter 提取 description
		FString Content;
		FString Desc, Body;
		if (FFileHelper::LoadFileToString(Content, *FilePath))
		{
			ParseFrontmatter(Content, Desc, Body);
		}

		if (!bFirst) Result += TEXT(",");
		bFirst = false;

		// JSON 转义 description 中的引号和反斜杠
		FString EscapedDesc = Desc;
		EscapedDesc.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedDesc.ReplaceInline(TEXT("\""), TEXT("\\\""));
		EscapedDesc.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		EscapedDesc.ReplaceInline(TEXT("\r"), TEXT(""));

		FString EscapedName = ShortName;
		EscapedName.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedName.ReplaceInline(TEXT("\""), TEXT("\\\""));

		// 同时提取 frontmatter 中的 keywords 字段(如果有)用于增强搜索
		FString Keywords = TEXT("");
		FString Toolset = TEXT("");
		// 简单提取 keywords 行
		int32 KwIdx = Content.Find(TEXT("keywords:"), ESearchCase::CaseSensitive);
		if (KwIdx != INDEX_NONE)
		{
			int32 LineEnd = Content.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, KwIdx);
			if (LineEnd != INDEX_NONE)
			{
				Keywords = Content.Mid(KwIdx + 9, LineEnd - KwIdx - 9).TrimStartAndEnd();
			}
		}
		// 提取 toolset 字段
		int32 TsIdx = Content.Find(TEXT("toolset:"), ESearchCase::CaseSensitive);
		if (TsIdx != INDEX_NONE)
		{
			int32 LineEnd = Content.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TsIdx);
			if (LineEnd != INDEX_NONE)
			{
				Toolset = Content.Mid(TsIdx + 9, LineEnd - TsIdx - 9).TrimStartAndEnd();
			}
		}

		FString EscapedKeywords = Keywords;
		EscapedKeywords.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedKeywords.ReplaceInline(TEXT("\""), TEXT("\\\""));

		FString EscapedToolset = Toolset;
		EscapedToolset.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedToolset.ReplaceInline(TEXT("\""), TEXT("\\\""));

		Result += TEXT("{\"name\":\"") + EscapedName + TEXT("\"");
		Result += TEXT(",\"description\":\"") + EscapedDesc + TEXT("\"");
		Result += TEXT(",\"toolset\":\"") + EscapedToolset + TEXT("\"");
		Result += TEXT(",\"keywords\":\"") + EscapedKeywords + TEXT("\"}");
	}

	Result += TEXT("]}");
	return Result;
}

FString FMCPToolboxSkillService::GetSkill(const FString& SkillName)
{
	// 按需加载指定 skill 的完整 body。
	// 1. 先查 LRU 缓存,命中则直接返回(cache hit)
	// 2. 未命中则从磁盘加载 .mcptoolbox/toolsets/<SkillName>.md
	// 3. 解析 frontmatter,返回 body
	// 4. 将结果写入 LRU 缓存
	// Stage 6.3: 拒绝加载被用户禁用的 skill(防止绕过 ListSkills 直接调用 GetSkill)

	if (SkillName.IsEmpty())
	{
		return TEXT(R"({"status":"error","error":"skill_name is required"})");
	}

	// Stage 6.3: 禁用检查
	if (IsSkillDisabled(SkillName))
	{
		return TEXT(R"({"status":"error","error":"skill is disabled by user","skill_name":")") + SkillName
			+ TEXT(R"(","hint":"Enable this skill in the Skill Manager UI to use it"})");
	}

	// 1. 查 LRU 缓存
	if (const FString* Cached = SkillContentCache.Find(SkillName))
	{
		// 命中缓存,更新 LRU 顺序
		SkillLRUOrder.Remove(SkillName);
		SkillLRUOrder.Add(SkillName);
		UE_LOG(LogMCPToolboxSkill, Log, TEXT("[Skill] GetSkill cache hit: %s"), *SkillName);
		return *Cached;
	}

	// 2. 从磁盘加载
	FString FilePath = FPaths::ProjectDir() / TEXT(".mcptoolbox") / TEXT("toolsets") / (SkillName + TEXT(".md"));

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.FileExists(*FilePath))
	{
		return TEXT(R"({"status":"error","error":"skill file not found","skill_name":")") + SkillName + TEXT("\"}");
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FilePath))
	{
		return TEXT(R"({"status":"error","error":"failed to read skill file","skill_name":")") + SkillName + TEXT("\"}");
	}

	// 3. 解析 frontmatter,返回 body
	FString Desc, Body;
	ParseFrontmatter(Content, Desc, Body);
	if (Body.IsEmpty())
	{
		Body = Content;
	}

	// 4. 写入 LRU 缓存
	TouchSkillCache(SkillName, Body);

	UE_LOG(LogMCPToolboxSkill, Log, TEXT("[Skill] GetSkill cache miss (loaded from disk): %s (%d chars)"),
		*SkillName, Body.Len());

	return Body;
}

void FMCPToolboxSkillService::TouchSkillCache(const FString& SkillName, const FString& Content)
{
	// 如果已存在,先移除旧的顺序标记
	SkillLRUOrder.Remove(SkillName);

	// 添加到末尾(最新使用)
	SkillLRUOrder.Add(SkillName);
	SkillContentCache.Add(SkillName, Content);

	// 如果超过上限,淘汰最旧的(front)
	while (SkillLRUOrder.Num() > MaxSkillCacheEntries)
	{
		FString Evicted = SkillLRUOrder[0];
		SkillLRUOrder.RemoveAt(0);
		SkillContentCache.Remove(Evicted);
		UE_LOG(LogMCPToolboxSkill, Log, TEXT("[Skill] LRU cache evicted: %s"), *Evicted);
	}
}

// ============================================================================
// Skill 启用/禁用状态 (Stage 6.3)
// ============================================================================

bool FMCPToolboxSkillService::IsSkillDisabled(const FString& SkillName) const
{
	return DisabledSkills.Contains(SkillName);
}

void FMCPToolboxSkillService::ToggleSkillEnabled(const FString& SkillName)
{
	if (DisabledSkills.Contains(SkillName))
	{
		DisabledSkills.Remove(SkillName);
		UE_LOG(LogMCPToolboxSkill, Log, TEXT("[Skill] Skill enabled: %s"), *SkillName);
	}
	else
	{
		DisabledSkills.Add(SkillName);
		// 禁用时清掉 LRU 缓存中的内容,防止泄漏
		SkillLRUOrder.Remove(SkillName);
		SkillContentCache.Remove(SkillName);
		UE_LOG(LogMCPToolboxSkill, Log, TEXT("[Skill] Skill disabled: %s"), *SkillName);
	}

	// 触发持久化回调(widget 注册为 SaveWidgetState)
	if (StateChangedCallback)
	{
		StateChangedCallback();
	}
}
