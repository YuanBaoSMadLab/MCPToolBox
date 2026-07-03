// ============================================================================
// MCPToolboxServiceRegistry — 实现 (Stage 6.4)
// ============================================================================

#include "MCPToolboxServiceRegistry.h"
#include "Json.h"

FMCPToolboxServiceRegistry& FMCPToolboxServiceRegistry::Get()
{
	static FMCPToolboxServiceRegistry Instance;
	return Instance;
}

void FMCPToolboxServiceRegistry::Register(const FMCPToolboxServiceInfo& Info)
{
	// 同名服务:覆盖现有条目(后注册者胜)
	for (FMCPToolboxServiceInfo& Existing : Services)
	{
		if (Existing.Name == Info.Name)
		{
			Existing = Info;
			UE_LOG(LogTemp, Log, TEXT("[ServiceRegistry] Updated service: %s (%d tools)"),
				*Info.Name, Info.ToolNames.Num());
			return;
		}
	}
	Services.Add(Info);
	UE_LOG(LogTemp, Log, TEXT("[ServiceRegistry] Registered service: %s (%d tools)"),
		*Info.Name, Info.ToolNames.Num());
}

FString FMCPToolboxServiceRegistry::ListServices() const
{
	// 返回 JSON: {"status":"ok","count":N,"services":[{name,description,source,tool_count},...]}
	FString Result = TEXT(R"({"status":"ok","count":)");
	Result += FString::FromInt(Services.Num());
	Result += TEXT(",\"services\":[");

	bool bFirst = true;
	for (const FMCPToolboxServiceInfo& S : Services)
	{
		if (!bFirst) Result += TEXT(",");
		bFirst = false;

		// JSON 转义
		FString EscName = S.Name;
		EscName.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscName.ReplaceInline(TEXT("\""), TEXT("\\\""));
		FString EscDesc = S.Description;
		EscDesc.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscDesc.ReplaceInline(TEXT("\""), TEXT("\\\""));
		EscDesc.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		EscDesc.ReplaceInline(TEXT("\r"), TEXT(""));
		FString EscSrc = S.Source;
		EscSrc.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscSrc.ReplaceInline(TEXT("\""), TEXT("\\\""));

		Result += TEXT("{\"name\":\"") + EscName + TEXT("\"");
		Result += TEXT(",\"description\":\"") + EscDesc + TEXT("\"");
		Result += TEXT(",\"source\":\"") + EscSrc + TEXT("\"");
		Result += TEXT(",\"tool_count\":") + FString::FromInt(S.ToolNames.Num());
		Result += TEXT("}");
	}

	Result += TEXT("]}");
	return Result;
}

FString FMCPToolboxServiceRegistry::GetServiceInfo(const FString& ServiceName) const
{
	// 查找指定服务,返回包含完整工具列表的 JSON
	const FMCPToolboxServiceInfo* Found = nullptr;
	for (const FMCPToolboxServiceInfo& S : Services)
	{
		if (S.Name == ServiceName)
		{
			Found = &S;
			break;
		}
	}

	if (!Found)
	{
		// 收集所有合法服务名,帮助 LLM 自纠正
		FString ValidNames;
		bool bFirst = true;
		for (const FMCPToolboxServiceInfo& S : Services)
		{
			if (!bFirst) ValidNames += TEXT(", ");
			bFirst = false;
			ValidNames += TEXT("\"") + S.Name + TEXT("\"");
		}
		return TEXT(R"({"status":"error","error_code":"SERVICE_NOT_FOUND","service_name":")") + ServiceName
			+ TEXT(R"(","valid_services":[)") + ValidNames + TEXT("]}");
	}

	// 构建成功响应
	FString EscName = Found->Name;
	EscName.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscName.ReplaceInline(TEXT("\""), TEXT("\\\""));
	FString EscDesc = Found->Description;
	EscDesc.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscDesc.ReplaceInline(TEXT("\""), TEXT("\\\""));
	EscDesc.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	EscDesc.ReplaceInline(TEXT("\r"), TEXT(""));
	FString EscSrc = Found->Source;
	EscSrc.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscSrc.ReplaceInline(TEXT("\""), TEXT("\\\""));

	FString Result = TEXT(R"({"status":"ok",)");
	Result += TEXT("\"name\":\"") + EscName + TEXT("\"");
	Result += TEXT(",\"description\":\"") + EscDesc + TEXT("\"");
	Result += TEXT(",\"source\":\"") + EscSrc + TEXT("\"");
	Result += TEXT(",\"tool_count\":") + FString::FromInt(Found->ToolNames.Num());
	Result += TEXT(",\"tools\":[");
	for (int32 i = 0; i < Found->ToolNames.Num(); ++i)
	{
		if (i > 0) Result += TEXT(",");
		FString EscTool = Found->ToolNames[i];
		EscTool.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscTool.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Result += TEXT("\"") + EscTool + TEXT("\"");
	}
	Result += TEXT("]}");
	return Result;
}
