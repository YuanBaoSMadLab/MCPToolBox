// ============================================================================
// MCPToolboxPythonExecutionService.cpp — 安全 Python 执行服务实现
// 移植自 VibeUE Tools/PythonExecutionService.cpp
// ============================================================================
#include "MCPToolboxPythonExecutionService.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformMisc.h"
#include "Internationalization/Regex.h"

DEFINE_LOG_CATEGORY(LogMCPToolboxPython);

bool FMCPToolboxPythonExecutionService::bPythonValidated = false;

// ═══════════════════════════════════════════════════════════════
// Windows SEH 崩溃保护
// ═══════════════════════════════════════════════════════════════
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <excpt.h>
#include "Windows/HideWindowsPlatformTypes.h"

// UE5 assert 异常代码(check() 失败时通过 RaiseException 抛出)
static constexpr DWORD UE_ASSERT_EXCEPTION_CODE = 0x4000;

// 跟踪 Python 解释器是否在本编辑器会话中崩溃
// 一旦崩溃,CPython 运行时状态不可恢复,需告知调用者重启编辑器
static bool GbPythonInterpreterCrashed = false;

// 镜像 UE5 的 FAssertInfo 结构布局
struct FAssertInfo
{
	const TCHAR* ErrorMessage;
	void* ProgramCounter;
};

// SEH 执行结果(必须是 POD 类型,__try 块内不能有 C++ 析构)
struct FSEHExecutionResult
{
	bool bSuccess = false;
	bool bCrashed = false;
	DWORD ExceptionCode = 0;
	TCHAR AssertMessage[512];
	bool bHasAssertMessage = false;
};

// SEH 异常过滤器,在异常处理前提取 assert 信息
static LONG WINAPI PythonSEHFilter(LPEXCEPTION_POINTERS ExInfo, FSEHExecutionResult* OutResult)
{
	OutResult->bCrashed = true;
	OutResult->ExceptionCode = ExInfo->ExceptionRecord->ExceptionCode;

	if (ExInfo->ExceptionRecord->ExceptionCode == UE_ASSERT_EXCEPTION_CODE &&
		ExInfo->ExceptionRecord->NumberParameters >= 1 &&
		ExInfo->ExceptionRecord->ExceptionInformation[0] != 0)
	{
		const FAssertInfo* Info = (const FAssertInfo*)ExInfo->ExceptionRecord->ExceptionInformation[0];
		if (Info->ErrorMessage)
		{
			int32 i = 0;
			for (; i < 511 && Info->ErrorMessage[i] != 0; i++)
			{
				OutResult->AssertMessage[i] = Info->ErrorMessage[i];
			}
			OutResult->AssertMessage[i] = 0;
			OutResult->bHasAssertMessage = true;
		}
	}

	return EXCEPTION_EXECUTE_HANDLER;
}

// 独立的 SEH 包装函数(__try 块内不能有需要展开的 C++ 对象)
static FSEHExecutionResult ExecutePythonWithSEH(IPythonScriptPlugin* PythonPlugin, FPythonCommandEx* Command)
{
	FSEHExecutionResult Result;
	__try
	{
		Result.bSuccess = PythonPlugin->ExecPythonCommandEx(*Command);
	}
	__except(PythonSEHFilter(GetExceptionInformation(), &Result))
	{
		// 结果已在 PythonSEHFilter 中填充
	}
	return Result;
}
#endif

// ═══════════════════════════════════════════════════════════════
// 危险模式检测
// ═══════════════════════════════════════════════════════════════

/** 可能导致编辑器崩溃的危险模式 */
static bool ContainsDangerousPattern(const FString& Code, FString& OutPattern, FString& OutReason)
{
	// EdGraphPinType 构造会崩溃 — 使用 BlueprintEditorLibrary.get_basic_type_by_name() 替代
	if (Code.Contains(TEXT("EdGraphPinType(")) && Code.Contains(TEXT("pin_category")))
	{
		OutPattern = TEXT("EdGraphPinType(pin_category=...)");
		OutReason = TEXT("EdGraphPinType cannot be constructed with arguments from Python. Use unreal.BlueprintEditorLibrary.get_basic_type_by_name('float') instead.");
		return true;
	}

	// CDO 修改会导致崩溃 — 仅在修改时阻止,不阻止读取
	{
		static FRegexPattern CdoModifyPattern(TEXT("get_default_object\\b[^\\n]*\\.\\s*set_editor_property"));
		FRegexMatcher CdoModifyMatcher(CdoModifyPattern, Code);
		if (CdoModifyMatcher.FindNext())
		{
			OutPattern = TEXT("get_default_object() modification");
			OutReason = TEXT("Modifying Class Default Objects (CDOs) from Python causes crashes. Modify instances instead.");
			return true;
		}
	}

	// input() 会无限阻塞编辑器
	static FRegexPattern InputPattern(TEXT("(?:^|[^_a-zA-Z0-9])input\\s*\\("));
	FRegexMatcher InputMatcher(InputPattern, Code);
	if (InputMatcher.FindNext() && !Code.Contains(TEXT("#")) && !Code.Contains(TEXT("Enhanced")))
	{
		OutPattern = TEXT("input()");
		OutReason = TEXT("input() blocks the editor. Use a different approach for user interaction.");
		return true;
	}

	// 模态对话框会冻结编辑器
	if (Code.Contains(TEXT("EditorDialog")) || Code.Contains(TEXT("show_modal")))
	{
		OutPattern = TEXT("Modal dialogs");
		OutReason = TEXT("Modal dialogs freeze the editor from Python. Use non-blocking alternatives.");
		return true;
	}

	// 无限循环检测
	if (Code.Contains(TEXT("while True:")) && !Code.Contains(TEXT("break")))
	{
		OutPattern = TEXT("while True without break");
		OutReason = TEXT("Infinite loops freeze the editor. Ensure your loop has a break condition.");
		return true;
	}

	return false;
}

// ═══════════════════════════════════════════════════════════════
// 核心 API 实现
// ═══════════════════════════════════════════════════════════════

TMCPToolboxResult<bool> FMCPToolboxPythonExecutionService::IsPythonAvailable()
{
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();

	if (!PythonPlugin)
	{
		return TMCPToolboxResult<bool>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_NOT_AVAILABLE,
			TEXT("PythonScriptPlugin is not loaded. Enable it in Project Settings -> Plugins -> Scripting -> Python.")
		);
	}

	if (!PythonPlugin->IsPythonAvailable())
	{
		return TMCPToolboxResult<bool>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_NOT_AVAILABLE,
			TEXT("Python is not initialized. Check that Python is enabled in project settings.")
		);
	}

	bPythonValidated = true;
	return TMCPToolboxResult<bool>::Success(true);
}

TMCPToolboxResult<FString> FMCPToolboxPythonExecutionService::GetPythonInfo()
{
	auto AvailableResult = IsPythonAvailable();
	if (AvailableResult.IsError())
	{
		return TMCPToolboxResult<FString>::Error(
			AvailableResult.GetErrorCode(),
			AvailableResult.GetErrorMessage()
		);
	}

	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	FString InterpreterPath = PythonPlugin->GetInterpreterExecutablePath();

	FPythonCommandEx Command;
	Command.Command = TEXT("import sys; print(sys.version)");
	Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	Command.FileExecutionScope = EPythonFileExecutionScope::Private;

	bool bSuccess = PythonPlugin->ExecPythonCommandEx(Command);

	if (bSuccess && Command.LogOutput.Num() > 0)
	{
		FString Version = Command.LogOutput[0].Output.TrimStartAndEnd();
		FString Info = FString::Printf(
			TEXT("Python Version: %s\nInterpreter: %s"),
			*Version,
			*InterpreterPath
		);
		return TMCPToolboxResult<FString>::Success(Info);
	}

	return TMCPToolboxResult<FString>::Success(
		FString::Printf(TEXT("Interpreter: %s"), *InterpreterPath)
	);
}

TMCPToolboxResult<FMCPToolboxPythonExecutionResult> FMCPToolboxPythonExecutionService::ExecuteCode(
	const FString& Code,
	EPythonFileExecutionScope ExecutionScope,
	int32 TimeoutMs)
{
	// 验证 Python 可用
	auto AvailableResult = IsPythonAvailable();
	if (AvailableResult.IsError())
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			AvailableResult.GetErrorCode(),
			AvailableResult.GetErrorMessage()
		);
	}

	// 验证代码非空
	if (Code.IsEmpty())
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			MCPToolboxPythonErrorCodes::PARAM_EMPTY,
			TEXT("Python code cannot be empty")
		);
	}

	// 阻止已知危险模式
	FString BlockedPattern;
	FString BlockedReason;
	if (ContainsDangerousPattern(Code, BlockedPattern, BlockedReason))
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_UNSAFE_CODE,
			FString::Printf(TEXT("Blocked unsafe Python code: %s. %s"), *BlockedPattern, *BlockedReason)
		);
	}

	// 设置命令
	FPythonCommandEx Command;
	Command.Command = Code;
	Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	Command.FileExecutionScope = ExecutionScope;
	Command.Flags = EPythonCommandFlags::None;

	double StartTime = FPlatformTime::Seconds();
	bool bSuccess = false;
	bool bCrashed = false;
	FString CrashMessage;

	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin)
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_NOT_AVAILABLE,
			TEXT("Python plugin is not initialized")
		);
	}

#if PLATFORM_WINDOWS
	FSEHExecutionResult SEHResult = ExecutePythonWithSEH(PythonPlugin, &Command);
	bSuccess = SEHResult.bSuccess;
	if (SEHResult.bCrashed)
	{
		bCrashed = true;
		if (SEHResult.ExceptionCode == UE_ASSERT_EXCEPTION_CODE && SEHResult.bHasAssertMessage)
		{
			CrashMessage = FString::Printf(TEXT("Python execution caused a UE assertion failure: %s"), SEHResult.AssertMessage);
		}
		else
		{
			CrashMessage = FString::Printf(TEXT("Python execution caused a crash (exception code: 0x%08X). The Python code may have accessed invalid memory."), SEHResult.ExceptionCode);
		}

		if (GbPythonInterpreterCrashed)
		{
			CrashMessage += TEXT(" NOTE: the Python interpreter has now crashed more than once this session and is unrecoverable in-process — restart the editor (BuildAndLaunch) to restore Python execution.");
		}
		else
		{
			CrashMessage += TEXT(" NOTE: the interpreter may now be unstable; if further commands keep failing identically, restart the editor (BuildAndLaunch).");
		}
		GbPythonInterpreterCrashed = true;

		UE_LOG(LogMCPToolboxPython, Error, TEXT("%s"), *CrashMessage);
	}
	else
	{
		GbPythonInterpreterCrashed = false;
	}
#else
	try
	{
		bSuccess = PythonPlugin->ExecPythonCommandEx(Command);
	}
	catch (...)
	{
		bCrashed = true;
		CrashMessage = TEXT("Python execution threw an unhandled exception");
	}
#endif

	if (bCrashed)
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_RUNTIME_ERROR,
			CrashMessage
		);
	}

	double ExecutionTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (TimeoutMs > 0 && ExecutionTimeMs > TimeoutMs)
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_EXECUTION_TIMEOUT,
			FString::Printf(TEXT("Python execution exceeded %dms timeout (took %.2fms)"),
				TimeoutMs, ExecutionTimeMs)
		);
	}

	FMCPToolboxPythonExecutionResult Result = ConvertExecutionResult(Command, static_cast<float>(ExecutionTimeMs));

	if (!bSuccess || !Result.bSuccess)
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_RUNTIME_ERROR,
			Result.ErrorMessage.IsEmpty() ? TEXT("Python execution failed") : Result.ErrorMessage
		);
	}

	return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Success(Result);
}

TMCPToolboxResult<FMCPToolboxPythonExecutionResult> FMCPToolboxPythonExecutionService::EvaluateExpression(const FString& Expression)
{
	auto AvailableResult = IsPythonAvailable();
	if (AvailableResult.IsError())
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			AvailableResult.GetErrorCode(),
			AvailableResult.GetErrorMessage()
		);
	}

	if (Expression.IsEmpty())
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_INVALID_EXPRESSION,
			TEXT("Python expression cannot be empty")
		);
	}

	FPythonCommandEx Command;
	Command.Command = Expression;
	Command.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
	Command.FileExecutionScope = EPythonFileExecutionScope::Private;
	Command.Flags = EPythonCommandFlags::None;

	double StartTime = FPlatformTime::Seconds();
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin)
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_NOT_AVAILABLE,
			TEXT("Python plugin is not initialized")
		);
	}
	bool bSuccess = PythonPlugin->ExecPythonCommandEx(Command);
	double ExecutionTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	FMCPToolboxPythonExecutionResult Result = ConvertExecutionResult(Command, static_cast<float>(ExecutionTimeMs));

	if (!bSuccess || !Result.bSuccess)
	{
		return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_RUNTIME_ERROR,
			Result.ErrorMessage.IsEmpty() ? TEXT("Python expression evaluation failed") : Result.ErrorMessage
		);
	}

	return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Success(Result);
}

TMCPToolboxResult<FMCPToolboxPythonExecutionResult> FMCPToolboxPythonExecutionService::ExecuteCodeSafe(
	const FString& Code,
	bool bValidateBeforeExecution)
{
	if (bValidateBeforeExecution)
	{
		auto ValidationResult = ValidateCode(Code);
		if (ValidationResult.IsError())
		{
			return TMCPToolboxResult<FMCPToolboxPythonExecutionResult>::Error(
				ValidationResult.GetErrorCode(),
				ValidationResult.GetErrorMessage()
			);
		}
	}

	return ExecuteCode(Code);
}

// ═══════════════════════════════════════════════════════════════
// 内部实现
// ═══════════════════════════════════════════════════════════════

FMCPToolboxPythonExecutionResult FMCPToolboxPythonExecutionService::ConvertExecutionResult(
	const FPythonCommandEx& CommandEx,
	float ExecutionTimeMs)
{
	FMCPToolboxPythonExecutionResult Result;
	Result.ExecutionTimeMs = ExecutionTimeMs;

	bool bHasError = false;
	for (const FPythonLogOutputEntry& LogEntry : CommandEx.LogOutput)
	{
		FString LogOutput = LogEntry.Output.TrimStartAndEnd();
		if (LogOutput.IsEmpty())
		{
			continue;
		}

		Result.LogMessages.Add(LogOutput);

		if (LogEntry.Type == EPythonLogOutputType::Info)
		{
			if (!Result.Output.IsEmpty()) Result.Output += TEXT("\n");
			Result.Output += LogOutput;
		}
		else if (LogEntry.Type == EPythonLogOutputType::Warning)
		{
			// 警告(如 DeprecationWarning)不应使执行失败
			if (!Result.Output.IsEmpty()) Result.Output += TEXT("\n");
			Result.Output += FString::Printf(TEXT("[warning] %s"), *LogOutput);
		}
		else if (LogEntry.Type == EPythonLogOutputType::Error)
		{
			bHasError = true;
			if (!Result.ErrorMessage.IsEmpty()) Result.ErrorMessage += TEXT("\n");
			Result.ErrorMessage += LogOutput;
		}
	}

	// 检查命令结果中的错误或返回值
	if (!CommandEx.CommandResult.IsEmpty())
	{
		if (CommandEx.CommandResult.Contains(TEXT("Error")) ||
		    CommandEx.CommandResult.Contains(TEXT("Traceback")))
		{
			bHasError = true;
			Result.ErrorMessage = ParsePythonException(CommandEx.CommandResult);
		}
		else
		{
			Result.Result = CommandEx.CommandResult;
		}
	}

	Result.bSuccess = !bHasError;
	return Result;
}

TMCPToolboxResult<void> FMCPToolboxPythonExecutionService::ValidateCode(const FString& Code)
{
	TArray<FString> DangerousPatterns = {
		TEXT("import subprocess"),
		TEXT("import os"),
		TEXT("os.system"),
		TEXT("open("),
		TEXT("__import__"),
		TEXT("eval("),
		TEXT("exec(")
	};

	for (const FString& Pattern : DangerousPatterns)
	{
		if (Code.Contains(Pattern))
		{
			UE_LOG(LogMCPToolboxPython, Warning, TEXT("Potentially dangerous pattern detected: %s"), *Pattern);
		}
	}

	return TMCPToolboxResult<void>::Success();
}

FString FMCPToolboxPythonExecutionService::ParsePythonException(const FString& Traceback)
{
	TArray<FString> Lines;
	Traceback.ParseIntoArrayLines(Lines);

	int32 FirstLine = 0;
	while (FirstLine < Lines.Num() && Lines[FirstLine].TrimStartAndEnd().IsEmpty()) { ++FirstLine; }
	int32 LastLine = Lines.Num() - 1;
	while (LastLine >= FirstLine && Lines[LastLine].TrimStartAndEnd().IsEmpty()) { --LastLine; }

	if (FirstLine > LastLine)
	{
		return Traceback;
	}

	constexpr int32 MaxLines = 40;
	FString ParsedError;
	if (LastLine - FirstLine + 1 > MaxLines)
	{
		ParsedError = TEXT("[traceback truncated]");
		FirstLine = LastLine - MaxLines + 1;
	}

	for (int32 i = FirstLine; i <= LastLine; ++i)
	{
		if (!ParsedError.IsEmpty()) ParsedError += TEXT("\n");
		ParsedError += Lines[i].TrimEnd();
	}

	return ParsedError;
}
