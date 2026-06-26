#pragma once

#include "CoreMinimal.h"
#include "MCPToolboxDAGTypes.h"

class MCPTOOLBOX_API FExecutionPlanner
{
public:
	FExecutionPlanner() = default;
	~FExecutionPlanner() = default;

	bool CreatePlan(
		const TArray<TSharedPtr<FJsonObject>>& ToolCalls,
		const TSet<FString>& AvailableToolIds,
		FExecutionPlan& OutPlan,
		FString& OutError
	);

	void GetParallelBatches(
		const FExecutionPlan& Plan,
		TArray<TArray<FDAGTaskNode>>& OutBatches
	) const;

	float EstimateSpeedup(const FExecutionPlan& Plan) const;

	FString VisualizeDAG(const FExecutionPlan& Plan) const;

private:
	bool DetectCycle(const FExecutionPlan& Plan) const;

	void BuildDAGStructures(FExecutionPlan& Plan) const;
};
