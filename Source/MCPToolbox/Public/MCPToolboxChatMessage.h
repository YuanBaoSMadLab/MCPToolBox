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

	/** DeepSeek thinking mode: 累加的推理内容（reasoning_content 字段）。
	 *  DeepSeek-v4 在 thinking 模式下要求所有 assistant 消息回传时携带此字段，
	 *  否则触发 HTTP 400 "The reasoning_content in the thinking mode must be passed back to the API"。
	 *  本地不解析、不展示此字段，仅原样回传。 */
	FString ReasoningContent;

	FMCPToolboxChatMessage()
		: Timestamp(FDateTime::Now())
	{
	}
};