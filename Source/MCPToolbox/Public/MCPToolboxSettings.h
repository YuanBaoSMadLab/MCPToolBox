// Copyright MCPToolbox. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MCPToolboxSettings.generated.h"

/**
 * MCP Toolbox 插件设置 (Project Settings -> Plugins -> MCP Toolbox)
 */
UCLASS(Config=MCPToolbox, DefaultConfig, meta=(DisplayName="MCP Toolbox"))
class MCPTOOLBOX_API UMCPToolboxSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMCPToolboxSettings();

	// ---- 语言与界面 ----

	UPROPERTY(EditAnywhere, Config, Category="界面")
	FString DefaultLanguage = TEXT("zh-CN");

	UPROPERTY(EditAnywhere, Config, Category="界面")
	FString UITheme = TEXT("Default");

	UPROPERTY(EditAnywhere, Config, Category="界面", meta=(ClampMin="400", ClampMax="4096"))
	int32 WindowWidth = 800;

	UPROPERTY(EditAnywhere, Config, Category="界面", meta=(ClampMin="300", ClampMax="4096"))
	int32 WindowHeight = 600;

	// ---- MCP 连接 ----

	UPROPERTY(EditAnywhere, Config, Category="MCP连接")
	FString MCPServerHost = TEXT("127.0.0.1");

	UPROPERTY(EditAnywhere, Config, Category="MCP连接")
	int32 MCPServerPort = 8000;

	UPROPERTY(EditAnywhere, Config, Category="MCP连接")
	bool MCPAutoReconnect = true;

	UPROPERTY(EditAnywhere, Config, Category="MCP连接", meta=(ClampMin="1", ClampMax="20"))
	int32 MCPMaxReconnectAttempts = 5;

	UPROPERTY(EditAnywhere, Config, Category="MCP连接", meta=(ClampMin="5", ClampMax="120"))
	int32 MCPHeartbeatInterval = 15;

	// ---- 对话 ----

	UPROPERTY(EditAnywhere, Config, Category="对话", meta=(ClampMin="10", ClampMax="10000"))
	int32 MaxChatHistory = 1000;

	UPROPERTY(EditAnywhere, Config, Category="对话")
	bool bAutoScrollToLatest = true;

	UPROPERTY(EditAnywhere, Config, Category="对话", meta=(ClampMin="0.0", ClampMax="2.0"))
	float DefaultTemperature = 0.7f;

	UPROPERTY(EditAnywhere, Config, Category="对话", meta=(ClampMin="64", ClampMax="128000"))
	int32 DefaultMaxTokens = 4096;

	// ---- 安全 ----

	UPROPERTY(EditAnywhere, Config, Category="安全")
	bool bEncryptAPIKeys = true;

	// ---- 兼容 ----

	UPROPERTY(EditAnywhere, Config, Category="兼容性")
	bool bAutoDetectWorkingDirectory = true;

	UPROPERTY(EditAnywhere, Config, Category="兼容性")
	bool bAutoDetectVisionSupport = true;
};
