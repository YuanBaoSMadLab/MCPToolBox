#pragma once

#include "CoreMinimal.h"
#include "Http.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"

DECLARE_MULTICAST_DELEGATE(FOnMCPConnected);
DECLARE_MULTICAST_DELEGATE(FOnMCPDisconnected);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPToolsDiscovered, const TArray<TSharedPtr<FJsonObject>>& /*Tools*/);

/**
 * HTTP JSON-RPC 2.0 client for UE's built-in MCP Server.
 * Connects to localhost:8000 and provides tool discovery + execution.
 */
class MCPTOOLBOX_API FMCPToolboxMCPServerClient
{
public:
	FMCPToolboxMCPServerClient();
	~FMCPToolboxMCPServerClient();

	/** Start connection to MCP server. Returns true if init request was sent. */
	bool Connect(const FString& Host = TEXT("127.0.0.1"), int32 Port = 8000);

	/** Check if connected and initialized */
	bool IsConnected() const { return bInitialized; }

	/** Auto-discover actual tools (not just meta-tools) via list_toolsets → describe_toolset.
	 *  OnProgress is called after each describe_toolset completes (Done/Total/ToolsetName). */
	void DiscoverRealTools(
		TFunction<void(const TArray<TSharedPtr<FJsonObject>>&)> OnComplete,
		TFunction<void(int32 Done, int32 Total, const FString& CurrentToolset)> OnProgress = nullptr);

	/** Execute a tool by name with JSON arguments (sends tools/call) */
	void ExecuteTool(const FString& ToolName, const FString& ArgumentsJson,
		TFunction<void(bool bSuccess, const FString& Result)> OnComplete);

	/** Call a direct JSON-RPC method (for meta-tools like list_toolsets) */
	void CallDirectMethod(const FString& Method, const TSharedPtr<FJsonObject>& Params,
		TFunction<void(bool bSuccess, const TSharedPtr<FJsonObject>& Result)> OnComplete);

	/** Get the list of discovered tools */
	TArray<TSharedPtr<FJsonObject>>& GetTools() { return Tools; }

	/** O(1) check if a tool name is an MCP tool */
	bool IsMCPTool(const FString& Name) const { return McpToolNameSet.Contains(Name); }
	void RebuildToolNameSet();

	// Delegates
	FOnMCPConnected OnConnected;
	FOnMCPDisconnected OnDisconnected;
	FOnMCPToolsDiscovered OnToolsDiscovered;

private:
	void SendJsonRpc(const FString& Method, const TSharedPtr<FJsonObject>& Params,
		TFunction<void(bool bSuccess, const TSharedPtr<FJsonObject>& Result)> OnComplete);

	void HandleInitializeResponse(FHttpResponsePtr Resp);
	void HandleToolsListResponse(FHttpResponsePtr Resp);
	void HandleToolCallResponse(FHttpResponsePtr Resp,
		TFunction<void(bool bSuccess, const FString& Result)> OnComplete);

	// Helpers for tool discovery
	void ExtractToolsetsFromResult(const TSharedPtr<FJsonObject>& Result, TArray<FString>& OutNames);
	void ExtractToolsFromDescribeResult(const TSharedPtr<FJsonObject>& Result,
		const FString& ToolsetName, TArray<TSharedPtr<FJsonObject>>& OutTools);

	FString GetBaseUrl() const;

	FString Host;
	int32 Port = 8000;
	int32 NextRequestId = 1;
	bool bInitialized = false;
	FString SessionEndpoint;
	FString McpSessionId; // from Mcp-Session-Id response header

	TArray<TSharedPtr<FJsonObject>> Tools;
	TSet<FString> McpToolNameSet;
};
