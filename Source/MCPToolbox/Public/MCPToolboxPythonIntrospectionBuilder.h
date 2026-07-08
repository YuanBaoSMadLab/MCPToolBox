#pragma once

#include "CoreMinimal.h"

class MCPTOOLBOX_API FMCPToolboxPythonIntrospectionBuilder
{
public:
	static FString BuildModuleIntrospectionScript(
		const FString& FilterCondition,
		const FString& TypeFiltering,
		const FString& MaxItemsCode);

	static FString BuildClassIntrospectionScript(
		const FString& ClassName,
		const FString& InheritanceFilter,
		const FString& PrivacyFilter,
		const FString& MethodFilterCondition,
		const FString& MaxMethodsCode);

	static FString BuildFunctionIntrospectionScript(
		const FString& FunctionName,
		const FString& ClassName,
		bool bIsClassMethod);

	static FString BuildSubsystemListScript();


};
