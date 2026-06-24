#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "MCPToolboxChatMessage.h"

struct MCPTOOLBOX_API FMCPToolboxChatSession
{
	FString SessionId;
	FString Title;
	FDateTime CreatedAt;
	FDateTime UpdatedAt;
	TArray<FMCPToolboxChatMessage> Messages;
	FString ActiveModelId;

	FMCPToolboxChatSession()
		: CreatedAt(FDateTime::Now())
		, UpdatedAt(FDateTime::Now())
	{
		SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).Left(13);
	}

	FMCPToolboxChatSession(const FString& InTitle)
		: Title(InTitle)
		, CreatedAt(FDateTime::Now())
		, UpdatedAt(FDateTime::Now())
	{
		SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).Left(13);
	}

	FString GetPreviewText() const;
	int32 GetMessageCount() const { return Messages.Num(); }
	
	bool Serialize(FJsonObject& OutJson) const;
	bool Deserialize(const FJsonObject& Json);
};

struct MCPTOOLBOX_API FMCPToolboxSkillInfo
{
	FString SkillId;
	FString Name;
	FString Description;
	FString Version;
	FString Author;
	FString DirectoryPath;
	bool bIsInstalled = false;
};

class MCPTOOLBOX_API FMCPToolboxChatSessionManager
{
public:
	static FMCPToolboxChatSessionManager& Get();

	FMCPToolboxChatSession* CreateNewSession(const FString& Title = TEXT(""));
	FMCPToolboxChatSession* GetCurrentSession() const;
	bool SetCurrentSession(const FString& SessionId);
	void DeleteSession(const FString& SessionId);
	TArray<FMCPToolboxChatSession*> GetAllSessions() const;
	FMCPToolboxChatSession* GetSessionById(const FString& SessionId) const;
	
	void SaveSession(FMCPToolboxChatSession& Session);
	void LoadAllSessions();
	
	void AddMessageToCurrentSession(const FMCPToolboxChatMessage& Message);
	void ClearCurrentSessionMessages();
	
	FString GetSessionsDirectory() const;
	FString GetSkillsDirectory() const;
	FString GetMCPToolboxRootDir() const;
	FString GetProjectName() const;
	
	TArray<FMCPToolboxSkillInfo> GetInstalledSkills() const;
	bool InstallSkillFromDirectory(const FString& SourceDir);
	bool UninstallSkill(const FString& SkillId);
	void RefreshSkills();

private:
	FMCPToolboxChatSessionManager();
	~FMCPToolboxChatSessionManager();
	
	FMCPToolboxChatSessionManager(const FMCPToolboxChatSessionManager&) = delete;
	FMCPToolboxChatSessionManager& operator=(const FMCPToolboxChatSessionManager&) = delete;
	
	TArray<TUniquePtr<FMCPToolboxChatSession>> Sessions;
	FString CurrentSessionId;
	
	mutable TArray<FMCPToolboxSkillInfo> CachedSkills;
	mutable bool bSkillsCached = false;
	
	FString GetSessionFilePath(const FString& SessionId) const;
	void LoadSessionFromFile(const FString& FilePath);
	bool LoadSkillManifest(const FString& ManifestPath, FMCPToolboxSkillInfo& OutInfo) const;
};
