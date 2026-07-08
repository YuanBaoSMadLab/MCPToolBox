// ============================================================================
// MCPToolboxPythonDiscoveryService.cpp — Python API 发现服务实现
// 移植自 VibeUE Tools/PythonDiscoveryService.cpp
// ============================================================================
#include "MCPToolboxPythonDiscoveryService.h"
#include "MCPToolboxPythonExecutionService.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"

// ═══════════════════════════════════════════════════════════════
// 静态缓存
// ═══════════════════════════════════════════════════════════════

TMap<FString, FMCPToolboxPythonModuleInfo>  FMCPToolboxPythonDiscoveryService::ModuleCache;
TMap<FString, FMCPToolboxPythonClassInfo>   FMCPToolboxPythonDiscoveryService::ClassCache;

// ═══════════════════════════════════════════════════════════════
// 模块内省
// ═══════════════════════════════════════════════════════════════

TMCPToolboxResult<FMCPToolboxPythonModuleInfo> FMCPToolboxPythonDiscoveryService::DiscoverUnrealModule(
	int32 MaxDepth,
	const FString& Filter,
	int32 MaxItems,
	bool IncludeClasses,
	bool IncludeFunctions,
	bool CaseSensitive)
{
	FString CacheKey = FString::Printf(TEXT("unreal_%d_%s_%d_%d_%d_%d"),
		MaxDepth, *Filter, MaxItems, IncludeClasses ? 1 : 0, IncludeFunctions ? 1 : 0, CaseSensitive ? 1 : 0);
	if (ModuleCache.Contains(CacheKey))
	{
		return TMCPToolboxResult<FMCPToolboxPythonModuleInfo>::Success(ModuleCache[CacheKey]);
	}

	// 构建过滤条件
	FString FilterCondition;
	if (Filter.IsEmpty())
	{
		FilterCondition = TEXT("True");
	}
	else if (CaseSensitive)
	{
		FilterCondition = FString::Printf(TEXT("'%s' in name"), *Filter);
	}
	else
	{
		FilterCondition = FString::Printf(TEXT("'%s'.lower() in name.lower()"), *Filter);
	}

	// 构建类型过滤
	FString TypeFiltering;
	if (!IncludeClasses || !IncludeFunctions)
	{
		if (IncludeClasses && !IncludeFunctions)
			TypeFiltering = TEXT(" and inspect.isclass(obj)");
		else if (!IncludeClasses && IncludeFunctions)
			TypeFiltering = TEXT(" and (inspect.isfunction(obj) or inspect.isbuiltin(obj))");
		else
		{
			FMCPToolboxPythonModuleInfo EmptyInfo;
			EmptyInfo.ModuleName = TEXT("unreal");
			EmptyInfo.TotalMembers = 0;
			return TMCPToolboxResult<FMCPToolboxPythonModuleInfo>::Success(EmptyInfo);
		}
	}

	FString MaxItemsCode = MaxItems > 0 ?
		FString::Printf(TEXT("    if result['total_members'] >= %d:\n        break\n"), MaxItems) : TEXT("");

	FString IntrospectionCode = FMCPToolboxPythonIntrospectionBuilder::BuildModuleIntrospectionScript(
		FilterCondition, TypeFiltering, MaxItemsCode);

	auto ExecResult = ExecuteIntrospectionScript(IntrospectionCode);
	if (ExecResult.IsError())
	{
		return TMCPToolboxResult<FMCPToolboxPythonModuleInfo>::Error(
			ExecResult.GetErrorCode(),
			ExecResult.GetErrorMessage()
		);
	}

	FMCPToolboxPythonModuleInfo ModuleInfo;
	if (!ParseModuleInfo(ExecResult.GetValue(), ModuleInfo))
	{
		return TMCPToolboxResult<FMCPToolboxPythonModuleInfo>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_INTROSPECTION_FAILED,
			TEXT("Failed to parse module introspection results")
		);
	}

	ModuleCache.Add(CacheKey, ModuleInfo);
	return TMCPToolboxResult<FMCPToolboxPythonModuleInfo>::Success(ModuleInfo);
}

// ═══════════════════════════════════════════════════════════════
// 类内省
// ═══════════════════════════════════════════════════════════════

TMCPToolboxResult<FMCPToolboxPythonClassInfo> FMCPToolboxPythonDiscoveryService::DiscoverClass(
	const FString& ClassName,
	const FString& MethodFilter,
	int32 MaxMethods,
	bool IncludeInherited,
	bool IncludePrivate)
{
	FString CacheKey = FString::Printf(TEXT("%s_%s_%d_%d_%d"),
		*ClassName, *MethodFilter, MaxMethods, IncludeInherited ? 1 : 0, IncludePrivate ? 1 : 0);
	if (ClassCache.Contains(CacheKey))
	{
		return TMCPToolboxResult<FMCPToolboxPythonClassInfo>::Success(ClassCache[CacheKey]);
	}

	FString NormalizedName = ClassName;
	if (ClassName.StartsWith(TEXT("unreal.")))
		NormalizedName = ClassName.RightChop(7);

	// 构建方法过滤(| 和 , 为 OR 分隔符)
	FString MethodFilterCondition = TEXT("True");
	if (!MethodFilter.IsEmpty())
	{
		const FString NormalizedFilter = MethodFilter.Replace(TEXT(","), TEXT("|"));
		TArray<FString> Patterns;
		NormalizedFilter.ParseIntoArray(Patterns, TEXT("|"), true);

		TArray<FString> QuotedPatterns;
		for (FString& Pattern : Patterns)
		{
			Pattern.TrimStartAndEndInline();
			Pattern.ReplaceInline(TEXT("\\"), TEXT(""));
			Pattern.ReplaceInline(TEXT("'"), TEXT(""));
			if (!Pattern.IsEmpty())
				QuotedPatterns.Add(FString::Printf(TEXT("'%s'"), *Pattern.ToLower()));
		}
		if (QuotedPatterns.Num() > 0)
		{
			MethodFilterCondition = FString::Printf(TEXT("any(p in name.lower() for p in [%s])"),
				*FString::Join(QuotedPatterns, TEXT(", ")));
		}
	}

	FString PrivacyFilter = IncludePrivate ? TEXT("") :
		TEXT("        if name.startswith('_'):\n            continue\n");

	FString InheritanceFilter = IncludeInherited ?
		TEXT("inspect.getmembers(cls)") :
		TEXT("[(n, getattr(cls, n)) for n in cls.__dict__ if not n.startswith('__')]");

	FString MaxMethodsCode = MaxMethods > 0 ?
		FString::Printf(TEXT("        if len(result['methods']) >= %d:\n            break\n"), MaxMethods) : TEXT("");

	FString IntrospectionCode = FMCPToolboxPythonIntrospectionBuilder::BuildClassIntrospectionScript(
		NormalizedName, InheritanceFilter, PrivacyFilter, MethodFilterCondition, MaxMethodsCode);

	auto ExecResult = ExecuteIntrospectionScript(IntrospectionCode);
	if (ExecResult.IsError())
	{
		return TMCPToolboxResult<FMCPToolboxPythonClassInfo>::Error(
			ExecResult.GetErrorCode(),
			ExecResult.GetErrorMessage()
		);
	}

	FMCPToolboxPythonClassInfo ClassInfo;
	if (!ParseClassInfo(ExecResult.GetValue(), ClassInfo))
	{
		return TMCPToolboxResult<FMCPToolboxPythonClassInfo>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_CLASS_NOT_FOUND,
			FString::Printf(TEXT("Class '%s' not found in unreal module"), *ClassName)
		);
	}

	ClassCache.Add(CacheKey, ClassInfo);
	return TMCPToolboxResult<FMCPToolboxPythonClassInfo>::Success(ClassInfo);
}

// ═══════════════════════════════════════════════════════════════
// 函数内省
// ═══════════════════════════════════════════════════════════════

TMCPToolboxResult<FMCPToolboxPythonFunctionInfo> FMCPToolboxPythonDiscoveryService::DiscoverFunction(const FString& FunctionPath)
{
	FString NormalizedName = FunctionPath;
	if (FunctionPath.StartsWith(TEXT("unreal.")))
		NormalizedName = FunctionPath.RightChop(7);

	int32 DotIndex;
	bool bIsClassMethod = NormalizedName.FindChar(TEXT('.'), DotIndex);

	FString IntrospectionCode;

	if (bIsClassMethod)
	{
		FString ClassName = NormalizedName.Left(DotIndex);
		FString MethodName = NormalizedName.Mid(DotIndex + 1);
		IntrospectionCode = FMCPToolboxPythonIntrospectionBuilder::BuildFunctionIntrospectionScript(
			MethodName, ClassName, true);
	}
	else
	{
		IntrospectionCode = FMCPToolboxPythonIntrospectionBuilder::BuildFunctionIntrospectionScript(
			NormalizedName, TEXT(""), false);
	}

	auto ExecResult = ExecuteIntrospectionScript(IntrospectionCode);
	if (ExecResult.IsError())
	{
		return TMCPToolboxResult<FMCPToolboxPythonFunctionInfo>::Error(
			ExecResult.GetErrorCode(),
			ExecResult.GetErrorMessage()
		);
	}

	FMCPToolboxPythonFunctionInfo FuncInfo;
	if (!ParseFunctionInfo(ExecResult.GetValue(), FuncInfo))
	{
		return TMCPToolboxResult<FMCPToolboxPythonFunctionInfo>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_FUNCTION_NOT_FOUND,
			FString::Printf(TEXT("Function '%s' not found in unreal module"), *FunctionPath)
		);
	}

	return TMCPToolboxResult<FMCPToolboxPythonFunctionInfo>::Success(FuncInfo);
}

// ═══════════════════════════════════════════════════════════════
// API 搜索
// ═══════════════════════════════════════════════════════════════

TMCPToolboxResult<TArray<FString>> FMCPToolboxPythonDiscoveryService::SearchAPI(
	const FString& SearchPattern,
	const FString& SearchType)
{
	auto ModuleResult = DiscoverUnrealModule(1, SearchPattern);
	if (ModuleResult.IsError())
	{
		return TMCPToolboxResult<TArray<FString>>::Error(
			ModuleResult.GetErrorCode(),
			ModuleResult.GetErrorMessage()
		);
	}

	const FMCPToolboxPythonModuleInfo& ModuleInfo = ModuleResult.GetValue();
	TArray<FString> Results;

	if (SearchType.Equals(TEXT("all"), ESearchCase::IgnoreCase) ||
		SearchType.Equals(TEXT("class"), ESearchCase::IgnoreCase))
	{
		for (const FString& ClassName : ModuleInfo.Classes)
			Results.Add(FString::Printf(TEXT("class: %s"), *ClassName));
	}

	if (SearchType.Equals(TEXT("all"), ESearchCase::IgnoreCase) ||
		SearchType.Equals(TEXT("function"), ESearchCase::IgnoreCase))
	{
		for (const FString& FunctionName : ModuleInfo.Functions)
			Results.Add(FString::Printf(TEXT("function: %s"), *FunctionName));
	}

	return TMCPToolboxResult<TArray<FString>>::Success(Results);
}

TMCPToolboxResult<TArray<FString>> FMCPToolboxPythonDiscoveryService::ListEditorSubsystems()
{
	FString IntrospectionCode = FMCPToolboxPythonIntrospectionBuilder::BuildSubsystemListScript();

	auto ExecResult = ExecuteIntrospectionScript(IntrospectionCode);
	if (ExecResult.IsError())
	{
		return TMCPToolboxResult<TArray<FString>>::Error(
			ExecResult.GetErrorCode(),
			ExecResult.GetErrorMessage()
		);
	}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExecResult.GetValue());
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return TMCPToolboxResult<TArray<FString>>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_INTROSPECTION_FAILED,
			TEXT("Failed to parse subsystem list")
		);
	}

	TArray<FString> Subsystems;
	const TArray<TSharedPtr<FJsonValue>>* SubsystemsArray;
	if (JsonObject->TryGetArrayField(TEXT("subsystems"), SubsystemsArray))
	{
		for (const auto& Value : *SubsystemsArray)
			Subsystems.Add(Value->AsString());
	}

	return TMCPToolboxResult<TArray<FString>>::Success(Subsystems);
}

// ═══════════════════════════════════════════════════════════════
// 源码读取
// ═══════════════════════════════════════════════════════════════

TMCPToolboxResult<FString> FMCPToolboxPythonDiscoveryService::ReadSourceFile(
	const FString& RelativePath,
	int32 StartLine,
	int32 MaxLines)
{
	if (!IsValidSourcePath(RelativePath))
	{
		return TMCPToolboxResult<FString>::Error(
			MCPToolboxPythonErrorCodes::PARAM_INVALID,
			FString::Printf(TEXT("Invalid source path: %s"), *RelativePath)
		);
	}

	FString FullPath = GetFullSourcePath(RelativePath);
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

	if (!PF.FileExists(*FullPath))
	{
		return TMCPToolboxResult<FString>::Error(
			MCPToolboxPythonErrorCodes::PYTHON_MODULE_NOT_FOUND,
			FString::Printf(TEXT("Source file not found: %s"), *RelativePath)
		);
	}

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FullPath))
	{
		return TMCPToolboxResult<FString>::Error(
			MCPToolboxPythonErrorCodes::OPERATION_FAILED,
			FString::Printf(TEXT("Failed to read source file: %s"), *RelativePath)
		);
	}

	int32 EndLine = FMath::Min(StartLine + MaxLines, Lines.Num());
	FString Result;

	for (int32 i = StartLine; i < EndLine; ++i)
	{
		if (i > StartLine) Result += TEXT("\n");
		Result += FString::Printf(TEXT("%5d: %s"), i + 1, *Lines[i]);
	}

	return TMCPToolboxResult<FString>::Success(Result);
}

TMCPToolboxResult<TArray<FMCPToolboxSourceSearchResult>> FMCPToolboxPythonDiscoveryService::SearchSourceFiles(
	const FString& Pattern,
	const FString& FilePattern,
	int32 ContextLines)
{
	TArray<FMCPToolboxSourceSearchResult> Results;

	TArray<FString> FilePatterns;
	FilePattern.ParseIntoArray(FilePatterns, TEXT(","), true);

	FString SearchPath = GetPluginSourceRoot();
	TArray<FString> AllFiles;

	for (const FString& SinglePattern : FilePatterns)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *SearchPath, *SinglePattern.TrimStartAndEnd(), true, false);
		AllFiles.Append(Files);
	}

	for (const FString& FilePath : AllFiles)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
			continue;

		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			if (Lines[LineIndex].Contains(Pattern))
			{
				FMCPToolboxSourceSearchResult Result;
				Result.FilePath = FilePath;
				FString PluginRoot = GetPluginSourceRoot();
				if (!PluginRoot.EndsWith(TEXT("/"))) PluginRoot += TEXT("/");
				FPaths::MakePathRelativeTo(Result.FilePath, *PluginRoot);

				Result.LineNumber = LineIndex + 1;
				Result.LineContent = Lines[LineIndex];

				int32 ContextStart = FMath::Max(0, LineIndex - ContextLines);
				for (int32 i = ContextStart; i < LineIndex; ++i)
					Result.ContextBefore.Add(Lines[i]);

				int32 ContextEnd = FMath::Min(Lines.Num(), LineIndex + ContextLines + 1);
				for (int32 i = LineIndex + 1; i < ContextEnd; ++i)
					Result.ContextAfter.Add(Lines[i]);

				Results.Add(Result);
			}
		}
	}

	return TMCPToolboxResult<TArray<FMCPToolboxSourceSearchResult>>::Success(Results);
}

TMCPToolboxResult<TArray<FString>> FMCPToolboxPythonDiscoveryService::ListSourceFiles(
	const FString& SubDirectory,
	const FString& FilePattern)
{
	FString SearchPath = GetPluginSourceRoot();
	if (!SubDirectory.IsEmpty())
		SearchPath = FPaths::Combine(SearchPath, SubDirectory);

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *SearchPath, *FilePattern, true, false);

	FString PluginRoot = GetPluginSourceRoot();
	if (!PluginRoot.EndsWith(TEXT("/"))) PluginRoot += TEXT("/");

	for (FString& File : Files)
		FPaths::MakePathRelativeTo(File, *PluginRoot);

	return TMCPToolboxResult<TArray<FString>>::Success(Files);
}

// ═══════════════════════════════════════════════════════════════
// 内部方法
// ═══════════════════════════════════════════════════════════════

TMCPToolboxResult<FString> FMCPToolboxPythonDiscoveryService::ExecuteIntrospectionScript(const FString& PythonCode)
{
	auto ExecResult = FMCPToolboxPythonExecutionService::ExecuteCode(
		PythonCode, EPythonFileExecutionScope::Private);

	if (ExecResult.IsError())
	{
		return TMCPToolboxResult<FString>::Error(
			ExecResult.GetErrorCode(),
			ExecResult.GetErrorMessage()
		);
	}

	return TMCPToolboxResult<FString>::Success(
		ExecResult.GetValue().Output.TrimStartAndEnd()
	);
}

bool FMCPToolboxPythonDiscoveryService::ParseModuleInfo(const FString& JsonResult, FMCPToolboxPythonModuleInfo& OutInfo)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonResult);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		return false;

	JsonObject->TryGetStringField(TEXT("module_name"), OutInfo.ModuleName);
	JsonObject->TryGetNumberField(TEXT("total_members"), OutInfo.TotalMembers);

	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (JsonObject->TryGetArrayField(TEXT("classes"), Arr))
		for (const auto& V : *Arr) OutInfo.Classes.Add(V->AsString());
	if (JsonObject->TryGetArrayField(TEXT("functions"), Arr))
		for (const auto& V : *Arr) OutInfo.Functions.Add(V->AsString());
	if (JsonObject->TryGetArrayField(TEXT("constants"), Arr))
		for (const auto& V : *Arr) OutInfo.Constants.Add(V->AsString());

	return true;
}

bool FMCPToolboxPythonDiscoveryService::ParseClassInfo(const FString& JsonResult, FMCPToolboxPythonClassInfo& OutInfo)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonResult);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		return false;

	FString Error;
	if (JsonObject->TryGetStringField(TEXT("error"), Error))
		return false;

	JsonObject->TryGetStringField(TEXT("name"), OutInfo.Name);
	JsonObject->TryGetStringField(TEXT("full_path"), OutInfo.FullPath);
	JsonObject->TryGetStringField(TEXT("docstring"), OutInfo.Docstring);
	JsonObject->TryGetBoolField(TEXT("is_abstract"), OutInfo.bIsAbstract);

	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (JsonObject->TryGetArrayField(TEXT("base_classes"), Arr))
		for (const auto& V : *Arr) OutInfo.BaseClasses.Add(V->AsString());
	if (JsonObject->TryGetArrayField(TEXT("methods"), Arr))
	{
		for (const auto& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* MethodObj;
			if (V->TryGetObject(MethodObj))
			{
				FMCPToolboxPythonFunctionInfo FuncInfo;
				(*MethodObj)->TryGetStringField(TEXT("name"), FuncInfo.Name);
				(*MethodObj)->TryGetStringField(TEXT("signature"), FuncInfo.Signature);
				(*MethodObj)->TryGetStringField(TEXT("docstring"), FuncInfo.Docstring);
				FuncInfo.bIsMethod = true;
				OutInfo.Methods.Add(FuncInfo);
			}
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("properties"), Arr))
		for (const auto& V : *Arr) OutInfo.Properties.Add(V->AsString());

	return true;
}

bool FMCPToolboxPythonDiscoveryService::ParseFunctionInfo(const FString& JsonResult, FMCPToolboxPythonFunctionInfo& OutInfo)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonResult);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		return false;

	FString Error;
	if (JsonObject->TryGetStringField(TEXT("error"), Error))
		return false;

	JsonObject->TryGetStringField(TEXT("name"), OutInfo.Name);
	JsonObject->TryGetStringField(TEXT("signature"), OutInfo.Signature);
	JsonObject->TryGetStringField(TEXT("docstring"), OutInfo.Docstring);
	JsonObject->TryGetStringField(TEXT("return_type"), OutInfo.ReturnType);
	JsonObject->TryGetBoolField(TEXT("is_method"), OutInfo.bIsMethod);
	JsonObject->TryGetBoolField(TEXT("is_static"), OutInfo.bIsStatic);
	JsonObject->TryGetBoolField(TEXT("is_class_method"), OutInfo.bIsClassMethod);

	const TArray<TSharedPtr<FJsonValue>>* ParamsArray;
	if (JsonObject->TryGetArrayField(TEXT("parameters"), ParamsArray))
		for (const auto& V : *ParamsArray) OutInfo.Parameters.Add(V->AsString());
	if (JsonObject->TryGetArrayField(TEXT("param_types"), ParamsArray))
		for (const auto& V : *ParamsArray) OutInfo.ParamTypes.Add(V->AsString());

	return true;
}

FString FMCPToolboxPythonDiscoveryService::GetPluginSourceRoot()
{
	FString EngineDir = FPaths::EngineDir();
	return FPaths::Combine(EngineDir, TEXT("Plugins/Experimental/PythonScriptPlugin"));
}

bool FMCPToolboxPythonDiscoveryService::IsValidSourcePath(const FString& Path)
{
	if (Path.Contains(TEXT("..")) || Path.Contains(TEXT("~")))
		return false;

	if (FPaths::IsRelative(Path) == false &&
		!Path.StartsWith(TEXT("Source/")) &&
		!Path.StartsWith(TEXT("Content/")) &&
		!Path.StartsWith(TEXT("Public/")) &&
		!Path.StartsWith(TEXT("Private/")))
	{
		return false;
	}

	return true;
}

FString FMCPToolboxPythonDiscoveryService::GetFullSourcePath(const FString& RelativePath)
{
	FString PluginRoot = GetPluginSourceRoot();

	if (RelativePath.StartsWith(TEXT("Public/")) || RelativePath.StartsWith(TEXT("Private/")))
		return FPaths::Combine(PluginRoot, TEXT("Source/PythonScriptPlugin"), RelativePath);
	else if (RelativePath.StartsWith(TEXT("Content/")))
		return FPaths::Combine(PluginRoot, RelativePath);
	else if (RelativePath.StartsWith(TEXT("Source/")))
		return FPaths::Combine(PluginRoot, RelativePath);

	return FPaths::Combine(PluginRoot, TEXT("Source/PythonScriptPlugin"), RelativePath);
}
