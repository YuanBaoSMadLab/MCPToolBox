// ============================================================================
// MCPToolboxPythonTypes — Python 相关数据结构 (移植自 VibeUE Tools/PythonTypes.h)
// ============================================================================
// 定义 Python 执行、内省、搜索操作中使用的数据结构。
// 独立于任何 VibeUE 依赖,可在 MCPToolbox 中直接使用。
// ============================================================================
#pragma once

#include "CoreMinimal.h"

/**
 * Python 函数/方法信息
 */
struct MCPTOOLBOX_API FMCPToolboxPythonFunctionInfo
{
	/** 函数名 */
	FString Name;

	/** 完整签名(如 "load_asset(path: str) -> Object") */
	FString Signature;

	/** 函数 docstring */
	FString Docstring;

	/** 参数名列表 */
	TArray<FString> Parameters;

	/** 参数类型提示 */
	TArray<FString> ParamTypes;

	/** 返回类型提示 */
	FString ReturnType;

	/** 是否为方法(vs 独立函数) */
	bool bIsMethod = false;

	/** 是否为静态方法 */
	bool bIsStatic = false;

	/** 是否为类方法 */
	bool bIsClassMethod = false;
};

/**
 * Python 类信息
 */
struct MCPTOOLBOX_API FMCPToolboxPythonClassInfo
{
	/** 类名 */
	FString Name;

	/** 完整路径(如 "unreal.EditorActorSubsystem") */
	FString FullPath;

	/** 类 docstring */
	FString Docstring;

	/** 基类名列表 */
	TArray<FString> BaseClasses;

	/** 类方法 */
	TArray<FMCPToolboxPythonFunctionInfo> Methods;

	/** 属性名列表 */
	TArray<FString> Properties;

	/** 是否为抽象类 */
	bool bIsAbstract = false;
};

/**
 * 模块发现结果
 */
struct MCPTOOLBOX_API FMCPToolboxPythonModuleInfo
{
	/** 模块名(如 "unreal") */
	FString ModuleName;

	/** 模块中的类名列表 */
	TArray<FString> Classes;

	/** 模块中的函数名列表 */
	TArray<FString> Functions;

	/** 模块中的常量名列表 */
	TArray<FString> Constants;

	/** 发现的成员总数 */
	int32 TotalMembers = 0;
};

/**
 * Python 代码执行结果
 */
struct MCPTOOLBOX_API FMCPToolboxPythonExecutionResult
{
	/** 执行是否成功 */
	bool bSuccess = false;

	/** stdout(print 输出) */
	FString Output;

	/** 返回值(EvaluateStatement 模式) */
	FString Result;

	/** 异常 traceback(错误时) */
	FString ErrorMessage;

	/** 捕获的日志输出 */
	TArray<FString> LogMessages;

	/** 执行时间(毫秒) */
	float ExecutionTimeMs = 0.0f;
};

/**
 * 源码搜索结果
 */
struct MCPTOOLBOX_API FMCPToolboxSourceSearchResult
{
	/** 相对路径(从插件源码根目录) */
	FString FilePath;

	/** 匹配行号 */
	int32 LineNumber = 0;

	/** 匹配行内容 */
	FString LineContent;

	/** 匹配行之前的上下文 */
	TArray<FString> ContextBefore;

	/** 匹配行之后的上下文 */
	TArray<FString> ContextAfter;
};
