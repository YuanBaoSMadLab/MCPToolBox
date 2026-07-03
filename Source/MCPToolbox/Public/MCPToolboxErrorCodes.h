#pragma once

#include "CoreMinimal.h"

// ============================================================================
// MCPToolboxErrorCodes — 命名空间分段错误码 (Stage 4)
// ============================================================================
// 设计参考: VibeUE error_code 枚举(命名空间分段 + LLM 可程序化分支)
// 用途: ExecuteToolCall 返回结构化 JSON,含 error_code + error_message + valid_parameters,
//       让 LLM 在遇到错误时能程序化自纠正(而非盲目重试)。
//
// 错误码格式: <NAMESPACE>_<SPECIFIC>
//   PARAM_*      — 参数错误(类型/缺失/无效值)
//   TOOL_*       — 工具执行错误(不存在/未注册/执行失败)
//   MCP_*        — MCP 服务器错误(连接/超时/协议)
//   DAG_*        — DAG 编排错误(依赖循环/引用解析)
//   PERMISSION_* — 权限错误
//
// LLM 自纠正流程:
//   1. 收到 {"error_code":"PARAM_INVALID_TYPE", "valid_parameters":{...}}
//   2. 根据 valid_parameters 修正参数类型
//   3. 重新调用 call_tool
// ============================================================================

namespace EMCPToolboxErrorCode
{
	// ── 参数错误 (PARAM_*) ──
	static const FString ParamMissing         = TEXT("PARAM_MISSING");          // 必填参数缺失
	static const FString ParamInvalidType     = TEXT("PARAM_INVALID_TYPE");    // 参数类型错误(如 string 传给 number)
	static const FString ParamInvalidValue    = TEXT("PARAM_INVALID_VALUE");   // 参数值无效(如越界)
	static const FString ParamInvalidPath     = TEXT("PARAM_INVALID_PATH");    // 路径格式错误(如非 /Game/ 前缀)
	static const FString ParamUnknown         = TEXT("PARAM_UNKNOWN");         // 未知参数(不在 schema 中)

	// ── 工具错误 (TOOL_*) ──
	static const FString ToolNotFound         = TEXT("TOOL_NOT_FOUND");        // 工具不存在
	static const FString ToolNotRegistered    = TEXT("TOOL_NOT_REGISTERED");   // 工具未注册到 FunctionTable
	static const FString ToolExecutionFailed  = TEXT("TOOL_EXECUTION_FAILED"); // 工具执行异常
	static const FString ToolTimeout          = TEXT("TOOL_TIMEOUT");          // 工具执行超时

	// ── MCP 服务器错误 (MCP_*) ──
	static const FString McpNotConnected      = TEXT("MCP_NOT_CONNECTED");     // MCP 服务器未连接
	static const FString McpTimeout           = TEXT("MCP_TIMEOUT");           // MCP 请求超时
	static const FString McpProtocolError     = TEXT("MCP_PROTOCOL_ERROR");    // MCP 协议错误(如 invalid method)
	static const FString McpSessionExpired    = TEXT("MCP_SESSION_EXPIRED");   // MCP 会话过期

	// ── DAG 编排错误 (DAG_*) ──
	static const FString DagCycleDetected     = TEXT("DAG_CYCLE_DETECTED");    // 检测到依赖循环
	static const FString DagRefUnresolved     = TEXT("DAG_REF_UNRESOLVED");    // $tN.field 引用无法解析
	static const FString DagTaskFailed        = TEXT("DAG_TASK_FAILED");       // DAG 中某个任务失败

	// ── 权限错误 (PERMISSION_*) ──
	static const FString PermissionDenied     = TEXT("PERMISSION_DENIED");     // 权限不足

	// ── 通用错误 ──
	static const FString Unknown              = TEXT("UNKNOWN");               // 未知错误
	static const FString InternalError        = TEXT("INTERNAL_ERROR");        // 内部错误
}

// ============================================================================
// 辅助函数:构建结构化错误 JSON
// ============================================================================
namespace MCPToolboxErrorFormat
{
	/**
	 * 构建参数错误 JSON(含 valid_parameters 帮助 LLM 自纠正)。
	 * @param ErrorCode  错误码(如 EMCPToolboxErrorCode::ParamInvalidType)
	 * @param ParamName  出错的参数名
	 * @param ExpectedType 期望的类型(如 "string", "number", "array", "object")
	 * @param ActualValue   实际收到的值(用于调试)
	 * @param ValidParameters  合法参数示例(JSON 字符串,可选)
	 * @return 结构化错误 JSON 字符串
	 */
	static FString FormatParamError(
		const FString& ErrorCode,
		const FString& ParamName,
		const FString& ExpectedType,
		const FString& ActualValue = TEXT(""),
		const FString& ValidParameters = TEXT(""))
	{
		FString Json = TEXT("{\"error_code\":\"") + ErrorCode + TEXT("\"");
		Json += TEXT(",\"error\":\"") + ErrorCode + TEXT("\"");
		Json += TEXT(",\"param_name\":\"") + ParamName + TEXT("\"");
		Json += TEXT(",\"expected_type\":\"") + ExpectedType + TEXT("\"");
		if (!ActualValue.IsEmpty())
		{
			Json += TEXT(",\"actual_value\":\"") + ActualValue + TEXT("\"");
		}
		if (!ValidParameters.IsEmpty())
		{
			Json += TEXT(",\"valid_parameters\":") + ValidParameters;
		}
		Json += TEXT(",\"hint\":\"Fix the parameter type and retry call_tool\"}");
		return Json;
	}

	/**
	 * 构建工具错误 JSON。
	 * @param ErrorCode  错误码
	 * @param ToolName   工具名
	 * @param Message    错误详情
	 * @param ValidToolsets  合法 toolset 列表(JSON 数组字符串,可选)
	 */
	static FString FormatToolError(
		const FString& ErrorCode,
		const FString& ToolName,
		const FString& Message,
		const FString& ValidToolsets = TEXT(""))
	{
		FString Json = TEXT("{\"error_code\":\"") + ErrorCode + TEXT("\"");
		Json += TEXT(",\"error\":\"") + ErrorCode + TEXT("\"");
		Json += TEXT(",\"tool_name\":\"") + ToolName + TEXT("\"");
		Json += TEXT(",\"message\":\"") + Message + TEXT("\"");
		if (!ValidToolsets.IsEmpty())
		{
			Json += TEXT(",\"valid_toolsets\":") + ValidToolsets;
		}
		Json += TEXT("}");
		return Json;
	}

	/**
	 * 构建通用错误 JSON。
	 */
	static FString FormatGenericError(
		const FString& ErrorCode,
		const FString& Message)
	{
		return TEXT("{\"error_code\":\"") + ErrorCode + TEXT("\",\"error\":\"") + ErrorCode + TEXT("\",\"message\":\"") + Message + TEXT("\"}");
	}
}
