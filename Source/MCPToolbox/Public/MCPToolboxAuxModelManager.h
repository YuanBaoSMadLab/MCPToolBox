#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonSerializer.h"

// ============================================================================
// Auxiliary Model Status
// ============================================================================
enum class EMCPToolboxAuxModelStatus : uint8
{
	Unknown,
	Unavailable,
	Ready
};

// ============================================================================
// IdleSpec — Speculative Execution Result
// ============================================================================
struct MCPTOOLBOX_API FSpeculativeResult
{
	bool bValid = false;
	FString PredictedToolName;
	FString PredictedReasoning;
	double InferenceTimeMs = 0.0;

	bool IsValid() const { return bValid && !PredictedToolName.IsEmpty(); }
};

// ============================================================================
// SWE-Pruner — Pruning Result
// ============================================================================
struct MCPTOOLBOX_API FPruningResult
{
	TArray<int32> RemoveIndices;
	FString PrunedContext;
	int32 OriginalTokens = 0;
	int32 PrunedTokens = 0;

	double ReductionPercent() const
	{
		if (OriginalTokens <= 0) return 0.0;
		return 100.0 * (1.0 - static_cast<double>(PrunedTokens) / OriginalTokens);
	}
};

// ============================================================================
// FMCPToolboxAuxModelManager — Singleton, llama-server HTTP backend
// ============================================================================
class MCPTOOLBOX_API FMCPToolboxAuxModelManager
{
public:
	static FMCPToolboxAuxModelManager& Get();

	// ---- Lifecycle ----

	void RefreshStatus();
	void StartServer();   // Launch llama-server.exe (keeps model loaded)
	void StopServer();    // Kill server process
	bool IsServerRunning() { return ServerProc.IsValid() && FPlatformProcess::IsProcRunning(ServerProc); }

	// ---- Status ----

	EMCPToolboxAuxModelStatus GetStatus() const { return CurrentStatus; }
	FString GetStatusText();
	bool IsReady() { return CurrentStatus == EMCPToolboxAuxModelStatus::Ready && IsServerRunning(); }

	// ---- Inference via HTTP API ----

	/** Async HTTP POST to llama-server /completion */
	void InferAsync(const FString& Prompt, int32 MaxTokens,
		TFunction<void(bool bSuccess, const FString& Output)> OnComplete);

	/** Analyze a base64 JPEG image using the local VL model.
	 *  @param Base64JPEG  JPEG data as base64 (no data URI prefix)
	 *  @param Question    What to ask about the image
	 *  @param OnComplete  Called with the text description */
	void AnalyzeImage(const FString& Base64JPEG, const FString& Question,
		TFunction<void(const FString&)> OnComplete);

	// ---- Speculative Execution (IdleSpec) ----

	void LaunchSpeculation(const FString& ConversationContext, const FString& CurrentToolName,
		const TArray<FString>& AvailableTools,
		TFunction<void(const FSpeculativeResult&)> OnComplete);

	static int32 EstimateTokenCount(const FString& Text);

	// ---- SWE-Pruner ----

	/** Prune conversation using pre-numbered message blocks and task goal
	 *  @param NumberedBlocks  Each block: "[N] role: truncated content (first 200 chars)"
	 *  @param TaskGoal        The user's task/goal to judge relevance against */
	void PruneContext(const TArray<FString>& NumberedBlocks, const FString& TaskGoal,
		TFunction<void(const FPruningResult&)> OnComplete);

private:
	FMCPToolboxAuxModelManager();
	~FMCPToolboxAuxModelManager();
	FMCPToolboxAuxModelManager(const FMCPToolboxAuxModelManager&) = delete;
	FMCPToolboxAuxModelManager& operator=(const FMCPToolboxAuxModelManager&) = delete;

	FString GetAuxModuleDir() const;
	bool HasAuxFile(const FString& RelativePath) const;

	/** Find a free TCP port starting from BasePort */
	int32 FindFreePort(int32 BasePort) const;

	static const TCHAR* const* GetRequiredFiles();
	static int32 GetRequiredFileCount();

	// ---- Data ----
	EMCPToolboxAuxModelStatus CurrentStatus = EMCPToolboxAuxModelStatus::Unknown;
	TArray<FString> MissingFiles;
	mutable FCriticalSection Lock;

	FProcHandle ServerProc;
	int32 ServerPort = 0;
};
