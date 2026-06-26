#include "MCPToolboxExecutionPlanner.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

bool FExecutionPlanner::CreatePlan(
	const TArray<TSharedPtr<FJsonObject>>& ToolCalls,
	const TSet<FString>& AvailableToolIds,
	FExecutionPlan& OutPlan,
	FString& OutError)
{
	OutPlan = FExecutionPlan();

	for (int32 Index = 0; Index < ToolCalls.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>& CallObj = ToolCalls[Index];
		if (!CallObj.IsValid())
		{
			OutError = FString::Printf(TEXT("Tool call at index %d is null"), Index);
			return false;
		}

		FString TaskId;
		if (!CallObj->TryGetStringField(TEXT("task_id"), TaskId))
		{
			TaskId = FString::Printf(TEXT("t%d"), Index + 1);
		}

		FString ToolId;
		if (!CallObj->TryGetStringField(TEXT("tool_id"), ToolId))
		{
			if (!CallObj->TryGetStringField(TEXT("name"), ToolId))
			{
				OutError = FString::Printf(TEXT("Tool call %d missing tool_id/name"), Index);
				return false;
			}
		}

		if (!AvailableToolIds.Contains(ToolId))
		{
			OutError = FString::Printf(TEXT("Tool '%s' not found in available tools"), *ToolId);
			return false;
		}

		TSharedPtr<FJsonObject> Parameters;
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (CallObj->TryGetObjectField(TEXT("parameters"), ParamsPtr))
		{
			Parameters = *ParamsPtr;
		}
		else
		{
			Parameters = MakeShareable(new FJsonObject());
		}

		TArray<FString> Dependencies;
		const TArray<TSharedPtr<FJsonValue>>* DepsArray = nullptr;
		if (CallObj->TryGetArrayField(TEXT("depends_on"), DepsArray))
		{
			for (const TSharedPtr<FJsonValue>& DepVal : *DepsArray)
			{
				FString DepStr = DepVal->AsString();
				if (!DepStr.IsEmpty())
				{
					Dependencies.Add(DepStr);
				}
			}
		}

		FDAGTaskNode Task(TaskId, ToolId, Parameters, Dependencies);
		OutPlan.Tasks.Add(Task);
		OutPlan.TaskIndexMap.Add(TaskId, OutPlan.Tasks.Num() - 1);
	}

	BuildDAGStructures(OutPlan);

	if (DetectCycle(OutPlan))
	{
		OutError = TEXT("Circular dependency detected in execution plan");
		return false;
	}

	return true;
}

void FExecutionPlanner::BuildDAGStructures(FExecutionPlan& Plan) const
{
	Plan.AdjacencyList.Empty();
	Plan.InDegree.Empty();

	for (const FDAGTaskNode& Task : Plan.Tasks)
	{
		Plan.InDegree.FindOrAdd(Task.TaskId, 0);
		Plan.AdjacencyList.FindOrAdd(Task.TaskId);
	}

	for (const FDAGTaskNode& Task : Plan.Tasks)
	{
		for (const FString& DepTaskId : Task.Dependencies)
		{
			TSet<FString>& Adj = Plan.AdjacencyList.FindOrAdd(DepTaskId);
			Adj.Add(Task.TaskId); // TSet auto-deduplicates

			int32& Degree = Plan.InDegree.FindOrAdd(Task.TaskId, 0);
			Degree++;
		}
	}
}

bool FExecutionPlanner::DetectCycle(const FExecutionPlan& Plan) const
{
	TMap<FString, int32> TempInDegree = Plan.InDegree;
	TArray<FString> Ready;

	for (const auto& Pair : TempInDegree)
	{
		if (Pair.Value == 0)
		{
			Ready.Add(Pair.Key);
		}
	}

	int32 VisitedCount = 0;

	while (Ready.Num() > 0)
	{
		FString Current = Ready.Pop(EAllowShrinking::No);
		VisitedCount++;

		const TSet<FString>* Successors = Plan.AdjacencyList.Find(Current);
		if (Successors)
		{
			for (const FString& Successor : *Successors)
			{
				int32* Degree = TempInDegree.Find(Successor);
				if (Degree && --(*Degree) == 0)
				{
					Ready.Add(Successor);
				}
			}
		}
	}

	return VisitedCount != Plan.Tasks.Num();
}

void FExecutionPlanner::GetParallelBatches(
	const FExecutionPlan& Plan,
	TArray<TArray<FDAGTaskNode>>& OutBatches) const
{
	// Return cached result if already computed
	if (Plan.CachedBatches.Num() > 0)
	{
		OutBatches = Plan.CachedBatches;
		return;
	}

	OutBatches.Empty();

	if (Plan.Tasks.Num() == 0)
		return;

	TMap<FString, int32> TempInDegree = Plan.InDegree;
	TArray<FString> Ready;

	for (const auto& Pair : TempInDegree)
	{
		if (Pair.Value == 0)
		{
			Ready.Add(Pair.Key);
		}
	}

	while (Ready.Num() > 0)
	{
		TArray<FDAGTaskNode> CurrentBatch;

		for (const FString& TaskId : Ready)
		{
			const int32* TaskIndex = Plan.TaskIndexMap.Find(TaskId);
			if (TaskIndex && Plan.Tasks.IsValidIndex(*TaskIndex))
			{
				CurrentBatch.Add(Plan.Tasks[*TaskIndex]);
			}
		}

		if (CurrentBatch.Num() > 0)
		{
			OutBatches.Add(CurrentBatch);
		}

		TArray<FString> NextReady;
		for (const FString& TaskId : Ready)
		{
			const TSet<FString>* Successors = Plan.AdjacencyList.Find(TaskId);
			if (Successors)
			{
				for (const FString& Successor : *Successors)
				{
					int32* Degree = TempInDegree.Find(Successor);
					if (Degree && --(*Degree) == 0)
					{
						NextReady.Add(Successor);
					}
				}
			}
		}

		Ready = MoveTemp(NextReady);
	}

	// Cache result for subsequent Visualize/Estimate calls
	Plan.CachedBatches = OutBatches;
}

float FExecutionPlanner::EstimateSpeedup(const FExecutionPlan& Plan) const
{
	if (Plan.Tasks.Num() == 0)
		return 1.0f;

	TArray<TArray<FDAGTaskNode>> Batches;
	GetParallelBatches(Plan, Batches);

	int32 SerialTime = Plan.Tasks.Num();
	int32 ParallelTime = Batches.Num();

	if (ParallelTime == 0)
		return 1.0f;

	return (float)SerialTime / (float)ParallelTime;
}

FString FExecutionPlanner::VisualizeDAG(const FExecutionPlan& Plan) const
{
	TArray<TArray<FDAGTaskNode>> Batches;
	GetParallelBatches(Plan, Batches);

	FString Output;
	Output += TEXT("Execution Plan (DAG):\n");
	Output += FString::ChrN(50, TEXT('=')) + TEXT("\n");

	for (int32 i = 0; i < Batches.Num(); ++i)
	{
		Output += FString::Printf(TEXT("\nBatch %d (Parallel):\n"), i + 1);
		for (const FDAGTaskNode& Task : Batches[i])
		{
			FString DepsStr;
			if (Task.Dependencies.Num() > 0)
			{
				DepsStr = FString::Printf(TEXT(" <- [%s]"), *FString::Join(Task.Dependencies, TEXT(", ")));
			}
			Output += FString::Printf(TEXT("  - %s: %s%s\n"), *Task.TaskId, *Task.ToolId, *DepsStr);
		}
	}

	Output += FString::Printf(TEXT("\nEstimated Speedup: %.2fx"), EstimateSpeedup(Plan));

	return Output;
}
