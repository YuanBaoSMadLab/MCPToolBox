#include "MCPToolboxAuxModelManager.h"
#include "MCPToolbox.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Async/Async.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogMCPToolboxAux, Log, All);

const TCHAR* const* FMCPToolboxAuxModelManager::GetRequiredFiles()
{
	static const TCHAR* Files[] = {
		TEXT("llama/llama.dll"),
		TEXT("llama/llama-server.exe"),
		TEXT("qwen3-vl/Qwen3VL-2B-Instruct-Q4_K_M.gguf"),
		TEXT("qwen3-vl/mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf"),
	};
	return Files;
}

int32 FMCPToolboxAuxModelManager::GetRequiredFileCount() { return 4; }

// ============================================================================
// Singleton
// ============================================================================
FMCPToolboxAuxModelManager& FMCPToolboxAuxModelManager::Get()
{
	static FMCPToolboxAuxModelManager Instance;
	return Instance;
}

FMCPToolboxAuxModelManager::FMCPToolboxAuxModelManager() { RefreshStatus(); }
FMCPToolboxAuxModelManager::~FMCPToolboxAuxModelManager() { StopServer(); }

// ============================================================================
// File Monitoring
// ============================================================================
FString FMCPToolboxAuxModelManager::GetAuxModuleDir() const
{
	FString PluginDir = FString(FPlatformProcess::BaseDir()) / TEXT("../../Plugins/MCPToolbox_Output");
	FPaths::CollapseRelativeDirectories(PluginDir);

	FString ResourcesPath = PluginDir / TEXT("Resources/AuxiliaryModule");
	if (IFileManager::Get().DirectoryExists(*ResourcesPath))
		return ResourcesPath;

	FString DevPath = PluginDir / TEXT("AuxiliaryModule");
	if (IFileManager::Get().DirectoryExists(*DevPath))
		return DevPath;

	return ResourcesPath;
}

bool FMCPToolboxAuxModelManager::HasAuxFile(const FString& RelativePath) const
{
	FString FullPath = FPaths::Combine(GetAuxModuleDir(), RelativePath);
	return IFileManager::Get().FileExists(*FullPath);
}

void FMCPToolboxAuxModelManager::RefreshStatus()
{
	FScopeLock ScopeLock(&Lock);
	MissingFiles.Empty();

	const TCHAR* const* Files = GetRequiredFiles();
	for (int32 i = 0; i < GetRequiredFileCount(); ++i)
	{
		if (!HasAuxFile(FString(Files[i])))
			MissingFiles.Add(FString(Files[i]));
	}

	CurrentStatus = MissingFiles.Num() == 0
		? EMCPToolboxAuxModelStatus::Ready
		: EMCPToolboxAuxModelStatus::Unavailable;

	UE_LOG(LogMCPToolboxAux, Log, TEXT("[AuxModel] Status: %s (Dir=%s)"), *GetStatusText(), *GetAuxModuleDir());
}

FString FMCPToolboxAuxModelManager::GetStatusText()
{
	switch (CurrentStatus)
	{
	case EMCPToolboxAuxModelStatus::Ready:
		return IsServerRunning() ? TEXT("辅助模型准备就绪") : TEXT("辅助模型就绪(服务启动中)");
	case EMCPToolboxAuxModelStatus::Unavailable:
		return TEXT("辅助模型不可用");
	default:
		return TEXT("辅助模型状态未知");
	}
}

// ============================================================================
// Port Conflict Prevention
// ============================================================================
int32 FMCPToolboxAuxModelManager::FindFreePort(int32 BasePort) const
{
	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSub) return BasePort;

	for (int32 Port = BasePort; Port < BasePort + 50; ++Port)
	{
		FSocket* TestSocket = SocketSub->CreateSocket(NAME_Stream, TEXT("AuxPortTest"), false);
		if (!TestSocket) continue;

		TSharedRef<FInternetAddr> Addr = SocketSub->CreateInternetAddr();
		Addr->SetLoopbackAddress();
		Addr->SetPort(Port);

		if (TestSocket->Bind(*Addr))
		{
			SocketSub->DestroySocket(TestSocket);
			return Port;
		}
		SocketSub->DestroySocket(TestSocket);
	}

	return BasePort;
}

// ============================================================================
// llama-server lifecycle
// ============================================================================
void FMCPToolboxAuxModelManager::StartServer()
{
	if (IsServerRunning()) return;
	if (CurrentStatus != EMCPToolboxAuxModelStatus::Ready) return;

	ServerPort = FindFreePort(8088);
	const FString AuxDir = GetAuxModuleDir();
	const FString ServerExe = FPaths::Combine(AuxDir, TEXT("llama/llama-server.exe"));
	const FString ModelPath = FPaths::Combine(AuxDir, TEXT("qwen3-vl/Qwen3VL-2B-Instruct-Q4_K_M.gguf"));
	const FString LlamaDir = FPaths::Combine(AuxDir, TEXT("llama"));

	FString Args = FString::Printf(
		TEXT("-m \"%s\" --port %d -ngl 999 -c 10240 --cache-type-k q8_0 --cache-type-v q8_0 --no-webui --host 127.0.0.1"),
		*ModelPath, ServerPort);

	UE_LOG(LogMCPToolboxAux, Log, TEXT("[AuxModel] Starting llama-server on port %d (free slot)..."), ServerPort);

	ServerProc = FPlatformProcess::CreateProc(
		*ServerExe, *Args,
		false, true, true,
		nullptr, 0,
		*LlamaDir,
		nullptr, nullptr
	);

	if (!ServerProc.IsValid())
	{
		UE_LOG(LogMCPToolboxAux, Error, TEXT("[AuxModel] Failed to start llama-server"));
		ServerPort = 0;
		return;
	}

	UE_LOG(LogMCPToolboxAux, Log, TEXT("[AuxModel] llama-server started on port %d"), ServerPort);
}

void FMCPToolboxAuxModelManager::StopServer()
{
	if (ServerProc.IsValid())
	{
		FPlatformProcess::TerminateProc(ServerProc, true);
		FPlatformProcess::CloseProc(ServerProc);
		ServerProc.Reset();
		UE_LOG(LogMCPToolboxAux, Log, TEXT("[AuxModel] llama-server stopped"));
	}
}

// ============================================================================
// HTTP Inference
// ============================================================================
void FMCPToolboxAuxModelManager::InferAsync(const FString& Prompt, int32 MaxTokens,
	TFunction<void(bool, const FString&)> OnComplete)
{
	if (!IsReady())
	{
		UE_LOG(LogMCPToolboxAux, Warning, TEXT("[AuxModel] InferAsync: not ready"));
		if (OnComplete) OnComplete(false, TEXT(""));
		return;
	}

	FString URL = FString::Printf(TEXT("http://127.0.0.1:%d/completion"), ServerPort);

	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject());
	Body->SetStringField(TEXT("prompt"), Prompt);
	Body->SetNumberField(TEXT("n_predict"), MaxTokens);
	Body->SetNumberField(TEXT("temperature"), 0.0);
	Body->SetNumberField(TEXT("top_k"), 1);
	Body->SetNumberField(TEXT("repeat_penalty"), 1.0);

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	auto Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(URL);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(BodyStr);
	Request->SetTimeout(30.0f);

	Request->OnProcessRequestComplete().BindLambda(
		[OnComplete](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
	{
		if (!bSuccess || !Resp.IsValid() || Resp->GetResponseCode() != 200)
		{
			if (OnComplete) OnComplete(false, TEXT(""));
			return;
		}

		FString Content = Resp->GetContentAsString();
		TSharedPtr<FJsonObject> RespObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		if (FJsonSerializer::Deserialize(Reader, RespObj) && RespObj.IsValid())
		{
			FString OutContent;
			RespObj->TryGetStringField(TEXT("content"), OutContent);
			if (OnComplete) OnComplete(!OutContent.IsEmpty(), OutContent);
		}
		else
		{
			if (OnComplete) OnComplete(false, TEXT(""));
		}
	});

	Request->ProcessRequest();
}

// ============================================================================
// Token Estimation
// ============================================================================
int32 FMCPToolboxAuxModelManager::EstimateTokenCount(const FString& Text)
{
	int32 CJK = 0, Other = 0;
	for (const TCHAR Ch : Text)
	{
		if ((Ch >= 0x4E00 && Ch <= 0x9FFF) || (Ch >= 0x3400 && Ch <= 0x4DBF) || (Ch >= 0xF900 && Ch <= 0xFAFF))
			CJK++;
		else
			Other++;
	}
	return FMath::Max(1, CJK / 2 + Other / 4);
}

// ============================================================================
// Image Analysis (Local VL)
// ============================================================================
void FMCPToolboxAuxModelManager::AnalyzeImage(
	const FString& Base64JPEG, const FString& Question,
	TFunction<void(const FString&)> OnComplete)
{
	if (!IsReady() || Base64JPEG.IsEmpty())
	{
		if (OnComplete) OnComplete(TEXT(""));
		return;
	}

	FString Prompt = FString::Printf(TEXT(
		"<|im_start|>system\nYou are a helpful assistant that accurately describes images.<|im_end|>\n"
		"<|im_start|>user\n%s<|im_end|>\n"
		"<|im_start|>assistant\n"),
		*Question.Left(500));

	FString URL = FString::Printf(TEXT("http://127.0.0.1:%d/completion"), ServerPort);

	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject());
	Body->SetStringField(TEXT("prompt"), Prompt);
	Body->SetNumberField(TEXT("n_predict"), 256);
	Body->SetNumberField(TEXT("temperature"), 0.0);

	// Image data array for Qwen3VL via llama-server
	TArray<TSharedPtr<FJsonValue>> Images;
	TSharedPtr<FJsonObject> ImgObj = MakeShareable(new FJsonObject());
	ImgObj->SetStringField(TEXT("data"), Base64JPEG);
	ImgObj->SetNumberField(TEXT("id"), 1);
	Images.Add(MakeShareable(new FJsonValueObject(ImgObj)));
	Body->SetArrayField(TEXT("image_data"), Images);

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	auto Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(URL);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(BodyStr);
	Request->SetTimeout(30.0f);

	Request->OnProcessRequestComplete().BindLambda(
		[OnComplete](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
	{
		if (!bSuccess || !Resp.IsValid() || Resp->GetResponseCode() != 200)
		{
			UE_LOG(LogMCPToolboxAux, Warning, TEXT("[VL] AnalyzeImage failed: HTTP error"));
			if (OnComplete) OnComplete(TEXT(""));
			return;
		}

		FString Content = Resp->GetContentAsString();
		TSharedPtr<FJsonObject> RespObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		if (FJsonSerializer::Deserialize(Reader, RespObj) && RespObj.IsValid())
		{
			FString OutContent;
			RespObj->TryGetStringField(TEXT("content"), OutContent);
			if (!OutContent.IsEmpty())
				UE_LOG(LogMCPToolboxAux, Log, TEXT("[VL] Image analyzed: %s..."), *OutContent.Left(80));
			if (OnComplete) OnComplete(OutContent);
		}
		else
		{
			if (OnComplete) OnComplete(TEXT(""));
		}
	});

	Request->ProcessRequest();
}

// ============================================================================
// IdleSpec
// ============================================================================
void FMCPToolboxAuxModelManager::LaunchSpeculation(
	const FString& ConversationContext,
	const FString& CurrentToolName,
	const TArray<FString>& AvailableTools,
	TFunction<void(const FSpeculativeResult&)> OnComplete)
{
	if (!IsReady()) { if (OnComplete) OnComplete(FSpeculativeResult{}); return; }

	FString TruncatedCtx = ConversationContext;
	if (TruncatedCtx.Len() > 3000)
		TruncatedCtx = TruncatedCtx.Right(3000);

	FString ToolListStr = FString::Join(AvailableTools, TEXT(", "));

	FString Prompt = FString::Printf(TEXT(
		"<|im_start|>system\n"
		"Given a conversation history, predict which of the available tools the AI should call next. "
		"Available tools: %s. Output only the exact tool name, nothing else.<|im_end|>\n"
		"<|im_start|>user\n"
		"Conversation:\n%s\n\n"
		"Tool \"%s\" just finished. Which available tool should be called next?<|im_end|>\n"
		"<|im_start|>assistant\n"),
		*ToolListStr, *TruncatedCtx, *CurrentToolName);

	double StartTime = FPlatformTime::Seconds();

	InferAsync(Prompt, 32, [this, AvailableTools, ToolListStr, OnComplete, StartTime](bool bSuccess, const FString& Output)
	{
		FSpeculativeResult Result;
		Result.InferenceTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

		if (bSuccess && !Output.IsEmpty())
		{
			FString Name = Output;
			Name.TrimStartAndEndInline();
			// Take first line only
			int32 NL = Name.Find(TEXT("\n"));
			if (NL != INDEX_NONE) Name = Name.Left(NL);
			Name.TrimStartAndEndInline();

			// Match against known tool names (case-insensitive)
			for (const FString& Tool : AvailableTools)
			{
				if (Tool.Equals(Name, ESearchCase::IgnoreCase))
				{
					Result.PredictedToolName = Tool;
					Result.bValid = true;
					break;
				}
			}
			if (!Result.bValid && !Name.IsEmpty())
			{
				// Try partial match
				for (const FString& Tool : AvailableTools)
				{
					if (Tool.Contains(Name) || Name.Contains(Tool))
					{
						Result.PredictedToolName = Tool;
						Result.bValid = true;
						break;
					}
				}
			}
			if (Result.bValid)
			{
				UE_LOG(LogMCPToolboxAux, Log, TEXT("[IdleSpec] %s (%.0fms)"), *Result.PredictedToolName, Result.InferenceTimeMs);
			}
			else
			{
				UE_LOG(LogMCPToolboxAux, Log, TEXT("[IdleSpec] No match: '%s' (available=%s)"), *Name, *ToolListStr);
			}
		}

		if (OnComplete) OnComplete(Result);
	});
}

// ============================================================================
// SWE-Pruner
// ============================================================================
void FMCPToolboxAuxModelManager::PruneContext(
	const TArray<FString>& NumberedBlocks,
	const FString& TaskGoal,
	TFunction<void(const FPruningResult&)> OnComplete)
{
	if (!IsReady() || NumberedBlocks.Num() <= 5)
	{
		int32 Tok = EstimateTokenCount(FString::Join(NumberedBlocks, TEXT("\n")));
		FPruningResult R; R.OriginalTokens = Tok; R.PrunedTokens = Tok;
		if (OnComplete) OnComplete(R);
		return;
	}

	// Build a compact prompt: list numbered blocks + query
	FString BlockList;
	BlockList.Reserve(NumberedBlocks.Num() * 256);
	for (int32 i = 0; i < NumberedBlocks.Num(); ++i)
		BlockList += FString::Printf(TEXT("%s\n"), *NumberedBlocks[i]);

	FString Prompt = FString::Printf(TEXT(
		"<|im_start|>system\n"
		"Judge each message's relevance to the Query. Mark messages as 'no' only when they are clearly unrelated "
		"(e.g., greetings, duplicate info, completed obsolete tasks). "
		"Output comma-separated yes/no only, %d values. Example: yes,yes,no,yes<|im_end|>\n"
		"<|im_start|>user\n"
		"Query: %s\n\nMessages:\n%s\n"
		"Output comma-separated yes/no (%d values):<|im_end|>\n"
		"<|im_start|>assistant\n"),
		NumberedBlocks.Num(), *TaskGoal.Left(250), *BlockList, NumberedBlocks.Num());

	int32 OrigTokens = EstimateTokenCount(BlockList);

	InferAsync(Prompt, 256, [this, NumberedBlocks, TaskGoal, OrigTokens, OnComplete](bool bSuccess, const FString& Output)
	{
		FPruningResult R;
		R.OriginalTokens = OrigTokens;
		TSet<int32> Remove;

		UE_LOG(LogMCPToolboxAux, Verbose, TEXT("[SWE-Pruner] Raw output: '%s'"), bSuccess ? *Output : TEXT("(request failed)"));

		FString Out;
		if (bSuccess && !Output.IsEmpty())
		{
			// Extract only the comma-separated part (ignore any extra text)
			Out = Output;
			Out.TrimStartAndEndInline();
			// Take first non-empty line
			int32 NL = Out.Find(TEXT("\n"));
			if (NL != INDEX_NONE) Out = Out.Left(NL);
			Out.TrimStartAndEndInline();

			TArray<FString> Parts;
			Out.ParseIntoArray(Parts, TEXT(","));
			for (int32 i = 0; i < Parts.Num() && i < NumberedBlocks.Num(); ++i)
			{
				FString P = Parts[i].TrimStartAndEnd();
				if (P.StartsWith(TEXT("no"), ESearchCase::IgnoreCase))
					Remove.Add(i);
			}
		}

		// Safety: never remove >50%, index 0 (system), or last 3 messages
		if (Remove.Num() > NumberedBlocks.Num() / 2) Remove.Empty();
		Remove.Remove(0);
		for (int32 i = FMath::Max(0, NumberedBlocks.Num() - 3); i < NumberedBlocks.Num(); ++i) Remove.Remove(i);

		R.RemoveIndices = Remove.Array();
		R.PrunedTokens = OrigTokens - (OrigTokens * Remove.Num() / FMath::Max(1, NumberedBlocks.Num()));

		UE_LOG(LogMCPToolboxAux, Log, TEXT("[SWE-Pruner] %d blocks, model='%s' → Remove=%d (%.1f%%)"),
			NumberedBlocks.Num(), *Out.Left(100), Remove.Num(), R.ReductionPercent());

		if (OnComplete) OnComplete(R);
	});
}
