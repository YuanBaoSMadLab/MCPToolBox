// ============================================================================
// MCPToolboxPythonExecutionService — 安全 Python 执行服务
// ============================================================================
// 移植自 VibeUE Tools/PythonExecutionService (.h + .cpp)
//
// 核心功能:
//   1. 安全的 Python 代码执行(multi-line script / expression evaluate)
//   2. Windows SEH 崩溃保护(捕获 access violation + UE assert)
//   3. 危险模式检测(EdGraphPinType 构造、CDO 修改、input() 等)
//   4. 超时检测 + 详细错误消息解析
//   5. 30 秒默认超时,可配置
//
// 依赖: IPythonScriptPlugin (需 Build.cs 添加 "PythonScriptPlugin")
// 不依赖 VibeUE 的 ServiceBase/ServiceContext — 使用 MCPToolbox 的静态类模式
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "MCPToolboxResult.h"
#include "MCPToolboxPythonTypes.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"

/** Python 执行服务日志分类 */
DECLARE_LOG_CATEGORY_EXTERN(LogMCPToolboxPython, Log, All);

// ═══════════════════════════════════════════════════════════════
// MCPToolbox Python 错误码(9100-9199,与 VibeUE 对齐)
// ═══════════════════════════════════════════════════════════════
namespace MCPToolboxPythonErrorCodes
{
	constexpr const TCHAR* PYTHON_NOT_AVAILABLE       = TEXT("PYTHON_NOT_AVAILABLE");
	constexpr const TCHAR* PYTHON_RUNTIME_ERROR        = TEXT("PYTHON_RUNTIME_ERROR");
	constexpr const TCHAR* PYTHON_UNSAFE_CODE          = TEXT("PYTHON_UNSAFE_CODE");
	constexpr const TCHAR* PYTHON_EXECUTION_TIMEOUT    = TEXT("PYTHON_EXECUTION_TIMEOUT");
	constexpr const TCHAR* PYTHON_INVALID_EXPRESSION   = TEXT("PYTHON_INVALID_EXPRESSION");
	constexpr const TCHAR* PYTHON_MODULE_NOT_FOUND     = TEXT("PYTHON_MODULE_NOT_FOUND");
	constexpr const TCHAR* PYTHON_CLASS_NOT_FOUND      = TEXT("PYTHON_CLASS_NOT_FOUND");
	constexpr const TCHAR* PYTHON_FUNCTION_NOT_FOUND   = TEXT("PYTHON_FUNCTION_NOT_FOUND");
	constexpr const TCHAR* PYTHON_INTROSPECTION_FAILED = TEXT("PYTHON_INTROSPECTION_FAILED");
	constexpr const TCHAR* PARAM_EMPTY                 = TEXT("PARAM_EMPTY");
	constexpr const TCHAR* PARAM_INVALID               = TEXT("PARAM_INVALID");
	constexpr const TCHAR* OPERATION_FAILED            = TEXT("OPERATION_FAILED");
}

/**
 * Python 代码执行安全服务。
 *
 * 使用模式:
 *   auto Result = FMCPToolboxPythonExecutionService::ExecuteCode(TEXT("print(1+1)"));
 *   if (Result.IsError()) { ... }
 *   auto& Data = Result.GetValue(); // FMCPToolboxPythonExecutionResult
 */
class MCPTOOLBOX_API FMCPToolboxPythonExecutionService
{
public:
	// ═══════════════════════════════════════════════════════════════
	// 核心 API
	// ═══════════════════════════════════════════════════════════════

	/** 执行多行 Python 代码 */
	static TMCPToolboxResult<FMCPToolboxPythonExecutionResult> ExecuteCode(
		const FString& Code,
		EPythonFileExecutionScope ExecutionScope = EPythonFileExecutionScope::Private,
		int32 TimeoutMs = 30000
	);

	/** 求值单个 Python 表达式并返回结果 */
	static TMCPToolboxResult<FMCPToolboxPythonExecutionResult> EvaluateExpression(
		const FString& Expression
	);

	/** 带验证的安全执行(检查危险模式) */
	static TMCPToolboxResult<FMCPToolboxPythonExecutionResult> ExecuteCodeSafe(
		const FString& Code,
		bool bValidateBeforeExecution = true
	);

	// ═══════════════════════════════════════════════════════════════
	// 状态检查
	// ═══════════════════════════════════════════════════════════════

	/** 检查 Python 是否可用并已初始化 */
	static TMCPToolboxResult<bool> IsPythonAvailable();

	/** 获取 Python 解释器版本和路径 */
	static TMCPToolboxResult<FString> GetPythonInfo();

private:
	// ═══════════════════════════════════════════════════════════════
	// 内部方法
	// ═══════════════════════════════════════════════════════════════

	/** 将 FPythonCommandEx 结果转换为我们的结果结构 */
	static FMCPToolboxPythonExecutionResult ConvertExecutionResult(
		const FPythonCommandEx& CommandEx,
		float ExecutionTimeMs
	);

	/** 验证代码中是否包含潜在危险操作 */
	static TMCPToolboxResult<void> ValidateCode(const FString& Code);

	/** 解析 Python 异常 traceback 为格式化错误消息 */
	static FString ParsePythonException(const FString& Traceback);

	/** Python 可用性已通过验证 */
	static bool bPythonValidated;
};
