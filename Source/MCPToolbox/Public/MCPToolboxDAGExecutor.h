// ============================================================================
// MCPToolboxDAGExecutor — DAG 工具调用分析与参数解析服务 (A2 God Object 拆分)
// ============================================================================
// 从 SMCPToolboxChatWidget 中抽取纯逻辑 DAG 方法:
//   - HasDAGDependencies
//   - ConvertToolCallsToDAGFormat
//   - ResolveDAGFieldPath
//   - SetResolvedParam
//
// 设计原则: 只包含纯逻辑,不依赖 Widget UI/消息/网络。
// 使用模式: FMCPToolboxDAGExecutor::HasDAGDependencies(ToolCalls);
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "MCPToolboxDAGTypes.h"
#include "MCPToolboxErrorFormat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * DAG 工具调用分析与参数解析服务。
 *
 * 职责:
 *   1. 检测工具调用是否包含 DAG 依赖($tN.field 引用)
 *   2. 将 OpenAI 风格 tool_calls 转换为 DAG 任务格式
 *   3. 解析 $tN.field.path 依赖引用为具体值
 */
class MCPTOOLBOX_API FMCPToolboxDAGExecutor
{
public:
	/** 检测工具调用数组是否包含 DAG 依赖 */
	static bool HasDAGDependencies(const TArray<TSharedPtr<FJsonValue>>& ToolCalls);

	/** 将 OpenAI 风格 tool_calls 转换为 DAG 任务格式 */
	static void ConvertToolCallsToDAGFormat(
		const TArray<TSharedPtr<FJsonValue>>& ToolCalls,
		TArray<TSharedPtr<FJsonObject>>& OutDAGCalls
	);

	/** 解析 $tN.field.path 引用 — 从 JSON 结果中按字段路径提取值 */
	static TSharedPtr<FJsonValue> ResolveDAGFieldPath(
		const FString& ResultJsonStr,
		const FString& FieldPath
	);

	/** 将解析后的值按原始类型设置到参数对象中 */
	static void SetResolvedParam(
		TSharedPtr<FJsonObject>& Target,
		const FString& Key,
		const TSharedPtr<FJsonValue>& Value
	);
};
