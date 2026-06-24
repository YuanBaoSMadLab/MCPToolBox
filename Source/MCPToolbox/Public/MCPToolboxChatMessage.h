#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

UENUM(BlueprintType)
enum class EMCPToolboxMessageRole : uint8
{
	User,
	Assistant,
	System,
	Thinking
};

struct MCPTOOLBOX_API FMCPToolboxChatMessage
{
	EMCPToolboxMessageRole Role = EMCPToolboxMessageRole::User;
	FString Content;
	FDateTime Timestamp;
	bool bIsStreaming = false;
	bool bHasImageAttachment = false;
	TArray<FString> ImageDataURIs;
	TArray<FString> FileAttachments;
	FString ModelName;

	FMCPToolboxChatMessage()
		: Timestamp(FDateTime::Now())
	{
	}
};