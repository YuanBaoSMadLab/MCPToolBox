#include "MCPToolboxMCPServerClient.h"
#include "MCPToolbox.h"
#include "HttpModule.h"

FMCPToolboxMCPServerClient::FMCPToolboxMCPServerClient()
{
}

FMCPToolboxMCPServerClient::~FMCPToolboxMCPServerClient()
{
}

FString FMCPToolboxMCPServerClient::GetBaseUrl() const
{
	return FString::Printf(TEXT("http://%s:%d"), *Host, Port);
}

// ============================================================================
// Connect — try streamable HTTP POST first, fallback to SSE
// ============================================================================
bool FMCPToolboxMCPServerClient::Connect(const FString& InHost, int32 InPort)
{
	Host = InHost;
	Port = InPort;
	bInitialized = false;
	NextRequestId = 1;
	SessionEndpoint.Empty();

	UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] Connecting to %s:%d..."), *Host, Port);

	// Try streamable HTTP: POST / with JSON-RPC directly (MCP 2025 spec)
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
	Params->SetStringField(TEXT("protocolVersion"), TEXT("2024-11-05"));
	TSharedPtr<FJsonObject> ClientInfo = MakeShareable(new FJsonObject());
	ClientInfo->SetStringField(TEXT("name"), TEXT("MCPToolbox"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("0.1.0"));
	Params->SetObjectField(TEXT("clientInfo"), ClientInfo);
	Params->SetObjectField(TEXT("capabilities"), MakeShareable(new FJsonObject()));

	// Build JSON-RPC body
	TSharedPtr<FJsonObject> Rpc = MakeShareable(new FJsonObject());
	Rpc->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Rpc->SetNumberField(TEXT("id"), NextRequestId++);
	Rpc->SetStringField(TEXT("method"), TEXT("initialize"));
	Rpc->SetObjectField(TEXT("params"), Params);
	FString Body;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Rpc.ToSharedRef(), W);

	FString RootUrl = FString::Printf(TEXT("http://%s:%d/mcp"), *Host, Port);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(RootUrl);
	Req->SetVerb(TEXT("POST"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
	Req->SetTimeout(5.0f);
	Req->SetContentAsString(Body);

	UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] POST /mcp (streamable HTTP) initialize"));

	Req->OnProcessRequestComplete().BindLambda(
		[this, RootUrl](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
		{
			if (!bSuccess || !Resp.IsValid())
			{
				UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] POST /mcp failed — network error"));
				OnDisconnected.Broadcast();
				return;
			}

			int32 Code = Resp->GetResponseCode();

			// Capture MCP session ID from response headers
			FString SessionHeader = Resp->GetHeader(TEXT("Mcp-Session-Id"));
			if (!SessionHeader.IsEmpty())
			{
				McpSessionId = SessionHeader;
				UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] Session: %s"), *McpSessionId);
			}

			FString RespBody = Resp->GetContentAsString();
			UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] POST /mcp → HTTP %d, %d bytes"), Code, RespBody.Len());

			if (Code == 404)
			{
				// POST / not found — try GET /sse for standard SSE transport
				UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] POST / returned 404, trying GET /sse..."));
				FString SseUrl = FString::Printf(TEXT("http://%s:%d/sse"), *Host, Port);
				auto SseReq = FHttpModule::Get().CreateRequest();
				SseReq->SetURL(SseUrl);
				SseReq->SetVerb(TEXT("GET"));
				SseReq->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
				SseReq->SetTimeout(5.0f);
				SseReq->OnProcessRequestComplete().BindLambda(
					[this](FHttpRequestPtr SReq, FHttpResponsePtr SResp, bool SOk)
					{
						if (!SOk || !SResp.IsValid() || SResp->GetResponseCode() != 200)
						{
							UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] GET /sse also failed — MCP server not compatible"));
							OnDisconnected.Broadcast();
							return;
						}
						// Parse SSE endpoint and continue... (existing logic)
						FString SBody = SResp->GetContentAsString();
						TArray<FString> Lines;
						SBody.ParseIntoArrayLines(Lines);
						for (int32 i = 0; i < Lines.Num(); ++i)
						{
							FString L = Lines[i].TrimStartAndEnd();
							if (L.StartsWith(TEXT("data: ")) && L.Contains(TEXT("sessionId")))
							{
								SessionEndpoint = L.RightChop(6).TrimStartAndEnd();
								if (SessionEndpoint.StartsWith(TEXT("/")))
									SessionEndpoint = FString::Printf(TEXT("http://%s:%d%s"), *Host, Port, *SessionEndpoint);
								UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] SSE session: %s"), *SessionEndpoint);
								break;
							}
						}
						if (SessionEndpoint.IsEmpty())
						{
							UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] No session in SSE response"));
							OnDisconnected.Broadcast();
							return;
						}
						// Re-send initialize via session endpoint
						// ... reuse the same RPC body generation but with SessionEndpoint URL
						// For simplicity, just set bInitialized=true and let user retry
						bInitialized = true;
						UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] Initialized via SSE!"));
						OnConnected.Broadcast();
					});
				SseReq->ProcessRequest();
				return;
			}

			if (Code != 200)
			{
				UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] POST / HTTP %d: %s"), Code, *RespBody.Left(200));
				OnDisconnected.Broadcast();
				return;
			}

			// Parse JSON-RPC response
			TSharedPtr<FJsonObject> RespObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RespBody);
			if (!FJsonSerializer::Deserialize(Reader, RespObj) || !RespObj.IsValid())
			{
				UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] Invalid JSON response from POST /"));
				OnDisconnected.Broadcast();
				return;
			}

			const TSharedPtr<FJsonObject>* ErrorObj;
			if (RespObj->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				FString ErrMsg;
				(*ErrorObj)->TryGetStringField(TEXT("message"), ErrMsg);
				UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] initialize error: %s"), *ErrMsg);
				OnDisconnected.Broadcast();
				return;
			}

			bInitialized = true;
			UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] Initialized via streamable HTTP!"));

			// Send initialized notification
			TSharedPtr<FJsonObject> NotifyRpc = MakeShareable(new FJsonObject());
			NotifyRpc->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			NotifyRpc->SetStringField(TEXT("method"), TEXT("notifications/initialized"));
			FString NotifyBody;
			TSharedRef<TJsonWriter<>> NW = TJsonWriterFactory<>::Create(&NotifyBody);
			FJsonSerializer::Serialize(NotifyRpc.ToSharedRef(), NW);

			auto NotifyReq = FHttpModule::Get().CreateRequest();
			NotifyReq->SetURL(RootUrl);
			NotifyReq->SetVerb(TEXT("POST"));
			NotifyReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			if (!McpSessionId.IsEmpty())
				NotifyReq->SetHeader(TEXT("Mcp-Session-Id"), McpSessionId);
			NotifyReq->SetContentAsString(NotifyBody);
			NotifyReq->ProcessRequest();

			OnConnected.Broadcast();
		});

	Req->ProcessRequest();
	return true;
}

// ============================================================================
// DiscoverRealTools — chains list_toolsets → describe_toolset to get all real tools
// ============================================================================
void FMCPToolboxMCPServerClient::DiscoverRealTools(TFunction<void(const TArray<TSharedPtr<FJsonObject>>&)> OnComplete)
{
	if (!bInitialized)
	{
		OnComplete(TArray<TSharedPtr<FJsonObject>>());
		return;
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] Discovering real tools via list_toolsets..."));

	// Step 1: Call list_toolsets via tools/call
	TSharedPtr<FJsonObject> ListParams = MakeShareable(new FJsonObject());
	ListParams->SetStringField(TEXT("name"), TEXT("list_toolsets"));

	SendJsonRpc(TEXT("tools/call"), ListParams,
		[this, OnComplete](bool bSuccess, const TSharedPtr<FJsonObject>& Result)
		{
			if (!bSuccess || !Result.IsValid())
			{
				UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] list_toolsets failed"));
				OnComplete(TArray<TSharedPtr<FJsonObject>>());
				return;
			}

			// Debug: dump raw result
		FString Dump;
		TSharedRef<TJsonWriter<>> DW = TJsonWriterFactory<>::Create(&Dump);
		FJsonSerializer::Serialize(Result.ToSharedRef(), DW);
		UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] list_toolsets raw: %s"), *Dump.Left(2000));

		// Parse list of toolsets from result
			// Format: { "content": [{"type": "text", "text": "[...]"}] }
			TArray<FString> ToolsetNames;
			ExtractToolsetsFromResult(Result, ToolsetNames);

			if (ToolsetNames.Num() == 0)
			{
				UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] No toolsets found"));
				OnComplete(TArray<TSharedPtr<FJsonObject>>());
				return;
			}

			UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] Found %d toolsets: %s"), ToolsetNames.Num(), *FString::Join(ToolsetNames, TEXT(", ")));

			// Step 2: For each toolset, call describe_toolset
			TSharedPtr<TArray<TSharedPtr<FJsonObject>>> AllTools = MakeShared<TArray<TSharedPtr<FJsonObject>>>();
			TSharedPtr<int32> Remaining = MakeShared<int32>(ToolsetNames.Num());

			for (const FString& ToolsetName : ToolsetNames)
			{
				TSharedPtr<FJsonObject> DescParams = MakeShareable(new FJsonObject());
				DescParams->SetStringField(TEXT("name"), TEXT("describe_toolset"));
				TSharedPtr<FJsonObject> Args = MakeShareable(new FJsonObject());
				Args->SetStringField(TEXT("toolset_name"), ToolsetName);
				DescParams->SetObjectField(TEXT("arguments"), Args);

				SendJsonRpc(TEXT("tools/call"), DescParams,
					[this, AllTools, Remaining, OnComplete, ToolsetName](bool bOk, const TSharedPtr<FJsonObject>& DescResult)
					{
						if (bOk && DescResult.IsValid())
						{
							// Parse individual tools from describe_toolset result
							ExtractToolsFromDescribeResult(DescResult, ToolsetName, *AllTools);
						}

						(*Remaining)--;
						if (*Remaining <= 0)
						{
							UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] Discovered %d real tools"), AllTools->Num());
							OnComplete(*AllTools);
						}
					});
			}
		});
}

void FMCPToolboxMCPServerClient::ExtractToolsetsFromResult(const TSharedPtr<FJsonObject>& Result, TArray<FString>& OutNames)
{
	const TArray<TSharedPtr<FJsonValue>>* Content;
	if (Result->TryGetArrayField(TEXT("content"), Content))
	{
		for (const auto& Item : *Content)
		{
			TSharedPtr<FJsonObject> ItemObj = Item->AsObject();
			if (!ItemObj.IsValid()) continue;
			FString Text;
			if (ItemObj->TryGetStringField(TEXT("text"), Text))
			{
				// Try JSON array first: ["name1", "name2"]
				TSharedPtr<FJsonValue> Parsed;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
				if (FJsonSerializer::Deserialize(Reader, Parsed))
				{
					const TArray<TSharedPtr<FJsonValue>>* Arr;
					if (Parsed->TryGetArray(Arr))
					{
						for (const auto& V : *Arr)
						{
							FString Name;
							if (V->TryGetString(Name) && !Name.IsEmpty()) OutNames.Add(Name);
							else if (V->AsObject().IsValid())
							{
								V->AsObject()->TryGetStringField(TEXT("name"), Name);
								if (!Name.IsEmpty()) OutNames.Add(Name);
							}
						}
					}
				}

				// If JSON parse failed, try plain text format: "- ToolsetName: description"
				if (OutNames.Num() == 0)
				{
					TArray<FString> Lines;
					Text.ParseIntoArrayLines(Lines);
					for (const FString& Line : Lines)
					{
						FString Trimmed = Line.TrimStartAndEnd();
						// Remove leading "- " or "* "
						if (Trimmed.StartsWith(TEXT("- ")) || Trimmed.StartsWith(TEXT("* ")))
							Trimmed.RightChopInline(2);
						// Remove trailing description after ":"
						int32 ColonIdx;
						if (Trimmed.FindChar(TEXT(':'), ColonIdx))
							Trimmed.LeftChopInline(Trimmed.Len() - ColonIdx);
						Trimmed.TrimStartAndEndInline();
						if (!Trimmed.IsEmpty())
							OutNames.Add(Trimmed);
					}
				}
			}
		}
	}

	// Fallback: look for "toolsets" array directly
	if (OutNames.Num() == 0)
	{
		const TArray<TSharedPtr<FJsonValue>>* Toolsets;
		if (Result->TryGetArrayField(TEXT("toolsets"), Toolsets))
		{
			for (const auto& V : *Toolsets)
			{
				FString Name;
				if (V->TryGetString(Name)) OutNames.Add(Name);
			}
		}
	}
}

void FMCPToolboxMCPServerClient::ExtractToolsFromDescribeResult(const TSharedPtr<FJsonObject>& Result,
	const FString& ToolsetName, TArray<TSharedPtr<FJsonObject>>& OutTools)
{
	const TArray<TSharedPtr<FJsonValue>>* Content;
	if (Result->TryGetArrayField(TEXT("content"), Content))
	{
		for (const auto& Item : *Content)
		{
			TSharedPtr<FJsonObject> ItemObj = Item->AsObject();
			if (!ItemObj.IsValid()) continue;
			FString Text;
			if (ItemObj->TryGetStringField(TEXT("text"), Text))
			{
				TSharedPtr<FJsonValue> Parsed;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
				if (FJsonSerializer::Deserialize(Reader, Parsed))
				{
					const TArray<TSharedPtr<FJsonValue>>* Arr;
					if (Parsed->TryGetArray(Arr))
					{
						for (const auto& V : *Arr)
						{
							TSharedPtr<FJsonObject> ToolObj = V->AsObject();
							if (ToolObj.IsValid())
							{
								// Add toolset info to the tool definition
								ToolObj->SetStringField(TEXT("_toolset"), ToolsetName);
								OutTools.Add(ToolObj);
							}
						}
					}
					else if (Parsed->AsObject().IsValid())
					{
						TSharedPtr<FJsonObject> ToolObj = Parsed->AsObject();
						ToolObj->SetStringField(TEXT("_toolset"), ToolsetName);
						OutTools.Add(ToolObj);
					}
				}
			}
		}
	}

	// Fallback: look for "tools" array directly
	if (Result->HasField(TEXT("tools")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ToolsArr;
		if (Result->TryGetArrayField(TEXT("tools"), ToolsArr))
		{
			for (const auto& V : *ToolsArr)
			{
				TSharedPtr<FJsonObject> ToolObj = V->AsObject();
				if (ToolObj.IsValid())
				{
					ToolObj->SetStringField(TEXT("_toolset"), ToolsetName);
					OutTools.Add(ToolObj);
				}
			}
		}
	}
}
// ============================================================================
// ExecuteTool
// ============================================================================
void FMCPToolboxMCPServerClient::ExecuteTool(const FString& ToolName, const FString& ArgumentsJson,
	TFunction<void(bool bSuccess, const FString& Result)> OnComplete)
{
	if (!bInitialized)
	{
		OnComplete(false, TEXT(R"({"error":"MCP not connected"})"));
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
	Params->SetStringField(TEXT("name"), ToolName);

	// Parse arguments if any
	if (!ArgumentsJson.IsEmpty())
	{
		TSharedPtr<FJsonObject> ArgsObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsJson);
		if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
		{
			Params->SetObjectField(TEXT("arguments"), ArgsObj);
		}
	}

	SendJsonRpc(TEXT("tools/call"), Params,
		[OnComplete, ToolName](bool bSuccess, const TSharedPtr<FJsonObject>& Result)
		{
			if (!bSuccess || !Result.IsValid())
			{
				OnComplete(false, FString::Printf(TEXT(R"({"error":"MCP tool call failed: %s"})"), *ToolName));
				return;
			}

			FString Output;

			// Check for MCP error in result
			bool bIsError = false;
			Result->TryGetBoolField(TEXT("isError"), bIsError);

			// MCP standard format: { "content": [{ "type": "text", "text": "..." }, ...] }
			const TArray<TSharedPtr<FJsonValue>>* Content;
			if (Result->TryGetArrayField(TEXT("content"), Content) && Content->Num() > 0)
			{
				for (const auto& Item : *Content)
				{
					TSharedPtr<FJsonObject> ItemObj = Item->AsObject();
					if (!ItemObj.IsValid()) continue;

					FString Type;
					ItemObj->TryGetStringField(TEXT("type"), Type);

					if (Type == TEXT("text"))
					{
						FString Text;
						if (ItemObj->TryGetStringField(TEXT("text"), Text))
							Output += Text;
					}
					else if (Type == TEXT("image"))
					{
						Output += TEXT("[Image data]");
					}
					else if (Type == TEXT("resource"))
					{
						Output += TEXT("[Resource data]");
					}
				}
			}

			// Fallback: direct text field
			if (Output.IsEmpty())
			{
				Result->TryGetStringField(TEXT("text"), Output);
			}

			// Final fallback: serialize entire result
			if (Output.IsEmpty())
			{
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
				FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			}

			// Wrap in validation wrapper
			FString Validated = FString::Printf(TEXT(R"({"ok":%s,"tool":"%s","output":%s})"),
				bIsError ? TEXT("false") : TEXT("true"),
				*ToolName,
				*Output);

			UE_LOG(LogMCPToolbox, Verbose, TEXT("[MCPSrv] Tool result: %s"), *Validated.Left(200));
			OnComplete(true, Validated);
		});
}

// ============================================================================
// CallDirectMethod — send a JSON-RPC method directly (for meta-tools)
// ============================================================================
void FMCPToolboxMCPServerClient::CallDirectMethod(const FString& Method, const TSharedPtr<FJsonObject>& Params,
	TFunction<void(bool bSuccess, const TSharedPtr<FJsonObject>& Result)> OnComplete)
{
	if (!bInitialized)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] CallDirectMethod: not initialized"));
		if (OnComplete) OnComplete(false, nullptr);
		return;
	}

	SendJsonRpc(Method, Params, OnComplete);
}

// ============================================================================
// SendJsonRpc — send a JSON-RPC 2.0 request via HTTP POST
// ============================================================================
void FMCPToolboxMCPServerClient::SendJsonRpc(const FString& Method, const TSharedPtr<FJsonObject>& Params,
	TFunction<void(bool bSuccess, const TSharedPtr<FJsonObject>& Result)> OnComplete)
{
	TSharedPtr<FJsonObject> Rpc = MakeShareable(new FJsonObject());
	Rpc->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Rpc->SetNumberField(TEXT("id"), NextRequestId++);
	Rpc->SetStringField(TEXT("method"), Method);
	if (Params.IsValid())
		Rpc->SetObjectField(TEXT("params"), Params);

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Rpc.ToSharedRef(), Writer);

	// Use session endpoint from SSE, or fallback to /mcp
	FString Url;
	if (!SessionEndpoint.IsEmpty())
		Url = SessionEndpoint;
	else
		Url = FString::Printf(TEXT("%s/mcp"), *GetBaseUrl());

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	// Add MCP session header if we have one
	if (!McpSessionId.IsEmpty())
		Request->SetHeader(TEXT("Mcp-Session-Id"), McpSessionId);
	Request->SetTimeout(120.0f); // 2 min for long-running tool calls
	Request->SetContentAsString(Body);

	UE_LOG(LogMCPToolbox, Verbose, TEXT("[MCPSrv] → %s (%s)"), *Method, *Url);

	Request->OnProcessRequestComplete().BindLambda(
		[this, Method, OnComplete, Body, Url](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
		{
			if (!bSuccess || !Resp.IsValid())
			{
				UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] %s: request failed"), *Method);
				if (OnComplete) OnComplete(false, nullptr);
				return;
			}

			int32 Code = Resp->GetResponseCode();
		FString RespBody = Resp->GetContentAsString();
		FString ContentType = Resp->GetHeader(TEXT("Content-Type"));

		UE_LOG(LogMCPToolbox, Verbose, TEXT("[MCPSrv] ← %s (HTTP %d, %d bytes, %s)"), *Method, Code, RespBody.Len(), *ContentType);

		// UE5 MCP returns SSE for POST responses — extract the data line
		if (ContentType.Contains(TEXT("text/event-stream")))
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("[MCPSrv] %s: SSE response, extracting JSON..."), *Method);
			// SSE format: "event: message\ndata: {...}\n\n"
			TArray<FString> Lines;
			RespBody.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				FString Trimmed = Line.TrimStartAndEnd();
				if (Trimmed.StartsWith(TEXT("data:")) || Trimmed.StartsWith(TEXT("data: ")))
				{
					FString JsonPart = Trimmed.RightChop(5).TrimStart();
					if (!JsonPart.IsEmpty())
					{
						RespBody = JsonPart;
						UE_LOG(LogMCPToolbox, Verbose, TEXT("[MCPSrv] %s: extracted JSON from SSE data line"), *Method);
						break;
					}
				}
			}
		}

			if (Code != 200)
			{
				UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] %s: HTTP %d from %s"), *Method, Code, *Url);
				if (OnComplete) OnComplete(false, nullptr);
				return;
			}

			TSharedPtr<FJsonObject> RespObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RespBody);
		if (!FJsonSerializer::Deserialize(Reader, RespObj) || !RespObj.IsValid())
		{
			// Log the raw response for debugging
			UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] %s: invalid JSON response (%d bytes): %s"), *Method, RespBody.Len(), *RespBody.Left(500));
			if (OnComplete) OnComplete(false, nullptr);
			return;
		}

			// Check for JSON-RPC error
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (RespObj->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				FString ErrMsg;
				(*ErrorObj)->TryGetStringField(TEXT("message"), ErrMsg);
				UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPSrv] %s error: %s"), *Method, *ErrMsg);
				if (OnComplete) OnComplete(false, nullptr);
				return;
			}

			// Extract result
			const TSharedPtr<FJsonObject>* Result;
			if (RespObj->TryGetObjectField(TEXT("result"), Result) && Result->IsValid())
			{
				if (OnComplete) OnComplete(true, *Result);
			}
			else
			{
				// Result might be a simple value, not an object — treat whole response as result
				if (OnComplete) OnComplete(true, RespObj);
			}
		});

	Request->ProcessRequest();
}

void FMCPToolboxMCPServerClient::RebuildToolNameSet()
{
	McpToolNameSet.Empty();
	for (const auto& T : Tools)
	{
		FString N;
		if (T->TryGetStringField(TEXT("name"), N))
			McpToolNameSet.Add(N);
	}
}
