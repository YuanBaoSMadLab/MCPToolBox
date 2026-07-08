// ============================================================================
// MCPToolboxPythonDiscoveryService — Python API 发现/内省服务
// ============================================================================
// 移植自 VibeUE Tools/PythonDiscoveryService (.h + .cpp)
//
// 核心功能:
//   1. 发现 unreal 模块成员(类/函数/常量)
//   2. 内省 Python 类(方法签名/属性/基类)
//   3. 内省 Python 函数(签名/参数类型/返回类型)
//   4. 搜索 API(类名/函数名模糊搜索)
//   5. 源码文件读取和搜索
//
// 依赖: MCPToolboxPythonExecutionService (通过 IPythonScriptPlugin 执行内省脚本)
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "MCPToolboxResult.h"
#include "MCPToolboxPythonTypes.h"
#include "MCPToolboxPythonIntrospectionBuilder.h"

/**
 * Python API 发现服务。
 *
 * 使用模式:
 *   auto Mod = FMCPToolboxPythonDiscoveryService::DiscoverUnrealModule(1, TEXT("Editor"));
 *   auto Cls = FMCPToolboxPythonDiscoveryService::DiscoverClass(TEXT("EditorActorSubsystem"));
 *   auto Func = FMCPToolboxPythonDiscoveryService::DiscoverFunction(TEXT("EditorActorSubsystem.GetSelectionSet"));
 */
class MCPTOOLBOX_API FMCPToolboxPythonDiscoveryService
{
public:
	// ═══════════════════════════════════════════════════════════════
	// 模块/类/函数内省
	// ═══════════════════════════════════════════════════════════════

	/** 发现 unreal 模块成员(可过滤类型) */
	static TMCPToolboxResult<FMCPToolboxPythonModuleInfo> DiscoverUnrealModule(
		int32 MaxDepth = 1,
		const FString& Filter = TEXT(""),
		int32 MaxItems = 200,
		bool IncludeClasses = true,
		bool IncludeFunctions = true,
		bool CaseSensitive = false
	);

	/** 发现类的详细信息(方法/属性/基类) */
	static TMCPToolboxResult<FMCPToolboxPythonClassInfo> DiscoverClass(
		const FString& ClassName,
		const FString& MethodFilter = TEXT(""),
		int32 MaxMethods = 100,
		bool IncludeInherited = false,
		bool IncludePrivate = false
	);

	/** 发现函数/方法的详细信息(签名/参数/返回类型) */
	static TMCPToolboxResult<FMCPToolboxPythonFunctionInfo> DiscoverFunction(
		const FString& FunctionPath
	);

	// ═══════════════════════════════════════════════════════════════
	// API 搜索
	// ═══════════════════════════════════════════════════════════════

	/** 按名称模式搜索 API 成员 */
	static TMCPToolboxResult<TArray<FString>> SearchAPI(
		const FString& SearchPattern,
		const FString& SearchType = TEXT("all")  // "all", "class", "function"
	);

	/** 列出所有编辑器子系统 */
	static TMCPToolboxResult<TArray<FString>> ListEditorSubsystems();

	// ═══════════════════════════════════════════════════════════════
	// 源码操作
	// ═══════════════════════════════════════════════════════════════

	/** 读取 PythonScriptPlugin 源码文件 */
	static TMCPToolboxResult<FString> ReadSourceFile(
		const FString& RelativePath,
		int32 StartLine = 0,
		int32 MaxLines = 200
	);

	/** 搜索 PythonScriptPlugin 源码中的模式 */
	static TMCPToolboxResult<TArray<FMCPToolboxSourceSearchResult>> SearchSourceFiles(
		const FString& Pattern,
		const FString& FilePattern = TEXT("*.py"),
		int32 ContextLines = 2
	);

	/** 列出 PythonScriptPlugin 源码目录中的文件 */
	static TMCPToolboxResult<TArray<FString>> ListSourceFiles(
		const FString& SubDirectory = TEXT(""),
		const FString& FilePattern = TEXT("*.py")
	);

private:
	// ═══════════════════════════════════════════════════════════════
	// 内部方法
	// ═══════════════════════════════════════════════════════════════

	/** 执行内省 Python 脚本并返回 stdout 输出 */
	static TMCPToolboxResult<FString> ExecuteIntrospectionScript(const FString& PythonCode);

	/** 解析模块内省 JSON 结果 */
	static bool ParseModuleInfo(const FString& JsonResult, FMCPToolboxPythonModuleInfo& OutInfo);

	/** 解析类内省 JSON 结果 */
	static bool ParseClassInfo(const FString& JsonResult, FMCPToolboxPythonClassInfo& OutInfo);

	/** 解析函数内省 JSON 结果 */
	static bool ParseFunctionInfo(const FString& JsonResult, FMCPToolboxPythonFunctionInfo& OutInfo);

	/** 获取 PythonScriptPlugin 源码根目录 */
	static FString GetPluginSourceRoot();

	/** 验证源码路径安全性(防止目录遍历) */
	static bool IsValidSourcePath(const FString& Path);

	/** 将相对路径解析为完整路径 */
	static FString GetFullSourcePath(const FString& RelativePath);

	// ═══════════════════════════════════════════════════════════════
	// 缓存
	// ═══════════════════════════════════════════════════════════════

	static TMap<FString, FMCPToolboxPythonModuleInfo> ModuleCache;
	static TMap<FString, FMCPToolboxPythonClassInfo> ClassCache;
};
