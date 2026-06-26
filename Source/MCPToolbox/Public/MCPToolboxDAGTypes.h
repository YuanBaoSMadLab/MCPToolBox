#pragma once

#include "CoreMinimal.h"
#include "Serialization/JsonSerializer.h"

struct MCPTOOLBOX_API FDAGTaskNode
{
	FString TaskId;
	FString ToolId;
	TSharedPtr<FJsonObject> Parameters;
	TArray<FString> Dependencies;

	FDAGTaskNode() = default;
	FDAGTaskNode(const FString& InTaskId, const FString& InToolId,
		TSharedPtr<FJsonObject> InParams, const TArray<FString>& InDeps)
		: TaskId(InTaskId), ToolId(InToolId), Parameters(InParams), Dependencies(InDeps) {}
};

struct MCPTOOLBOX_API FExecutionPlan
{
	TArray<FDAGTaskNode> Tasks;
	TMap<FString, int32> TaskIndexMap;

	TMap<FString, TSet<FString>> AdjacencyList;  // TSet avoids O(n) Contains
	TMap<FString, int32> InDegree;

	// Cached topology sort result (computed once, reused by Visualize/Estimate)
	mutable TArray<TArray<FDAGTaskNode>> CachedBatches;

	bool IsValid() const { return Tasks.Num() > 0; }
};

struct MCPTOOLBOX_API FTaskExecutionResult
{
	FString TaskId;
	bool bSuccess = false;
	FString ResultJson;
	FString ErrorMessage;
	double LatencyMs = 0.0;
	int32 Attempts = 0;

	FTaskExecutionResult() = default;
	FTaskExecutionResult(const FString& InTaskId, bool bInSuccess,
		const FString& InResult, const FString& InError, double InLatency, int32 InAttempts)
		: TaskId(InTaskId), bSuccess(bInSuccess), ResultJson(InResult),
		  ErrorMessage(InError), LatencyMs(InLatency), Attempts(InAttempts) {}
};
